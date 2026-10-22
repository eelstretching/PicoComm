#include "picocomm/leader.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace picocomm {

Leader::Leader(const LeaderConfig &config, Transport &transport, LeaderApp &app)
    : config_(config), transport_(transport), app_(app) {}

void Leader::log(const char *fmt, ...) {
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

Header Leader::header(uint8_t flags) {
    Header h;
    h.seq = seq_++;
    h.src = kLeaderId;
    h.flags = flags;
    return h;
}

int Leader::activeFollowers() const {
    int n = 0;
    for (const Member &m : members_) {
        if (m.used && m.state != NodeState::Lost) {
            n++;
        }
    }
    return n;
}

Leader::Member *Leader::findMember(uint8_t nodeId, uint32_t ip) {
    if (nodeId == 0 || nodeId > kMaxFollowers) {
        return nullptr;
    }
    Member &m = members_[nodeId - 1];
    return (m.used && m.ip == ip) ? &m : nullptr;
}

void Leader::heard(Member &m, uint64_t rxUs) {
    m.lastHeardUs = rxUs;
    if (m.state == NodeState::Stale || m.state == NodeState::Lost) {
        log("node %u (type %u #%u) is back", m.nodeId, m.propType, m.propIndex);
        m.state = NodeState::Synced;
    }
}

void Leader::onPacket(const uint8_t *data, size_t len, uint32_t srcIp, uint16_t srcPort,
                      uint64_t rxUs) {
    Header h;
    if (!decodeHeader(data, len, h)) {
        return;
    }
    rxUs += (uint64_t)config_.showOffsetUs;  // everything below runs on show time
    switch (h.type) {
        case PacketType::Join:
            handleJoin(data, srcIp, srcPort, rxUs);
            break;
        case PacketType::SyncReq:
            handleSyncReq(h, data, srcIp, srcPort, rxUs);
            break;
        case PacketType::Ack:
            handleAck(h, data, srcIp, rxUs);
            break;
        case PacketType::Status:
            handleStatus(h, data, srcIp, rxUs);
            break;
        case PacketType::Query:
            handleQuery(data, srcIp, srcPort);
            break;
        case PacketType::Override:
            handleOverride(h, data, srcIp, srcPort);
            break;
        default:
            break;  // not for the leader (including our own beacons coming back)
    }
}

void Leader::handleSyncReq(const Header &h, const uint8_t *data, uint32_t ip, uint16_t port,
                           uint64_t rxUs) {
    SyncReq req;
    decode(data, req);
    // Observers may always sync, and followers we know. A node ID we never gave out at that
    // address is a follower of a previous generation of ours (we rebooted, and it hasn't heard our
    // new beacon yet): our new show time would only throw its clock fit off, so it gets no answer
    // until it rejoins.
    Member *m = findMember(h.src, ip);
    if (m == nullptr && h.src != kObserverId && h.src != kUnassignedId) {
        return;
    }
    // Answer first, bookkeeping after.
    SyncResp resp;
    resp.t1 = req.t1;
    resp.t2 = rxUs;
    uint8_t pkt[kPacketSize];
    Header rh = header();
    resp.t3 = showTime();
    encode(rh, resp, pkt);
    transport_.send(ip, port, pkt);

    if (m != nullptr) {
        heard(*m, rxUs);
        if (m->state == NodeState::Joining) {
            m->state = NodeState::Synced;
        }
    }
}

void Leader::handleJoin(const uint8_t *data, uint32_t ip, uint16_t port, uint64_t rxUs) {
    Join j;
    decode(data, j);

    // A board that has been here before keeps its node ID.
    Member *m = nullptr;
    for (Member &c : members_) {
        if (c.used && memcmp(c.boardId, j.boardId, 8) == 0) {
            m = &c;
            break;
        }
    }
    bool rejoin = m != nullptr;
    if (m == nullptr) {
        for (Member &c : members_) {
            if (!c.used) {
                m = &c;
                break;
            }
        }
    }
    if (m == nullptr) {
        // Full: reuse the slot of a board that's been gone the longest.
        for (Member &c : members_) {
            if (c.state == NodeState::Lost && (m == nullptr || c.lastHeardUs < m->lastHeardUs)) {
                m = &c;
            }
        }
        if (m == nullptr) {
            log("roster full, ignoring a JOIN");
            return;
        }
        *m = Member();
    }

    int slot = (int)(m - members_);
    if (!m->used) {
        *m = Member();
        m->used = true;
        m->nodeId = (uint8_t)(slot + 1);
        memcpy(m->boardId, j.boardId, 8);
    }
    m->propType = j.propType;
    m->propIndex = j.propIndex;
    m->fwVersion = j.fwVersion;
    m->catalogVersion = j.catalogVersion;
    m->capabilities = j.capabilities;
    m->ip = ip;
    m->port = port;
    m->state = NodeState::Joining;
    m->lastHeardUs = rxUs;
    for (PendingCue &p : m->pending) {
        p.used = false;
    }

    log("%s: node %u, type %u #%u, fw %u, catalog %u", rejoin ? "rejoin" : "join", m->nodeId,
        j.propType, j.propIndex, j.fwVersion, j.catalogVersion);
    if (j.catalogVersion != kCatalogVersion) {
        log("node %u has effect catalog %u, leader has %u", m->nodeId, j.catalogVersion,
            kCatalogVersion);
    }

    Welcome w;
    w.generation = config_.generation;
    w.nodeId = m->nodeId;
    w.groupMask = j.propType < 16 ? (uint16_t)(1u << j.propType) : 0;
    memcpy(w.boardId, j.boardId, 8);
    uint8_t pkt[kPacketSize];
    encode(header(), w, pkt);
    transport_.send(ip, port, pkt);

    // Catch the late joiner up on the current scene.
    uint64_t now = showTime();
    if (j.propType < 16) {
        if (haveSceneCue_[j.propType]) {
            queueCue(*m, sceneCue_[j.propType], sceneCue_[j.propType].atUs < now, now);
        }
        if (haveBrightnessCue_[j.propType]) {
            queueCue(*m, brightnessCue_[j.propType], brightnessCue_[j.propType].atUs < now, now);
        }
    }
}

void Leader::handleAck(const Header &h, const uint8_t *data, uint32_t ip, uint64_t rxUs) {
    Member *m = findMember(h.src, ip);
    if (m == nullptr) {
        return;
    }
    heard(*m, rxUs);
    Ack a;
    decode(data, a);
    if (a.ackedType != PacketType::Cue) {
        return;
    }
    for (PendingCue &p : m->pending) {
        if (!p.used || p.cue.cueId != a.cueId) {
            continue;
        }
        p.used = false;
        if (a.status == AckStatus::Late && !p.catchUp) {
            m->cuesLate++;
            m->lastLateCueId = a.cueId;
            m->lastLateUs = a.lateByUs;
            log("node %u: cue %u was %ld us late (%u tries)", m->nodeId, a.cueId,
                (long)a.lateByUs, p.tries);
        } else if (a.status == AckStatus::UnknownEffect) {
            m->cuesUnknown++;
            log("node %u: cue %u: effect 0x%04x unknown", m->nodeId, a.cueId, p.cue.effect);
        }
    }
}

void Leader::handleStatus(const Header &h, const uint8_t *data, uint32_t ip, uint64_t rxUs) {
    Member *m = findMember(h.src, ip);
    if (m == nullptr) {
        return;
    }
    heard(*m, rxUs);
    decode(data, m->status);
    m->hasStatus = true;
}

void Leader::handleQuery(const uint8_t *data, uint32_t ip, uint16_t port) {
    Query q;
    decode(data, q);
    if (q.what != kQueryRoster) {
        return;
    }
    uint64_t now = showTime();
    int count = 1;
    for (const Member &m : members_) {
        if (m.used) {
            count++;
        }
    }

    // Row 0 is the leader itself.
    Status own;
    app_.fillStatus(own);
    RosterEntry e;
    e.nodeId = kLeaderId;
    e.propType = (uint8_t)config_.propType;
    e.propIndex = config_.propIndex;
    e.state = NodeState::Synced;
    e.effect = own.effect;
    e.cueId = own.cueId;
    e.rssi = own.rssi;
    e.fps = own.fps;
    e.missedFrames = own.missedFrames;
    e.ipv4 = ownIp_;
    e.entryIndex = 0;
    e.entryCount = (uint8_t)count;
    uint8_t pkt[kPacketSize];
    encode(header(), e, pkt);
    transport_.send(ip, port, pkt);

    int index = 1;
    for (const Member &m : members_) {
        if (!m.used) {
            continue;
        }
        RosterEntry r;
        r.nodeId = m.nodeId;
        r.propType = m.propType;
        r.propIndex = m.propIndex;
        r.state = m.state;
        uint64_t heardMs = (now - m.lastHeardUs) / 1000;
        r.lastHeardMs = heardMs > 65535 ? 65535 : (uint16_t)heardMs;
        if (m.hasStatus) {
            r.effect = m.status.effect;
            r.cueId = m.status.cueId;
            r.syncDelayUs = m.status.syncDelayUs;
            r.rssi = m.status.rssi;
            r.fps = m.status.fps;
            r.missedFrames = m.status.missedFrames;
        }
        r.ipv4 = m.ip;
        r.entryIndex = (uint8_t)index++;
        r.entryCount = (uint8_t)count;
        encode(header(), r, pkt);
        transport_.send(ip, port, pkt);
    }
}

void Leader::handleOverride(const Header &h, const uint8_t *data, uint32_t ip, uint16_t port) {
    uint16_t id;
    if (lastOverrideCueId_ != 0 && ip == lastOverrideIp_ && port == lastOverridePort_ &&
        h.seq == lastOverrideSeq_) {
        id = lastOverrideCueId_;  // a resend: our ACK was lost, so just ACK again
    } else {
        Override o;
        decode(data, o);
        Cue spec;
        spec.kind = o.kind;
        spec.transition = o.transition;
        spec.effect = o.effect;
        spec.transitionMs = o.transitionMs;
        memcpy(spec.params, o.params, 8);
        uint32_t lead = o.delayMs == 0 ? kDefaultLeadMs : o.delayMs;
        id = cue(o.targetGroupMask, spec, lead);
        log("override from observer: cue %u, effect 0x%04x, groups %04x, in %lu ms", id, o.effect,
            o.targetGroupMask, (unsigned long)lead);
        lastOverrideIp_ = ip;
        lastOverridePort_ = port;
        lastOverrideSeq_ = h.seq;
        lastOverrideCueId_ = id;
    }

    Ack a;
    a.ackedType = PacketType::Override;
    a.status = AckStatus::Ok;
    a.ackedSeq = h.seq;
    a.cueId = id;
    uint8_t pkt[kPacketSize];
    encode(header(), a, pkt);
    transport_.send(ip, port, pkt);
}

uint16_t Leader::cue(uint16_t groupMask, const Cue &spec, uint32_t leadMs) {
    return cueAt(groupMask, spec, showTime() + (uint64_t)leadMs * 1000);
}

uint16_t Leader::cueAt(uint16_t groupMask, const Cue &spec, uint64_t atShowUs) {
    Cue c = spec;
    c.cueId = nextCueId_++;
    if (nextCueId_ == 0) {
        nextCueId_ = 1;  // 0 means "no cue" (the follower's own ResumeLocal on orphaning)
    }
    c.atUs = atShowUs;
    bool brightness = c.kind == CueKind::Brightness;
    if (!brightness) {
        sceneId_++;
    }
    for (int g = 0; g < 16; g++) {
        if (groupMask & (1u << g)) {
            if (brightness) {
                brightnessCue_[g] = c;
                haveBrightnessCue_[g] = true;
            } else {
                sceneCue_[g] = c;
                haveSceneCue_[g] = true;
            }
        }
    }

    if ((uint8_t)config_.propType < 16 && (groupMask & groupBit(config_.propType))) {
        app_.deliverCue(c);
    }
    uint64_t now = showTime();
    for (Member &m : members_) {
        if (m.used && m.state != NodeState::Lost && m.propType < 16 &&
            (groupMask & (1u << m.propType))) {
            queueCue(m, c, false, now);
        }
    }
    return c.cueId;
}

void Leader::queueCue(Member &m, const Cue &c, bool catchUp, uint64_t now) {
    PendingCue *slot = nullptr;
    for (PendingCue &p : m.pending) {
        if (!p.used) {
            slot = &p;
            break;
        }
    }
    if (slot == nullptr) {
        // Full: drop the oldest. Four unacknowledged cues means the follower isn't answering.
        slot = &m.pending[0];
        for (PendingCue &p : m.pending) {
            if (p.firstSentUs < slot->firstSentUs) {
                slot = &p;
            }
        }
        m.cuesMissed++;
        log("node %u: too many unacknowledged cues, dropped cue %u", m.nodeId, slot->cue.cueId);
    }
    slot->used = true;
    slot->catchUp = catchUp;
    slot->cue = c;
    slot->tries = 0;
    slot->firstSentUs = now;
    slot->giveUpUs = (c.atUs > now ? c.atUs : now) + kGiveUpUs;
    m.cuesSent++;
    sendCue(m, *slot, now);
}

void Leader::sendCue(Member &m, PendingCue &p, uint64_t now) {
    p.lastSentUs = now;
    p.tries++;
    uint8_t pkt[kPacketSize];
    encode(header(kFlagAckRequested), p.cue, pkt);
    transport_.send(m.ip, m.port, pkt);
}

void Leader::sendBeacon(uint64_t now) {
    Beacon b;
    b.generation = config_.generation;
    b.showTimeUs = now;
    uint64_t unixUs = app_.unixTimeUs(now);
    b.unixTime = (uint32_t)(unixUs / 1000000);
    b.unixFraction = (uint8_t)(unixUs % 1000000 * 256 / 1000000);
    b.sceneId = sceneId_;
    int n = activeFollowers();
    b.followerCount = (uint8_t)(n > 255 ? 255 : n);
    b.priority = config_.priority;
    uint8_t pkt[kPacketSize];
    encode(header(), b, pkt);
    transport_.broadcast(kPort, pkt);
}

void Leader::poll() {
    uint64_t now = showTime();
    if (now >= nextBeaconUs_) {
        sendBeacon(now);
        nextBeaconUs_ = now + kBeaconIntervalUs;
    }
    for (Member &m : members_) {
        if (!m.used) {
            continue;
        }
        uint64_t quiet = now - m.lastHeardUs;
        if (quiet > kLostUs && m.state != NodeState::Lost) {
            m.state = NodeState::Lost;
            log("node %u (type %u #%u) lost", m.nodeId, m.propType, m.propIndex);
        } else if (quiet > kStaleUs && m.state != NodeState::Stale &&
                   m.state != NodeState::Lost) {
            m.state = NodeState::Stale;
            log("node %u (type %u #%u) stale", m.nodeId, m.propType, m.propIndex);
        }
        for (PendingCue &p : m.pending) {
            if (!p.used) {
                continue;
            }
            if (now >= p.giveUpUs) {
                p.used = false;
                m.cuesMissed++;
                log("node %u: gave up on cue %u after %u tries", m.nodeId, p.cue.cueId, p.tries);
            } else if (now - p.lastSentUs >= kResendIntervalUs) {
                sendCue(m, p, now);
            }
        }
    }
}

}  // namespace picocomm
