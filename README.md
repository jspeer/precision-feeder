# Precision Feeder for ESPHome

ESPHome firmware for the
[Automated Precision Fish Feeder with WiFi (Rev2)](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2),
a 3D-printed fish feeder that drops one measured portion at a time from a servo-driven tray.

It started as firmware for that one print. Since 2.1.0 it also drives custom prints: what moves the food is a
*mechanism*, and one firmware carries two of them, chosen on the device:

- **A servo tray:** a 180-degree hobby servo (MG90S or SG92R) swinging a fan of slots, as in the original feeder.
- **A direct-drive disc:** a small gear motor with an encoder (GA12-N20) behind a DRV8833 motor driver, turning a
  printed disc of slots (3 to 60 of them) one slot per portion, with a calibration wizard in the web app.

Another motor or tray can be added by following [docs/MECHANISMS.md](docs/MECHANISMS.md).

- **The feeder to print:** <https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2>
  (by FlyingGyroscope; the model is CC BY-NC-SA 4.0, see [Licence](#licence))
- **This firmware:** <https://github.com/jspeer/precision-feeder>
- **Install it from your browser:** <https://jspeer.github.io/precision-feeder/>

**Local first.** The feeder is a complete appliance on its own. The schedule lives on the device, and its built-in web
app runs the whole feeder from any browser, on a computer or a phone: no Home Assistant, no internet, no app to
install, no account. Open `precision-feeder-xxxxxx.local` (or its IP address) on your own network, over HTTPS, and you have
the dashboard, the schedule, the settings, calibration and diagnostics. The app is stored in the firmware; nothing is
loaded from the internet.

Everything else is optional, on top: full **Home Assistant** control (the native API, or **MQTT** with discovery), and
a **Bluetooth (BLE) interface** for a PLC or another controller.

It does everything the [original Arduino firmware](https://github.com/FlyingGyroscope/PrecisionFeederRev2) does, then
adds minute-accurate scheduling with sunrise and sunset events, three switchable schedule profiles, and a feed guard
that protects the fish from being over-fed.

> **Status: release 2.3.1.** One complete servo feeder (Waveshare ESP32-S3-Zero, MG90S servo) has
> been calibrated and tested on real hardware: feeding, schedules, power cuts, Home Assistant, BLE, and for 2.0 the
> install page, the setup wizard, firmware updates, NTP servers and upgrades from 1.1. For 2.1 the direct-drive disc
> has been calibrated and fed on a bench rig (with BLE and MQTT), and the two-drive firmware switched drives on both
> rigs. 2.2.0's release images were run on both rigs, including the servo's upgrade from 2.1.1; 2.3.0 was run on
> the disc rig ([docs/BENCH.md](docs/BENCH.md)); 2.3.1's fixes are logic only, checked with host tests. 2.4.0 (Bluetooth on NimBLE), 2.4.1 (HTTPS and MQTT over
> TLS), 2.4.2 (ACME), 2.4.3 (their web app pages and guides) and 2.4.4 (fixes from their first bench run) are not tagged; HTTPS, MQTT over TLS, ACME
> and Bluetooth on NimBLE have been run on the disc rig. The disc has not yet fed animals, and neither has had months of daily use. Watch it for a few days, and do not rely on it as the only way your animals get
> fed.

> **Written with AI.** This project was designed and written with the assistance of an AI (Anthropic's Claude). A
> human directed the work and tested it on real hardware, but the code and documentation may contain mistakes.

## How the feeder works

The tray is a fan of 7 slots, 25 degrees apart, turned by a hobby servo over a base with a feeding hole. **Slot 0 is
closed** (it has a floor); **slots 1 to 6 each hold one portion.** Each feed turns the tray one slot further, so the
next portion falls through the hole. A full tray is therefore **6 portions**. When it is empty you press **Tray
Reload**, which turns the tray back to the closed slot, and refill it.

**The direct-drive disc** works the same way with more slots: one slot is always over the hole, so a disc of N slots
holds N - 1 portions (14 on a 15-slot disc). It only ever turns forward, a slot per portion; Tray Reload marks it
full without moving it. The motor's encoder places each slot, but the firmware still treats a move that was cut short
as unknown.

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
- **Two drives, one firmware:** the servo tray or the direct-drive disc, chosen in the web app, with every pin
  (motor, encoder, button, LED) chosen there too.
- **Calibration** in the web app or Home Assistant: the servo's pulse range and a fine offset for each slot, or the
  disc's step-by-step wizard (direction, one full turn, slot count, trim).
- **Time from NTP** (the public pool, or your own servers, set on the feeder), optionally also from Home Assistant,
  or pushed over BLE when there is no network. Shows where the clock came from and how old it is.
- **An appliance** (2.0): install from a web page, a setup wizard, Wi-Fi, name, time zone, pins and keys set in the
  web app, and firmware updates from the web app, checked before they are applied.
- **Who can change what** (2.2.0): Home Assistant gets the everyday controls and status; the settings, calibration
  and pins stay in the web app, behind the settings password; MQTT can also edit the schedule; BLE is the headless
  channel ([docs/SECURITY.md](docs/SECURITY.md)).
- **HTTPS** (2.4): the web app over HTTPS with a certificate the feeder makes itself, one you upload, or one from your
  own ACME server (step-ca, say), renewed by itself; MQTT over TLS, checked against your CA or pinned on first
  contact ([docs/HTTPS.md](docs/HTTPS.md)).
- **Optional:** push button (as in the original), status LED, MQTT with Home Assistant discovery and the schedule on its
  own topics (broker and login set in the web app; [docs/MQTT.md](docs/MQTT.md)), BLE interface (every write authenticated with a key;
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

For the direct-drive disc, instead of the servo and its capacitor:

| Part | Notes |
|---|---|
| GA12-N20 gear motor with a hall encoder | the bench one is 6 V, 35 rpm; another speed works after calibration |
| DRV8833 motor driver board | one of its two channels is used |
| Capacitor, about 100 µF | across the driver's 5 V supply |
| Your printed disc | any number of slots from 3 to 60 |

**Wiring** (defaults for the S3-Zero; pins can be changed in the web app or your YAML):

| From | To |
|---|---|
| Servo signal | GPIO1 |
| Servo + | 5 V (**not** the board's 3.3 V regulator) |
| Servo − | GND |
| Button (optional) | GPIO2 and GND (no resistor needed) |
| Status LED | built into the S3-Zero (GPIO21) |

Direct-drive disc: motor M1 and M2 to the DRV8833's AO1 and AO2; AIN1 to GPIO1, AIN2 to GPIO2, STBY to GPIO6; encoder
C1 to GPIO4, C2 to GPIO5, encoder VCC to 3.3 V; the driver's VM to 5 V; all grounds together. Put the button on GPIO3
(GPIO2 is AIN2), or choose its pin in the web app.

If the servo twitches when the board powers up, add a 10 kΩ resistor from the signal wire to GND. (It did not on the
bench build.)

## Getting started

There are two ways to run it:

1. **Prebuilt firmware (most people).** Flash a release from
   **<https://jspeer.github.io/precision-feeder/>** in Chrome or Edge, then follow the setup wizard. Nothing to
   build. Guide: [docs/INSTALL.md](docs/INSTALL.md).
2. **Build your own from YAML (advanced).** ESPHome 2026.10.0 and the packages in this repository, for full control of
   what is compiled in. Guide: [docs/BUILDING.md](docs/BUILDING.md).

Then, either way:

1. **Choose the drive** if yours is the direct-drive disc: Configuration, Drive, then restart. The prebuilt firmware
   starts as a servo feeder.
2. **Calibrate** the tray so every slot lines up with the hole: [docs/CALIBRATION.md](docs/CALIBRATION.md).
3. **Load it:** press **Tray Reload** (it closes the servo tray so food cannot fall out), then fill the slots.
4. **Set a schedule:** [docs/SCHEDULING.md](docs/SCHEDULING.md). For sunrise or sunset events, set the location
   first.
5. **Watch it for a few days** before you trust it while you are away.

## Documentation

| Document | For |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | installing the prebuilt firmware, the setup wizard, updates and recovery |
| [docs/BUILDING.md](docs/BUILDING.md) | building your own from YAML: packages, settings, flashing |
| [docs/USING.md](docs/USING.md) | everyday use, the settings, safety behaviour, troubleshooting and limits |
| [docs/WEB_UI.md](docs/WEB_UI.md) | the built-in web app, page by page |
| [docs/SCHEDULING.md](docs/SCHEDULING.md) | building schedule profiles |
| [docs/CALIBRATION.md](docs/CALIBRATION.md) | lining up the tray |
| [docs/HTTPS.md](docs/HTTPS.md) | HTTPS, certificates (your own, or from your ACME server), the trust store, MQTT over TLS |
| [docs/SECURITY.md](docs/SECURITY.md) | the security model: who can change what, and the limits |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the BLE protocol, for controller and app developers |
| [docs/MQTT.md](docs/MQTT.md) | the MQTT topics: what MQTT reaches, and the schedule's own topics |
| [docs/MECHANISMS.md](docs/MECHANISMS.md) | adapting the firmware to another motor or tray (a mechanism) |
| [DESIGN.md](DESIGN.md) | the design and the decisions behind it |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each firmware version |
| [docs/BENCH.md](docs/BENCH.md) | what has been tested on real hardware |
| [AGENTS.md](AGENTS.md) | rules for contributors (human or AI); run `tools/check.sh` before sending changes |

### Repository layout

```
components/precision_feeder/      the logic (tray, guard, scheduler, sun maths, settings), plain C++ and tested
                                  on a PC, plus the ESPHome adapter
components/precision_feeder_n20/  the direct-drive disc's motor driver (only compiled when its package is used)
components/precision_feeder_ble/  the BLE protocol, on ESP-IDF's NimBLE (only compiled when the BLE package is used)
components/precision_feeder_mqtt/ MQTT's own topics for the schedule (only compiled with the MQTT package)
components/precision_feeder_tls/  HTTPS, the trust store, MQTT over TLS and ACME (only compiled with their packages)
packages/                         the feature packages (docs/BUILDING.md)
examples/                         basic.yaml, ble.yaml, disc.yaml, secrets.example.yaml
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

[MIT](LICENSE), copyright (c) 2026 Jeremy Speer: use it, change it and share it, also commercially, as long as the
copyright and permission notice in [LICENSE](LICENSE) goes with every copy or substantial portion. This covers this
repository's own files only. (Versions up to 2.1.1 were released into the public domain under The Unlicense.)

The firmware is built with [ESPHome](https://esphome.io), whose C++ runtime is GPLv3 (its Python code is MIT). The
prebuilt firmware images contain that runtime, so an image as a whole is distributed under the GPLv3's terms; its
source is this repository and ESPHome's.

The feeder itself, the [Printables model](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2)
by FlyingGyroscope, is licensed separately under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/): credit the author, no commercial use, and
share remixes under the same licence. This repository contains none of its files (no STL, 3MF or images); if you
print, remix or sell anything made from the model, its licence applies to that, not this one.
