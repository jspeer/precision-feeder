# Precision Feeder for ESPHome

ESPHome firmware for the [Automated Precision Fish Feeder with WiFi (Rev2)](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2)
print. It does everything the original Arduino firmware does, then adds
minute-accurate on-board scheduling with sunrise/sunset events, three
switchable schedule profiles, a feed guard that protects the fish from
over-feeding, and native Home Assistant control.

> **Status: early development.** The firmware compiles but has **not yet been
> run on real hardware**. Expect rough edges, and do not rely on it as the only
> way your animals get fed until you have tested it on your own build.

> **Written with AI.** This project was designed and written with the assistance
> of an AI (Anthropic's Claude). A human reviewed and directed the work, but the
> code and documentation may contain mistakes. Test on your own hardware.

## How the feeder works

The tray has 7 positions (0 to 6). Position 0 is the closed slot; positions 1 to
6 each hold one portion. Each feed advances the tray one position, so **a full
tray is 6 portions**, then you press *Tray Reload* (which closes the tray) and
load it again. The position is
tracked without any sensor, so the firmware is careful about power loss: if a
move is interrupted the position is marked *unknown* and feeding stops until you
say where the tray is.

## Features

- **On-board scheduler.** Feeding continues with no Home Assistant and no network
  once the clock is set.
- **Three profiles (A, B, C)**, each with four events. An event is a clock time,
  or an offset from sunrise or sunset ("sunset + 3 h"). Each event has its own
  portions and weekdays; each profile has an "every N days" cadence and
  no-feed "fast days".
- **No late feeds.** A missed event (offline, held, reloading) is skipped, never
  made up afterwards, so the fish are never double-fed to catch up.
- **One-shot feeds** for a specific date and time, and a **hold** to pause the
  schedule for any length of time, or until you cancel it (for example while you
  are away).
- **Feed guard** applied to every request from every source: minimum gap
  between feeds, a rolling 24-hour portion cap, a per-command portion limit,
  and refusal when the tray is empty, unknown or being calibrated.
- **Runway.** The firmware knows exactly how many portions are left and tells you
  when the tray will run out under the active schedule.
- **Calibration** of each tray position from Home Assistant or the web UI.
- **Optional push button** (as in the original), **web dashboard**, and
  **status LED**.
- Time from NTP (configurable servers), optionally also from Home Assistant.

Not yet implemented: the BLE control interface described in
[DESIGN.md](DESIGN.md).

## Hardware

- Waveshare ESP32-S3-Zero (the 4 MB flash / 2 MB PSRAM variant), or another
  ESP32-S3 board with pin changes
- MG90S servo (or an SG92R)
- Optional push button between a GPIO and GND

Default pins (all are YAML substitutions): servo `GPIO1`, button `GPIO2`, status
LED `GPIO21` (onboard WS2812). Power the servo from a proper 5 V supply, not the
board's 3.3 V regulator, and add a bulk capacitor (about 470 to 1000 µF) close
to the servo. If the servo twitches at power-up, add a 10 kΩ resistor from the
signal line to GND.

## Quick start

1. Install [ESPHome](https://esphome.io) and copy this repository.
2. Copy `examples/secrets.example.yaml` to `examples/secrets.yaml` and fill it in.
3. Copy `examples/basic.yaml`, set `timezone`, and comment packages in or out.
4. First flash over USB-C (hold BOOT while plugging in the board), then
   `esphome run basic.yaml`. Later updates go over WiFi.
5. Calibrate the tray ([docs/CALIBRATION.md](docs/CALIBRATION.md)).
6. Press **Tray Reload** first (it closes the tray so food cannot fall out), then
   fill the tray. Set your **Latitude/Longitude**
   if you use sunrise/sunset events, and set up a profile in the schedule editor.

## Repository layout

```
components/precision_feeder/   the logic: tray, guard, scheduler, solar maths (C++)
packages/                      reusable YAML: core, status, controls, editor, ...
examples/                      a ready-to-edit configuration
tests/                         host-side tests for the pure logic
docs/                          calibration walkthrough
DESIGN.md                      design, decisions and open questions
AGENTS.md                      guidance for AI agents and contributors
```

The logic lives in a component because it has safety consequences and needs to
be testable. The YAML packages are thin glue: entities for Home Assistant and
the web UI, so you can switch features on and off by including or commenting
out a package.

## Limits to know about

- **Timezone is set at build time** (the `timezone` substitution), or taken from
  Home Assistant when the HA time package is enabled.
- **After a power loss with no network**, scheduled feeding waits until the clock
  is set again. Manual feeds still work. There is no battery-backed clock.
- The 24-hour portion cap is remembered across reboots only if the clock was set
  when the feed happened.

## Credits and licence

MIT licensed (see [LICENSE](LICENSE)). This is an independent, clean-room
implementation. The hardware is the Printables model linked above, and the
original firmware by FlyingGyroscope
([PrecisionFeederRev2](https://github.com/FlyingGyroscope/PrecisionFeederRev2),
AGPL-3.0) was read only to learn how the mechanism behaves; none of its code is
used here.
