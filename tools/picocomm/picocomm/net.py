"""UDP plumbing for the laptop: an observer client that talks to the leader, and a small socket
helper shared with the fake nodes."""

from __future__ import annotations

import random
import select
import socket
import time

from . import wire
from .clock import SyncSample, make_sample


def now_us() -> int:
    """The laptop's local clock (monotonic, never jumps)."""
    return time.monotonic_ns() // 1000


def udp_socket(port: int = 0, reuse: bool = True) -> socket.socket:
    """A UDP socket bound to port on all interfaces, allowed to send broadcast. With reuse,
    several processes on one laptop (watch, a fake leader, fake followers) can share port 7420 to
    hear beacons."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    if reuse:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", port))
    return s


class Lossy:
    """Drops a fraction of packets in each direction, for testing retries on purpose."""

    def __init__(self, drop: float = 0.0, seed: int | None = None):
        self.drop = drop
        self.rng = random.Random(seed)

    def lose(self) -> bool:
        return self.drop > 0 and self.rng.random() < self.drop


def receive(socks, timeout: float):
    """Waits up to timeout for packets on any of socks. Returns a list of
    (sock, header, payload, (ip, port), rx_us); non-PicoComm datagrams are skipped."""
    out = []
    ready, _, _ = select.select(socks, [], [], max(0.0, timeout))
    for s in ready:
        # Drain everything already queued on this socket without blocking.
        s.setblocking(False)
        try:
            while True:
                try:
                    data, addr = s.recvfrom(64)
                except (BlockingIOError, InterruptedError):
                    break
                rx = now_us()
                decoded = wire.decode(data)
                if decoded is not None:
                    out.append((s, decoded[0], decoded[1], addr, rx))
        finally:
            s.setblocking(True)
    return out


class Observer:
    """The laptop as a PicoComm observer (node 255): finds the leader, reads the roster, syncs
    its clock, and sends overrides. It only ever talks to the leader."""

    def __init__(self, leader_ip: str | None = None, port: int = wire.PORT,
                 listen_beacons: bool | None = None):
        self.port = port
        self.leader = (leader_ip, port) if leader_ip else None
        self.beacon: wire.Beacon | None = None
        self.beacon_rx_us = 0
        self.sock = udp_socket(0, reuse=False)
        # Listening for beacons means sharing the protocol port, which a fake leader on the same
        # laptop also uses; with the leader's address given there's no need.
        if listen_beacons is None:
            listen_beacons = leader_ip is None
        self.beacon_sock = udp_socket(port) if listen_beacons else None
        self.seq = 0

    def close(self):
        self.sock.close()
        if self.beacon_sock:
            self.beacon_sock.close()

    def _socks(self):
        return [self.sock] + ([self.beacon_sock] if self.beacon_sock else [])

    def send(self, payload, flags: int = 0, seq: int | None = None) -> int:
        """Sends to the leader. Returns the sequence number used; pass it back as seq to resend
        the same packet (the leader uses it to spot resends)."""
        if seq is None:
            seq = self.seq
            self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendto(wire.encode(payload, seq, wire.OBSERVER_ID, flags), self.leader)
        return seq

    def _note_beacon(self, hdr, payload, addr, rx):
        if hdr.type == wire.PacketType.BEACON and hdr.src == wire.LEADER_ID:
            self.beacon = payload
            self.beacon_rx_us = rx
            if self.leader is None:
                self.leader = (addr[0], addr[1])

    def poll(self, timeout: float):
        """Receives for up to timeout seconds, noting beacons. Returns the other packets."""
        others = []
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            for _, hdr, payload, addr, rx in receive(self._socks(), left):
                if hdr.type == wire.PacketType.BEACON:
                    self._note_beacon(hdr, payload, addr, rx)
                else:
                    others.append((hdr, payload, addr, rx))
            if left <= 0:
                return others

    def find_leader(self, timeout: float = 5.0) -> bool:
        deadline = time.monotonic() + timeout
        while self.leader is None and time.monotonic() < deadline:
            self.poll(0.2)
        return self.leader is not None

    def roster(self, timeout: float = 0.3) -> list[wire.RosterEntry]:
        """One QUERY; returns the rows that arrived, in entry order (they can arrive out of
        order)."""
        self.send(wire.Query(wire.QUERY_ROSTER))
        rows = {}
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for hdr, payload, _, _ in self.poll(min(0.05, deadline - time.monotonic())):
                if hdr.type == wire.PacketType.ROSTER_ENTRY:
                    rows[payload.entry_index] = payload
            if rows and len(rows) == next(iter(rows.values())).entry_count:
                break
        return [rows[i] for i in sorted(rows)]

    def sync_burst(self, count: int = 4, spacing: float = 0.02,
                   window: float = 0.26) -> SyncSample | None:
        """A burst of SYNC_REQs; returns the sample with the smallest round trip."""
        best = None
        start = time.monotonic()
        sent = 0
        while time.monotonic() - start < window:
            if sent < count and time.monotonic() - start >= sent * spacing:
                self.send(wire.SyncReq(now_us()))
                sent += 1
            for hdr, payload, _, rx in self.poll(0.005):
                if hdr.type != wire.PacketType.SYNC_RESP:
                    continue
                s = make_sample(payload.t1, payload.t2, payload.t3, rx)
                if s is not None and (best is None or s.delay_us < best.delay_us):
                    best = s
        return best

    def override(self, ov: wire.Override, tries: int = 3, timeout: float = 0.3):
        """Sends an OVERRIDE and waits for the leader's ACK (resending a couple of times).
        Returns the Ack, or None."""
        seq = None
        for _ in range(tries):
            seq = self.send(ov, seq=seq)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                for hdr, payload, _, _ in self.poll(0.05):
                    if (hdr.type == wire.PacketType.ACK and
                            payload.acked_type == wire.PacketType.OVERRIDE and
                            payload.acked_seq == seq):
                        return payload
        return None
