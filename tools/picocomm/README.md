# picocomm (laptop tool)

The laptop's view of the show network, speaking the same 32-byte protocol as the Picos
([`docs/protocol-design.md`](../../docs/protocol-design.md), section 9). Python 3.10 or newer,
standard library only.

```sh
cd tools/picocomm
python3 -m venv .venv && .venv/bin/pip install -e .   # installs the `picocomm` command
# or, without installing:  python3 -m picocomm ...
```

Join the show network's WiFi first. The leader is found from its beacon; `--leader IP` skips that.

| Command | What it does |
|---|---|
| `picocomm watch` | Live table of every prop: state, effect, cue, sync round trip, RSSI, fps, missed frames, last heard. |
| `picocomm stats` | How each prop's animation loop is doing: frames, average and worst frame time, step and show time, how busy core 0 is, missed frames, uptime, die temperature, and the leader's tally of cues sent, late, lost and refused. `--once` prints one table and exits. |
| `picocomm clock` | Syncs the laptop to show time and prints round trip, offset and drift every 2 s. |
| `picocomm cue --group arches --effect leap --in 500ms` | Sends an override through the leader, which cues the targets like any other cue. Also `--rgb 255,0,0`, `--params`, `--kind blackout`, `--transition crossfade --transition-time 800ms`. |
| `picocomm log roster.csv` | Writes the watch table to CSV once a second, stamped with show time. |
| `picocomm fake-leader` | Pretends to be the leader Pico. `--scene-every 5` cycles scenes; `--drop 0.2` loses 20% of packets. |
| `picocomm fake-follower` | Pretends to be a follower. `--prop arch --index 2`, `--unknown leap` to refuse an effect, `--drop`. |

## Trying it all on one laptop

Several programs on one laptop can't all share port 7420 for unicast, so give each fake its own
port and send the leader's beacons straight to them:

```sh
python3 -m picocomm --port 7420 fake-leader --beacon-to 127.0.0.1:7421,127.0.0.1:7422 --scene-every 5
python3 -m picocomm fake-follower --listen-port 7421 --prop arch --index 1
python3 -m picocomm fake-follower --listen-port 7422 --prop minitree --unknown leap
python3 -m picocomm --leader 127.0.0.1 watch
python3 -m picocomm --leader 127.0.0.1 stats --once
python3 -m picocomm --leader 127.0.0.1 cue --group arches --effect cylon --in 1s
```

On the real show network the defaults are right: the fakes and the Picos all use 7420 and find
each other by broadcast, so a fake follower on the laptop can join a real leader Pico.

## Tests

```sh
python3 -m unittest discover -s tests -t .
```

`test_wire.py` checks the encoder against the same golden packets as the C++ tests
(`tests/vectors/packets.txt`), `test_catalog.py` checks the effect table against
`include/picocomm/effects.h`, and `test_loopback.py` runs a fake leader, two fake followers and
the observer over real UDP on 127.0.0.1.
