# Precision Feeder for ESPHome

ESPHome firmware for the
[Automated Precision Fish Feeder with WiFi (Rev2)](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2),
a 3D-printed fish feeder that drops one measured portion at a time from a servo-driven tray.

- **The feeder to print:** <https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2>
  (by FlyingGyroscope; the model is CC BY-NC-SA 4.0, see [Licence](#licence))
- **This firmware:** <https://github.com/jspeer/precision-feeder>
- **Install it from your browser:** <https://jspeer.github.io/precision-feeder/>

**Local first.** The feeder is a complete appliance on its own. The schedule lives on the device, and its built-in web
app runs the whole feeder from any browser, on a computer or a phone: no Home Assistant, no internet, no app to
install, no account. Open `http://precision-feeder-xxxxxx.local` (or its IP address) on your own network and you have
the dashboard, the schedule, the settings, calibration and diagnostics. The app is stored in the firmware; nothing is
loaded from the internet.

Everything else is optional, on top: full **Home Assistant** control, and a **Bluetooth (BLE) interface** for a PLC
or another controller.

It does everything the [original Arduino firmware](https://github.com/FlyingGyroscope/PrecisionFeederRev2) does, then
adds minute-accurate scheduling with sunrise and sunset events, three switchable schedule profiles, and a feed guard
that protects the fish from being over-fed.

> **Status: release 2.0.1** (2.0.2 in development). One complete feeder (Waveshare ESP32-S3-Zero, MG90S servo) has
> been calibrated and tested on real hardware: feeding, schedules, power cuts, Home Assistant, BLE, and for 2.0 the
> install page, the setup wizard, firmware updates and upgrades from 1.1 ([docs/BENCH.md](docs/BENCH.md)). It has
> not yet had months of daily use. Watch it for a few days, and do not rely on it as the only way your animals get
> fed.

> **Written with AI.** This project was designed and written with the assistance of an AI (Anthropic's Claude). A
> human directed the work and tested it on real hardware, but the code and documentation may contain mistakes.

## How the feeder works

The tray is a fan of 7 slots, 25 degrees apart, turned by a hobby servo over a base with a feeding hole. **Slot 0 is
closed** (it has a floor); **slots 1 to 6 each hold one portion.** Each feed turns the tray one slot further, so the
next portion falls through the hole. A full tray is therefore **6 portions**. When it is empty you press **Tray
Reload**, which turns the tray back to the closed slot, and refill it.

There is no position sensor: the firmware keeps track of where the tray is. It saves where it is going *before* every
move, so if the power is cut halfway, the position is marked **unknown** on the next boot and feeding stops until you
say where the tray is (or reload it). It never guesses.

> **Use pellets or other hard food. Flake food is not advised.** Hard food drops out of each slot cleanly. Flakes can
> stick to the slots while the tray turns, so portions come out unevenly or not at all.

## Features

- **On-board scheduler.** Three profiles (A, B, C) of four events each. An event is a clock time (`08:30`) or an
  offset from sunrise or sunset (`sunset -00:30`), with its own portions and weekdays. A profile can feed every N
  days and skip "fast days". Switch profiles with one click (for example "normal" and "holiday").
- **No late feeds.** A missed event (power off, on hold, reloading) is skipped, never made up afterwards.
- **Feed guard** on every feed from every source (schedule, web app, Home Assistant, button, BLE): a minimum gap
  between feeds, a rolling 24-hour portion cap, a per-command portion limit, and refusal when the tray is empty, its
  position is unknown, or it is being calibrated.
- **Knows what is left.** Portions remaining is exact. It tells you when the tray will run out, and **Needs Food**
  turns on when the tray is empty or the next feed would come up short, ready for a notification.
- **Hold** the schedule for a number of hours or until you cancel it, and add **one-shot feeds** for a date and time.
- **Calibration** in the web app or Home Assistant: the servo's pulse range and a fine offset for each slot.
- **Time from NTP** (the public pool, or your own servers, set on the feeder), optionally also from Home Assistant,
  or pushed over BLE when there is no network. Shows where the clock came from and how old it is.
- **An appliance** (2.0): install from a web page, a setup wizard, Wi-Fi, name, time zone, pins and keys set in the
  web app, and firmware updates from the web app, checked before they are applied.
- **Optional:** push button (as in the original), status LED, BLE interface (every write authenticated with a key;
  documented byte for byte in [docs/PROTOCOL.md](docs/PROTOCOL.md), with a Python reference client).

## What you need

| Part | Notes |
|---|---|
| The printed feeder | <https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2> |
| An ESP32-S3 board, 4 MB flash or more | the Waveshare ESP32-S3-Zero is the reference; presets also for the ESP32-S3-DevKitC-1, and any pins can be chosen. No PSRAM needed. |
| MG90S servo | or the TowerPro SG92R the original was built with (same size and pins) |
| 5 V supply | for the servo; USB-C or the board's 5 V pad for the board |
| Capacitor, 470 to 1000 µF | across the servo's power, close to the servo |
| Push button (optional) | any normally-open button |

**Wiring** (defaults for the S3-Zero; pins can be changed in the web app or your YAML):

| From | To |
|---|---|
| Servo signal | GPIO1 |
| Servo + | 5 V (**not** the board's 3.3 V regulator) |
| Servo − | GND |
| Button (optional) | GPIO2 and GND (no resistor needed) |
| Status LED | built into the S3-Zero (GPIO21) |

If the servo twitches when the board powers up, add a 10 kΩ resistor from the signal wire to GND. (It did not on the
bench build.)

## Getting started

There are two ways to run it:

1. **Prebuilt firmware (most people).** Flash a release from
   **<https://jspeer.github.io/precision-feeder/>** in Chrome or Edge, then follow the setup wizard. Nothing to
   build. Guide: [docs/INSTALL.md](docs/INSTALL.md).
2. **Build your own from YAML (advanced).** ESPHome 2026.9.1 and the packages in this repository, for full control of
   what is compiled in. Guide: [docs/BUILDING.md](docs/BUILDING.md).

Then, either way:

1. **Calibrate** the tray so every slot lines up with the hole: [docs/CALIBRATION.md](docs/CALIBRATION.md).
2. **Load it:** press **Tray Reload** (it closes the tray so food cannot fall out), then fill slots 1 to 6.
3. **Set a schedule:** [docs/SCHEDULING.md](docs/SCHEDULING.md). For sunrise or sunset events, set the location
   first.
4. **Watch it for a few days** before you trust it while you are away.

## Documentation

| Document | For |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | installing the prebuilt firmware, the setup wizard, updates and recovery |
| [docs/BUILDING.md](docs/BUILDING.md) | building your own from YAML: packages, settings, flashing |
| [docs/USING.md](docs/USING.md) | everyday use, the settings, safety behaviour, troubleshooting and limits |
| [docs/WEB_UI.md](docs/WEB_UI.md) | the built-in web app, page by page |
| [docs/SCHEDULING.md](docs/SCHEDULING.md) | building schedule profiles |
| [docs/CALIBRATION.md](docs/CALIBRATION.md) | lining up the tray |
| [docs/SECURITY.md](docs/SECURITY.md) | the security model: who can change what, and the limits |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the BLE protocol, for controller and app developers |
| [DESIGN.md](DESIGN.md) | the design and the decisions behind it |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each firmware version |
| [docs/BENCH.md](docs/BENCH.md) | what has been tested on real hardware |
| [AGENTS.md](AGENTS.md) | rules for contributors (human or AI); run `tools/check.sh` before sending changes |

### Repository layout

```
components/precision_feeder/      the logic (tray, guard, scheduler, sun maths, settings), plain C++ and tested
                                  on a PC, plus the ESPHome adapter
components/precision_feeder_ble/  the BLE protocol (only compiled when the BLE package is used)
packages/                         the feature packages (docs/BUILDING.md)
examples/                         basic.yaml, ble.yaml, secrets.example.yaml
firmware/                         the prebuilt binaries' configurations (full.yaml, no-ble.yaml)
install/                          the install page, published with each release
web/                              the web app: src/ and the built dist/ the firmware embeds
tests/                            host-side tests: C++ for the logic and the protocol, Python, the web app's parsers
tools/                            check.sh (every check), the Python BLE client, the release tools
docs/                             the guides above
```

The logic is a C++ component rather than YAML because it has safety consequences and needs to be testable; the
packages are thin glue that expose it to Home Assistant and the web app.

## Credits

- Inspired by the Precision Feeder Rev2 by **FlyingGyroscope**: the
  [original firmware](https://github.com/FlyingGyroscope/PrecisionFeederRev2) and the
  [Printables model](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2) this
  firmware drives. This is an independent rewrite; none of the original code is used (it is AGPL-3.0, and was read
  only to learn how the mechanism behaves).
- Designed and written with **Claude** (Anthropic's AI), directed and tested on real hardware by a human.

## Licence

Released into the public domain under [The Unlicense](LICENSE). Do anything you like with it. This covers this
repository's own files only.

The feeder itself, the [Printables model](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2)
by FlyingGyroscope, is licensed separately under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/): credit the author, no commercial use, and
share remixes under the same licence. This repository contains none of its files (no STL, 3MF or images); if you
print, remix or sell anything made from the model, its licence applies to that, not this one.
