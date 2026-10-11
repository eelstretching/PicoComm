#include "picocomm/follower.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace picocomm {

Follower::Follower(const FollowerConfig &config, Transport &transport, FollowerApp &app)
    : config_(config), transport_(transport), app_(app) {}

void Follower::log(const char *fmt, ...) {
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

Header Follower::header() {
    Header h;
    h.seq = seq_++;
    h.src = nodeId_;
    return h;
}

void Follower::onPacket(const uint8_t *data, size_t len, uint32_t srcIp, uint16_t srcPort,
                        uint64_t rxUs) {
    Header h;
    if (!decodeHeader(data, len, h)) {
        return;
    }
    switch (h.type) {
        case PacketType::Beacon:
            handleBeacon(h, data, srcIp, srcPort);
            break;
        case PacketType::Welcome:
            handleWelcome(data, srcIp, rxUs);
            break;
        case PacketType::SyncResp:
            handleSyncResp(data, srcIp, rxUs);
            break;
        case PacketType::Cue:
            handleCue(h, data, srcIp, rxUs);
            break;
        default:
            break;  // not for followers
    }
}

void Follower::handleBeacon(const Header &h, const uint8_t *data, uint32_t ip, uint16_t port) {
    if (h.src != kLeaderId) {
        return;
    }
    Beacon b;
    decode(data, b);
    if (haveGeneration_ && b.generation != generation_) {
        leaderRebooted(b.generation);
    } else if (state_ != State::Searching) {
        return;  // already joining or joined this leader
    }
    haveGeneration_ = true;
    generation_ = b.generation;
    leaderIp_ = ip;
    leaderPort_ = port;
    state_ = State::Joining;
    sendJoin(transport_.nowUs());
}

void Follower::leaderRebooted(uint32_t newGeneration) {
    log("leader rebooted (generation %08lx -> %08lx), rejoining", (unsigned long)generation_,
        (unsigned long)newGeneration);
    stats_.leaderReboots++;
    // Show time started over: forget the old clock and the cue IDs, which will be reused.
    fit_.clear();
    burstActive_ = false;
    recentCount_ = 0;
    recentNext_ = 0;
    nodeId_ = kUnassignedId;
    forceStep_ = true;
    if (clock_.locked()) {
        clock_.reset();
        app_.clockLost();
    }
}

void Follower::handleWelcome(const uint8_t *data, uint32_t ip, uint64_t rxUs) {
    if (state_ == State::Searching || ip != leaderIp_) {
        return;
    }
    Welcome w;
    decode(data, w);
    if (memcmp(w.boardId, config_.boardId, 8) != 0 || w.generation != generation_) {
        return;
    }
    if (state_ == State::Joining) {
        log("joined as node %u", w.nodeId);
        // Start syncing right away; the first burst locks the clock.
        nextBurstUs_ = rxUs;
        nextStatusUs_ = rxUs + kStatusIntervalUs;
    }
    nodeId_ = w.nodeId;
    state_ = State::Joined;
    orphaned_ = false;
    lastLeaderUs_ = rxUs;
}

void Follower::handleSyncResp(const uint8_t *data, uint32_t ip, uint64_t rxUs) {
    if (state_ != State::Joined || ip != leaderIp_ || !burstActive_) {
        return;
    }
    SyncResp r;
    decode(data, r);
    if (r.t1 < burstStartUs_ || r.t1 > rxUs) {
        return;  // a response to an earlier burst, or garbage
    }
    SyncSample s;
    if (!makeSample(r.t1, r.t2, r.t3, rxUs, s)) {
        return;
    }
    burst_.add(s);
    stats_.syncSamples++;
    lastLeaderUs_ = rxUs;
}

void Follower::handleCue(const Header &h, const uint8_t *data, uint32_t ip, uint64_t rxUs) {
    if (state_ != State::Joined || ip != leaderIp_ || h.src != kLeaderId) {
        return;
    }
    if (!clock_.locked()) {
        // Can't tell when "at" is yet. No ACK, so the leader resends, and by then the first sync
        // burst (which starts the moment WELCOME arrives) has locked the clock.
        return;
    }
    Cue c;
    decode(data, c);

    for (int i = 0; i < recentCount_; i++) {
        if (recent_[i].cueId == c.cueId) {
            // A resend: our ACK was lost. ACK again, but don't deliver twice.
            stats_.cueDuplicates++;
            sendAck(h.seq, recent_[i]);
            return;
        }
    }

    RecentCue r{c.cueId, AckStatus::Ok, 0};
    if (c.kind == CueKind::Effect && !app_.knowsEffect(c.effect)) {
        r.status = AckStatus::UnknownEffect;
        log("cue %u: unknown effect 0x%04x", c.cueId, c.effect);
    } else {
        int64_t late = (int64_t)(showTime(rxUs) - c.atUs);
        if (late > 0) {
            r.status = AckStatus::Late;
            r.lateByUs = late > INT32_MAX ? INT32_MAX : (int32_t)late;
            stats_.cuesLate++;
        }
        stats_.cuesAccepted++;
        if (c.kind == CueKind::Effect) {
            lastEffect_ = c.effect;
        }
        lastCueId_ = c.cueId;
        app_.deliverCue(c);
    }
    recent_[recentNext_] = r;
    recentNext_ = (recentNext_ + 1) % kRecentCues;
    if (recentCount_ < kRecentCues) {
        recentCount_++;
    }
    sendAck(h.seq, r);
}

void Follower::poll() {
    uint64_t now = transport_.nowUs();

    if (state_ == State::Joining && now - lastJoinUs_ >= kJoinRetryUs) {
        sendJoin(now);
    }

    if (state_ == State::Joined) {
        if (!burstActive_ && now >= nextBurstUs_) {
            burstActive_ = true;
            burstStartUs_ = now;
            burstSent_ = 0;
            burst_.reset();
            nextBurstUs_ = now + kBurstIntervalUs;
            stats_.bursts++;
        }
        if (burstActive_) {
            while (burstSent_ < kBurstSize && now >= burstStartUs_ + burstSent_ * kBurstSpacingUs) {
                sendSyncReq();
                burstSent_++;
            }
            if (now >= burstStartUs_ + kBurstWindowUs) {
                finishBurst(now);
            }
        }
        if (clock_.locked() && now >= nextStatusUs_) {
            sendStatus(now);
            nextStatusUs_ = now + kStatusIntervalUs;
        }
        if (now - lastLeaderUs_ > kOrphanTimeoutUs) {
            orphan(now);
        }
    }

    if (clock_.update((int64_t)now)) {
        app_.clockChanged(clock_.published());
    }
}

void Follower::finishBurst(uint64_t now) {
    burstActive_ = false;
    SyncSample best;
    if (!burst_.best(best)) {
        return;
    }
    fit_.add(best);
    lastDelayUs_ = best.delayUs;
    lastSampleUs_ = now;
    ClockModel target;
    fit_.fit(target);
    bool wasLocked = clock_.locked();
    int64_t before = clock_.published().offsetAt((int64_t)now);
    if (clock_.setTarget(target, (int64_t)now, forceStep_)) {
        stats_.steps++;
        if (wasLocked && !forceStep_) {
            log("clock stepped by %lld us",
                (long long)(clock_.published().offsetAt((int64_t)now) - before));
        } else {
            log("clock locked, round trip %lu us", (unsigned long)best.delayUs);
        }
        app_.clockChanged(clock_.published());
    }
    forceStep_ = false;
}

void Follower::orphan(uint64_t now) {
    log("no sync response for %llu s: orphaned, back to the local playlist",
        (unsigned long long)(kOrphanTimeoutUs / 1000000));
    stats_.orphanings++;
    orphaned_ = true;
    state_ = State::Searching;  // rejoin on the next beacon
    // Back on the local playlist, so the current scene's cue (resent on rejoin) must play again.
    recentCount_ = 0;
    recentNext_ = 0;
    burstActive_ = false;
    nodeId_ = kUnassignedId;
    Cue c;
    c.cueId = 0;
    c.kind = CueKind::ResumeLocal;
    c.atUs = showTime(now);
    app_.deliverCue(c);
}

void Follower::sendJoin(uint64_t now) {
    lastJoinUs_ = now;
    Join j;
    memcpy(j.boardId, config_.boardId, 8);
    j.propType = (uint8_t)config_.propType;
    j.propIndex = config_.propIndex;
    j.fwVersion = config_.fwVersion;
    j.catalogVersion = kCatalogVersion;
    j.capabilities = config_.capabilities;
    uint8_t pkt[kPacketSize];
    encode(header(), j, pkt);
    transport_.send(leaderIp_, leaderPort_, pkt);
}

void Follower::sendSyncReq() {
    SyncReq r;
    uint8_t pkt[kPacketSize];
    Header h = header();
    // t1 as late as possible, right before the send.
    r.t1 = transport_.nowUs();
    encode(h, r, pkt);
    transport_.send(leaderIp_, leaderPort_, pkt);
}

void Follower::sendStatus(uint64_t now) {
    Status s;
    s.generation = generation_;
    s.cueId = lastCueId_;
    s.effect = lastEffect_;
    s.syncDelayUs = lastDelayUs_ > 65535 ? 65535 : (uint16_t)lastDelayUs_;
    uint64_t age = (now - lastSampleUs_) / 1000;
    s.syncAgeMs = age > 65535 ? 65535 : (uint16_t)age;
    s.uptimeS = (uint32_t)(now / 1000000);
    app_.fillStatus(s);
    uint8_t pkt[kPacketSize];
    encode(header(), s, pkt);
    transport_.send(leaderIp_, leaderPort_, pkt);
}

void Follower::sendAck(uint16_t ackedSeq, const RecentCue &r) {
    Ack a;
    a.ackedType = PacketType::Cue;
    a.status = r.status;
    a.ackedSeq = ackedSeq;
    a.cueId = r.cueId;
    a.lateByUs = r.lateByUs;
    uint8_t pkt[kPacketSize];
    encode(header(), a, pkt);
    transport_.send(leaderIp_, leaderPort_, pkt);
}

}  // namespace picocomm
