#include "picocomm/sync.h"

namespace picocomm {

bool makeSample(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4, SyncSample &out,
                uint32_t maxDelayUs) {
    int64_t roundTrip = (int64_t)(t4 - t1);
    int64_t leaderHeld = (int64_t)(t3 - t2);
    int64_t delay = roundTrip - leaderHeld;
    if (roundTrip < 0 || leaderHeld < 0 || delay < 0 || delay > (int64_t)maxDelayUs) {
        return false;
    }
    // Differences first, so nothing overflows however far apart the two clocks are.
    int64_t a = (int64_t)(t2 - t1);
    int64_t b = (int64_t)(t3 - t4);
    out.offsetUs = (a + b) / 2;
    out.localUs = (int64_t)(t1 + (uint64_t)(roundTrip / 2));
    out.delayUs = (uint32_t)delay;
    return true;
}

void BurstFilter::add(const SyncSample &s) {
    if (count_ == 0 || s.delayUs < best_.delayUs) {
        best_ = s;
    }
    count_++;
}

bool BurstFilter::best(SyncSample &out) const {
    if (count_ == 0) {
        return false;
    }
    out = best_;
    return true;
}

void DriftFit::add(const SyncSample &s) {
    samples_[next_] = s;
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) {
        count_++;
    }
}

uint32_t DriftFit::minDelayUs() const {
    uint32_t m = UINT32_MAX;
    for (int i = 0; i < count_; i++) {
        if (samples_[i].delayUs < m) {
            m = samples_[i].delayUs;
        }
    }
    return m;
}

bool DriftFit::fit(ClockModel &out) const {
    if (count_ == 0) {
        return false;
    }
    const SyncSample &newest = samples_[(next_ + kCapacity - 1) % kCapacity];

    // Gate: drop samples whose round trip is more than 2x (plus 500 us of slack) the fastest.
    // The rest are weighted by how close they come to the fastest: a sample's offset error is at
    // most half its round trip, so the extra time a slower sample spent on the air is extra room
    // for asymmetry.
    uint32_t minDelay = minDelayUs();
    uint64_t gate = (uint64_t)minDelay * 2 + 500;
    double weights[kCapacity];

    // Work relative to the newest sample, so the doubles hold small numbers.
    double sw = 0, sx = 0, sy = 0;
    int n = 0;
    int64_t oldest = newest.localUs;
    for (int i = 0; i < count_; i++) {
        const SyncSample &s = samples_[i];
        if (s.delayUs > gate) {
            weights[i] = 0;
            continue;
        }
        double extra = (double)(s.delayUs - minDelay) / kWeightScaleUs;
        double w = 1.0 / (1.0 + extra * extra);
        weights[i] = w;
        sw += w;
        sx += w * (double)(s.localUs - newest.localUs);
        sy += w * (double)(s.offsetUs - newest.offsetUs);
        n++;
        if (s.localUs < oldest) {
            oldest = s.localUs;
        }
    }
    double mx = sx / sw, my = sy / sw;
    double sxx = 0, sxy = 0;
    for (int i = 0; i < count_; i++) {
        if (weights[i] == 0) {
            continue;
        }
        const SyncSample &s = samples_[i];
        double dx = (double)(s.localUs - newest.localUs) - mx;
        double dy = (double)(s.offsetUs - newest.offsetUs) - my;
        sxx += weights[i] * dx * dx;
        sxy += weights[i] * dx * dy;
    }

    double slope = 0;  // offset change per local microsecond
    if (n >= 2 && newest.localUs - oldest >= kMinSpanUs && sxx > 0) {
        slope = sxy / sxx;
    }
    double ppb = slope * 1e9;
    if (ppb > kMaxDriftPpb) {
        ppb = kMaxDriftPpb;
    } else if (ppb < -kMaxDriftPpb) {
        ppb = -kMaxDriftPpb;
    }
    slope = ppb / 1e9;

    // The fitted line evaluated at the newest sample's time (x = 0).
    double atNewest = my - slope * mx;
    out.refLocalUs = newest.localUs;
    out.refOffsetUs = newest.offsetUs + (int64_t)(atNewest >= 0 ? atNewest + 0.5 : atNewest - 0.5);
    out.driftPpb = (int32_t)(ppb >= 0 ? ppb + 0.5 : ppb - 0.5);
    return true;
}

void ClockDiscipline::reset() {
    locked_ = false;
    published_ = ClockModel();
    target_ = ClockModel();
}

bool ClockDiscipline::setTarget(const ClockModel &target, int64_t nowLocalUs, bool forceStep) {
    target_ = target;
    int64_t err = errorUs(nowLocalUs);
    if (!locked_ || forceStep || err > kStepThresholdUs || err < -kStepThresholdUs) {
        published_ = target;
        lastUpdateUs_ = nowLocalUs;
        locked_ = true;
        return true;
    }
    return false;
}

bool ClockDiscipline::update(int64_t nowLocalUs) {
    if (!locked_) {
        return false;
    }
    int64_t elapsed = nowLocalUs - lastUpdateUs_;
    if (elapsed <= 0) {
        return false;
    }
    lastUpdateUs_ = nowLocalUs;
    int64_t err = errorUs(nowLocalUs);
    int64_t maxAdj = elapsed * kMaxSlewPpm / 1000000;
    int64_t adj = err > maxAdj ? maxAdj : (err < -maxAdj ? -maxAdj : err);
    if (adj == 0 && published_.driftPpb == target_.driftPpb) {
        return false;
    }
    // Re-reference at now: same show time as before plus the adjustment, and the target's drift.
    ClockModel next;
    next.refLocalUs = nowLocalUs;
    next.refOffsetUs = published_.offsetAt(nowLocalUs) + adj;
    next.driftPpb = target_.driftPpb;
    published_ = next;
    return true;
}

}  // namespace picocomm
