"""A fake leader and fake follower that speak the real protocol, for trying `picocomm watch`, `cue`
and `clock` on a laptop with no Picos (stage 0 of the test plan). They follow the same rules as
the C++ Leader and Follower but are simpler: the C++ simulation in tests/test_sim.cpp is where the
protocol logic itself is tested.

Run each in its own terminal; --drop makes them lose packets on purpose.
"""

from __future__ import annotations

import os
import random
import time
from dataclasses import dataclass, field

from . import catalog, wire
from .clock import DriftFit, make_sample
from .net import Lossy, now_us, receive, udp_socket


def _log(who: str, msg: str):
    print(f"{time.strftime('%H:%M:%S')} [{who}] {msg}", flush=True)


@dataclass
class _Member:
    node_id: int
    board_id: bytes
    prop_type: int
    prop_index: int
    addr: tuple
    state: int = wire.NodeState.JOINING
    last_heard_us: int = 0
    status: wire.Status | None = None
    pending: dict = field(default_factory=dict)  # cue_id -> [Cue, last_sent, give_up, catch_up]


class FakeLeader:
    BEACON_US = 1_000_000
    RESEND_US = 50_000
    GIVE_UP_US = 2_000_000
    STALE_US = 6_000_000
    LOST_US = 30_000_000

    def __init__(self, port=wire.PORT, beacon_addrs=(("255.255.255.255", wire.PORT),),
                 prop_type=catalog.PROP_TYPES["megatree"], scene_every=0.0, drop=0.0):
        self.sock = udp_socket(port)
        self.beacon_addrs = list(beacon_addrs)
        self.prop_type = prop_type
        self.scene_every_us = int(scene_every * 1e6)
        self.lossy = Lossy(drop)
        self.generation = random.getrandbits(32)
        self.boot_us = now_us()
        self.seq = 0
        self.next_cue_id = 1
        self.scene_id = 0
        self.members: dict[bytes, _Member] = {}
        self.scene_cue: dict[int, wire.Cue] = {}
        self.own_cue = wire.Cue()
        self.last_override = None
        self.next_beacon = 0
        self.next_scene = 0

    def show_us(self) -> int:
        return now_us() - self.boot_us

    def _send(self, payload, addr, flags=0):
        pkt = wire.encode(payload, self.seq, wire.LEADER_ID, flags)
        self.seq = (self.seq + 1) & 0xFFFF
        if not self.lossy.lose():
            self.sock.sendto(pkt, addr)

    def cue(self, mask: int, spec: wire.Cue, lead_ms: int = 300) -> int:
        c = wire.Cue(self.next_cue_id, spec.kind, spec.transition, self.show_us() + lead_ms * 1000,
                     spec.effect, spec.transition_ms, spec.params)
        self.next_cue_id = self.next_cue_id % 0xFFFF + 1
        self.scene_id += 1
        for g in range(16):
            if mask & (1 << g):
                self.scene_cue[g] = c
        if mask & (1 << self.prop_type):
            self.own_cue = c
        for m in self.members.values():
            if m.state != wire.NodeState.LOST and mask & (1 << m.prop_type):
                self._queue(m, c)
        return c.cue_id

    def _queue(self, m: _Member, c: wire.Cue):
        now = self.show_us()
        # [cue, last sent, give up at, catch-up (already due: lateness expected)]
        m.pending[c.cue_id] = [c, now, max(c.at_us, now) + self.GIVE_UP_US, c.at_us < now]
        self._send(c, m.addr, wire.FLAG_ACK_REQUESTED)

    def _member(self, hdr, addr):
        for m in self.members.values():
            if m.node_id == hdr.src and m.addr[0] == addr[0]:
                return m
        return None

    def handle(self, hdr, p, addr, rx):
        show_rx = rx - self.boot_us
        t = wire.PacketType
        if hdr.type == t.SYNC_REQ:
            self._send(wire.SyncResp(p.t1, show_rx, self.show_us()), addr)
            m = self._member(hdr, addr)
            if m:
                m.last_heard_us = show_rx
                if m.state != wire.NodeState.SYNCED:
                    m.state = wire.NodeState.SYNCED
        elif hdr.type == t.JOIN:
            m = self.members.get(p.board_id)
            if m is None:
                m = _Member(len(self.members) + 1, p.board_id, p.prop_type, p.prop_index, addr)
                self.members[p.board_id] = m
                _log("leader", f"join: node {m.node_id}, {catalog.prop_name(p.prop_type)} "
                     f"#{p.prop_index} from {addr[0]}")
            m.addr, m.state, m.last_heard_us, m.pending = addr, wire.NodeState.JOINING, show_rx, {}
            self._send(wire.Welcome(self.generation, m.node_id, 1 << p.prop_type, p.board_id), addr)
            if p.prop_type in self.scene_cue:
                self._queue(m, self.scene_cue[p.prop_type])
        elif hdr.type == t.ACK:
            m = self._member(hdr, addr)
            if m and p.acked_type == t.CUE:
                m.last_heard_us = show_rx
                entry = m.pending.pop(p.cue_id, None)
                if entry and p.status == wire.AckStatus.LATE and not entry[3]:
                    _log("leader", f"node {m.node_id}: cue {p.cue_id} {p.late_by_us} us late")
                elif entry and p.status == wire.AckStatus.UNKNOWN_EFFECT:
                    _log("leader", f"node {m.node_id}: cue {p.cue_id} unknown effect")
        elif hdr.type == t.STATUS:
            m = self._member(hdr, addr)
            if m:
                m.last_heard_us, m.status = show_rx, p
        elif hdr.type == t.QUERY:
            rows = [wire.RosterEntry(0, self.prop_type, 0, wire.NodeState.SYNCED,
                                     effect=self.own_cue.effect, cue_id=self.own_cue.cue_id,
                                     fps=40)]
            now = self.show_us()
            for m in self.members.values():
                s = m.status or wire.Status()
                rows.append(wire.RosterEntry(
                    m.node_id, m.prop_type, m.prop_index, m.state,
                    min(65535, (now - m.last_heard_us) // 1000), s.effect, s.cue_id,
                    s.sync_delay_us, s.rssi, s.fps, s.missed_frames, m.addr[0]))
            for i, r in enumerate(rows):
                r.entry_index, r.entry_count = i, len(rows)
                self._send(r, addr)
        elif hdr.type == t.OVERRIDE:
            key = (addr, hdr.seq)
            if self.last_override and self.last_override[0] == key:
                cue_id = self.last_override[1]
            else:
                spec = wire.Cue(0, p.kind, p.transition, 0, p.effect, p.transition_ms, p.params)
                cue_id = self.cue(p.target_group_mask, spec, p.delay_ms or 300)
                self.last_override = (key, cue_id)
                _log("leader", f"override: cue {cue_id} {catalog.effect_name(p.effect)} "
                     f"groups {p.target_group_mask:04x} in {p.delay_ms or 300} ms")
            self._send(wire.Ack(t.OVERRIDE, wire.AckStatus.OK, hdr.seq, cue_id), addr)

    def tick(self):
        now = self.show_us()
        if now >= self.next_beacon:
            self.next_beacon = now + self.BEACON_US
            active = sum(1 for m in self.members.values() if m.state != wire.NodeState.LOST)
            for addr in self.beacon_addrs:
                self._send(wire.Beacon(self.generation, now, int(time.time()), self.scene_id,
                                       active, wire.ALWAYS_LEAD), addr)
        if self.scene_every_us and now >= self.next_scene:
            self.next_scene = now + self.scene_every_us
            effects = ["starlight", "twinklefox", "leap", "chase"]
            name = effects[self.scene_id % len(effects)]
            cue_id = self.cue(catalog.ALL_GROUPS, wire.Cue(effect=catalog.EFFECTS[name]))
            _log("leader", f"scene: cue {cue_id} {name}")
        for m in self.members.values():
            quiet = now - m.last_heard_us
            if quiet > self.LOST_US and m.state != wire.NodeState.LOST:
                m.state = wire.NodeState.LOST
                _log("leader", f"node {m.node_id} lost")
            elif quiet > self.STALE_US and m.state not in (wire.NodeState.STALE,
                                                            wire.NodeState.LOST):
                m.state = wire.NodeState.STALE
                _log("leader", f"node {m.node_id} stale")
            for cue_id, entry in list(m.pending.items()):
                c, last, give_up, _ = entry
                if now >= give_up:
                    del m.pending[cue_id]
                    _log("leader", f"node {m.node_id}: gave up on cue {cue_id}")
                elif now - last >= self.RESEND_US:
                    entry[1] = now
                    self._send(c, m.addr, wire.FLAG_ACK_REQUESTED)

    def run(self, duration: float | None = None):
        to = ", ".join(f"{a[0]}:{a[1]}" for a in self.beacon_addrs)
        _log("leader", f"generation {self.generation:08x}, beacons to {to}")
        end = None if duration is None else time.monotonic() + duration
        while end is None or time.monotonic() < end:
            for _, hdr, p, addr, rx in receive([self.sock], 0.005):
                if not self.lossy.lose():
                    self.handle(hdr, p, addr, rx)
            self.tick()
        self.sock.close()


class FakeFollower:
    BURST_US = 2_000_000
    STATUS_US = 2_000_000
    ORPHAN_US = 10_000_000

    def __init__(self, port=wire.PORT, prop_type=catalog.PROP_TYPES["arch"], prop_index=1,
                 offset_ms=0.0, drift_ppm=0.0, unknown_effects=(), drop=0.0, quiet=False):
        self.sock = udp_socket(port)
        self.prop_type = prop_type
        self.prop_index = prop_index
        self.board_id = os.urandom(8)
        # A deliberately wrong local clock, so the sync has something to do.
        self.offset_us = int(offset_ms * 1000)
        self.drift = drift_ppm * 1e-6
        self.t0 = now_us()
        self.unknown = set(unknown_effects)
        self.lossy = Lossy(drop)
        self.quiet = quiet
        self.seq = 0
        self.node_id = wire.UNASSIGNED_ID
        self.leader = None
        self.generation = None
        self.joined = False
        self.fit = DriftFit()
        self.model = None
        self.recent = {}  # cue_id -> (status, late)
        self.delivered: list[wire.Cue] = []
        self.next_burst = 0
        self.next_status = 0
        self.last_leader_us = 0
        self.last_join = 0
        self.burst = None  # [start, sent, best]
        self.last_delay = 0

    def local_us(self) -> int:
        n = now_us()
        return int(self.offset_us + n + (n - self.t0) * self.drift)

    def show_us(self, local: int | None = None):
        if self.model is None:
            return None
        return self.model.to_show(self.local_us() if local is None else local)

    def _send(self, payload, flags=0):
        pkt = wire.encode(payload, self.seq, self.node_id, flags)
        self.seq = (self.seq + 1) & 0xFFFF
        if not self.lossy.lose():
            self.sock.sendto(pkt, self.leader)

    def _join(self):
        self.last_join = self.local_us()
        self._send(wire.Join(self.board_id, self.prop_type, self.prop_index, 1,
                             catalog.CATALOG_VERSION, 0))

    def handle(self, hdr, p, addr, rx_local):
        t = wire.PacketType
        if hdr.type == t.BEACON and hdr.src == wire.LEADER_ID:
            if self.generation is not None and p.generation != self.generation:
                _log("follower", "leader rebooted, rejoining")
                self.fit.clear()
                self.model = None
                self.recent.clear()
                self.joined = False
                self.leader = None
            if self.leader is None:
                self.leader, self.generation = addr, p.generation
                self._join()
        elif addr != self.leader:
            return
        elif hdr.type == t.WELCOME and p.board_id == self.board_id:
            if not self.joined:
                _log("follower", f"joined as node {p.node_id}")
                self.next_burst = rx_local
            self.node_id, self.joined, self.last_leader_us = p.node_id, True, rx_local
        elif hdr.type == t.SYNC_RESP and self.burst and p.t1 >= self.burst[0]:
            s = make_sample(p.t1, p.t2, p.t3, rx_local)
            if s and (self.burst[2] is None or s.delay_us < self.burst[2].delay_us):
                self.burst[2] = s
            self.last_leader_us = rx_local
        elif hdr.type == t.CUE and self.joined and self.model is not None:
            if p.cue_id not in self.recent:
                if p.kind == wire.CueKind.EFFECT and p.effect in self.unknown:
                    self.recent[p.cue_id] = (wire.AckStatus.UNKNOWN_EFFECT, 0)
                else:
                    late = self.show_us(rx_local) - p.at_us
                    status = wire.AckStatus.LATE if late > 0 else wire.AckStatus.OK
                    self.recent[p.cue_id] = (status, max(0, late))
                    self.delivered.append(p)
                    if not self.quiet:
                        _log("follower", f"cue {p.cue_id}: {catalog.effect_name(p.effect)} in "
                             f"{-late / 1000:.1f} ms")
            status, late = self.recent[p.cue_id]
            self._send(wire.Ack(t.CUE, status, hdr.seq, p.cue_id, min(late, 2**31 - 1)))

    def tick(self):
        now = self.local_us()
        if self.leader and not self.joined and now - self.last_join >= 1_000_000:
            self._join()
        if not self.joined:
            return
        if self.burst is None and now >= self.next_burst:
            self.burst = [now, 0, None]
            self.next_burst = now + self.BURST_US
        if self.burst is not None:
            start, sent, best = self.burst
            if sent < 4 and now >= start + sent * 20_000:
                self._send(wire.SyncReq(self.local_us()))
                self.burst[1] += 1
            if now >= start + 260_000:
                self.burst = None
                if best is not None:
                    self.fit.add(best)
                    first = self.model is None
                    self.model = self.fit.fit()
                    self.last_delay = best.delay_us
                    if first:
                        _log("follower", f"clock locked, round trip {best.delay_us} us")
        if self.model is not None and now >= self.next_status:
            self.next_status = now + self.STATUS_US
            last = self.delivered[-1] if self.delivered else wire.Cue()
            self._send(wire.Status(self.generation, last.cue_id, last.effect,
                                   min(65535, self.last_delay), 0, 40, -60, 0, 200,
                                   (now_us() - self.t0) // 1_000_000))
        if now - self.last_leader_us > self.ORPHAN_US:
            _log("follower", "orphaned: no sync response for 10 s")
            self.joined, self.leader, self.node_id = False, None, wire.UNASSIGNED_ID
            self.recent.clear()

    def run(self, duration: float | None = None):
        end = None if duration is None else time.monotonic() + duration
        while end is None or time.monotonic() < end:
            for _, hdr, p, addr, rx in receive([self.sock], 0.005):
                if not self.lossy.lose():
                    # rx is the laptop clock; convert to this fake's skewed local clock.
                    self.handle(hdr, p, addr, rx + (self.local_us() - now_us()))
            self.tick()
        self.sock.close()
