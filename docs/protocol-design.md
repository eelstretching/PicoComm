# PicoComm protocol design

Status: draft for discussion (2026-10-11). Nothing here is built yet. The numbers marked
"expected" are estimates that the basement tests in section 8 are meant to confirm or kill.

This answers [`prompt.md`](../prompt.md): 4 to 8 Picos on the lawn (MegaTree with topper, 4 or 6
leaping arches, maybe some small trees), each one computing its own show, coordinating over WiFi so
that "everybody switch to Starlight in 300 ms" happens on the same frame everywhere.

## 1. The idea in one paragraph

Nobody ships frames. The leader ships *decisions* ("at show time T, run effect E with parameters
P"), and those decisions are sent well before T. So WiFi latency and retries don't matter at all.
The only thing that has to be tight is everyone's agreement on what time it is, and that is a
solved problem (NTP-style timestamp exchange, keeping the fastest round trips). xLights is right
that WiFi can't stream 30 frames a second to thousands of pixels with no hiccups. It doesn't need
to: a cue is 32 bytes, sent a few times a minute.

## 2. Hardware and network

Both boards in play have the same radio: the Pico 2 W has a CYW43439, and the
universal-strip-controller has the RM2 module (also a CYW43439) next to its RP2350B. So one network
stack (pico-sdk's `pico_cyw43_arch` + lwIP) covers every prop.

### A dedicated show network

Use a separate small router as the show's access point, and use **the exact same router and
settings in the basement** that will go out on the lawn, so nothing changes between testing and
the show. A 5 V travel router (the GL.iNet sort) is convenient because it can live in the same
weatherproof box as a Pico power supply.

Router settings that matter:

- **2.4 GHz only** for the show SSID (the CYW43439 has no 5 GHz). 20 MHz channel width, fixed
  channel (1, 6 or 11, whichever is quietest on the street), not "auto".
- **WPA2-PSK (AES)**. That is what the existing `wifi_rtc_sync` example connects with.
- **DHCP reservations** for each Pico's MAC, so each prop always gets the same IP. The protocol
  doesn't need this (discovery handles it), but it makes the laptop view and debugging friendlier.
- **DTIM interval 1** if the router exposes it (see "broadcast is slow" below).
- Internet is optional. If the router's WAN port is plugged into the house, the leader can set its
  DS3231 from NTP at boot, just like `wifi_rtc_sync` already does.

Two radio gotchas that shape the protocol:

1. **Turn off WiFi power saving on every Pico.** By default the CYW43 naps between beacons, which
   adds tens to hundreds of milliseconds of jitter to every packet and would wreck time sync. After
   connecting: `cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);`. The props run off mains power, so
   the extra few tens of milliamps don't matter.
2. **Broadcast and multicast are slow and unreliable on WiFi.** The access point sends them at the
   lowest basic rate with no acknowledgement or retry, and if *any* client on the network is in
   power-save mode (say, a phone or laptop) the access point holds them until the next DTIM beacon,
   which can be 100 to 300 ms later. Unicast gets link-layer ACKs and retries for free. So:
   **broadcast is used only for discovery; everything time-sensitive is unicast.**

## 3. Roles

- **Leader**: one Pico, chosen by election (section 5). It runs the *director* (the show script:
  what plays when, on which props), owns show time, keeps the roster of followers, and answers the
  laptop. The leader is also a prop (most likely the MegaTree); its own core 0 gets cues exactly like
  a follower's does, through the same code path.
- **Follower**: every other prop. It finds the leader, keeps its clock locked to the leader's, and
  plays whatever it is cued to play.
- **Candidate**: a prop that can lead (it has the show script, and ideally an RTC) but doesn't
  insist. It follows like any other prop until the election says it should lead. Each candidate has
  a **leader priority**, a number that means something: by convention its LED count, so the
  MegaTree (6,400) beats an arch group (a few hundred). Priority 0 means "never lead" (a plain
  follower), and `kAlwaysLead` (0xffffffff) is a fixed leader that never steps down, for the bench
  or a laptop fake leader.
- **Observer**: the laptop (or anything else running the Python tool). It can watch the roster and
  the clocks, and can send manual override cues through the leader. It never talks to followers
  directly, so the leader always knows the full state of the show.

Every prop has a **prop type** (MEGATREE, TOPPER, ARCH, MINI_TREE, ...) and a **prop index**
(arch 1 to 6), fixed at build time, plus the RP2350's unique board ID read from flash. The leader
assigns each follower a small **node ID** (1 to 254; the leader is 0, observers are 255) when it
joins. Prop types double as **groups** for targeting ("all arches").

## 4. Show time and clock sync

### What "show time" is

Show time is the leader's `time_us_64()` plus a fixed offset: microseconds, as a `uint64_t`. The
first leader after everything powers on uses offset 0 (show time is time since it booted). A
candidate that takes over from another leader sets its offset so that show time **carries on**
from the clock it was following, so a handover doesn't make show time jump. Show time only ever
moves forward and never jumps, which is exactly what animations want. Wall-clock time (for "start
the show at 5 pm" or the New Year countdown) comes from the leader's DS3231, and the leader's beacon
carries "unix time U corresponds to show time T", so any node can convert. Each time a node starts
leading it picks a random 32-bit **leader generation**, so followers can tell that the leader
changed, and (by comparing show times) whether show time started over.

### The sync exchange

Standard NTP arithmetic, run between each follower and the leader over unicast UDP:

```
follower                         leader
   t1 = local clock   --SYNC_REQ-->
                                   t2 = leader clock on receive
                      <--SYNC_RESP-- t3 = leader clock on send (carries t1, t2, t3)
   t4 = local clock

   offset = ((t2 - t1) + (t3 - t4)) / 2     leader_time ~= local_time + offset
   delay  = (t4 - t1) - (t3 - t2)           round trip spent on the air
```

The offset's error is at most half the *asymmetry* of the round trip, and it is bounded by
`delay / 2`. WiFi round trips vary a lot, but the *fastest* ones are nearly symmetric, so:

- Every 2 seconds, a follower sends a **burst of 4** SYNC_REQs about 20 ms apart, and keeps only
  the sample with the smallest `delay`.
- Over the last ~16 bursts (about half a minute) it fits a line through the best samples. That
  gives the offset *and* the relative crystal drift. Pico crystals are good to roughly ±30 ppm, so
  two boards can drift apart by up to ~2 ms per minute if nobody corrects for it; the fit does.
- The first lock **steps** the clock. After that the follower **slews** it (at most ~100 µs of
  correction per frame), so a running animation never visibly jumps.
- Timestamps are taken as close to the radio as the stack allows: t2 at the top of the leader's
  lwIP receive callback, t3 immediately before `udp_sendto`. Core 1 runs lwIP in **poll mode** in
  a tight loop (`pico_cyw43_arch_lwip_poll`), which keeps the time between a packet arriving and
  its callback running short and consistent. Core 0 is never interrupted by networking at all.

Expected: on a quiet network with power saving off, best-of-4 round trips of roughly 2 to 5 ms and
offset error well under 1 ms, which is the goal in `prompt.md` and comfortably less than one frame
(25 ms at 40 fps). Section 8 is how we find out whether that holds.

### Frame phase

Agreeing on the time isn't quite enough. Two props both running at 40 fps can still have their
frames up to 25 ms apart if they started at different moments, and then "switch at T" lands on
different frames. So core 0 should start each frame on a show-time boundary (frame `n` starts at
`n * usPerFrame` of show time) rather than whenever the previous frame finished. Then every prop
running the same frame rate ticks in step, and a cue at time T takes effect on the first frame
starting at or after T, on every prop, on the same frame. This is a small change to how `Animator`
paces frames; it would take a show-time source instead of `time_us_64()` directly.

A second, optional step for later: effects that should look continuous *across* props (a chase
that runs from one arch to the next) compute their state from `showTime - cueTime` instead of
counting frames, and random effects take their seed from the cue's parameters. Then a follower that
reboots mid-effect can jump straight to the right place.

## 5. Discovery and membership

1. The leader **broadcasts a BEACON** once a second to `255.255.255.255:7420`: its generation,
   show time (informational only; broadcast delay is unknown), wall-clock time, current scene,
   follower count, and leader priority.
2. A follower that hears a beacon learns the leader's IP from the packet's source and sends a
   unicast **JOIN** with its board ID, prop type and index, firmware version, and effect catalog
   version.
3. The leader answers with **WELCOME** (assigned node ID, group mask), adds it to the roster, and
   immediately sends it the current scene's cue so a late joiner catches up instead of waiting for
   the next scene change.
4. The follower starts its sync bursts. Those double as **heartbeats**; there's no separate
   keepalive. It also sends a **STATUS** packet every 2 seconds (what it's playing, frame rate,
   missed frames, sync quality, RSSI, free RAM).
5. The leader marks a follower **stale** after 6 seconds with no packets and **lost** after 30,
   and shows both to the laptop.

Why broadcast and not scanning an IP range: a beacon finds the leader no matter what addresses the
router hands out, and a follower that boots late or reboots finds it within a second. Scanning
would work too, but needs everyone to agree on the range, and costs more packets. Multicast would
also work but needs IGMP in lwIP and the CYW43's multicast filter configured, for no benefit over
broadcast on a one-subnet network.

### Electing the leader

Any prop might have an RTC and the show script, so the leader isn't fixed at build time. The
election runs on BEACONs alone, with no extra packet types. A node's **rank** is
`(priority, IPv4 address)`: higher priority wins, and a tie goes to the higher address (unique on
the show network, and already in every packet's source, so it costs no bytes).

- **Listen first.** At boot a candidate follows for 3 seconds plus a random 0 to 1 s of jitter.
  Beacons come once a second, so that's enough to hear a leader that's already running. If it heard
  none, it starts leading. The jitter keeps boards powered on together from all claiming at once.
- **Two leaders settle within a beacon.** A leader that hears a higher-ranked leader's BEACON steps
  down and follows it. That covers boards booted together and a network split healing. A follower
  always follows the highest-ranked leader it has heard lately.
- **The best prop takes the show back.** A candidate that outranks the leader it is following takes
  over once its clock is locked. So when the MegaTree reboots mid-show, an arch leads for a few
  seconds, then the tree takes over again, carrying on the arch's show time.
- **Taking over when the leader dies.** If a candidate hears nothing from the leader (no beacon, no
  sync response) for 8 seconds plus its jitter, it starts leading. That's before followers would
  orphan at 10 s, so the show keeps going. If several candidates are left, they may all claim, and
  the "two leaders" rule leaves the highest-ranked one within a beacon.
- **Show time carries on.** A node that takes over sets its show-time offset from its own locked
  clock (section 4), so its BEACONs carry on the old show time. A follower that sees a new
  generation whose beacon show time is within 100 ms of its own clock treats it as a **handover**:
  it keeps its clock running, rejoins, and re-locks with an ordinary slew. A new generation far
  from its clock is a reboot, as before: the clock is reset and re-locked with a step. A leader that
  steps down likewise keeps its own show clock running until its first sync with the new leader.
- **Fixed leaders.** Priority `kAlwaysLead` leads from boot and never steps down; priority 0 never
  leads. A show with one fixed leader and the rest plain followers works exactly as before the
  election existed.

### Mixed shows: some boards with an RTC, some without

Until every board has its DS3231, some props have a wall clock and some don't. Show time doesn't
care (it's the leader's clock, RTC or not); only the wall clock does, for "start at 5 pm".

- **An RTC board leads when one is up.** `candidatePriority(ledCount, hasRtc)` sets the top bit of
  the priority for a board with an RTC, so it outranks every board without one, whatever their LED
  counts; within each group the LED count decides as before. A board without an RTC can still be a
  candidate, so the show goes on if the RTC boards are down.
- **Everyone learns the wall clock from the beacons.** Each BEACON carries unix time and the
  fraction of its second (byte 31, in 1/256 s), against the beacon's show time. A follower keeps the
  latest; following, it prefers the leader's wall clock over its own RTC, so every prop agrees.
  It's good to a few ms (the beacon's trip plus the clock error). It's forgotten when show time
  starts over, and when a node starts leading with a fresh show time.
- **A board without an RTC that takes over carries the wall clock on**, from the last beacon it
  heard, the same way it carries on show time. Without the fraction each such handover could lose
  up to a second; with it, a chain of handovers stays within a few ms.

All of this lives in `Elector` (`include/picocomm/elector.h`), which owns a `Leader` or a
`Follower` and switches between them, so the Pico glue and the simulator both get it for free.

## 6. Cues: telling props what to do

A **CUE** says: "at show time `at`, do `kind` (run an effect, set brightness, black out, or go back
to your own local playlist) with effect `e`, parameters `p`, and transition `t` over `ms`
milliseconds."

- The leader sends cues **unicast to each follower**, scheduled a **lead time** ahead (300 ms by
  default). A scene change is one cue per prop, all with the same `at`; props can get *different*
  effects at the same instant (tree does Starlight while the arches do a slow gold FillWipe).
- Every cue needs an **ACK**. Without one the leader resends every 50 ms, which gives about six
  tries inside a 300 ms lead time. Cue IDs make resends harmless: a follower ignores a cue ID it
  has already accepted.
- If a cue arrives *after* its time (all six tries lost), the follower applies it immediately and
  says how late it was in the ACK. The leader records that, so the laptop shows "arch 3 was 420 ms
  late at 21:14".
- An effect ID the follower doesn't know gets an ACK with status UNKNOWN_EFFECT, and the follower
  keeps playing what it was playing.

### Effect catalog

Effects and their parameters are a shared table in this repo (`include/picocomm/effects.h`, to be
written): `enum class EffectId : uint16_t` plus a comment per effect saying what its 8 parameter
bytes mean (palette, speed, color, random seed, ...). The table has a version number that goes in
JOIN, so the leader can warn when a prop is running an older catalog. Each prop's firmware maps the
IDs it supports to its own `Animation` objects (the MegaTree's Starlight, the arches' Leap, ...).
16-bit IDs leave room for growth.

### When the leader goes away

Followers never go dark because of the network:

- No sync response for 10 seconds: the follower keeps running on its free-running clock and
  finishes the current cue's effect, then falls back to its own local playlist (the
  `RandomAnimator` each prop has today). Its status light can show "orphaned".
- If the leader was a candidate's to replace, a candidate takes over after 8 s (section 5) and
  carries on show time; followers rejoin it without a jump.
- A beacon with a **new leader generation** whose show time is far from the follower's means the
  leader rebooted and show time started over: the follower rejoins and re-locks its clock (a step
  is allowed this one time).
- A follower that reboots simply rejoins and gets the current scene's cue on join.

## 7. Wire format

Same approach as OfficeSign: fixed-size, hand-rolled little-endian encoding on both sides (C++ on
the Picos, Python `struct` on the laptop), with no structs sent raw, so padding can never differ.
**Every packet is exactly 32 bytes**, in one UDP datagram, on **UDP port 7420** (leader listens
there; followers and the laptop use any local port). One size means one receive buffer, and makes
"is this ours?" checks trivial.

### Header (bytes 0 to 7, every packet)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u8[2] | magic | `'P' 'C'` |
| 2 | u8 | version | 1 |
| 3 | u8 | type | see below |
| 4 | u16 | seq | per-sender counter, for ACKs and loss stats |
| 6 | u8 | src | sender's node ID (leader 0, observer 255, 254 = not yet assigned) |
| 7 | u8 | flags | bit 0: ACK requested |

### Packet types (bytes 8 to 31)

| Type | Name | Direction | Payload (offset: type field) |
|---|---|---|---|
| 1 | BEACON | leader to broadcast | 8: u32 generation; 12: u64 showTimeUs; 20: u32 unixTime (0 if unknown); 24: u16 sceneId; 26: u8 followerCount; 27: u32 priority (0 never leads, 0xffffffff fixed leader); 31: u8 unixFraction (how far into unixTime's second, in 1/256 s) |
| 2 | JOIN | follower to leader | 8: u8[8] boardId; 16: u8 propType; 17: u8 propIndex; 18: u16 fwVersion; 20: u16 catalogVersion; 22: u16 capabilities; 24 to 31: reserved |
| 3 | WELCOME | leader to follower | 8: u32 generation; 12: u8 nodeId; 13: reserved; 14: u16 groupMask; 16: u8[8] boardId (echo); 24 to 31: reserved |
| 4 | SYNC_REQ | follower or observer to leader | 8: u64 t1; 16 to 31: reserved |
| 5 | SYNC_RESP | leader to requester | 8: u64 t1 (echo); 16: u64 t2; 24: u64 t3 |
| 6 | CUE | leader to follower | 8: u16 cueId; 10: u8 kind; 11: u8 transition; 12: u64 atUs; 20: u16 effect; 22: u16 transitionMs; 24: u8[8] params |
| 7 | ACK | any | 8: u8 ackedType; 9: u8 status; 10: u16 ackedSeq; 12: u16 cueId; 14: reserved; 16: i32 lateByUs; 20 to 31: reserved |
| 8 | STATUS | follower to leader | 8: u32 generation; 12: u16 cueId; 14: u16 effect; 16: u16 syncDelayUs; 18: u16 syncAgeMs; 20: u8 fps; 21: i8 rssi; 22: u16 missedFrames; 24: u16 freeRamKB; 26: u32 uptimeS; 30 to 31: reserved |
| 9 | QUERY | observer to leader | 8: u8 what (1 = roster); 9 to 31: reserved |
| 10 | ROSTER_ENTRY | leader to observer | 8: u8 nodeId; 9: u8 propType; 10: u8 propIndex; 11: u8 state; 12: u16 lastHeardMs; 14: u16 effect; 16: u16 cueId; 18: u16 syncDelayUs; 20: i8 rssi; 21: u8 fps; 22: u16 missedFrames; 24: u32 ipv4; 28: u8 entryIndex; 29: u8 entryCount; 30 to 31: reserved |
| 11 | OVERRIDE | observer to leader | 8: u16 targetGroupMask; 10: u8 kind; 11: u8 transition; 12: u16 delayMs; 14: u16 effect; 16: u16 transitionMs; 18: u8[8] params; 26 to 31: reserved |

Enumerations: CUE `kind` is 1 effect, 2 brightness (target in params[0], ramp over transitionMs),
3 blackout, 4 resume local playlist. `transition` is 0 cut, 1 fade through black, 2 crossfade.
ACK `status` is 0 ok, 1 unknown effect, 2 late, 3 stale generation. Roster `state` is 1 joining,
2 synced, 3 stale, 4 lost.

There's no authentication: WPA2 on the show network is the fence. If that ever matters, a
truncated HMAC fits in the reserved bytes of the packets that need it.

## 8. Testing it in the basement

The plan is to test in layers, so each problem shows up where it is cheapest to find.

### Stage 0: laptop only, no hardware

- The protocol core (packet encode/decode, sync math and line fit, cue scheduling, roster rules)
  is written as plain C++ with no pico-sdk dependency, so it compiles and runs unit tests on the
  laptop. The Pico-specific glue (lwIP, cyw43, multicore) is a thin layer on top.
- A Python implementation of the same wire format (also used by the laptop tool), plus a fake
  leader and fake followers that run as separate processes on the laptop. That exercises
  discovery, joins, cues, retries, and leader reboots before any Pico is involved. A "drop X% of
  packets" option on the fakes tests retry logic deliberately.

### Stage 1: two or three Picos on the bench, measuring sync for real

The show router on a shelf, two or three Pico 2 Ws (or custom boards) on USB power. Firmware does
nothing but discovery and sync, and **pulses a GPIO pin at every whole second of show time**.

The measurement is how far apart those pulses land. Two ways to see it:

- **A cheap logic analyzer** (the ~$15 8-channel 24 MHz kind, with PulseView/sigrok): one channel
  per Pico, common ground, and read the skew directly to a fraction of a microsecond.
- **A judge Pico**, if you'd rather use parts on hand: one more Pico (no WiFi needed) with each
  prop's pulse pin wired to one of its inputs and a common ground. It timestamps every edge with
  `time_us_64()` and prints the spread per second over USB serial. A few lines of code, 1 µs
  resolution.

Judge wiring (one row per prop; add GP5, GP6, ... for more):

| Prop pin | Through | Judge pin |
|---|---|---|
| Prop A GP22 (pin 29) | 1 kΩ resistor | GP2 (pin 4) |
| Prop B GP22 (pin 29) | 1 kΩ resistor | GP3 (pin 5) |
| Prop C GP22 (pin 29) | 1 kΩ resistor | GP4 (pin 6) |
| every prop's GND (pin 28) | wire | judge GND (pin 3 or 8) |

GP22 is free on the Pico 2 W header (the radio uses GP23 to GP25 and GP29); on a
universal-strip-controller board, use any spare GPIO. The resistors only protect against a pin
accidentally set as an output on both ends; the 1 ms, once-a-second pulse doesn't care about them.
Don't join 3V3 or VSYS between boards; each runs from its own USB power. Rather than per-pin
interrupts, the judge should spin in a tight loop on `gpio_get_all()` and timestamp every change,
so simultaneous edges don't queue behind each other. The judge prints, once a second, each prop's
edge time relative to the first one.

Then make it hard:

- Load the network: stream video on the laptop over the same router.
- Distance and walls: carry one Pico to the far end of the basement, or upstairs.
- Leave a phone joined to the show network (this checks that power-save buffering of broadcasts
  doesn't hurt anything except discovery speed).
- Run it overnight with the laptop logging sync quality, to see drift handling over hours.

Pass bar: 99th-percentile skew under 1 ms, worst case under one frame.

### Stage 2: cues with LEDs

Each bench Pico drives a short strip (a meter of last year's pixels is plenty). The leader's
director cycles scenes every 5 seconds: everyone cuts to the same color, then a brightness ramp,
then a chase. If the cuts look like one cut to your eye on video, that's the real test. A phone's
240 fps slow-motion video is a nice cross-check at about 4 ms resolution.

### Stage 3: break things on purpose

Pull a follower's power mid-effect, reboot the leader, switch the router off for a minute, turn on
packet dropping in the firmware. Each should match section 6: followers keep playing, fall back to
their local playlist, and rejoin cleanly, all visible in the laptop view.

Election drills, with two or three boards running `sync_bench_candidate.uf2`: power them on
together (exactly one should lead within about 5 s); pull the leader's power (another takes over
in about 9 s, and the GPIO pulses on the rest stay lined up through it); plug it back in (the
highest-ranked board takes the show back, again without the pulses stepping). The host simulator
runs the same drills on every build (`testElection*` in `tests/test_sim.cpp`).

### Stage 4: the lawn

The same router and settings go outside. The one new variable is range; check RSSI per prop in the
laptop view before trusting it.

## 9. Talking to the leader from the laptop

The laptop is just another node on the show network (join the show SSID). It speaks the same
32-byte protocol through a Python package in this repo (`tools/picocomm/`, reusing the wire format
code from stage 0), so there's one protocol to maintain instead of a separate HTTP API on the
leader.

- **Finding the leader**: listen on UDP 7420 for the BEACON.
- **`picocomm watch`**: sends QUERY once a second and redraws a live table: every prop, its state,
  what it's playing, sync round trip, RSSI, frame rate, missed frames, last heard.
- **`picocomm clock`**: the laptop runs the same sync exchange against the leader, so it knows
  show time too. That lets it timestamp everything it logs in show time, and plot per-prop sync
  quality over a night.
- **`picocomm cue --group arches --effect leap --in 500ms`**: sends an OVERRIDE; the leader turns
  it into ordinary cues for the targets, so overrides are scheduled, retried, and logged like
  everything else.
- **`picocomm log`**: records all of the above to CSV for later plotting.

If a browser page turns out to be nicer than a terminal table, a small FastAPI page on the laptop
(the same setup as the OfficeSign laptop app) can sit on top of the same package later. Detailed
logs from the leader itself stay on USB serial, which is easier to read on the bench than anything
squeezed into 32 bytes.

## 10. Inside each Pico

```
core 0: animation                          core 1: network
---------------------------------          -----------------------------------
Animator / Canvas / Renderer               cyw43 + lwIP (poll mode, tight loop)
frames start on show-time boundaries       discovery, JOIN, sync bursts, STATUS
each frame: pop cues that are due    <---  cue queue (pico queue_t, 4 deep)
read show clock                      <---  show clock (offset + drift, seqlock)
publish fps, missed frames, effect   --->  atomics, sent in STATUS
                                           leader only: director, roster, observer
```

- Core 1 owns all networking, like OfficeSign's BLE server: `cyw43_arch_init()` runs on core 1,
  so its interrupts land there.
- **Keep OfficeSign's startup ordering.** The cyw43 driver claims a specific PIO state machine,
  and the LED renderer claims "any free" one, so core 0 must wait for core 1 to finish
  `cyw43_arch_init()` before `canvas.setup()`. OfficeSign's `BtReadySignal` solves exactly this;
  PicoComm needs the same handshake.
- Cues go through a **queue**, not OfficeSign's latest-wins `Mailbox`: a scene change can carry a
  brightness cue and an effect cue for the same instant, and neither may be dropped. pico-sdk's
  `queue_t` is already safe across cores.
- The show clock is two numbers (offset and drift) that core 1 updates a few times a second and
  core 0 reads every frame. A sequence-counter ("seqlock") read never blocks core 0.

### Memory

Expected cost on top of the show: lwIP and the cyw43 driver need tens of kilobytes of RAM with lean
settings (the existing `wifi_rtc_sync` lwipopts are a reasonable start; the TCP buffers can shrink
or go entirely since PicoComm is UDP only), and the WiFi firmware blob lives in flash, not RAM. The
protocol itself needs a few hundred bytes per follower on the leader and almost nothing on a
follower. The RP2350 has 520 KB of SRAM, and the 32 x 200 MegaTree double-buffered fits with room
to spare, so this should fit. The bench firmware will print real numbers from the linker map and
the heap, rather than trusting this estimate.

## 11. Suggested build order

1. Wire format in C++ and Python, with round-trip tests between the two.
2. Fake leader and followers on the laptop (stage 0).
3. Pico firmware: discovery and sync only, with the GPIO pulse; measure (stage 1).
4. Cues and the core 0 side, including show-time frame pacing in `Animator` (stage 2). The
   `Animator` change belongs in PicoLEDs.
5. `picocomm watch` and `picocomm cue`.
6. Failure drills (stage 3), then the lawn.

## 12. Open questions

- Which prop is the leader? Settled: elected (section 5). Every prop with the show script enters
  with its LED count, so the MegaTree leads when it's up.
- Do all props run the same frame rate? Frame-phase alignment (section 4) assumes a common rate, or
  rates that divide evenly (20/40/80 fps).
- Should the director's show script be compiled into the leader's firmware, or loaded from the
  laptop? Compiled in is simpler and needs no laptop on show nights; it's the suggested start.
- Lead time: 300 ms is the starting point. Calm, slow Christmas effects can tolerate a longer one,
  which buys more retries.
