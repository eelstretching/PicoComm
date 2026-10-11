// Clock sync math tests: the NTP sample arithmetic, best-of-burst filtering, the drift fit, and
// the discipline that steps once and then slews without ever running backwards.

#include <cmath>
#include <random>

#include "check.h"
#include "picocomm/seqlock.h"
#include "picocomm/sync.h"

using namespace picocomm;

namespace {

void testSample() {
    // Leader is 5 s ahead. 1 ms out, 0.2 ms in the leader, 1 ms back: symmetric, so exact.
    SyncSample s;
    CHECK(makeSample(1000000, 6001000, 6001200, 1002200, s));
    CHECK_EQ(s.offsetUs, 5000000);
    CHECK_EQ(s.delayUs, 2000);
    CHECK_EQ(s.localUs, 1001100);

    // Asymmetric: 3 ms out, 1 ms back. The offset error is half the asymmetry (1 ms), and is
    // bounded by delay / 2.
    CHECK(makeSample(1000000, 6003000, 6003000, 1004000, s));
    CHECK_EQ(s.delayUs, 4000);
    CHECK_EQ(s.offsetUs, 5001000);

    // Leader behind (negative offset), and clocks billions of microseconds apart.
    CHECK(makeSample(9000000000ull, 1000500, 1000600, 9000001100ull, s));
    CHECK_EQ(s.offsetUs, 1000000 - 9000000000ll);
    CHECK_EQ(s.delayUs, 1000);

    // Garbage is refused.
    CHECK(!makeSample(1000, 5000, 5000, 900, s));       // response before request
    CHECK(!makeSample(1000, 5000, 4000, 2000, s));      // t3 before t2
    CHECK(!makeSample(1000, 5000, 5900, 1500, s));      // leader held it longer than the round trip
    CHECK(!makeSample(0, 0, 0, 300000, s));             // slower than maxDelayUs
}

void testBurst() {
    BurstFilter f;
    SyncSample out;
    CHECK(!f.best(out));
    f.add({100, 10, 5000});
    f.add({200, 20, 1500});
    f.add({300, 30, 9000});
    f.add({400, 40, 1600});
    CHECK(f.best(out));
    CHECK_EQ(out.delayUs, 1500);
    CHECK_EQ(out.offsetUs, 20);
    CHECK_EQ(f.count(), 4);
    f.reset();
    CHECK(!f.best(out));
}

void testModel() {
    ClockModel m;
    m.refLocalUs = 1000000;
    m.refOffsetUs = 5000;
    m.driftPpb = 30000;  // 30 ppm
    CHECK_EQ(m.offsetAt(1000000), 5000);
    CHECK_EQ(m.offsetAt(2000000), 5030);    // 30 us per second
    CHECK_EQ(m.offsetAt(0), 4970);
    CHECK_EQ(m.toShow(61000000), 61000000 + 5000 + 1800);
    // A week of uptime doesn't overflow.
    uint64_t week = 7ull * 24 * 3600 * 1000000;
    CHECK_EQ(m.offsetAt((int64_t)week), 5000 + ((int64_t)week - 1000000) * 30000 / 1000000000);
}

void testFit() {
    // True clock: offset 12345 us at local 0, follower crystal 25 ppm slow relative to leader.
    const double trueDriftPpm = 25.0;
    auto trueOffset = [&](int64_t local) { return 12345.0 + local * trueDriftPpm * 1e-6; };

    DriftFit fit;
    ClockModel m;
    CHECK(!fit.fit(m));

    // One sample: offset only, no drift yet.
    fit.add({1000000, (int64_t)trueOffset(1000000), 2000});
    CHECK(fit.fit(m));
    CHECK_EQ(m.driftPpb, 0);
    CHECK_NEAR(m.offsetAt(1000000), trueOffset(1000000), 1);

    // 16 bursts, 2 s apart, with up to +-150 us of asymmetry noise on the good samples and a few
    // slow (and badly asymmetric) ones the gate should throw out.
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> noise(-150, 150);
    fit.clear();
    int64_t local = 0;
    for (int i = 0; i < 16; i++) {
        local = 3000000 + i * 2000000;
        bool slow = (i % 5 == 3);
        double err = slow ? 6000 : noise(rng);
        uint32_t delay = slow ? 20000 : 2000 + (uint32_t)(i * 37 % 400);
        fit.add({local, (int64_t)(trueOffset(local) + err), delay});
    }
    CHECK(fit.fit(m));
    CHECK_EQ(fit.count(), 16);
    CHECK_NEAR(m.driftPpb / 1000.0, trueDriftPpm, 5.0);
    CHECK_NEAR(m.offsetAt(local), trueOffset(local), 200);
    // Extrapolated 10 s ahead (no new samples), still well under a millisecond.
    CHECK_NEAR(m.offsetAt(local + 10000000), trueOffset(local + 10000000), 300);

    // The window keeps only the last 16.
    for (int i = 0; i < 40; i++) {
        local += 2000000;
        fit.add({local, (int64_t)trueOffset(local), 2000});
    }
    CHECK_EQ(fit.count(), 16);
    CHECK(fit.fit(m));
    CHECK_NEAR(m.driftPpb / 1000.0, trueDriftPpm, 0.5);

    // An absurd drift (a broken sample stream) is clamped.
    fit.clear();
    fit.add({0, 0, 1000});
    fit.add({10000000, 50000, 1000});  // 5000 ppm
    CHECK(fit.fit(m));
    CHECK_EQ(m.driftPpb, DriftFit::kMaxDriftPpb);
}

void testDiscipline() {
    ClockDiscipline d;
    CHECK(!d.locked());
    ClockModel t;
    t.refLocalUs = 1000000;
    t.refOffsetUs = 7000000;
    CHECK(d.setTarget(t, 1000000));  // first lock steps
    CHECK(d.locked());
    CHECK_EQ(d.published().offsetAt(1000000), 7000000);

    // A new target 2 ms away is slewed, at no more than 4000 ppm.
    ClockModel t2 = t;
    t2.refOffsetUs = 7002000;
    CHECK(!d.setTarget(t2, 1000000));
    CHECK_EQ(d.errorUs(1000000), 2000);

    int64_t now = 1000000;
    uint64_t lastShow = d.published().toShow(now);
    int64_t maxRate = 0;
    for (int i = 0; i < 1000; i++) {
        now += 1000;  // poll every 1 ms
        d.update(now);
        uint64_t show = d.published().toShow(now);
        CHECK(show > lastShow);  // never backwards, never stuck
        int64_t extra = (int64_t)(show - lastShow) - 1000;
        if (extra > maxRate) {
            maxRate = extra;
        }
        lastShow = show;
    }
    CHECK(maxRate <= 4);  // 4 us per 1 ms
    CHECK_EQ(d.errorUs(now), 0);  // 2 ms at 4 us/ms takes 500 ms

    // Slewing backwards (target behind) also keeps show time increasing.
    ClockModel t3 = t2;
    t3.refOffsetUs = 7002000 - 3000;
    t3.refLocalUs = 1000000;
    d.setTarget(t3, now);
    for (int i = 0; i < 2000; i++) {
        now += 1000;
        d.update(now);
        uint64_t show = d.published().toShow(now);
        CHECK(show > lastShow);
        CHECK((int64_t)(show - lastShow) >= 996);
        lastShow = show;
    }
    CHECK_EQ(d.errorUs(now), 0);

    // More than kStepThresholdUs off: step.
    ClockModel far = t3;
    far.refOffsetUs += 500000;
    CHECK(d.setTarget(far, now));
    CHECK_EQ(d.errorUs(now), 0);

    // Forced step (leader rebooted) even when close.
    ClockModel near = far;
    near.refOffsetUs += 10;
    CHECK(d.setTarget(near, now, true));

    // Drift is adopted from the target.
    ClockModel drifting = near;
    drifting.driftPpb = 20000;
    d.setTarget(drifting, now);
    d.update(now + 1000);
    CHECK_EQ(d.published().driftPpb, 20000);

    d.reset();
    CHECK(!d.locked());
}

void testSeqLock() {
    SeqLock<ClockModel> lock;
    ClockModel m;
    CHECK(!lock.read(m));
    ClockModel w;
    w.refLocalUs = 1;
    w.refOffsetUs = 2;
    w.driftPpb = 3;
    lock.write(w);
    CHECK(lock.read(m));
    CHECK(m.refLocalUs == 1 && m.refOffsetUs == 2 && m.driftPpb == 3);
}

}  // namespace

int main() {
    testSample();
    testBurst();
    testModel();
    testFit();
    testDiscipline();
    testSeqLock();
    return check::finish("test_sync");
}
