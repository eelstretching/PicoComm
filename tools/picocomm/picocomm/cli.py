"""picocomm: the laptop's view of the show network (docs/protocol-design.md, section 9).

    picocomm watch                          live table of every prop
    picocomm stats [--once]                 frame counts and times, cue tallies, temperatures
    picocomm clock                          sync the laptop's clock to show time, print quality
    picocomm cue --group arches --effect leap --in 500ms
    picocomm log roster.csv                 the watch table, once a second, to CSV
    picocomm fake-leader / fake-follower    stand-ins for Picos, for trying all of the above

Join the show network's WiFi first. The leader is found from its BEACON; --leader IP skips that.
"""

from __future__ import annotations

import argparse
import csv
import sys
import time

from . import catalog, wire
from .clock import DriftFit
from .fake import FakeFollower, FakeLeader
from .net import Observer, now_us

_STATE = {1: "joining", 2: "synced", 3: "STALE", 4: "LOST"}


def _observer(args) -> Observer:
    obs = Observer(args.leader, args.port)
    if not obs.find_leader(5.0):
        sys.exit("No leader beacon heard in 5 s. On the show network? (--leader IP to skip this)")
    return obs


def _row_text(r: wire.RosterEntry) -> list[str]:
    prop = catalog.prop_name(r.prop_type) + (f" #{r.prop_index}" if r.prop_index else "")
    leader = r.node_id == wire.LEADER_ID
    return [
        str(r.node_id), prop,
        "leader" if leader else _STATE.get(r.state, str(r.state)),
        catalog.effect_name(r.effect) if r.cue_id or r.effect else "-",
        str(r.cue_id) if r.cue_id else "-",
        "-" if leader or not r.sync_delay_us else f"{r.sync_delay_us / 1000:.1f} ms",
        str(r.rssi) if r.rssi else "-",
        str(r.fps) if r.fps else "-",
        str(r.missed_frames),
        "-" if leader else f"{r.last_heard_ms / 1000:.1f} s",
        r.ipv4,
    ]


_HEADINGS = ["node", "prop", "state", "effect", "cue", "sync rtt", "rssi", "fps", "missed",
             "heard", "address"]


def _priority(p):
    return "fixed" if p == wire.ALWAYS_LEAD else str(p)


def cmd_watch(args):
    obs = _observer(args)
    try:
        while True:
            started = time.monotonic()
            rows = obs.roster()
            b = obs.beacon
            lines = []
            if b:
                wall = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(b.unix_time)) \
                    if b.unix_time else "no wall clock"
                lines.append(f"leader {obs.leader[0]}  generation {b.generation:08x}  "
                             f"scene {b.scene_id}  {b.follower_count} follower(s)  "
                             f"priority {_priority(b.priority)}  {wall}")
            else:
                why = "not listening for beacons with --leader" if obs.beacon_sock is None \
                    else "no beacon heard yet"
                lines.append(f"leader {obs.leader[0]} ({why})")
            table = [_HEADINGS] + [_row_text(r) for r in rows]
            widths = [max(len(row[i]) for row in table) for i in range(len(_HEADINGS))]
            for row in table:
                lines.append("  ".join(cell.ljust(w) for cell, w in zip(row, widths)))
            if not rows:
                lines.append("(no answer to QUERY)")
            # Clear the screen and redraw.
            print("\033[H\033[J" + "\n".join(lines), flush=True)
            time.sleep(max(0.0, args.interval - (time.monotonic() - started)))
    except KeyboardInterrupt:
        pass
    finally:
        obs.close()


def _ms(us: int) -> str:
    return f"{us / 1000:.1f}" if us else "-"


def _uptime(s: int) -> str:
    if s >= 86400:
        return f"{s // 86400}d{s % 86400 // 3600:02}h"
    if s >= 3600:
        return f"{s // 3600}h{s % 3600 // 60:02}m"
    return f"{s // 60}m{s % 60:02}s"


_STATS_HEADINGS = ["node", "prop", "effect", "fps", "frames", "frame ms", "worst ms", "step ms",
                   "show ms", "busy", "missed", "uptime", "temp", "cues", "late", "lost",
                   "refused", "rssi"]


def _stats_row(node_id, f, c, r) -> list[str]:
    if r is not None:
        prop = catalog.prop_name(r.prop_type) + (f" #{r.prop_index}" if r.prop_index else "")
        effect = catalog.effect_name(r.effect) if r.cue_id or r.effect else "-"
        rssi = str(r.rssi) if r.rssi else "-"
    else:
        prop, effect, rssi = "?", "-", "-"
    if node_id == wire.LEADER_ID:
        prop += " (leader)"
    f = f or wire.FrameStats()
    c = c or wire.CueStats()
    # How much of each frame's time core 0 spends computing and sending it.
    busy = f"{f.avg_frame_us * f.fps / 10_000:.0f}%" if f.fps and f.avg_frame_us else "-"
    return [
        str(node_id), prop, effect,
        str(f.fps) if f.fps else "-",
        str(f.frame_count) if f.frame_count else "-",
        _ms(f.avg_frame_us), _ms(f.worst_frame_us), _ms(f.avg_step_us), _ms(f.avg_show_us),
        busy, str(f.missed_frames),
        _uptime(f.uptime_s) if f.uptime_s else "-",
        "-" if f.temp_c == wire.NO_TEMPERATURE else f"{f.temp_c} C",
        str(c.cues_sent),
        str(c.cues_late) + (f" ({c.last_late_us / 1000:.1f} ms)" if c.cues_late else ""),
        str(c.cues_missed), str(c.cues_unknown), rssi,
    ]


def _stats_text(obs: Observer) -> str:
    stats = obs.stats()
    roster = {r.node_id: r for r in obs.roster()}
    lines = []
    b = obs.beacon
    if b:
        wall = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(b.unix_time)) \
            if b.unix_time else "no wall clock"
        lines.append(f"leader {obs.leader[0]}  {b.follower_count} follower(s)  "
                     f"scene {b.scene_id}  show time {b.show_time_us / 1e6:.0f} s  {wall}")
    else:
        lines.append(f"leader {obs.leader[0]}")
    if not stats:
        lines.append("(no answer to QUERY(stats); is the leader's firmware older than FRAME_STATS?)")
        return "\n".join(lines)
    table = [_STATS_HEADINGS] + [_stats_row(n, f, c, roster.get(n)) for n, (f, c) in stats.items()]
    widths = [max(len(row[i]) for row in table) for i in range(len(_STATS_HEADINGS))]
    for row in table:
        lines.append("  ".join(cell.ljust(w) for cell, w in zip(row, widths)).rstrip())
    return "\n".join(lines)


def cmd_stats(args):
    obs = _observer(args)
    try:
        if args.once:
            if obs.beacon_sock is not None and obs.beacon is None:
                obs.poll(1.1)  # catch a beacon for the header line
            print(_stats_text(obs))
            return
        while True:
            started = time.monotonic()
            print("\033[H\033[J" + _stats_text(obs), flush=True)
            time.sleep(max(0.0, args.interval - (time.monotonic() - started)))
    except KeyboardInterrupt:
        pass
    finally:
        obs.close()


def cmd_clock(args):
    obs = _observer(args)
    fit = DriftFit()
    print("burst  round trip  offset (show - laptop)  drift     show time")
    n = 0
    try:
        while args.count == 0 or n < args.count:
            n += 1
            started = time.monotonic()
            s = obs.sync_burst()
            if s is None:
                print(f"{n:5}  no response")
            else:
                fit.add(s)
                m = fit.fit()
                print(f"{n:5}  {s.delay_us / 1000:7.2f} ms  {m.offset_at(s.local_us):>18} us  "
                      f"{m.drift_ppb / 1000:+6.1f} ppm  {m.to_show(now_us()) / 1e6:12.3f} s",
                      flush=True)
            time.sleep(max(0.0, 2.0 - (time.monotonic() - started)))
    except KeyboardInterrupt:
        pass
    finally:
        obs.close()


def _parse_ms(text: str) -> int:
    t = text.strip().lower()
    if t.endswith("ms"):
        return int(float(t[:-2]))
    if t.endswith("s"):
        return int(float(t[:-1]) * 1000)
    return int(t)


def cmd_cue(args):
    params = bytearray(8)
    if args.rgb:
        params[0:3] = bytes(int(v) for v in args.rgb.split(","))
    if args.params:
        for i, v in enumerate(args.params.split(",")):
            params[i] = int(v, 0)
    kind = wire.CueKind[args.kind.upper().replace("-", "_")]
    ov = wire.Override(
        target_group_mask=catalog.group_mask(args.group),
        kind=kind,
        transition=wire.Transition[args.transition.upper().replace("-", "_")],
        delay_ms=_parse_ms(args.delay),
        effect=catalog.effect_id(args.effect) if kind == wire.CueKind.EFFECT else 0,
        transition_ms=_parse_ms(args.transition_time),
        params=bytes(params),
    )
    obs = _observer(args)
    try:
        ack = obs.override(ov)
    finally:
        obs.close()
    if ack is None:
        sys.exit("The leader didn't acknowledge the override.")
    print(f"cue {ack.cue_id}: {args.kind} {args.effect if kind == wire.CueKind.EFFECT else ''} "
          f"for {args.group} in {ov.delay_ms} ms")


def cmd_log(args):
    obs = _observer(args)
    fit = DriftFit()
    last_sync = 0.0
    rows_written = 0
    with open(args.file, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["laptop_time", "show_time_s", "node", "prop_type", "prop_index", "state",
                    "effect", "cue", "sync_delay_us", "rssi", "fps", "missed_frames",
                    "last_heard_ms", "address"])
        try:
            while True:
                started = time.monotonic()
                if started - last_sync >= 2.0:
                    s = obs.sync_burst()
                    if s:
                        fit.add(s)
                    last_sync = started
                model = fit.fit()
                show = f"{model.to_show(now_us()) / 1e6:.3f}" if model else ""
                stamp = time.strftime("%Y-%m-%d %H:%M:%S")
                for r in obs.roster():
                    w.writerow([stamp, show, r.node_id, r.prop_type, r.prop_index, r.state,
                                r.effect, r.cue_id, r.sync_delay_us, r.rssi, r.fps,
                                r.missed_frames, r.last_heard_ms, r.ipv4])
                    rows_written += 1
                f.flush()
                print(f"\r{rows_written} rows", end="", flush=True)
                time.sleep(max(0.0, args.interval - (time.monotonic() - started)))
        except KeyboardInterrupt:
            print()
        finally:
            obs.close()


def cmd_fake_leader(args):
    addrs = []
    for item in args.beacon_to.split(","):
        host, _, port = item.strip().partition(":")
        addrs.append((host, int(port) if port else args.port))
    FakeLeader(args.port, addrs, catalog.PROP_TYPES[args.prop], args.scene_every,
               args.drop).run()


def cmd_fake_follower(args):
    unknown = [catalog.effect_id(e) for e in args.unknown.split(",")] if args.unknown else []
    FakeFollower(args.listen_port, catalog.PROP_TYPES[args.prop], args.index, args.offset_ms,
                 args.drift_ppm, unknown, args.drop).run()


def main(argv=None):
    p = argparse.ArgumentParser(prog="picocomm", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--leader", help="leader's IP address (default: find it from its beacon)")
    p.add_argument("--port", type=int, default=wire.PORT, help="protocol port (default 7420)")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("watch", help="live table of every prop")
    s.add_argument("--interval", type=float, default=1.0)
    s.set_defaults(func=cmd_watch)

    s = sub.add_parser("stats", help="frame counts and times, cue tallies, temperatures")
    s.add_argument("--once", action="store_true", help="print one table and exit")
    s.add_argument("--interval", type=float, default=2.0,
                   help="seconds between refreshes (followers report every 2 s)")
    s.set_defaults(func=cmd_stats)

    s = sub.add_parser("clock", help="sync to show time and print the quality")
    s.add_argument("--count", type=int, default=0, help="bursts to run (default: until Ctrl-C)")
    s.set_defaults(func=cmd_clock)

    s = sub.add_parser("cue", help="send an override cue through the leader")
    s.add_argument("--group", default="all", help="all, arches, topper, megatree, ... (comma list)")
    s.add_argument("--effect", default="none", help=f"one of {', '.join(catalog.EFFECTS)}, or a number")
    s.add_argument("--kind", default="effect", choices=["effect", "brightness", "blackout",
                                                         "resume-local"])
    s.add_argument("--in", dest="delay", default="300ms", help="lead time, e.g. 500ms or 2s")
    s.add_argument("--transition", default="cut", choices=["cut", "fade-through-black",
                                                            "crossfade"])
    s.add_argument("--transition-time", default="0", help="e.g. 800ms")
    s.add_argument("--rgb", help="r,g,b into params[0..2]")
    s.add_argument("--params", help="up to 8 comma-separated byte values")
    s.set_defaults(func=cmd_cue)

    s = sub.add_parser("log", help="write the roster to CSV once a second")
    s.add_argument("file")
    s.add_argument("--interval", type=float, default=1.0)
    s.set_defaults(func=cmd_log)

    s = sub.add_parser("fake-leader", help="pretend to be the leader Pico")
    s.add_argument("--beacon-to", default="255.255.255.255",
                   help="beacon address(es), host[:port], comma-separated; e.g. "
                        "127.0.0.1:7421,127.0.0.1:7422 for fake followers on this laptop")
    s.add_argument("--prop", default="megatree", choices=list(catalog.PROP_TYPES))
    s.add_argument("--scene-every", type=float, default=0.0, help="cycle scenes every N seconds")
    s.add_argument("--drop", type=float, default=0.0, help="fraction of packets to lose")
    s.set_defaults(func=cmd_fake_leader)

    s = sub.add_parser("fake-follower", help="pretend to be a follower Pico")
    s.add_argument("--listen-port", type=int, default=wire.PORT,
                   help="port to hear beacons on (each fake on one laptop needs its own)")
    s.add_argument("--prop", default="arch", choices=list(catalog.PROP_TYPES))
    s.add_argument("--index", type=int, default=1)
    s.add_argument("--offset-ms", type=float, default=12345.0, help="skew this fake's clock")
    s.add_argument("--drift-ppm", type=float, default=20.0, help="and make it drift")
    s.add_argument("--unknown", help="effects to refuse, comma-separated")
    s.add_argument("--drop", type=float, default=0.0, help="fraction of packets to lose")
    s.set_defaults(func=cmd_fake_follower)

    args = p.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
