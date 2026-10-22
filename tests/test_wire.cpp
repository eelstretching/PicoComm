// Wire format tests: every packet type round-trips, the header rejects foreign packets, and the
// encoder reproduces the golden packets in tests/vectors/packets.txt byte for byte. The Python
// tool's tests (tools/picocomm/tests/test_wire.py) check the same file, so the two sides can't
// drift apart.
//
// Run with --write to regenerate packets.txt after a deliberate wire format change.

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "check.h"
#include "picocomm/wire.h"

using namespace picocomm;

namespace {

Header hdr(uint16_t seq, uint8_t src, uint8_t flags = 0) {
    Header h;
    h.seq = seq;
    h.src = src;
    h.flags = flags;
    return h;
}

const uint8_t kBoard[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// The canonical packets. Keep in step with CANONICAL in tools/picocomm/tests/test_wire.py.
std::map<std::string, std::string> canonical() {
    std::map<std::string, std::string> out;
    uint8_t pkt[kPacketSize];
    auto hex = [&]() {
        char s[2 * kPacketSize + 1];
        for (size_t i = 0; i < kPacketSize; i++) {
            snprintf(s + 2 * i, 3, "%02x", pkt[i]);
        }
        return std::string(s);
    };

    Beacon b;
    b.generation = 0xdeadbeef;
    b.showTimeUs = 0x0123456789abcdefull;
    b.unixTime = 1798761600;
    b.sceneId = 0x0a0b;
    b.followerCount = 7;
    b.priority = 6400;
    encode(hdr(0x0102, kLeaderId), b, pkt);
    out["beacon"] = hex();

    Join j;
    memcpy(j.boardId, kBoard, 8);
    j.propType = 3;
    j.propIndex = 2;
    j.fwVersion = 0x0105;
    j.catalogVersion = 1;
    j.capabilities = 0x8001;
    encode(hdr(1, kUnassignedId), j, pkt);
    out["join"] = hex();

    Welcome w;
    w.generation = 0xdeadbeef;
    w.nodeId = 5;
    w.groupMask = 0x0008;
    memcpy(w.boardId, kBoard, 8);
    encode(hdr(2, kLeaderId), w, pkt);
    out["welcome"] = hex();

    SyncReq sq;
    sq.t1 = 0x0000001122334455ull;
    encode(hdr(0xffff, 5), sq, pkt);
    out["sync_req"] = hex();

    SyncResp sr;
    sr.t1 = 0x0000001122334455ull;
    sr.t2 = 0x00000000aabbccddull;
    sr.t3 = 0x00000000aabbcceeull;
    encode(hdr(3, kLeaderId), sr, pkt);
    out["sync_resp"] = hex();

    Cue c;
    c.cueId = 0x0102;
    c.kind = CueKind::Effect;
    c.transition = Transition::Crossfade;
    c.atUs = 10000000000ull;
    c.effect = 0x0002;
    c.transitionMs = 1500;
    for (int i = 0; i < 8; i++) {
        c.params[i] = (uint8_t)(0xa0 + i);
    }
    encode(hdr(4, kLeaderId, kFlagAckRequested), c, pkt);
    out["cue"] = hex();

    Ack a;
    a.ackedType = PacketType::Cue;
    a.status = AckStatus::Late;
    a.ackedSeq = 4;
    a.cueId = 0x0102;
    a.lateByUs = -12345;
    encode(hdr(5, 5), a, pkt);
    out["ack"] = hex();

    Status s;
    s.generation = 0xdeadbeef;
    s.cueId = 0x0102;
    s.effect = 0x0200;
    s.syncDelayUs = 2500;
    s.syncAgeMs = 1200;
    s.fps = 40;
    s.rssi = -67;
    s.missedFrames = 3;
    s.freeRamKB = 212;
    s.uptimeS = 86400;
    encode(hdr(6, 5), s, pkt);
    out["status"] = hex();

    Query q;
    q.what = kQueryRoster;
    encode(hdr(7, kObserverId), q, pkt);
    out["query"] = hex();

    RosterEntry r;
    r.nodeId = 5;
    r.propType = 3;
    r.propIndex = 2;
    r.state = NodeState::Synced;
    r.lastHeardMs = 150;
    r.effect = 0x0200;
    r.cueId = 0x0102;
    r.syncDelayUs = 2500;
    r.rssi = -67;
    r.fps = 40;
    r.missedFrames = 3;
    r.ipv4 = 0x0700000a;  // 10.0.0.7
    r.entryIndex = 1;
    r.entryCount = 3;
    encode(hdr(8, kLeaderId), r, pkt);
    out["roster_entry"] = hex();

    Override o;
    o.targetGroupMask = 0x0008;
    o.kind = CueKind::Effect;
    o.transition = Transition::FadeThroughBlack;
    o.delayMs = 500;
    o.effect = 0x0200;
    o.transitionMs = 800;
    for (int i = 0; i < 8; i++) {
        o.params[i] = (uint8_t)(0xb0 + i);
    }
    encode(hdr(9, kObserverId), o, pkt);
    out["override"] = hex();

    return out;
}

std::map<std::string, std::string> readVectors(const char *path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ss(line);
        std::string name, hex;
        ss >> name >> hex;
        out[name] = hex;
    }
    return out;
}

void testGolden() {
    auto want = readVectors(PICOCOMM_VECTORS);
    auto got = canonical();
    CHECK_EQ(want.size(), got.size());
    for (auto &kv : got) {
        if (want[kv.first] != kv.second) {
            printf("  %s:\n    want %s\n    got  %s\n", kv.first.c_str(), want[kv.first].c_str(),
                   kv.second.c_str());
        }
        CHECK(want[kv.first] == kv.second);
    }
}

void testFixedLayout() {
    // Spot-check the layout by hand, independently of the golden file.
    uint8_t pkt[kPacketSize];
    Cue c;
    c.cueId = 0x0102;
    c.atUs = 0x1122334455667788ull;
    c.effect = 0xbeef;
    encode(hdr(0x3456, kLeaderId, kFlagAckRequested), c, pkt);
    CHECK_EQ(pkt[0], 'P');
    CHECK_EQ(pkt[1], 'C');
    CHECK_EQ(pkt[2], 1);
    CHECK_EQ(pkt[3], 6);
    CHECK_EQ(pkt[4], 0x56);
    CHECK_EQ(pkt[5], 0x34);
    CHECK_EQ(pkt[6], 0);
    CHECK_EQ(pkt[7], 1);
    CHECK_EQ(pkt[8], 0x02);
    CHECK_EQ(pkt[9], 0x01);
    CHECK_EQ(pkt[12], 0x88);
    CHECK_EQ(pkt[19], 0x11);
    CHECK_EQ(pkt[20], 0xef);
    CHECK_EQ(pkt[21], 0xbe);
}

void testRoundTrips() {
    uint8_t pkt[kPacketSize];
    Header h;

    Beacon b, b2;
    b.generation = 0x12345678;
    b.showTimeUs = 0xfedcba9876543210ull;
    b.unixTime = 0xffffffff;
    b.sceneId = 65535;
    b.followerCount = 255;
    b.priority = kAlwaysLead;
    encode(hdr(1, 0), b, pkt);
    CHECK(decodeHeader(pkt, kPacketSize, h));
    CHECK(h.type == PacketType::Beacon);
    decode(pkt, b2);
    CHECK(b2.generation == b.generation && b2.showTimeUs == b.showTimeUs &&
          b2.unixTime == b.unixTime && b2.sceneId == b.sceneId &&
          b2.followerCount == b.followerCount && b2.priority == b.priority);

    Ack a, a2;
    a.ackedType = PacketType::Override;
    a.status = AckStatus::UnknownEffect;
    a.ackedSeq = 0xabcd;
    a.cueId = 0x1234;
    a.lateByUs = INT32_MIN;
    encode(hdr(2, 9), a, pkt);
    CHECK(decodeHeader(pkt, kPacketSize, h));
    CHECK(h.type == PacketType::Ack && h.seq == 2 && h.src == 9);
    decode(pkt, a2);
    CHECK(a2.ackedType == a.ackedType && a2.status == a.status && a2.ackedSeq == a.ackedSeq &&
          a2.cueId == a.cueId && a2.lateByUs == a.lateByUs);

    Status s, s2;
    s.rssi = -128;
    s.uptimeS = 0xfffffffe;
    s.freeRamKB = 520;
    encode(hdr(3, 1), s, pkt);
    decode(pkt, s2);
    CHECK(s2.rssi == -128 && s2.uptimeS == 0xfffffffe && s2.freeRamKB == 520);

    RosterEntry r, r2;
    r.state = NodeState::Lost;
    r.ipv4 = 0x6401a8c0;  // 192.168.1.100
    r.rssi = -90;
    encode(hdr(4, 0), r, pkt);
    decode(pkt, r2);
    CHECK(r2.state == NodeState::Lost && r2.ipv4 == r.ipv4 && r2.rssi == -90);
    CHECK_EQ(pkt[24], 192);
    CHECK_EQ(pkt[27], 100);

    Override o, o2;
    o.targetGroupMask = 0xffff;
    o.kind = CueKind::Blackout;
    o.params[7] = 0x99;
    encode(hdr(5, kObserverId), o, pkt);
    decode(pkt, o2);
    CHECK(o2.targetGroupMask == 0xffff && o2.kind == CueKind::Blackout && o2.params[7] == 0x99);

    // Reserved bytes are zero.
    Query q;
    encode(hdr(6, kObserverId), q, pkt);
    for (size_t i = 9; i < kPacketSize; i++) {
        CHECK_EQ(pkt[i], 0);
    }
}

void testRejects() {
    uint8_t pkt[kPacketSize];
    Header h;
    Query q;
    encode(hdr(1, kObserverId), q, pkt);
    CHECK(decodeHeader(pkt, kPacketSize, h));
    CHECK(!decodeHeader(pkt, kPacketSize - 1, h));
    CHECK(!decodeHeader(pkt, kPacketSize + 1, h));

    uint8_t bad[kPacketSize];
    memcpy(bad, pkt, kPacketSize);
    bad[0] = 'X';
    CHECK(!decodeHeader(bad, kPacketSize, h));
    memcpy(bad, pkt, kPacketSize);
    bad[2] = 2;  // future protocol version
    CHECK(!decodeHeader(bad, kPacketSize, h));
    memcpy(bad, pkt, kPacketSize);
    bad[3] = 0;
    CHECK(!decodeHeader(bad, kPacketSize, h));
    bad[3] = 12;
    CHECK(!decodeHeader(bad, kPacketSize, h));
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--write") == 0) {
        std::ofstream out(PICOCOMM_VECTORS);
        out << "# Golden PicoComm v1 packets: name, then the 32 bytes in hex.\n"
            << "# Generated by tests/test_wire.cpp --write; checked by the C++ and Python tests.\n";
        for (auto &kv : canonical()) {
            out << kv.first << " " << kv.second << "\n";
        }
        printf("wrote %s\n", PICOCOMM_VECTORS);
        return 0;
    }
    testGolden();
    testFixedLayout();
    testRoundTrips();
    testRejects();
    return check::finish("test_wire");
}
