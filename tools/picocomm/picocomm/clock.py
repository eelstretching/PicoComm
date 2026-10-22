"""Clock sync math: the Python mirror of src/sync.cpp (sample, best of burst, drift fit).

The laptop uses it to learn the leader's show time (`picocomm clock`, and timestamps in `picocomm
log`), and the fake follower uses it too. Times are integer microseconds.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class SyncSample:
    local_us: int  # midpoint of t1 and t4, on the requester's clock
    offset_us: int  # show time minus local time
    delay_us: int  # round trip on the air


def make_sample(t1: int, t2: int, t3: int, t4: int, max_delay_us: int = 200_000):
    """NTP arithmetic. Returns a SyncSample, or None for impossible timestamps."""
    round_trip = t4 - t1
    held = t3 - t2
    delay = round_trip - held
    if round_trip < 0 or held < 0 or delay < 0 or delay > max_delay_us:
        return None
    # C++ divides with truncation toward zero; int() of a true division matches for these sizes.
    offset = int(((t2 - t1) + (t3 - t4)) / 2)
    return SyncSample(t1 + round_trip // 2, offset, delay)


@dataclass
class ClockModel:
    ref_local_us: int = 0
    ref_offset_us: int = 0
    drift_ppb: int = 0

    def offset_at(self, local_us: int) -> int:
        return self.ref_offset_us + int((local_us - self.ref_local_us) * self.drift_ppb / 1e9)

    def to_show(self, local_us: int) -> int:
        return local_us + self.offset_at(local_us)


class DriftFit:
    """Weighted least-squares line through the best sample of each recent burst."""

    CAPACITY = 16
    MIN_SPAN_US = 4_000_000
    MAX_DRIFT_PPB = 200_000
    WEIGHT_SCALE_US = 500.0

    def __init__(self):
        self.samples: list[SyncSample] = []

    def clear(self):
        self.samples.clear()

    def add(self, s: SyncSample):
        self.samples.append(s)
        if len(self.samples) > self.CAPACITY:
            self.samples.pop(0)

    def fit(self) -> ClockModel | None:
        if not self.samples:
            return None
        newest = self.samples[-1]
        min_delay = min(s.delay_us for s in self.samples)
        gate = min_delay * 2 + 500
        pts = []
        for s in self.samples:
            if s.delay_us > gate:
                continue
            extra = (s.delay_us - min_delay) / self.WEIGHT_SCALE_US
            pts.append((s.local_us - newest.local_us, s.offset_us - newest.offset_us,
                        1.0 / (1.0 + extra * extra)))
        sw = sum(w for _, _, w in pts)
        mx = sum(w * x for x, _, w in pts) / sw
        my = sum(w * y for _, y, w in pts) / sw
        sxx = sum(w * (x - mx) ** 2 for x, _, w in pts)
        sxy = sum(w * (x - mx) * (y - my) for x, y, w in pts)
        span = -min(x for x, _, _ in pts)
        slope = sxy / sxx if len(pts) >= 2 and span >= self.MIN_SPAN_US and sxx > 0 else 0.0
        ppb = max(-self.MAX_DRIFT_PPB, min(self.MAX_DRIFT_PPB, slope * 1e9))
        slope = ppb / 1e9
        at_newest = my - slope * mx
        return ClockModel(newest.local_us, newest.offset_us + round(at_newest), round(ppb))
