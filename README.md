# Precision Feeder for ESPHome

A local fish feeder that drops measured portions on its own schedule. Set it up in a browser, fill the tray, and
let the ESP32 handle the timing. No account, cloud service, phone app or Home Assistant required.

Built for FlyingGyroscope's [Precision Feeder Rev2](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2),
with support for a servo fan tray and a gear-motor-driven disc. Both use the same scheduler, feed guard and web app.

**[Install a published release](https://jspeer.github.io/precision-feeder/)** ·
**[First-time setup](docs/INSTALL.md)** · **[Daily use](docs/USING.md)** · **[Build from source](docs/BUILDING.md)**

> **Validation status:** the current audit fixes have passed host tests and compilation; physical testing is
> pending. Tested behavior and outstanding checks are recorded in [BENCH.md](docs/BENCH.md) and
> [DESIGN.md](DESIGN.md#14-open-items). Neither mechanism has months of unattended daily use behind it.

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Designed and written with the assistance
of an AI (Anthropic's Claude), directed and tested on real hardware by a human. The code and documentation may
contain mistakes; test your own feeder before relying on it.*

## Why all this for feeding a fish?

Feeding fish by hand is easy. This project is a fairly elaborate answer to a very small problem.

I bought a cheap feeder from overseas. It worked fine. Then I broke it. Looking at how simple it was, I realized
that I wanted something more interesting. Commercial options tended to mean a proprietary app, somebody else's
custom solution, or no useful way to monitor what the feeder was doing. I wanted something with personality that
I could make my own.

Once it worked, the question became: how far could I push this little thing?

Some 32,000 lines of code later, the fish still just want dinner. Each connection had a reason:
ESPHome made Home Assistant integration straightforward. Optional BLE came from
the idea of a custom PLC controlling a fish tank. MQTT opened it up to smart-home systems beyond Home Assistant.
TLS and ACME came from wanting that certificate support on my own device.

There was another experiment running alongside the feeder: how far could AI go writing the software? How far could
I take this “vibe coding” thing? A fish feeder turned out to have plenty of real problems to work through: motion,
timing, power cuts, retries, authentication and the consequences of feeding twice.

So yes, I could just get up and feed the fish. But then I would not have built this.

## What it does

| Feature | What you get |
|---|---|
| On-device scheduling | Three profiles with four events each; clock times or sunrise/sunset offsets, weekdays, every-N-days cadence and fast days |
| One-shot feeds and holds | A dated extra meal, or a pause for a chosen duration or until cancelled |
| One feed guard | Minimum gap, rolling 24-hour portion cap and tray-state checks for every source; a per-command limit for manual feeds |
| Food tracking | Portions remaining, next feed, recent results, projected refill date and a Needs Food signal |
| Built-in web app | Setup, feeding, schedules, configuration, calibration and diagnostics from a phone or computer |
| Runtime setup | Choose the drive, pins, network, time zone, time servers and keys without rebuilding |
| Home Assistant | Optional everyday controls and status through the ESPHome API or MQTT discovery |
| MQTT | Optional broker connection, everyday controls, schedule topics and TLS |
| Bluetooth | Optional authenticated BLE control with retries that cannot feed twice; a documented protocol and Python client |
| HTTPS | A self-signed certificate, an uploaded certificate or renewal from your own ACME server |
| Updates | Browser upload authenticated with the settings password when set, with an image-integrity check |

The web app is stored in the firmware and loads no scripts from the internet. Only its optional release check needs
the browser to go online. NTP supplies time; a local time server, Home Assistant or BLE can supply it without
internet access.

## How the food moves

| Mechanism | A load | Feeding | Reloading |
|---|---|---|---|
| Servo fan tray | Six portions in seven positions; position 0 is closed | Advance one slot, with a short wiggle | Press Tray Reload **before** filling; it returns to the closed slot |
| Direct-drive disc | Slot count minus one; 14 portions on a 15-slot disc | Advance one slot using motor encoder feedback | Fill every slot **except** the one over the chute, then press Tray Reload; it does not move |

One portion is the food you put in one slot, not a measured weight. Portion tracking counts completed or attempted
moves; there is no sensor that confirms food fell. Use hard food such as pellets; flakes can stick and are not
advised.

Every feed passes the same guard, whether requested by the schedule, web app, Home Assistant, MQTT, BLE or the
optional button. Missed meals are skipped rather than made up later. An interrupted move makes the position
unknown and blocks feeding until you recover it. Loss of Wi-Fi or Home Assistant never triggers an automatic reboot.
See [Safety behavior](docs/USING.md#safety-behaviour) for the details and limits.

## What you need

The reference board is a **Waveshare ESP32-S3-Zero**, with **4 MB flash**. The prebuilt firmware needs no PSRAM.
The ESP32-S3-DevKitC-1 has a board preset; check your board's available pins and LED before choosing it.

| Servo tray | Direct-drive disc |
|---|---|
| [Printed Precision Feeder Rev2](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2) | A suitable printed disc with 3 to 60 slots |
| MG90S or SG92R hobby servo | GA12-N20 gear motor with encoder and a DRV8833 driver |
| Suitable 5 V supply and a 470–1000 µF capacitor near the servo | Suitable motor supply; the reference rig uses 5 V and about 100 µF at the driver |

A normally-open push button is optional. Everything works without it. Only the motor, its driver and feedback, and
the optional button are supported beyond the board itself; there are no external clocks or food sensors.

### Wiring

Defaults for the S3-Zero; pins can be changed in Configuration or in a source build:

| Connection | Servo tray | Direct-drive disc |
|---|---|---|
| Motor control | Servo signal → GPIO1 | DRV8833 AIN1 → GPIO1, AIN2 → GPIO2, STBY → GPIO6 |
| Motor power | Servo + → 5 V, − → GND | Motor → AO1/AO2, driver VM → 5 V on the reference rig |
| Encoder | None | C1 → GPIO4, C2 → GPIO5, encoder VCC → 3.3 V |
| Optional button | GPIO2 to GND | GPIO3 to GND; choose this pin in Configuration |
| Status LED | Onboard GPIO21 | Onboard GPIO21 |

Connect all grounds together. Power the motor from a suitable supply, not the board's 3.3 V regulator. If a servo
twitches at power-up, a 10 kΩ resistor from signal to GND can help. A pin change takes effect after a requested
restart; the firmware checks saved pins for prohibited pins and conflicts.

## Getting started

1. **Install:** use the [browser installer](https://jspeer.github.io/precision-feeder/) for a published release, or
   [build from source](docs/BUILDING.md) for a custom configuration. Read [INSTALL.md](docs/INSTALL.md) before updating
   an existing feeder; erasing the board also erases its settings.
2. **Connect and configure:** join Wi-Fi, open the feeder's address, set the time zone, access-point password and
   settings password. Choose the correct drive and pins; restart if requested.
3. **Calibrate empty:** follow [CALIBRATION.md](docs/CALIBRATION.md) for your mechanism.
4. **Load:** follow the mechanism's reload order above, then check the reported portions remaining.
5. **Schedule:** use [SCHEDULING.md](docs/SCHEDULING.md), check the daily cap and next feed, and watch it operate
   before leaving it unattended.

## Find the right guide

| I want to… | Read |
|---|---|
| Install, update or recover a feeder | [Installation](docs/INSTALL.md) |
| Feed, refill, pause or troubleshoot | [Daily use](docs/USING.md) |
| Find a control in the browser | [Web app](docs/WEB_UI.md) |
| Plan meals and check refill timing | [Scheduling](docs/SCHEDULING.md) |
| Line up the tray or disc | [Calibration](docs/CALIBRATION.md) |
| Configure HTTPS, certificates or MQTT TLS | [HTTPS](docs/HTTPS.md) |
| Understand permissions and security limits | [Security](docs/SECURITY.md) |
| Connect an MQTT controller | [MQTT topics](docs/MQTT.md) |
| Write a BLE client | [BLE protocol](docs/PROTOCOL.md) |
| Build a custom firmware configuration | [Building](docs/BUILDING.md) |
| Add a mechanism | [Mechanism guide](docs/MECHANISMS.md) |

For contributors, [AGENTS.md](AGENTS.md) gives the repository rules and checks. [DESIGN.md](DESIGN.md) records the
architecture, decisions and open questions. [CHANGELOG.md](CHANGELOG.md), [BENCH.md](docs/BENCH.md) and
[development plans](docs/plans/README.md) preserve the project history; they are not setup instructions.

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
