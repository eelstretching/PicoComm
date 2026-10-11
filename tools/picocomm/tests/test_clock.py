"""Clock math, mirroring the cases in tests/test_sync.cpp."""

import random
import unittest

from picocomm.clock import DriftFit, SyncSample, make_sample


class ClockTest(unittest.TestCase):
    def test_sample(self):
        s = make_sample(1_000_000, 6_001_000, 6_001_200, 1_002_200)
        self.assertEqual((s.offset_us, s.delay_us, s.local_us), (5_000_000, 2000, 1_001_100))
        s = make_sample(1_000_000, 6_003_000, 6_003_000, 1_004_000)
        self.assertEqual((s.offset_us, s.delay_us), (5_001_000, 4000))
        s = make_sample(9_000_000_000, 1_000_500, 1_000_600, 9_000_001_100)
        self.assertEqual((s.offset_us, s.delay_us), (1_000_000 - 9_000_000_000, 1000))
        self.assertIsNone(make_sample(1000, 5000, 5000, 900))
        self.assertIsNone(make_sample(1000, 5000, 4000, 2000))
        self.assertIsNone(make_sample(1000, 5000, 5900, 1500))
        self.assertIsNone(make_sample(0, 0, 0, 300_000))

    def test_fit_recovers_drift(self):
        def true_offset(local):
            return 12345 + local * 25e-6

        rng = random.Random(42)
        fit = DriftFit()
        self.assertIsNone(fit.fit())
        fit.add(SyncSample(1_000_000, int(true_offset(1_000_000)), 2000))
        m = fit.fit()
        self.assertEqual(m.drift_ppb, 0)
        fit.clear()
        local = 0
        for i in range(16):
            local = 3_000_000 + i * 2_000_000
            slow = i % 5 == 3
            err = 6000 if slow else rng.uniform(-150, 150)
            fit.add(SyncSample(local, int(true_offset(local) + err), 20000 if slow else 2000 + i * 37 % 400))
        m = fit.fit()
        self.assertAlmostEqual(m.drift_ppb / 1000, 25.0, delta=5.0)
        self.assertAlmostEqual(m.offset_at(local), true_offset(local), delta=200)
        self.assertAlmostEqual(m.offset_at(local + 10_000_000), true_offset(local + 10_000_000),
                               delta=300)

    def test_drift_clamped(self):
        fit = DriftFit()
        fit.add(SyncSample(0, 0, 1000))
        fit.add(SyncSample(10_000_000, 50_000, 1000))
        self.assertEqual(fit.fit().drift_ppb, DriftFit.MAX_DRIFT_PPB)


if __name__ == "__main__":
    unittest.main()
