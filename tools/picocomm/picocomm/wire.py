"""PicoComm wire format, version 1: the Python mirror of include/picocomm/wire.h.

Every packet is exactly 32 bytes, little-endian, on UDP port 7420: an 8-byte header and a 24-byte
payload whose layout depends on the type (docs/protocol-design.md, section 7). The struct formats
below spell out every byte, reserved ones as padding, so nothing depends on alignment.
tests/vectors/packets.txt holds golden packets that this module and the C++ encoder are both
tested against.
"""

from __future__ import annotations

import socket
import struct
from dataclasses import dataclass, fields
from enum import IntEnum

PORT = 7420
PACKET_SIZE = 32
MAGIC = b"PC"
VERSION = 1

LEADER_ID = 0
UNASSIGNED_ID = 254
OBSERVER_ID = 255

FLAG_ACK_REQUESTED = 0x01

# BEACON priority (leader election): a fixed leader outranks every candidate.
NEVER_LEAD = 0
ALWAYS_LEAD = 0xFFFFFFFF
QUERY_ROSTER = 1

_HEADER = struct.Struct("<2sBBHBB")


class PacketType(IntEnum):
    BEACON = 1
    JOIN = 2
    WELCOME = 3
    SYNC_REQ = 4
    SYNC_RESP = 5
    CUE = 6
    ACK = 7
    STATUS = 8
    QUERY = 9
    ROSTER_ENTRY = 10
    OVERRIDE = 11


class CueKind(IntEnum):
    EFFECT = 1
    BRIGHTNESS = 2
    BLACKOUT = 3
    RESUME_LOCAL = 4


class Transition(IntEnum):
    CUT = 0
    FADE_THROUGH_BLACK = 1
    CROSSFADE = 2


class AckStatus(IntEnum):
    OK = 0
    UNKNOWN_EFFECT = 1
    LATE = 2
    STALE_GENERATION = 3


class NodeState(IntEnum):
    JOINING = 1
    SYNCED = 2
    STALE = 3
    LOST = 4


@dataclass
class Header:
    type: int = PacketType.BEACON
    seq: int = 0
    src: int = UNASSIGNED_ID
    flags: int = 0


@dataclass
class Beacon:
    generation: int = 0
    show_time_us: int = 0
    unix_time: int = 0
    scene_id: int = 0
    follower_count: int = 0
    priority: int = 0


@dataclass
class Join:
    board_id: bytes = bytes(8)
    prop_type: int = 0
    prop_index: int = 0
    fw_version: int = 0
    catalog_version: int = 0
    capabilities: int = 0


@dataclass
class Welcome:
    generation: int = 0
    node_id: int = UNASSIGNED_ID
    group_mask: int = 0
    board_id: bytes = bytes(8)


@dataclass
class SyncReq:
    t1: int = 0


@dataclass
class SyncResp:
    t1: int = 0
    t2: int = 0
    t3: int = 0


@dataclass
class Cue:
    cue_id: int = 0
    kind: int = CueKind.EFFECT
    transition: int = Transition.CUT
    at_us: int = 0
    effect: int = 0
    transition_ms: int = 0
    params: bytes = bytes(8)


@dataclass
class Ack:
    acked_type: int = PacketType.CUE
    status: int = AckStatus.OK
    acked_seq: int = 0
    cue_id: int = 0
    late_by_us: int = 0


@dataclass
class Status:
    generation: int = 0
    cue_id: int = 0
    effect: int = 0
    sync_delay_us: int = 0
    sync_age_ms: int = 0
    fps: int = 0
    rssi: int = 0
    missed_frames: int = 0
    free_ram_kb: int = 0
    uptime_s: int = 0


@dataclass
class Query:
    what: int = QUERY_ROSTER


@dataclass
class RosterEntry:
    node_id: int = 0
    prop_type: int = 0
    prop_index: int = 0
    state: int = NodeState.JOINING
    last_heard_ms: int = 0
    effect: int = 0
    cue_id: int = 0
    sync_delay_us: int = 0
    rssi: int = 0
    fps: int = 0
    missed_frames: int = 0
    ipv4: str = "0.0.0.0"  # first octet first on the wire
    entry_index: int = 0
    entry_count: int = 0


@dataclass
class Override:
    target_group_mask: int = 0
    kind: int = CueKind.EFFECT
    transition: int = Transition.CUT
    delay_ms: int = 0
    effect: int = 0
    transition_ms: int = 0
    params: bytes = bytes(8)


# Payload layouts, bytes 8 to 31, in dataclass field order. Each is exactly 24 bytes.
_PAYLOADS: dict[int, tuple[type, struct.Struct]] = {
    PacketType.BEACON: (Beacon, struct.Struct("<IQIHBIx")),
    PacketType.JOIN: (Join, struct.Struct("<8sBBHHH8x")),
    PacketType.WELCOME: (Welcome, struct.Struct("<IBxH8s8x")),
    PacketType.SYNC_REQ: (SyncReq, struct.Struct("<Q16x")),
    PacketType.SYNC_RESP: (SyncResp, struct.Struct("<QQQ")),
    PacketType.CUE: (Cue, struct.Struct("<HBBQHH8s")),
    PacketType.ACK: (Ack, struct.Struct("<BBHH2xi12x")),
    PacketType.STATUS: (Status, struct.Struct("<IHHHHBbHHI2x")),
    PacketType.QUERY: (Query, struct.Struct("<B23x")),
    PacketType.ROSTER_ENTRY: (RosterEntry, struct.Struct("<BBBBHHHHbBH4sBB2x")),
    PacketType.OVERRIDE: (Override, struct.Struct("<HBBHHH8s6x")),
}
_TYPE_OF = {cls: t for t, (cls, _) in _PAYLOADS.items()}

for _t, (_cls, _fmt) in _PAYLOADS.items():
    assert _fmt.size == PACKET_SIZE - _HEADER.size, f"{_cls.__name__} payload is {_fmt.size} bytes"


def encode(payload, seq: int = 0, src: int = UNASSIGNED_ID, flags: int = 0) -> bytes:
    """Encodes a payload dataclass into one 32-byte packet."""
    ptype = _TYPE_OF[type(payload)]
    _, fmt = _PAYLOADS[ptype]
    values = []
    for f in fields(payload):
        v = getattr(payload, f.name)
        if f.name == "ipv4":
            v = socket.inet_aton(v)
        values.append(v)
    return _HEADER.pack(MAGIC, VERSION, ptype, seq & 0xFFFF, src, flags) + fmt.pack(*values)


def decode(data: bytes):
    """Returns (Header, payload) for a PicoComm v1 packet, or None for anything else."""
    if len(data) != PACKET_SIZE:
        return None
    magic, version, ptype, seq, src, flags = _HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or ptype not in _PAYLOADS:
        return None
    cls, fmt = _PAYLOADS[ptype]
    values = list(fmt.unpack_from(data, _HEADER.size))
    names = [f.name for f in fields(cls)]
    if "ipv4" in names:
        i = names.index("ipv4")
        values[i] = socket.inet_ntoa(values[i])
    return Header(PacketType(ptype), seq, src, flags), cls(*values)
