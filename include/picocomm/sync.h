// Clock sync math (docs/protocol-design.md, section 4). Portable C++, no pico-sdk.
//
// A follower's "local" clock is its own time_us_64(). "Show time" is the leader's time_us_64().
// Each SYNC_REQ/SYNC_RESP round trip gives one sample of the offset between them. Samples are
// filtered (best of a burst of 4, smallest round trip wins), a line is fitted through the best
// samples of the last ~16 bursts to get offset and crystal drift, and the result is applied to the
// clock core 0 reads gradually (slewed) once it is first locked, so animations never jump.

#pragma once

#include <cstdint>

namespace picocomm {

// One offset measurement from one round trip.
struct SyncSample {
    int64_t localUs = 0;   // local time the sample describes (midpoint of t1 and t4)
    int64_t offsetUs = 0;  // show time minus local time
    uint32_t delayUs = 0;  // round trip time spent on the air (and in the stacks)
};

// The standard NTP arithmetic. t1 and t4 are on the requester's clock, t2 and t3 on the leader's.
// Returns false for impossible timestamps (a negative round trip, or a response that took longer
// than maxDelayUs).
bool makeSample(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4, SyncSample &out,
                uint32_t maxDelayUs = 200000);

// Maps local time to show time: showTime = local + offset, where the offset drifts linearly.
// Integer only, so core 0 can evaluate it every frame without touching floating point.
struct ClockModel {
    int64_t refLocalUs = 0;
    int64_t refOffsetUs = 0;
    int32_t driftPpb = 0;  // how fast the offset grows, in parts per billion of local time

    int64_t offsetAt(int64_t localUs) const {
        return refOffsetUs + (localUs - refLocalUs) * driftPpb / 1000000000;
    }
    uint64_t toShow(uint64_t localUs) const { return localUs + offsetAt((int64_t)localUs); }
};

// Keeps the sample with the smallest round trip out of one burst.
class BurstFilter {
   public:
    void reset() { count_ = 0; }
    void add(const SyncSample &s);
    int count() const { return count_; }
    bool best(SyncSample &out) const;

   private:
    SyncSample best_;
    int count_ = 0;
};

// Weighted least-squares line through the best sample of each of the last kCapacity bursts, giving
// offset and drift. Samples with slower round trips count for less, and much slower ones are left
// out, since a slow round trip is also likely to be an asymmetric one.
class DriftFit {
   public:
    static constexpr int kCapacity = 16;
    // Drift is only estimated once the samples span at least this long; before that it stays 0.
    static constexpr int64_t kMinSpanUs = 4000000;
    // Pico crystals are good to tens of ppm; anything past this is a bad fit, not a real crystal.
    static constexpr int32_t kMaxDriftPpb = 200000;
    // A sample this much slower than the fastest in the window counts half as much in the fit.
    static constexpr double kWeightScaleUs = 500.0;

    void clear() { count_ = 0; next_ = 0; }
    void add(const SyncSample &s);
    int count() const { return count_; }
    uint32_t minDelayUs() const;
    // Fits the window. The model is referenced at the newest sample, so it extrapolates forward
    // from the most recent information. Returns false if there are no samples.
    bool fit(ClockModel &out) const;

   private:
    SyncSample samples_[kCapacity];
    int count_ = 0;
    int next_ = 0;
};

// Owns the clock core 0 reads (the "published" model) and moves it toward the latest fit (the
// "target") without ever jumping or running backwards, once locked.
class ClockDiscipline {
   public:
    // Correction is applied at no more than this rate: 4000 ppm is 100 us per 25 ms frame.
    static constexpr int64_t kMaxSlewPpm = 4000;
    // Further off than this and slewing would take too long (25 s at 4000 ppm); step instead.
    static constexpr int64_t kStepThresholdUs = 100000;

    // Unlocks; the next target is stepped to.
    void reset();
    bool locked() const { return locked_; }

    // Sets a new target. Steps the published clock to it if not yet locked, if forceStep is set
    // (the leader rebooted), or if the target is more than kStepThresholdUs away. Returns true if
    // it stepped.
    bool setTarget(const ClockModel &target, int64_t nowLocalUs, bool forceStep = false);

    // Slews the published clock toward the target. Call often (every few ms). Returns true if the
    // published model changed.
    bool update(int64_t nowLocalUs);

    const ClockModel &published() const { return published_; }
    const ClockModel &target() const { return target_; }
    // Target minus published, at the given local time: the correction still to be applied.
    int64_t errorUs(int64_t nowLocalUs) const {
        return target_.offsetAt(nowLocalUs) - published_.offsetAt(nowLocalUs);
    }

   private:
    ClockModel published_;
    ClockModel target_;
    int64_t lastUpdateUs_ = 0;
    bool locked_ = false;
};

}  // namespace picocomm
