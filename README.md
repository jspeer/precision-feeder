# Precision Feeder for ESPHome

ESPHome firmware for the
[Automated Precision Fish Feeder with WiFi (Rev2)](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2),
a 3D-printed fish feeder that drops one measured portion at a time from a
servo-driven tray.

It runs the feeder on its own: the schedule lives on the device, so the fish
are fed with no Home Assistant, no internet and no app, once the clock is set.
On top of that you get full **Home Assistant** control, a **web dashboard**, and
an optional **Bluetooth (BLE) interface** for a PLC or another controller.

It does everything the
[original Arduino firmware](https://github.com/FlyingGyroscope/PrecisionFeederRev2)
does, then adds minute-accurate scheduling with sunrise and sunset events,
three switchable schedule profiles, and a feed guard that protects the fish
from being over-fed.

> **Status: version 1.0.0, tested on a bench build.** One complete feeder
> (Waveshare ESP32-S3-Zero, MG90S servo) has been calibrated and tested on real
> hardware: feeding, schedules, power cuts, Home Assistant and BLE. It has not
> yet had months of daily use. Test it on your own build, watch it for a few
> days, and do not rely on it as the only way your animals get fed.

> **Written with AI.** This project was designed and written with the assistance
> of an AI (Anthropic's Claude). A human directed the work and tested it on real
> hardware, but the code and documentation may contain mistakes.

## Contents

- [How the feeder works](#how-the-feeder-works)
- [Features](#features)
- [What you need](#what-you-need)
- [Choosing your build: packages](#choosing-your-build-packages)
- [Setting it up](#setting-it-up)
- [Everyday use](#everyday-use)
- [Settings you can change](#settings-you-can-change)
- [The web dashboard password](#the-web-dashboard-password)
- [Safety behaviour](#safety-behaviour)
- [Troubleshooting](#troubleshooting)
- [Limits](#limits)
- [Documentation and repository layout](#documentation-and-repository-layout)
- [Credits](#credits) and [Licence](#licence)

## How the feeder works

The tray is a fan of 7 slots, 25 degrees apart, turned by a hobby servo over a
base with a feeding hole. **Slot 0 is closed** (it has a floor); **slots 1 to 6
each hold one portion.** Each feed turns the tray one slot further, so the next
portion falls through the hole. A full tray is therefore **6 portions**. When it
is empty you press **Tray Reload**, which turns the tray back to the closed
slot, and refill it.

There is no position sensor: the firmware keeps track of where the tray is. It
saves where it is going *before* every move, so if the power is cut halfway,
the position is marked **unknown** on the next boot and feeding stops until you
say where the tray is (or reload it). It never guesses.

> **Use pellets or other hard food. Flake food is not advised.** Hard food drops
> out of each slot cleanly. Flakes can stick to the sides and bottom of the
> slots while the disc turns, so portions come out unevenly or not at all.

## Features

- **On-board scheduler.** Three profiles (A, B, C) of four events each. An event
  is a clock time (`08:30`) or an offset from sunrise or sunset (`sunset -00:30`),
  with its own portions and weekdays. Each profile can feed every N days and
  skip "fast days". Switch profiles with one click (for example "normal" and
  "holiday").
- **No late feeds.** A missed event (power off, on hold, reloading) is skipped,
  never made up afterwards, so the fish are never double-fed to catch up.
- **Feed guard** on every feed from every source (schedule, Home Assistant, web
  UI, button, BLE): minimum gap between feeds, a rolling 24-hour portion cap, a
  per-command portion limit, and refusal when the tray is empty, its position is
  unknown, or it is being calibrated.
- **Knows what is left.** Portions remaining is exact. It tells you when the
  tray will run out (Runway, Tray Empty On), and **Needs Food** turns on when the
  tray is empty or the next feed would come up short, ready to trigger a
  notification.
- **Hold** the schedule for any number of hours or until you cancel it, and add
  **one-shot feeds** for a particular date and time.
- **Calibration** from Home Assistant or the web UI: the servo's pulse range and
  a fine offset for each slot, without reflashing.
- **Time from NTP**, optionally also from Home Assistant, or pushed over BLE
  when there is no network. Shows where the clock came from and how old it is.
- **Optional extras:** push button (as in the original), status LED, web
  dashboard, memory diagnostics.
- **Optional BLE interface** for a PLC, a phone app or a script: feed, read
  status, edit the schedule, calibrate, set the clock. Every write is
  authenticated with a shared key, so a stranger in Bluetooth range cannot
  control it. Documented byte for byte in [docs/PROTOCOL.md](docs/PROTOCOL.md),
  with a Python reference client.
- **Secure updates.** OTA updates are encrypted with the API key; the web
  server's own upload page is turned off.

## What you need

| Part | Notes |
|---|---|
| The printed feeder | [Printables model](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2) |
| Waveshare ESP32-S3-Zero | the 4 MB flash / 2 MB PSRAM version (ESP32-S3FH4R2). Another ESP32-S3 board works if you change the pins. |
| MG90S servo | or the TowerPro SG92R the original was built with (same size and pins) |
| 5 V supply | for the servo; USB-C or the board's 5 V pad for the board |
| Capacitor, 470 to 1000 µF | across the servo's power, close to the servo |
| Push button (optional) | any normally-open button |

**Wiring** (defaults; every pin can be changed, see [Settings](#settings-you-can-change)):

| From | To |
|---|---|
| Servo signal | GPIO1 |
| Servo + | 5 V (**not** the board's 3.3 V regulator) |
| Servo − | GND |
| Button (optional) | GPIO2 and GND (no resistor needed) |
| Status LED | built into the S3-Zero (GPIO21) |

If the servo twitches when the board powers up, add a 10 kΩ resistor from the
signal wire to GND. (It did not on the bench build.)

## Choosing your build: packages

The firmware is built from **packages**: one YAML file per feature in
[`packages/`](packages/). A configuration file lists the packages it wants, so
you switch a feature on or off by adding or removing one line.

| Package | What it gives you | Include it? |
|---|---|---|
| `core` | the feeder itself: servo, WiFi, Home Assistant API, OTA, NTP time, settings | **always** |
| `status` | sensors: tray position, portions left, next feed, runway, Needs Food, faults, time source | **always** |
| `controls` | buttons and settings: feed, tray reload, hold, active profile, guard settings, location | **always** |
| `schedule_editor` | editing profiles and events, one-shot feeds | **always** (unless something else, like a PLC over BLE, owns the schedule) |
| `calibration` | calibration mode, servo pulse range, per-slot offsets, recovery after a power cut | **always** |
| `web_server` | a dashboard at `http://precision-feeder.local` | recommended; no password by default (see [the dashboard password](#the-web-dashboard-password)); leave it out to save memory |
| `status_led` | colour-coded state on the board's LED | recommended if your board has the LED |
| `button` | a physical button: short press feeds (or reloads when empty), long press toggles calibration | only if you fitted one |
| `time_homeassistant` | Home Assistant as a second clock source | **on in both examples**: keeps the clock right when the internet is down but Home Assistant is up (harmless without Home Assistant). It also uses Home Assistant's timezone unless you pin yours (see the package's comment) |
| `ble_gatt` | the Bluetooth control interface ([PROTOCOL.md](docs/PROTOCOL.md)) | only if a PLC, app or script will talk to it over BLE. Costs about 320 KB of flash and 30 KB of RAM |
| `diagnostics` | free memory and loop time sensors | only for troubleshooting or development |

**Two ready-made configurations** are in [`examples/`](examples/):

- [`basic.yaml`](examples/basic.yaml): Home Assistant (including its time) and the
  web dashboard, no BLE. The right start for most people.
- [`ble.yaml`](examples/ble.yaml): the same plus the BLE interface and
  diagnostics, for a feeder driven by a PLC or other BLE controller.

**Enabling or disabling a package** is one line in the `packages:` section:

```yaml
packages:
  core: !include ../packages/core.yaml
  # ...
  button: !include ../packages/button.yaml        # enabled
  # time_ha: !include ../packages/time_homeassistant.yaml   # disabled (commented out)
```

**Keep your own settings separate (recommended).** Rather than editing an
example, make a file next to it, for example `examples/local.yaml` (any
`examples/local*.yaml` is ignored by git), that includes an example and adds
your packages and settings on top:

```yaml
# examples/local.yaml: this feeder's own build
packages:
  base: !include ble.yaml                       # or basic.yaml
  button: !include ../packages/button.yaml      # extra packages

substitutions:                                  # your settings (see below)
  timezone: Europe/London
```

Values in your file override the ones from the packages, and updating the
repository never touches your file.

## Setting it up

1. **Install ESPHome 2026.9.1** (the version this was built and tested with;
   newer versions may need small fixes). It needs Python 3.12 to 3.14:
   ```sh
   python3 -m venv .venv
   .venv/bin/pip install esphome==2026.9.1
   ```
   Or use the ESPHome add-on in Home Assistant.
2. **Get this repository** onto the machine that builds the firmware.
3. **Secrets.** Copy `examples/secrets.example.yaml` to `examples/secrets.yaml`
   (ignored by git) and fill it in: WiFi name and password, a password for the
   fallback hotspot, and an API key. Make the key with
   `openssl rand -base64 32`. Keep it: Home Assistant asks for it. With the BLE
   package, also a `ble_key`, made the same way (a different one): your BLE
   client needs it.
4. **Your configuration.** Make `examples/local.yaml` as shown above. At least
   set `timezone` (an IANA name such as `Europe/London`, or a POSIX string such
   as `<-06>6` for a fixed UTC−6).
5. **First flash over USB.** The S3-Zero has no USB-serial chip: hold **BOOT**,
   plug in the USB-C cable, release BOOT, then
   ```sh
   .venv/bin/esphome run examples/local.yaml --device /dev/ttyACM0
   ```
   (macOS: `/dev/cu.usbmodem…`; Windows: `COM3` or similar). The first build
   downloads the toolchain (1 to 2 GB). Later updates go over WiFi:
   `--device precision-feeder.local`.
6. **Add it to Home Assistant.** It is usually discovered automatically
   (Settings → Devices & services). If it is on a different network segment,
   for example an IoT VLAN, discovery does not reach it: add the **ESPHome**
   integration by hand with the feeder's IP address, port 6053, and your API
   key as the encryption key.
7. **Calibrate the tray** so every slot lines up with the hole:
   [docs/CALIBRATION.md](docs/CALIBRATION.md). Set the servo's pulse range
   first, then the small per-slot offsets.
8. **Load it.** Press **Tray Reload** first (it closes the tray so food cannot
   fall out), then fill slots 1 to 6.
9. **Set a schedule:** [docs/SCHEDULING.md](docs/SCHEDULING.md) walks through
   building a profile step by step. If you use sunrise or sunset events, set
   **Latitude** and **Longitude** first.
10. **Watch it for a few days** before you trust it while you are away.

## Everyday use

| To | Use |
|---|---|
| Feed now | **Feed One Portion**, or **Manual Portions** + **Feed Selected Portions**, or a short button press |
| Refill | **Tray Reload**, then fill the tray. Scheduled feeding pauses for the **Reload Pause** (default 10 minutes) while you load. |
| Pause the schedule | **Hold Duration** (hours; 0 = until cancelled) + **Hold Scheduled Feeding**; **Cancel Hold** to end it. Manual feeds still work. |
| Switch plans | **Active Profile** |
| Feed once at a set time | **One-shot Portions** + **Add One-shot Feed** (`2026-10-20 18:00`) |
| After a power cut mid-feed | look at the tray, set **Declared Tray Position**, press **Declare Tray Position**; or press **Tray Reload** |
| Stop a feed in progress | **Stop** (the position becomes unknown, as above) |

**Get told when it needs food.** "Needs Food" turns on when the tray is empty
or the next feed needs more portions than are left. A Home Assistant automation
(check the entity ID on your system):

```yaml
alias: Fish feeder needs food
triggers:
  - trigger: state
    entity_id: binary_sensor.precision_feeder_needs_food
    to: "on"
actions:
  - action: notify.notify
    data:
      message: "The fish feeder needs refilling."
```

**Status LED** (with the `status_led` package): dim green all fine; yellow clock
not set; red tray empty; magenta position unknown; blue moving; white
calibration mode; dim cyan hold or reload pause.

## Settings you can change

**At build time**, in the `substitutions:` of your own YAML (defaults in
[`packages/core.yaml`](packages/core.yaml) and
[`packages/ble_gatt.yaml`](packages/ble_gatt.yaml)):

| Substitution | Default | What it is |
|---|---|---|
| `name` / `friendly_name` | `precision-feeder` / `Precision Feeder` | device name (network, Home Assistant). Change both if you have more than one feeder. |
| `timezone` | `America/New_York` | IANA name or POSIX string. Daylight saving and sun times depend on it. |
| `servo_pin` | `GPIO1` | servo signal |
| `button_pin` | `GPIO2` | push button (button package) |
| `status_led_pin` | `GPIO21` | onboard RGB LED (status_led package) |
| `ntp_server_1` to `_3` | public pool | point at a local NTP server if there is no internet |
| `min_pulse` / `max_pulse` | `544us` / `2400us` | starting servo pulse range. Only used until you calibrate; then the saved values win. An MG90S is closer to 500 / 2500. |
| `servo_invert` | `"false"` | `"true"` if the servo is mounted the other way round |
| `sweep_degrees` | `"175"` | angle the 7 slots span (25° each). Leave it for the Rev2 print. |
| `step_time` | `1s` | time for each tray move |
| `wiggle` | `"true"` | forward, back, forward on each feed to shake food loose |
| `detach_delay` | `1s` | the servo is released this long after a move (no buzzing, no heat) |
| `ble_name` | `Precision Feeder` | name shown to BLE scanners (BLE package) |
| `ble_time_policy` | `fallback_only` | whether a clock pushed over BLE is accepted: only when network time is stale (recommended), `always`, or `off` |

The BLE key itself is `ble_key` in `secrets.yaml`, not a substitution.

**At run time**, from Home Assistant or the web UI (saved on the device): the
schedule, Active Profile, Hold, Minimum Gap Between Feeds (60 s), Gap Between
Portions (5 s), Daily Portion Cap (4), Max Portions Per Command (1), Reload Pause
(10 min), Refill Interval (3 days, for the runway warning), Latitude, Longitude,
the servo pulse range and calibration offsets.

## The web dashboard password

The dashboard (`web_server` package) has no password by default. To add one,
uncomment the `auth` lines in [`packages/web_server.yaml`](packages/web_server.yaml)
and add `web_username` and `web_password` to your `secrets.yaml`.

Be aware that the dashboard is plain HTTP: the password is sent unencrypted on
every request, so it only keeps casual visitors out. It is not real security on a
network you do not trust. Home Assistant's connection (the encrypted API) and BLE
(the shared key) do not have this weakness; if you do not use the dashboard,
leave the `web_server` package out instead.

## Safety behaviour

These are deliberate, so a fault ends in a missed meal rather than an
over-fed tank:

- **Every feed passes the same guard**, whatever asked for it. The daily cap
  counts every portion in the last 24 hours. (For debugging, the count it uses
  can be edited in Home Assistant: "Portions Counted (24 h)", logged.)
- **No catching up.** Events feed only in the minute they are due.
- **Each event feeds at most once a day**, also across reboots.
- **Power cut mid-feed → position unknown → feeding stops** until you confirm
  the tray position.
- **The clock is never restored from memory after a power cut** (it would be
  wrong by the length of the outage). Scheduled feeding waits for a real time
  source; manual feeds still work. After a restart *without* a power cut (an
  update, a crash) the chip keeps good time, shown as "kept across restart".
- **No automatic reboots**, and losing WiFi or Home Assistant does not reboot it.

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| Nothing is fed on schedule | Check **Faults** and **Next Feed**. Clock not set, Scheduler switch off, a hold or reload pause, the wrong Active Profile, or the tray empty or unknown. |
| A meal was skipped | Missed events are never made up (device off, on hold, refused by the guard). **Recent Feeds** and **Last Result** say what happened. |
| "Feed refused: daily cap" | More portions in 24 hours than **Daily Portion Cap**. The **Edited Profile** sensor warns when a profile's day exceeds the cap. |
| Slots do not line up, worse towards slot 6 | The servo's pulse range: calibrate **Servo Max Pulse** first ([CALIBRATION.md](docs/CALIBRATION.md)). |
| Servo buzzes or strains at one end | Pulse range too wide for your servo; narrow **Servo Min/Max Pulse**. |
| Tray position "unknown" | Power was cut or Stop pressed during a move. Declare the position or press Tray Reload. |
| Home Assistant does not find it | Different network segment: add the ESPHome integration by IP with the API key (port 6053). |
| Wrong times (an hour off) | The `timezone` substitution. With the `time_homeassistant` package, Home Assistant's timezone is used instead unless you pin yours (see that package's comment). |
| Food sticks | Use pellets; flake food is not advised. |

## Limits

- **The timezone is fixed when you build** (or comes from Home Assistant with the
  `time_homeassistant` package); it cannot be changed from the dashboard.
- **After a power cut with no network,** scheduled feeding waits until the clock
  is set again (NTP, Home Assistant, or BLE). There is no battery-backed clock.
- **Six portions per load.** Schedules that feed several times a day need
  frequent refills; the runway sensors tell you how long a load lasts.
- **BLE trusts anyone with the key, and does not prove itself to the client.**
  Writes need the shared `ble_key`; answers are not signed, so a client should
  pair (encrypted link) if it must trust what it reads. Leave the BLE package out
  if you do not need it. Details: [PROTOCOL.md](docs/PROTOCOL.md), Authentication.
- **Sunrise and sunset events** need Latitude and Longitude set. On days with
  no sunrise or sunset (far north or south) they count from 06:00 and 18:00
  instead, and Faults says so.

## Documentation and repository layout

| Document | For |
|---|---|
| [docs/SCHEDULING.md](docs/SCHEDULING.md) | building schedule profiles |
| [docs/CALIBRATION.md](docs/CALIBRATION.md) | lining up the tray |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the BLE protocol, for controller and app developers |
| [DESIGN.md](DESIGN.md) | the design and the decisions behind it |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each firmware version |
| [docs/BENCH.md](docs/BENCH.md) | what has been tested on real hardware |
| [AGENTS.md](AGENTS.md) | rules for contributors (human or AI); run `tools/check.sh` before sending changes |

```
components/precision_feeder/      the logic (tray, guard, scheduler, sun maths), plain C++ and
                                  tested on a PC, plus the ESPHome adapter
components/precision_feeder_ble/  the BLE protocol (only compiled when the BLE package is used)
packages/                         the feature packages described above
examples/                         basic.yaml, ble.yaml, secrets.example.yaml
tests/                            host-side C++ tests for the logic and the protocol
tools/                            Python BLE reference client (precision_feeder_client/; its protocol
                                  module is pure Python for reuse), tests, and check.sh
docs/                             the guides above
```

The logic is a C++ component rather than YAML because it has safety
consequences and needs to be testable; the packages are thin glue that expose it
to Home Assistant and the web UI.

## Credits

- Inspired by the Precision Feeder Rev2 by **FlyingGyroscope**: the
  [original firmware](https://github.com/FlyingGyroscope/PrecisionFeederRev2) and
  the [Printables model](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2)
  this firmware drives. This is an independent rewrite; none of the original code
  is used (it is AGPL-3.0, and was read only to learn how the mechanism behaves).
- Designed and written with **Claude** (Anthropic's AI), directed and tested on
  real hardware by a human.

## Licence

Released into the public domain under [The Unlicense](LICENSE). Do anything you
like with it. This covers this repository's own files only; the Printables model
has its own licence.
