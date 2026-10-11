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
