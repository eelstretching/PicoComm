#include "picocomm/wire.h"

#include <cstring>

namespace picocomm {

namespace {

void begin(Header h, PacketType type, uint8_t *out) {
    memset(out, 0, kPacketSize);
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = kProtocolVersion;
    out[3] = (uint8_t)type;
    put16(out + 4, h.seq);
    out[6] = h.src;
    out[7] = h.flags;
}

}  // namespace

bool decodeHeader(const uint8_t *in, size_t len, Header &h) {
    if (len != kPacketSize || in[0] != kMagic0 || in[1] != kMagic1 || in[2] != kProtocolVersion) {
        return false;
    }
    if (in[3] < (uint8_t)PacketType::Beacon || in[3] > (uint8_t)PacketType::CueStats) {
        return false;
    }
    h.type = (PacketType)in[3];
    h.seq = get16(in + 4);
    h.src = in[6];
    h.flags = in[7];
    return true;
}

void encode(Header h, const Beacon &p, uint8_t *out) {
    begin(h, PacketType::Beacon, out);
    put32(out + 8, p.generation);
    put64(out + 12, p.showTimeUs);
    put32(out + 20, p.unixTime);
    put16(out + 24, p.sceneId);
    out[26] = p.followerCount;
    put32(out + 27, p.priority);
    out[31] = p.unixFraction;
}

void decode(const uint8_t *in, Beacon &p) {
    p.generation = get32(in + 8);
    p.showTimeUs = get64(in + 12);
    p.unixTime = get32(in + 20);
    p.sceneId = get16(in + 24);
    p.followerCount = in[26];
    p.priority = get32(in + 27);
    p.unixFraction = in[31];
}

void encode(Header h, const Join &p, uint8_t *out) {
    begin(h, PacketType::Join, out);
    memcpy(out + 8, p.boardId, 8);
    out[16] = p.propType;
    out[17] = p.propIndex;
    put16(out + 18, p.fwVersion);
    put16(out + 20, p.catalogVersion);
    put16(out + 22, p.capabilities);
}

void decode(const uint8_t *in, Join &p) {
    memcpy(p.boardId, in + 8, 8);
    p.propType = in[16];
    p.propIndex = in[17];
    p.fwVersion = get16(in + 18);
    p.catalogVersion = get16(in + 20);
    p.capabilities = get16(in + 22);
}

void encode(Header h, const Welcome &p, uint8_t *out) {
    begin(h, PacketType::Welcome, out);
    put32(out + 8, p.generation);
    out[12] = p.nodeId;
    put16(out + 14, p.groupMask);
    memcpy(out + 16, p.boardId, 8);
}

void decode(const uint8_t *in, Welcome &p) {
    p.generation = get32(in + 8);
    p.nodeId = in[12];
    p.groupMask = get16(in + 14);
    memcpy(p.boardId, in + 16, 8);
}

void encode(Header h, const SyncReq &p, uint8_t *out) {
    begin(h, PacketType::SyncReq, out);
    put64(out + 8, p.t1);
}

void decode(const uint8_t *in, SyncReq &p) { p.t1 = get64(in + 8); }

void encode(Header h, const SyncResp &p, uint8_t *out) {
    begin(h, PacketType::SyncResp, out);
    put64(out + 8, p.t1);
    put64(out + 16, p.t2);
    put64(out + 24, p.t3);
}

void decode(const uint8_t *in, SyncResp &p) {
    p.t1 = get64(in + 8);
    p.t2 = get64(in + 16);
    p.t3 = get64(in + 24);
}

void encode(Header h, const Cue &p, uint8_t *out) {
    begin(h, PacketType::Cue, out);
    put16(out + 8, p.cueId);
    out[10] = (uint8_t)p.kind;
    out[11] = (uint8_t)p.transition;
    put64(out + 12, p.atUs);
    put16(out + 20, p.effect);
    put16(out + 22, p.transitionMs);
    memcpy(out + 24, p.params, 8);
}

void decode(const uint8_t *in, Cue &p) {
    p.cueId = get16(in + 8);
    p.kind = (CueKind)in[10];
    p.transition = (Transition)in[11];
    p.atUs = get64(in + 12);
    p.effect = get16(in + 20);
    p.transitionMs = get16(in + 22);
    memcpy(p.params, in + 24, 8);
}

void encode(Header h, const Ack &p, uint8_t *out) {
    begin(h, PacketType::Ack, out);
    out[8] = (uint8_t)p.ackedType;
    out[9] = (uint8_t)p.status;
    put16(out + 10, p.ackedSeq);
    put16(out + 12, p.cueId);
    put32(out + 16, (uint32_t)p.lateByUs);
}

void decode(const uint8_t *in, Ack &p) {
    p.ackedType = (PacketType)in[8];
    p.status = (AckStatus)in[9];
    p.ackedSeq = get16(in + 10);
    p.cueId = get16(in + 12);
    p.lateByUs = (int32_t)get32(in + 16);
}

void encode(Header h, const Status &p, uint8_t *out) {
    begin(h, PacketType::Status, out);
    put32(out + 8, p.generation);
    put16(out + 12, p.cueId);
    put16(out + 14, p.effect);
    put16(out + 16, p.syncDelayUs);
    put16(out + 18, p.syncAgeMs);
    out[20] = p.fps;
    out[21] = (uint8_t)p.rssi;
    put16(out + 22, p.missedFrames);
    put16(out + 24, p.freeRamKB);
    put32(out + 26, p.uptimeS);
}

void decode(const uint8_t *in, Status &p) {
    p.generation = get32(in + 8);
    p.cueId = get16(in + 12);
    p.effect = get16(in + 14);
    p.syncDelayUs = get16(in + 16);
    p.syncAgeMs = get16(in + 18);
    p.fps = in[20];
    p.rssi = (int8_t)in[21];
    p.missedFrames = get16(in + 22);
    p.freeRamKB = get16(in + 24);
    p.uptimeS = get32(in + 26);
}

void encode(Header h, const Query &p, uint8_t *out) {
    begin(h, PacketType::Query, out);
    out[8] = p.what;
}

void decode(const uint8_t *in, Query &p) { p.what = in[8]; }

void encode(Header h, const RosterEntry &p, uint8_t *out) {
    begin(h, PacketType::RosterEntry, out);
    out[8] = p.nodeId;
    out[9] = p.propType;
    out[10] = p.propIndex;
    out[11] = (uint8_t)p.state;
    put16(out + 12, p.lastHeardMs);
    put16(out + 14, p.effect);
    put16(out + 16, p.cueId);
    put16(out + 18, p.syncDelayUs);
    out[20] = (uint8_t)p.rssi;
    out[21] = p.fps;
    put16(out + 22, p.missedFrames);
    put32(out + 24, p.ipv4);
    out[28] = p.entryIndex;
    out[29] = p.entryCount;
}

void decode(const uint8_t *in, RosterEntry &p) {
    p.nodeId = in[8];
    p.propType = in[9];
    p.propIndex = in[10];
    p.state = (NodeState)in[11];
    p.lastHeardMs = get16(in + 12);
    p.effect = get16(in + 14);
    p.cueId = get16(in + 16);
    p.syncDelayUs = get16(in + 18);
    p.rssi = (int8_t)in[20];
    p.fps = in[21];
    p.missedFrames = get16(in + 22);
    p.ipv4 = get32(in + 24);
    p.entryIndex = in[28];
    p.entryCount = in[29];
}

void encode(Header h, const Override &p, uint8_t *out) {
    begin(h, PacketType::Override, out);
    put16(out + 8, p.targetGroupMask);
    out[10] = (uint8_t)p.kind;
    out[11] = (uint8_t)p.transition;
    put16(out + 12, p.delayMs);
    put16(out + 14, p.effect);
    put16(out + 16, p.transitionMs);
    memcpy(out + 18, p.params, 8);
}

void decode(const uint8_t *in, Override &p) {
    p.targetGroupMask = get16(in + 8);
    p.kind = (CueKind)in[10];
    p.transition = (Transition)in[11];
    p.delayMs = get16(in + 12);
    p.effect = get16(in + 14);
    p.transitionMs = get16(in + 16);
    memcpy(p.params, in + 18, 8);
}

void encode(Header h, const FrameStats &p, uint8_t *out) {
    begin(h, PacketType::FrameStats, out);
    out[8] = p.nodeId;
    out[9] = p.fps;
    put16(out + 10, p.avgFrameUs);
    put16(out + 12, p.worstFrameUs);
    put16(out + 14, p.avgStepUs);
    put16(out + 16, p.avgShowUs);
    out[18] = (uint8_t)p.tempC;
    out[19] = p.nodeCount;
    put32(out + 20, p.frameCount);
    put32(out + 24, p.missedFrames);
    put32(out + 28, p.uptimeS);
}

void decode(const uint8_t *in, FrameStats &p) {
    p.nodeId = in[8];
    p.fps = in[9];
    p.avgFrameUs = get16(in + 10);
    p.worstFrameUs = get16(in + 12);
    p.avgStepUs = get16(in + 14);
    p.avgShowUs = get16(in + 16);
    p.tempC = (int8_t)in[18];
    p.nodeCount = in[19];
    p.frameCount = get32(in + 20);
    p.missedFrames = get32(in + 24);
    p.uptimeS = get32(in + 28);
}

void encode(Header h, const CueStats &p, uint8_t *out) {
    begin(h, PacketType::CueStats, out);
    out[8] = p.nodeId;
    out[9] = p.nodeCount;
    put16(out + 10, p.lastLateCueId);
    put32(out + 12, p.cuesSent);
    put32(out + 16, p.cuesLate);
    put32(out + 20, p.cuesMissed);
    put32(out + 24, p.cuesUnknown);
    put32(out + 28, (uint32_t)p.lastLateUs);
}

void decode(const uint8_t *in, CueStats &p) {
    p.nodeId = in[8];
    p.nodeCount = in[9];
    p.lastLateCueId = get16(in + 10);
    p.cuesSent = get32(in + 12);
    p.cuesLate = get32(in + 16);
    p.cuesMissed = get32(in + 20);
    p.cuesUnknown = get32(in + 24);
    p.lastLateUs = (int32_t)get32(in + 28);
}

}  // namespace picocomm
