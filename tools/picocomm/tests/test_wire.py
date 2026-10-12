"""The Python encoder must produce the same bytes as the C++ one: both are checked against the
golden packets in tests/vectors/packets.txt (written by tests/test_wire.cpp --write)."""

import unittest
from pathlib import Path

from picocomm import wire

VECTORS = Path(__file__).resolve().parents[3] / "tests" / "vectors" / "packets.txt"

BOARD = bytes(range(1, 9))

# Keep in step with canonical() in tests/test_wire.cpp: (payload, seq, src, flags).
CANONICAL = {
    "beacon": (wire.Beacon(0xDEADBEEF, 0x0123456789ABCDEF, 1798761600, 0x0A0B, 7),
               0x0102, wire.LEADER_ID, 0),
    "join": (wire.Join(BOARD, 3, 2, 0x0105, 1, 0x8001), 1, wire.UNASSIGNED_ID, 0),
    "welcome": (wire.Welcome(0xDEADBEEF, 5, 0x0008, BOARD), 2, wire.LEADER_ID, 0),
    "sync_req": (wire.SyncReq(0x0000001122334455), 0xFFFF, 5, 0),
    "sync_resp": (wire.SyncResp(0x0000001122334455, 0xAABBCCDD, 0xAABBCCEE), 3,
                  wire.LEADER_ID, 0),
    "cue": (wire.Cue(0x0102, wire.CueKind.EFFECT, wire.Transition.CROSSFADE, 10_000_000_000,
                     0x0002, 1500, bytes(range(0xA0, 0xA8))),
            4, wire.LEADER_ID, wire.FLAG_ACK_REQUESTED),
    "ack": (wire.Ack(wire.PacketType.CUE, wire.AckStatus.LATE, 4, 0x0102, -12345), 5, 5, 0),
    "status": (wire.Status(0xDEADBEEF, 0x0102, 0x0200, 2500, 1200, 40, -67, 3, 212, 86400),
               6, 5, 0),
    "query": (wire.Query(wire.QUERY_ROSTER), 7, wire.OBSERVER_ID, 0),
    "roster_entry": (wire.RosterEntry(5, 3, 2, wire.NodeState.SYNCED, 150, 0x0200, 0x0102,
                                      2500, -67, 40, 3, "10.0.0.7", 1, 3),
                     8, wire.LEADER_ID, 0),
    "override": (wire.Override(0x0008, wire.CueKind.EFFECT, wire.Transition.FADE_THROUGH_BLACK,
                               500, 0x0200, 800, bytes(range(0xB0, 0xB8))),
                 9, wire.OBSERVER_ID, 0),
}


def read_vectors():
    out = {}
    for line in VECTORS.read_text().splitlines():
        if line and not line.startswith("#"):
            name, hexstr = line.split()
            out[name] = bytes.fromhex(hexstr)
    return out


class WireTest(unittest.TestCase):
    def test_golden_encode(self):
        vectors = read_vectors()
        self.assertEqual(set(vectors), set(CANONICAL))
        for name, (payload, seq, src, flags) in CANONICAL.items():
            with self.subTest(name):
                self.assertEqual(wire.encode(payload, seq, src, flags).hex(), vectors[name].hex())

    def test_golden_decode(self):
        for name, data in read_vectors().items():
            payload, seq, src, flags = CANONICAL[name]
            with self.subTest(name):
                hdr, decoded = wire.decode(data)
                self.assertEqual(decoded, payload)
                self.assertEqual((hdr.seq, hdr.src, hdr.flags), (seq, src, flags))

    def test_every_packet_is_32_bytes(self):
        for name, (payload, *_rest) in CANONICAL.items():
            with self.subTest(name):
                self.assertEqual(len(wire.encode(payload)), wire.PACKET_SIZE)

    def test_rejects(self):
        good = wire.encode(wire.Query())
        self.assertIsNotNone(wire.decode(good))
        self.assertIsNone(wire.decode(good[:-1]))
        self.assertIsNone(wire.decode(good + b"\0"))
        self.assertIsNone(wire.decode(b"XC" + good[2:]))
        self.assertIsNone(wire.decode(good[:2] + b"\x02" + good[3:]))  # future version
        self.assertIsNone(wire.decode(good[:3] + b"\x0c" + good[4:]))  # unknown type


if __name__ == "__main__":
    unittest.main()
