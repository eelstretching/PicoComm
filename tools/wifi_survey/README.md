# wifi_survey

A Pico 2 W firmware for walking the lawn and checking the show network's signal at each prop. Keep
the Pico on a USB cable from a laptop and read its serial output.

Once a second it prints:

- **RSSI** of the access point it's joined to, with a label: excellent (-60 dBm or better), good
  (-70 or better), marginal (-80 or better), POOR below that. Aim for good or better at every prop.
- **min and average RSSI over the last 10 seconds**, since single readings bounce around.
- **pings to the gateway** (the show router): four ICMP echoes a second with a 32-byte payload (one
  PicoComm packet), reported as ok/sent and min/avg/max round trip in ms. PicoComm's time sync uses
  the fastest round trips, so watch the min and the losses more than the max.

At startup it scans and lists every network it can hear, marks the show network with `>`, and says
how many other networks share overlapping channels.

**Press Enter** at a prop to print a numbered marker with a summary of the last 10 seconds. That
makes a saved log of the walk easy to read back.

WiFi power saving is off (`CYW43_NONE_PM`), as PicoComm will run it, so ping times reflect the
network and not the radio napping between beacons. If the link drops, it says so and reconnects.

## Using it

1. Hold BOOTSEL, plug the Pico 2 W in, and copy `wifi_survey.uf2` onto the `RP2350` drive.
2. Open the serial port, e.g. on macOS: `screen /dev/tty.usbmodem* 115200` (quit with Ctrl-A then
   K). To keep a log as well: `screen -L -Logfile survey.log /dev/tty.usbmodem* 115200`.
3. It waits for the serial port to be opened, then asks for the SSID and password (unless they were
   compiled in), scans, connects, and starts printing.

Ping round trips to a consumer router measure the router's ICMP handling as well as the air, so
treat them as an upper bound on what Pico-to-Pico UDP will see.

## Building

Standalone project, pico-sdk 2.3.0, board fixed to `pico2_w`:

```sh
PICO_SDK_PATH=/path/to/pico-sdk cmake -S tools/wifi_survey -B build-survey -G Ninja
ninja -C build-survey        # -> build-survey/wifi_survey.uf2
```

To skip typing the network at startup, add `-DWIFI_SSID=... -DWIFI_PASSWORD=...` to the cmake line.
