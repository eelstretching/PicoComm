# Bench firmware

For stages 1 and 2 of the basement tests in [`docs/protocol-design.md`](../docs/protocol-design.md)
(section 8). Built for the Pico 2 W with pico-sdk 2.3.0:

```sh
PICO_SDK_PATH=/path/to/pico-sdk cmake -S firmware -B build -G Ninja
ninja -C build
```

To bake the show network into the images instead of typing it in at startup, add
`-DWIFI_SSID=... -DWIFI_PASSWORD=...` to the `cmake` line.

## sync_bench_leader.uf2 and sync_bench_follower.uf2

Flash one board with the leader image and the others with the follower image (hold BOOTSEL, plug
in, copy the `.uf2` onto the `RP2350` drive). Open each one's serial port, e.g. on macOS
`screen /dev/tty.usbmodem* 115200`. Unless the network was compiled in, each asks for the SSID and
password first.

Every board then:

- pulses **GP22** (pin 29) high for 1 ms at every whole second of show time;
- pulses **GP21** (pin 27) high for 1 ms whenever a cue takes effect;
- prints protocol events (joined, clock locked, late cues, orphaned, ...) and a status line every
  10 s.

The leader cues a test scene to every board every 5 seconds, so there's a GP21 pulse every 5 s on
every board, all at the same show time.

Measure how far apart the GP22 pulses land with a logic analyzer, or with the judge below. The
design's pass bar is a 99th-percentile spread under 1 ms. GP21 measures cue alignment the same way.

## sync_bench_candidate.uf2

The same bench, but the board enters the leader election (design doc section 5) with priority
100 instead of being a fixed leader or follower. Flash two or three boards with it (no leader image
needed): one of them starts leading a few seconds after power-on and runs the test scene. Pull its
power and another takes over about 9 s later; plug it back in and the higher-ranked board (same
priority, so the higher IP address) takes the show back. The serial log says who leads and why,
and the GP22 pulses should stay lined up through every handover.

## judge.uf2

A plain Pico 2 or Pico 2 W (it doesn't use WiFi) that timestamps the other boards' pulses. Wire
each board's GP22 through a 1 kOhm resistor to the judge's GP2, GP3, GP4, ... (boards A, B, C,
...), and join all the grounds; the table is in the design doc. Open its serial port and it prints,
once a second, each board's edge relative to the earliest:

```
spread    37 us |  A +0  B +37  C +12
```

and every 10 rounds a summary: mean and worst spread, and how many rounds fell under 100 us,
500 us, 1 ms, or over.

The image is built for the Pico 2 W; it runs on a plain Pico 2 as well. For an original RP2040
Pico, configure a separate build directory with `-DPICO_BOARD=pico`; for a board without a radio
only the judge is built.

## led_bench_leader.uf2 and led_bench_follower.uf2

Stage 2: the same idea with LEDs, drawn by a `picocomm::Conductor`. Wire 100 pixels (WS2811) to
**GP2** on each board, with the usual level shifter and a shared ground; set up the network the
same way as the sync bench.

Before a follower finds the leader it plays its local playlist (Starlight). Once it's in, the
leader runs the show for everyone: Starlight for 20 s, warm white for 5 s, then four 100 ms
flashes a second apart (red, green, blue, white), and round again. Put the strips side by side, or
film them in slow motion: the flashes should land on the same frame (25 ms at 40 fps) everywhere.
Each board prints the cues it plays and a status line every 10 s, including missed frames.
