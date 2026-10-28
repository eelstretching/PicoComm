"""Stage 0 on one machine: a fake leader, two fake followers and the observer client, all on
127.0.0.1, exercising discovery, sync, roster queries, overrides and cue delivery for real over
UDP. One follower drops 10% of its packets, so cue resends get exercised. Takes about 8
seconds."""

import threading
import time
import unittest

from picocomm import catalog, wire
from picocomm.fake import FakeFollower, FakeLeader
from picocomm.net import Observer

LEADER_PORT = 47420
FOLLOWER_PORTS = (47421, 47422)


class LoopbackTest(unittest.TestCase):
    def test_show_on_localhost(self):
        leader = FakeLeader(LEADER_PORT, [("127.0.0.1", p) for p in FOLLOWER_PORTS])
        arch = FakeFollower(FOLLOWER_PORTS[0], catalog.PROP_TYPES["arch"], 1, offset_ms=5000,
                            drift_ppm=30, drop=0.1, quiet=True)
        tree = FakeFollower(FOLLOWER_PORTS[1], catalog.PROP_TYPES["minitree"], 1,
                            offset_ms=-3000, unknown_effects=[catalog.EFFECTS["leap"]], quiet=True)
        failures = []

        def run(node):
            try:
                node.run(8.0)
            except Exception as e:  # surface crashes in the fakes as test failures
                failures.append(e)
                raise

        threads = [threading.Thread(target=run, args=(n,), daemon=True)
                   for n in (leader, arch, tree)]
        for t in threads:
            t.start()

        obs = Observer("127.0.0.1", LEADER_PORT)
        try:
            time.sleep(4.0)
            rows = obs.roster()
            for _ in range(3):  # QUERY isn't retried by the protocol; a lost row is possible
                if len(rows) == 3:
                    break
                rows = obs.roster()
            self.assertEqual(len(rows), 3)
            self.assertEqual(rows[0].node_id, wire.LEADER_ID)
            self.assertEqual({r.prop_type for r in rows[1:]},
                             {catalog.PROP_TYPES["arch"], catalog.PROP_TYPES["minitree"]})
            self.assertTrue(all(r.state == wire.NodeState.SYNCED for r in rows[1:]))

            # The followers' clocks agree with the leader's show time (loopback: well under 1 ms).
            for f in (arch, tree):
                err = f.show_us() - leader.show_us()
                self.assertLess(abs(err), 1000, f"follower clock off by {err} us")

            # Stats: a FRAME_STATS and a CUE_STATS for every prop, the leader first.
            stats = obs.stats()
            for _ in range(3):
                if len(stats) == 3 and all(f and c for f, c in stats.values()):
                    break
                stats = obs.stats()
            self.assertEqual(sorted(stats), [r.node_id for r in rows])
            for node_id, (f, c) in stats.items():
                self.assertEqual(f.node_count, 3)
                self.assertEqual(f.fps, 40)
                self.assertGreater(f.frame_count, 0, f"node {node_id} has no frame stats")
                self.assertNotEqual(f.temp_c, wire.NO_TEMPERATURE)
                self.assertIsNotNone(c)

            # The laptop can sync too.
            s = obs.sync_burst()
            self.assertIsNotNone(s)

            ack = obs.override(wire.Override(catalog.ALL_GROUPS, wire.CueKind.EFFECT,
                                             wire.Transition.CUT, 300, catalog.EFFECTS["leap"]))
            self.assertIsNotNone(ack)
            time.sleep(1.0)
            self.assertEqual([c.cue_id for c in arch.delivered], [ack.cue_id])
            self.assertEqual(tree.delivered, [])  # refused: it doesn't know Leap
            self.assertTrue(tree.joined)
            self.assertIn(ack.cue_id, tree.recent)
        finally:
            obs.close()
            for t in threads:
                t.join()
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
