// PicoComm wire format, version 1 (docs/protocol-design.md, section 7).
//
// Every packet is exactly 32 bytes, little-endian, in one UDP datagram on port 7420. Packets are
// encoded and decoded field by field; no struct is ever sent raw, so compiler padding can't leak
// onto the wire. tools/picocomm/picocomm/wire.py is the Python mirror of this file, and
// tests/vectors/packets.txt holds golden packets that both sides are tested against.
//
// Plain C++17 with no pico-sdk dependency, so it builds and tests on a laptop.

#pragma once

#include <cstddef>
#include <cstdint>

namespace picocomm {

constexpr uint16_t kPort = 7420;
constexpr size_t kPacketSize = 32;
constexpr uint8_t kMagic0 = 'P';
constexpr uint8_t kMagic1 = 'C';
constexpr uint8_t kProtocolVersion = 1;

// Node IDs carried in the header's src field.
constexpr uint8_t kLeaderId = 0;
constexpr uint8_t kUnassignedId = 254;
constexpr uint8_t kObserverId = 255;

// Header flags.
constexpr uint8_t kFlagAckRequested = 0x01;

enum class PacketType : uint8_t {
    Beacon = 1,
    Join = 2,
    Welcome = 3,
    SyncReq = 4,
    SyncResp = 5,
    Cue = 6,
    Ack = 7,
    Status = 8,
    Query = 9,
    RosterEntry = 10,
    Override = 11,
};

enum class CueKind : uint8_t {
    Effect = 1,
    Brightness = 2,  // target in params[0], ramp over transitionMs
    Blackout = 3,
    ResumeLocal = 4,  // go back to the prop's own local playlist
};

enum class Transition : uint8_t {
    Cut = 0,
    FadeThroughBlack = 1,
    Crossfade = 2,
};

enum class AckStatus : uint8_t {
    Ok = 0,
    UnknownEffect = 1,
    Late = 2,
    StaleGeneration = 3,
};

// Roster state of a follower, as the leader sees it.
enum class NodeState : uint8_t {
    Joining = 1,
    Synced = 2,
    Stale = 3,
    Lost = 4,
};

// QUERY "what" values.
constexpr uint8_t kQueryRoster = 1;

struct Header {
    PacketType type = PacketType::Beacon;
    uint16_t seq = 0;
    uint8_t src = kUnassignedId;
    uint8_t flags = 0;
};

struct Beacon {
    uint32_t generation = 0;
    uint64_t showTimeUs = 0;  // informational only: broadcast delay is unknown
    uint32_t unixTime = 0;    // 0 if the leader doesn't know the wall-clock time
    uint16_t sceneId = 0;
    uint8_t followerCount = 0;
};

struct Join {
    uint8_t boardId[8] = {};
    uint8_t propType = 0;
    uint8_t propIndex = 0;
    uint16_t fwVersion = 0;
    uint16_t catalogVersion = 0;
    uint16_t capabilities = 0;
};

struct Welcome {
    uint32_t generation = 0;
    uint8_t nodeId = kUnassignedId;
    uint16_t groupMask = 0;
    uint8_t boardId[8] = {};  // echo of the JOIN's board ID
};

struct SyncReq {
    uint64_t t1 = 0;
};

struct SyncResp {
    uint64_t t1 = 0;  // echo of the request's t1 (requester's clock)
    uint64_t t2 = 0;  // leader clock when the request arrived
    uint64_t t3 = 0;  // leader clock just before the response was sent
};

struct Cue {
    uint16_t cueId = 0;
    CueKind kind = CueKind::Effect;
    Transition transition = Transition::Cut;
    uint64_t atUs = 0;  // show time
    uint16_t effect = 0;
    uint16_t transitionMs = 0;
    uint8_t params[8] = {};
};

struct Ack {
    PacketType ackedType = PacketType::Cue;
    AckStatus status = AckStatus::Ok;
    uint16_t ackedSeq = 0;
    uint16_t cueId = 0;
    int32_t lateByUs = 0;
};

struct Status {
    uint32_t generation = 0;
    uint16_t cueId = 0;
    uint16_t effect = 0;
    uint16_t syncDelayUs = 0;
    uint16_t syncAgeMs = 0;
    uint8_t fps = 0;
    int8_t rssi = 0;
    uint16_t missedFrames = 0;
    uint16_t freeRamKB = 0;
    uint32_t uptimeS = 0;
};

struct Query {
    uint8_t what = kQueryRoster;
};

struct RosterEntry {
    uint8_t nodeId = 0;
    uint8_t propType = 0;
    uint8_t propIndex = 0;
    NodeState state = NodeState::Joining;
    uint16_t lastHeardMs = 0;
    uint16_t effect = 0;
    uint16_t cueId = 0;
    uint16_t syncDelayUs = 0;
    int8_t rssi = 0;
    uint8_t fps = 0;
    uint16_t missedFrames = 0;
    // IPv4 address with the first octet in the lowest byte, which is how lwIP's ip4_addr_t.addr
    // already holds it on a little-endian Pico (10.0.0.7 is bytes 0a 00 00 07 on the wire).
    uint32_t ipv4 = 0;
    uint8_t entryIndex = 0;
    uint8_t entryCount = 0;
};

struct Override {
    uint16_t targetGroupMask = 0;
    CueKind kind = CueKind::Effect;
    Transition transition = Transition::Cut;
    uint16_t delayMs = 0;
    uint16_t effect = 0;
    uint16_t transitionMs = 0;
    uint8_t params[8] = {};
};

// Encoders: write the header (type filled in from the payload) and payload into exactly
// kPacketSize bytes. Reserved bytes are zeroed.
void encode(Header h, const Beacon &p, uint8_t *out);
void encode(Header h, const Join &p, uint8_t *out);
void encode(Header h, const Welcome &p, uint8_t *out);
void encode(Header h, const SyncReq &p, uint8_t *out);
void encode(Header h, const SyncResp &p, uint8_t *out);
void encode(Header h, const Cue &p, uint8_t *out);
void encode(Header h, const Ack &p, uint8_t *out);
void encode(Header h, const Status &p, uint8_t *out);
void encode(Header h, const Query &p, uint8_t *out);
void encode(Header h, const RosterEntry &p, uint8_t *out);
void encode(Header h, const Override &p, uint8_t *out);

// Checks length, magic and version and reads the header. Returns false for anything that isn't a
// PicoComm v1 packet (or has an unknown type), which callers should silently drop.
bool decodeHeader(const uint8_t *in, size_t len, Header &h);

// Payload decoders. Call only after decodeHeader() said the packet has the matching type.
void decode(const uint8_t *in, Beacon &p);
void decode(const uint8_t *in, Join &p);
void decode(const uint8_t *in, Welcome &p);
void decode(const uint8_t *in, SyncReq &p);
void decode(const uint8_t *in, SyncResp &p);
void decode(const uint8_t *in, Cue &p);
void decode(const uint8_t *in, Ack &p);
void decode(const uint8_t *in, Status &p);
void decode(const uint8_t *in, Query &p);
void decode(const uint8_t *in, RosterEntry &p);
void decode(const uint8_t *in, Override &p);

// Little-endian helpers, exposed for tests.
inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
inline void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
inline void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
inline uint64_t get64(const uint8_t *p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

}  // namespace picocomm
