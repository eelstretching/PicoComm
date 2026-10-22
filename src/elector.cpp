#include "picocomm/elector.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace picocomm {

Elector::Elector(const ElectorConfig &config, Transport &transport, ElectorApp &app)
    : config_(config), transport_(transport), app_(app) {
    jitterUs_ = config_.random32 != nullptr ? config_.random32() % kJitterUs : 0;
    startUs_ = transport_.nowUs();
    if (config_.priority == kAlwaysLead) {
        lead(startUs_, "fixed leader");
    } else {
        follow();
    }
}

void Elector::log(const char *fmt, ...) {
    if (config_.log == nullptr) {
        return;
    }
    char line[120];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    config_.log(line);
}

void Elector::follow(const ClockModel *seed) {
    leader_.reset();
    FollowerConfig fc;
    memcpy(fc.boardId, config_.boardId, 8);
    fc.propType = config_.propType;
    fc.propIndex = config_.propIndex;
    fc.fwVersion = config_.fwVersion;
    fc.capabilities = config_.capabilities;
    fc.log = config_.log;
    follower_.emplace(fc, transport_, app_);
    if (seed != nullptr) {
        follower_->seedClock(*seed);
    }
}

void Elector::lead(uint64_t now, const char *why) {
    // Carry on the show time we were following, if we have it, so the props don't jump.
    int64_t offset = 0;
    if (follower_ && follower_->locked()) {
        offset = (int64_t)(follower_->showTime(now) - now);
    }
    uint32_t generation = config_.random32 != nullptr ? config_.random32() : (uint32_t)now;
    if (generation == 0) {
        generation = 1;
    }
    follower_.reset();

    LeaderConfig lc;
    lc.generation = generation;
    lc.propType = config_.propType;
    lc.propIndex = config_.propIndex;
    lc.priority = config_.priority;
    lc.showOffsetUs = offset;
    lc.log = config_.log;
    leader_.emplace(lc, transport_, app_);
    leader_->setOwnAddress(config_.ownIp);
    leaderIp_ = 0;
    if (config_.priority != kAlwaysLead) {
        takeovers_++;
    }
    if (offset == 0) {
        app_.wallClockHeard(0, 0);  // our own new show time: nothing heard before applies
    }
    log("leading (%s), generation %08lx, priority %lu%s", why, (unsigned long)generation,
        (unsigned long)config_.priority, offset != 0 ? ", carrying on show time" : "");

    ClockModel m;
    m.refLocalUs = (int64_t)now;
    m.refOffsetUs = offset;
    app_.clockChanged(m);
    app_.leadingChanged(true);
}

void Elector::heardBeacon(const Beacon &b, uint32_t ip, uint64_t now) {
    Rank theirs{b.priority, ip};
    bool silent = leaderIp_ == 0 || (int64_t)(now - leaderHeardUs_) > (int64_t)kLeaderSilentUs;
    if (ip != leaderIp_ && !silent && !(theirs > leaderRank_)) {
        return;  // a lower-ranked leader, which steps down once it hears ours
    }
    if (ip != leaderIp_) {
        log("following the leader at %lu.%lu.%lu.%lu, priority %lu", (unsigned long)(ip & 0xff),
            (unsigned long)((ip >> 8) & 0xff), (unsigned long)((ip >> 16) & 0xff),
            (unsigned long)(ip >> 24), (unsigned long)b.priority);
    }
    leaderIp_ = ip;
    leaderRank_ = theirs;
    leaderHeardUs_ = now;
}

void Elector::onPacket(const uint8_t *data, size_t len, uint32_t srcIp, uint16_t srcPort,
                       uint64_t rxUs) {
    Header h;
    if (!decodeHeader(data, len, h)) {
        return;
    }
    if (h.type == PacketType::Beacon) {
        if (srcIp == config_.ownIp || h.src != kLeaderId) {
            return;  // our own beacon, broadcast back to us
        }
        Beacon b;
        decode(data, b);
        if (leader_) {
            if (config_.priority == kAlwaysLead || !(Rank{b.priority, srcIp} > ownRank())) {
                return;  // it will step down when it hears ours
            }
            log("stepping down: a leader with priority %lu outranks us", (unsigned long)b.priority);
            stepDowns_++;
            // Keep our show clock running; the new leader's is close if we both carried on the
            // same show, and then the first sync slews instead of stepping.
            ClockModel own;
            own.refLocalUs = (int64_t)rxUs;
            own.refOffsetUs = (int64_t)(leader_->showTime() - transport_.nowUs());
            follow(&own);
            app_.leadingChanged(false);
        }
        heardBeacon(b, srcIp, rxUs);
        if (srcIp == leaderIp_) {
            follower_->onPacket(data, len, srcIp, srcPort, rxUs);
            if (b.unixTime != 0) {
                // The middle of the 1/256 s step the fraction was rounded down into.
                uint64_t unixUs = (uint64_t)b.unixTime * 1000000 +
                                  ((uint64_t)b.unixFraction * 2 + 1) * 1000000 / 512;
                app_.wallClockHeard(unixUs, b.showTimeUs);
            }
        }
        return;
    }
    if (leader_) {
        leader_->onPacket(data, len, srcIp, srcPort, rxUs);
    } else {
        follower_->onPacket(data, len, srcIp, srcPort, rxUs);
    }
}

void Elector::poll() {
    if (leader_) {
        leader_->poll();
        return;
    }
    follower_->poll();
    uint64_t now = transport_.nowUs();
    if (!candidate() || now - startUs_ < kListenUs + jitterUs_) {
        return;
    }
    // The leader is alive if we heard its beacon, or a sync answer from it, lately.
    uint64_t heard = leaderHeardUs_;
    if (leaderIp_ != 0 && follower_->leaderIp() == leaderIp_ &&
        follower_->state() == Follower::State::Joined &&
        (int64_t)(follower_->lastLeaderUs() - heard) > 0) {
        heard = follower_->lastLeaderUs();
    }
    bool alive = leaderIp_ != 0 && (int64_t)(now - heard) < (int64_t)(kLeaderSilentUs + jitterUs_);
    if (!alive) {
        lead(now, leaderIp_ != 0 ? "the leader went quiet" : "no leader heard");
    } else if (ownRank() > leaderRank_ && follower_->state() == Follower::State::Joined &&
               follower_->locked()) {
        lead(now, "we outrank the leader");
    }
}

uint16_t Elector::cue(uint16_t groupMask, const Cue &spec, uint32_t leadMs) {
    return leader_ ? leader_->cue(groupMask, spec, leadMs) : 0;
}

}  // namespace picocomm
