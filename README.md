# PicoComm

WiFi coordination for the Christmas show's props: 4 to 8 Picos (MegaTree with topper, arches,
small trees), each computing its own animations, agreeing on the time to well under a millisecond
so that "everybody switch to Starlight at show time T" lands on the same frame everywhere.

- [`prompt.md`](prompt.md): the original brief.
- [`docs/protocol-design.md`](docs/protocol-design.md): the design. This code implements it.

## Layout

| Path | What |
|---|---|
| `include/picocomm/`, `src/` | The protocol core: wire format, clock sync, `Leader` and `Follower`. Plain C++17, no pico-sdk, so it builds and is tested on a laptop. |
| `include/picocomm/effects.h` | The shared effect catalog (effect IDs and what their parameters mean) and prop types. |
| `pico/` | The Pico glue: cyw43 + lwIP on core 1, show clock and cue queue for core 0 (`picocomm::pico::Node`). |
| `firmware/` | Bench firmware for the basement tests: `sync_bench_leader`, `sync_bench_follower`, and the `judge`. See [`firmware/README.md`](firmware/README.md). |
| `tests/` | Host tests, including a whole-protocol simulation over a lossy network. |
| `tools/picocomm/` | The laptop tool (`picocomm watch`, `cue`, `clock`, `log`, and fake Picos). See [`tools/picocomm/README.md`](tools/picocomm/README.md). |
| `tools/wifi_survey/` | Signal-strength survey firmware for walking the lawn. |

## Building and testing

Host tests (any laptop with CMake and a C++17 compiler):

```sh
cmake -S . -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Python tests (Python 3.10 or newer, no packages needed):

```sh
cd tools/picocomm
python3 -m unittest discover -s tests -t .
```

Pico firmware (pico-sdk 2.3.0, Pico 2 W):

```sh
PICO_SDK_PATH=/path/to/pico-sdk cmake -S firmware -B build -G Ninja
ninja -C build        # build/sync_bench_leader.uf2, build/sync_bench_follower.uf2, build/judge.uf2
```

## Using it in a prop's firmware

Add PicoComm's `pico/` directory to the prop's CMake project and link `picocomm_pico`:

```cmake
add_subdirectory(path/to/PicoComm/pico picocomm)
target_link_libraries(MegaTree PRIVATE picocomm_pico)
```

Then, on core 0 (see the comment at the top of `pico/include/picocomm/pico/node.h`): make a
`picocomm::pico::Node`, `start()` it, `waitForRadio()` **before** `canvas.setup()` (cyw43 has to
claim its PIO state machine before the LED renderer takes "any free" one), and each frame read
`showTime()` and drain `popCue()`. The leader's director calls `node.cue(groups, spec, leadMs)`.

## Choices made in v1 that the design left open

- **Followers listen on port 7420 too.** Beacons are broadcast to port 7420, so every Pico binds
  it; all Pico-to-Pico traffic uses 7420 at both ends. The laptop uses a separate port for its own
  requests.
- **Roster row 0 is the leader itself**, so `picocomm watch` shows every prop, leader included.
  ROSTER_ENTRY's `ipv4` has the first octet in the lowest byte (lwIP's own order).
- **Rows can arrive out of order**; the observer sorts them by `entryIndex`.
- **OVERRIDE resends are spotted by sequence number**, so a resend after a lost ACK doesn't cue
  twice.
- **A follower ignores cues until its clock is locked** (no ACK, so the leader resends). Locking
  takes one sync burst, which starts as soon as WELCOME arrives.
- **Catch-up cues** (the current scene, resent to a follower when it joins) are not counted as
  "late" in the leader's log, since they usually are.
- **Orphaning delivers a RESUME_LOCAL cue** to core 0 right away; core 0 decides whether to finish
  its current effect first. An orphaned follower forgets the cue IDs it has seen, so the scene cue
  it gets on rejoining plays again.
- **Clock discipline**: the first lock (and the first after a leader reboot) steps; after that
  corrections are slewed at up to 4000 ppm (100 us per 25 ms frame). A follower more than 100 ms
  off steps instead, since slewing that far would take over 25 s.
- **The drift fit is weighted**: each burst's best sample counts less the slower its round trip
  was than the fastest in the window, and samples over twice the fastest are dropped.
- **Limits**: 16 followers per leader, 4 unacknowledged cues per follower, an 8-deep cue queue to
  core 0 (the design said 4; a scene change can carry several cues). The leader stops resending a
  cue 2 s after its time.

Not done yet: show-time frame pacing in PicoLEDs' `Animator` (section 4, "Frame phase"; that
change belongs in PicoLEDs), reading the DS3231 into `Node::setWallClock()` on the leader, and the
director's show script.
