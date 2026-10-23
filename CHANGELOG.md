# Changelog

Firmware versions (`fw_version` in packages/core.yaml), newest first. The BLE
protocol has its own version (docs/PROTOCOL.md). Bench results are in
[docs/BENCH.md](docs/BENCH.md).

*Written with the assistance of an AI (Anthropic's Claude).*

## 2.4.1: HTTPS and MQTT over TLS (2026-10-23, not tagged)

**Upgrade note:** nothing to do. The web app is now also at `https://<name>.local`, and opening it on plain HTTP sends
the browser there; your browser warns once about the feeder's own certificate. Everything else on port 80 keeps
working until you turn on HTTPS Only.

- **HTTPS for the web app** (`packages/tls.yaml`, in the prebuilt firmware and the examples). The feeder makes its
  own certificate for `<name>.local` at its first start (made again if the name changes), or uses a certificate and
  key you upload (from your own CA, for example). TLS 1.2, at most 4 sessions at once, about 15 KB of memory each
  while open. Everything the app does works the same over HTTPS: the live page, settings, the signed firmware upload.
- **Home Assistant's link lands on HTTPS.** Home Assistant builds the "Visit" link itself, always
  `http://<address>`, so the feeder cannot change it; instead, port 80 answers the app's page with a redirect to
  `https://<name>.local/` (HTTPS Redirect, on by default).
- **HTTPS Only** (off by default) refuses the rest of port 80. It never applies while the setup access point is up,
  and nothing is refused until HTTPS is running, so the feeder cannot lock you out.
- **MQTT over TLS** (`packages/mqtt_tls.yaml`, in the prebuilt firmware): the broker is checked against a CA
  certificate you upload, or pinned on first contact (the feeder keeps the certificate the broker showed the first
  time) instead of a check that trusts anything. With TLS on, MQTT never falls back to plain text.
- **A trust store** for that CA, and endpoints to upload certificates (`/feeder/tls`), signed with the settings
  password; the private key is masked with it on the way. The web app's page for them comes in 2.4.3.
- All new settings are web app only and protected by the settings password; none is in Home Assistant or MQTT.
- Size: the full prebuilt image is 41 KB larger (1,493,615 bytes, 81% of the app slot), with 2 KB more static RAM.

## 2.4.0: Bluetooth on NimBLE (2026-10-23, not tagged)

**Upgrade note:** a Bluetooth client that had *paired* with the feeder pairs again (the bonds are not carried over).
A client that does not pair, such as a PLC bridge or `tools/precision_feeder_ble.py`, needs nothing: the protocol,
the key, the UUIDs and the advertising are unchanged.

- **Bluetooth runs on ESP-IDF's NimBLE instead of Bluedroid,** trimmed to one client. The firmware is about 180 KB
  smaller (79% of the app slot instead of 89%) and needs about 17 KB less static RAM; on the bench, the same trim
  left about 33 KB more heap free than 2.3.x. That room is for HTTPS, next.
- Nothing a client sees changes: protocol 2, the same service and characteristics, the Device Information Service,
  the name (with the end of the MAC in the prebuilt firmware) and the service UUID in the advertising.
- Pairing is still optional (LE Secure Connections, "Just Works"); at most 3 bonds are kept, and the oldest makes
  room.
- YAML builds: `packages/ble_gatt.yaml` keeps its name and substitutions; it no longer uses `esp32_ble` or
  `esp32_ble_server`, and a build that adds either (or `bluetooth_proxy`, `esp32_ble_tracker`, Improv over BLE) is
  refused, since the two Bluetooth stacks cannot be linked together.

## 2.3.1: fixes from a code review of 2.3.0 (2026-10-23)

Fixes for the three findings of a Codex review of 2.3.0, each reproduced in a host test first. No settings change.

- **Calibration after a failed save no longer moves the tray.** If saving to flash failed (a reload refused because
  its save failed), the position was unknown in memory but still trusted on flash, and a calibration move was let
  through: a power cut then could bring the old position back. A calibration move now waits until "unknown" is
  really on flash, and is refused (`STORAGE_FAILED`) while saves fail.
- **Event times and one-shot feeds are read strictly.** `08:00 PM`, `08:00garbage` or a huge hour (`71582789:00`)
  was taken as some other time, over MQTT or Home Assistant; now the whole text must be a time (`HH:MM`, or for the
  sun `+HH:MM`, `-HH:MM`, `+90`; spaces around it are fine) or it is refused and the event is unchanged. One-shot
  feeds take `YYYY-MM-DD HH:MM` and nothing after it.
- **The Join dialog keeps your choice.** Picking "Other network..." (or a network) while a scan was still running
  was undone when the scan finished, so Join could go to another network than the one typed. A choice you made is
  now kept, and a network you picked stays listed if the next scan misses it.

## 2.3.0: loose ends (2026-10-23)

**Upgrade note:** if you turned on **MQTT Can Change Guard Limits** in 2.2.0, it is gone and MQTT can no longer change
the guard's limits; change them in the web app (Configuration). Everything else is kept.

- **Protected settings are read-only over MQTT.** The feed guard's limits (daily cap, gaps, portions per command) are
  still published on their MQTT topics, but changing them there is refused ("not allowed", with the value in force).
  The **MQTT Can Change Guard Limits** switch from 2.2.0 is gone; if you turned it on, change the limits in the web
  app (Configuration) instead. The schedule and one-shot feeds can still be edited over MQTT.
- **Clear the Bluetooth key** (web app, Configuration, Keys; behind the settings password), as the Home Assistant key
  could be. Bluetooth clients are then refused until a new key is made (a YAML build goes back to its own `ble_key`),
  and a connected client is disconnected.
- **Forget the Wi-Fi network** (web app, Configuration, Network; behind the settings password), in the prebuilt
  firmware and any build with no network in its YAML. The feeder restarts, once the tray is still, into its setup
  access point (`<name>-setup`) to pick a network; everything else is kept.
- **Pick the Wi-Fi network from a list** when joining one (Configuration, Network, and the setup wizard): the
  networks the feeder can see, strongest first, with Scan again. "Other network..." still takes a typed name, for a
  hidden network. The feeder stays connected while it scans.

## 2.2.0: who can change what (2026-10-22)

The first release since 2.1.1. It also carries 2.1.2's safety fixes and the move to ESPHome 2026.10.0 and the MIT
licence, listed under 2.1.2 below (2.1.2 was never released on its own).

**Upgrade note: Home Assistant sees far fewer entities.** It keeps the everyday controls and status: Feed One Portion,
Feed Selected Portions with Manual Portions, Tray Reload, Stop, Hold Scheduled Feeding with Hold Duration, Cancel
Hold, Scheduler, Active Profile, Portions Remaining, Tray State, Next Feed, Last Feed, Last Result, Recent Feeds,
Today's Feeds, Faults, Needs Food and Firmware Version. Everything else (the guard limits, calibration, pins, the
schedule editor, diagnostics) is in the web app only. Home Assistant's ESPHome integration removes the others by itself
when the feeder reconnects after the update. Automations that used them need the web app or MQTT instead.

- **Scopes** ([docs/plans/2.2.0-scopes.md](docs/plans/2.2.0-scopes.md), DESIGN.md section 10.2): operate, safety
  policy and components, and what each channel reaches. Every other entity is `internal`; the web app still has them
  all. `tools/check_scopes.py` checks every build for exactly the slim set.
- **MQTT's own topics** ([docs/MQTT.md](docs/MQTT.md)): the schedule (every event's fields, each profile's cadence
  and fast days), one-shot feeds, and the guard limits behind a new switch, **MQTT Can Change Guard Limits**
  (Configuration, MQTT; off by default). Plain topics, one value each, with retained state; a refused value is
  answered with the one in force. Changes made in the web app or over BLE are published too. Never calibration, pins,
  keys or network.
- **The 24 h count** (Portions Counted (24 h)) can be edited only in the web app, behind the settings password.
- The schedule editor checks event portions and every-N-days against their limits (it used the YAML's boxes only).
- Adding a one-shot feed identical to a pending one adds nothing (a command sent twice, or kept by a broker).
- New result: `not allowed` (18), a value this channel may not change.

## 2.1.2: safety fixes from two code reviews, and ESPHome 2026.10.0 (not released; see 2.2.0)

Two reviews of 2.1.1 (OpenAI Codex) found paths where a power cut, a lost encoder or a failed flash write could feed
twice or leave a position trusted that is not. Each was reproduced in a host test, then fixed. Nothing changes in
everyday use, except that a reload or a declared position now counts as moving for a moment (the guard answers
"busy" until the tray has settled).

- **Daily cap on a large disc:** the cap counted only the last 8 feeds, enough for the servo's 6 portions but not a
  disc's: a 9th feed in 24 h went uncounted. The history now keeps 64. The tray state record moves to version 3 under
  a new key; a 2.1.1 record is migrated at the first boot with everything kept.
- **Reload, declare and the end of calibration** saved the new position at once and counted as stationary while the
  servo was still turning: a power cut on the way left a trusted position, and a restart did not wait. They now save
  the move as an intent first and count as moving until the tray has settled.
- **Disc calibration:** Stop did not stop a calibration move; a restart did not wait for one; and a power cut in
  calibration left the old position trusted. Now the position is saved as unknown before the disc can move in
  calibration (and before a change of slot count, ticks per turn, trim or direction), Stop stops it, and a restart
  waits.
- **Disc, lost encoder:** while creeping onto a slot, or in the wiggle, the motor could drive for up to 500 ms with no
  encoder edge, its pulses growing: in the simulation a 60-slot disc moved about 4.8 slots uncounted. Powered time
  without an edge is now bounded in every phase to three quarters of a slot at full speed (25 to 50 ms).
- **One-shot feeds** could feed twice after a power cut within their minute (marked done two seconds after the
  feed). They are saved as done first.
- **Events and the clock:** a clock set back across a date could repeat an event that had already fed. An event's
  fed day now only moves forward and is saved before it feeds.
- **Flash write failures** were ignored. A feed whose intent cannot be saved now does not move and answers
  `STORAGE_FAILED` (BLE result 17, new); Faults says "saving to flash failed".
- **MQTT settings:** once MQTT has connected after a restart, changing the broker, port or login, or turning it on
  again after turning it off, did nothing (ESPHome builds its MQTT client once per boot), while the web app said they
  applied at once. They are now saved and Restart Needed turns on; before MQTT has started, they still apply at once.
- **BLE:** an event could not have more than 6 portions, even on a disc (now up to the portions per load); a failed
  AUTH on an authenticated connection left Status and the other records readable (now cleared, as on a disconnect).
- **Checks:** `tools/check.sh --compile` and `tools/build_bins.sh` reported success when ESPHome failed to build.
- **A build without the web app** failed to validate: `captive_portal` (core.yaml) auto-loads ESPHome's web OTA, which
  the feeder took for the web app. packages/web_server.yaml now names its web server for the feeder; check.sh checks
  a headless build (the disc example without the web app, with BLE).
- **ESPHome 2026.10.0** (ESP-IDF 5.5.6) for the prebuilt binaries and CI; YAML builds should use it too.
- **Licence: MIT** (copyright (c) 2026 Jeremy Speer), replacing The Unlicense: keep the copyright and permission
  notice (LICENSE) with copies. 2.1.1 and earlier stay public domain. The prebuilt images contain ESPHome's GPLv3
  runtime (README, Licence).
- Docs: DESIGN.md's status and password line, the servo's travel in CALIBRATION.md (150 degrees from slot 0 to 6),
  the protocol document's credits.

## 2.1.1: MQTT topic prefix (2026-10-21)

The first release of 2.1. 2.0.2's feeders update to it directly; 2.1.0 was never released (its bench run found the
fault below), and its changes are listed under 2.1.0.

- Fixed on the bench: with no MQTT topic prefix set, the feeder published under the build's name
  (`precision-feeder-<6 hex digits>` in the prebuilt firmware) instead of a name saved on the feeder. It now uses the
  name in use.

## 2.1.0: two drives, the direct-drive disc, and MQTT (not released; see 2.1.1)

Nothing changes for a servo feeder: same entities, same behaviour, same saved settings, same BLE protocol, and the
prebuilt firmware still starts as a servo feeder. Underneath, the firmware no longer assumes a servo: it carries a
second drive, a direct-drive disc, for custom prints, and other motors and trays can be added
([docs/MECHANISMS.md](docs/MECHANISMS.md)).

Mechanisms:

- What moves the tray is now a *mechanism* (DESIGN.md section 3.0). The servo fan tray is the first; its timing,
  wiggle and angles are unchanged, checked against a recording of what 2.0.2 did.
- Portions per load come from the mechanism (6 for the servo) instead of a fixed 6: the guard limits, event and
  one-shot portions, Declare Tray Position and the BLE ranges follow it.
- New diagnostic entities: **Mechanism** (`servo` or `n20-direct`) and **Capacity**. Faults names a motor fault
  when a mechanism reports one.
- The servo's YAML settings moved to `packages/mechanism_servo.yaml`, which `core.yaml` includes: your YAML and its
  substitutions work as before. Calibration Mode and the position recovery entities moved to
  `packages/tray_recovery.yaml`, which `core.yaml` includes too (a build without `calibration.yaml` now has them).
- `AGENTS.md` gains the rules for adding a mechanism; [docs/MECHANISMS.md](docs/MECHANISMS.md) is the walkthrough.

The direct-drive disc (DESIGN.md section 3.3):

- A GA12-N20 gear motor with an encoder behind a DRV8833 turns a printed disc of open slots (15 on the bench print,
  14 portions per load). It only turns forward; Tray Reload moves nothing.
- Its motion is the owner's tuned bench sketch, rewritten as firmware and checked against it on a simulated motor,
  with extra safety limits: a short stall window at full power, a travel limit, a timeout from the distance and a
  watchdog on the control loop.
- A guided calibration in the web app (docs/CALIBRATION.md): direction test, zero, a full-turn measurement, a check
  of every slot, a global trim, and Trim here (the trim from a slot nudged to the centre). Each step says which
  button to press.
- Tuning on the device (web app Maintenance, and Home Assistant): ticks per turn, drive power, braking distance,
  wiggle amplitude and cycles, slot trim, calibration turns, reverse motor and encoder. **Disc Slots** (3 to 60) is a
  setting too, so a 14- or 16-slot disc needs no rebuild. The values that move the slots can only be changed in
  calibration mode.
- Its five pins (IN1, IN2, STBY, encoder A and B) are chosen on the feeder like the others (D19); the button and LED
  can never be moved onto them.

One firmware, both drives (D17):

- Including `packages/mechanism_n20.yaml` adds the disc beside the servo, and **Drive** (web app Configuration, Home
  Assistant) chooses between them, from the next restart. The drive not chosen is never set up, so its pins stay
  free. Changing the drive marks the tray position unknown: reload before feeding.
- The prebuilt binaries carry both and start with the servo (`disc_build_default: "false"`); `examples/disc.yaml`
  starts with the disc, with the button on GPIO3.
- A build's button or LED pin that the drive in use drives (the servo's button default, GPIO2, is the disc's IN2) is
  left off, with a warning, until another pin is chosen.
- **Restart**, **Restart Needed** and **Pins In Use** are in `packages/restart.yaml` (hardware.yaml includes it).

MQTT (D20):

- New optional package `packages/mqtt.yaml`, in the prebuilt binaries: ESPHome's MQTT client beside the native API,
  with Home Assistant discovery for every entity Home Assistant sees, and ESPHome's device discovery
  (`esphome/discover`). Off until it is set up.
- Set up in the web app (Configuration, MQTT): on/off, broker, port, username, password and topic prefix, saved on
  the feeder. On/off, broker and login apply at once; the topic prefix after a restart. The settings are web app
  only (neither Home Assistant nor MQTT can redirect the feeder) and need the settings password. The password is
  never sent back. Losing the broker never restarts the feeder.
- Discovery unique IDs are made from the MAC, so two feeders never clash.

Also:

- GPIO3 can now be chosen for the button or LED, with a warning: on the ESP32-S3 it only selects the JTAG source,
  and only once an eFuse is burned that is not by default.
- The web app's update check tells a feeder built from a branch (a variant the releases do not publish) that there
  are no published updates for it, instead of offering the main firmware.
- A password text in the web app (the MQTT password) gets a password box and starts empty.
- The web app's size budget is 66 KB gzipped (was 64 KB).
- The Python BLE client reads calibration offsets only from a feeder that has them, and checks the tray position
  against the positions the feeder reports.
- Fixed: a build without the native API failed to compile (a missing include).

## 2.0.2: NTP servers (2026-10-16)

- New **NTP Servers** setting (Configuration, the setup wizard, and Home Assistant): up to three host names or IP
  addresses, for example a time server on your own network. Applied at once; empty uses the build's servers (the
  public pool in the prebuilt firmware). Like the time zone, it needs no settings password.
- Fixed: only the first of the three NTP servers in the YAML was ever asked (ESP-IDF allowed one by default); all
  three are now.
- Documentation reworked and checked against the code: the README is now an overview, with new guides for building
  from YAML ([docs/BUILDING.md](docs/BUILDING.md)) and everyday use ([docs/USING.md](docs/USING.md)) beside the
  install guide; stale statements (the 1.1.0 status, the calibration bench notes, missing substitutions) fixed.
- The device settings record moves to version 2; one saved by 2.0.0 or 2.0.1 is migrated at the first boot, with
  everything kept.

## 2.0.1: appliance, fixed on the bench (2026-10-16)

The first release of 2.0, "appliance": prebuilt binaries installed from a web page, a setup wizard, settings changed
at run time, and updates from the web app. 2.0.0 was never released: the bench (2.0.0 N8,
[docs/BENCH.md](docs/BENCH.md)) found faults in it, fixed here. Its changes are listed under 2.0.0 below.

- New guides: [docs/INSTALL.md](docs/INSTALL.md) (installing the prebuilt firmware, updates, recovery, the binary
  and flash matrix) and [docs/SECURITY.md](docs/SECURITY.md) (the security model).

- Fixed on the bench (N8): every protected write through the web server (guard limits, calibration, pins) crashed
  the feeder with a stack overflow in the web server's task, which on a fresh upgrade made ESPHome roll the update
  back. The settings guard no longer keeps its own stack frame while the web server handles the write.
- Fixed on the bench (N8): the firmware upload accepted a first-flash `.factory.bin` (the bootloader then refused to
  start it, so the old firmware came back). The feeder and the Firmware page now refuse anything that is not an app
  image.
- Fixed on the bench (N8): the update check waited indefinitely on a network that drops packets silently; it now
  says "Couldn't check for updates" after 10 seconds.

## 2.0.0: appliance (not released; see 2.0.1)

Prebuilt binaries set up in a browser ([docs/plans/2.0.0-appliance.md](docs/plans/2.0.0-appliance.md)).

- Timezone without a rebuild: the new **Timezone** entity (Configuration) takes a
  POSIX TZ string such as `CET-1CEST,M3.5.0,M10.5.0/3` and applies it at once. One
  set there wins over Home Assistant's and the build's; empty goes back to them.
- A new saved record, Device, for the settings 2.0 changes at run time (pins,
  names, keys, timezone). A YAML build keeps following its YAML until a value is
  changed on the device. Nothing existing is migrated or lost.
- Settings password (none by default): set in the app, it protects the feeding safety
  limits and the calibration from changes through the web server. The password never
  crosses the network: the app proves it knows it for each change. Holding BOOT for 10
  seconds clears it. Home Assistant and BLE are unaffected. New Settings Password sensor.
- The Configuration page edits the timezone and manages the settings password.
- Pins without a rebuild: the new `hardware` package (on in both examples) adds Board
  (Waveshare ESP32-S3-Zero and ESP32-S3-DevKitC-1 presets), Servo Pin, Pins In Use,
  Restart Needed and a Restart button; the button and status LED packages add Button Pin
  and LED Pin, where "none" means not fitted. Pins saved on the device win over the YAML
  and apply at the next restart; only pins the ESP32-S3 can spare are accepted. They are
  protected by the settings password. The Configuration page has a "Board and pins" card.
- Keys on the device. The web app's Configuration page has a Keys card: generate (shown
  once) or enter a Bluetooth key, which is saved on the feeder and wins over `ble_key`
  (now optional); with no key at all, every BLE client is refused. With the new
  `api_key_on_device` package the Home Assistant key is not in the YAML: Home Assistant
  sets it on adoption, or the card generates, takes or clears it; OTA then refuses
  plaintext. All of this needs the settings password when one is set.
- Network settings in the web app: Configuration has a Network card to change the
  feeder's name (used after the next restart; wins over `name`), join another Wi-Fi
  network (at once; the access point is the way back if it is wrong) and change the
  fallback access point's password (after the next restart). Protected by the settings
  password, with the passwords masked by it; the feeder never sends one back. The access
  point is now named `<name>-setup` from the name in use.
- Setup wizard: a new feeder opens a step-by-step setup in the web app (network, time zone
  and location, board and pins, keys, settings password, then calibration). Every step
  saves as it goes and can be skipped; Configuration can start it again. A feeder updated
  from 1.x is not sent through it.
- Time zone by place: the web app lists every IANA zone and sets the Timezone entity to the
  POSIX string ESPHome would compile for it (setup wizard and Configuration).
- Fixed on the bench: refusals of the settings password (wrong, missing or too soon) reached
  the browser as HTTP 500 instead of 403 or 429; joining another Wi-Fi network from the web
  app did not switch a connected feeder; a new feeder lost its setup wizard after its first
  restart. The BLE client's `config set` failed with every key name.
- New `name_add_mac_suffix` substitution (off by default): adds the end of the MAC
  address to the name, so feeders built from one YAML do not clash.
- New `improv_serial` package (on in both examples): set Wi-Fi over the USB cable from an
  install page (ESP Web Tools).
- What BLE may change: two new switches, BLE Can Change Guard Limits and BLE Can Change
  Calibration (both on; protected by the settings password). Off, those BLE writes are
  refused with NOT_AUTHORIZED detail 7. A missing key is detail 6. Still protocol 2;
  the Python client names both and shows unknown details by number. BLE Info also says
  where the key comes from and shows the two permissions.
- Prebuilt binaries: `full` (with BLE) and `no-ble`, built by GitHub Actions from
  `firmware/` for each release tag, with an install page on GitHub Pages (ESP Web Tools,
  then Wi-Fi over USB). They carry nothing personal; the fallback access point has no
  password until one is set (the setup wizard strongly recommends it).
- Firmware updates in the web app (Diagnostics, Updates and firmware upload): the browser
  checks GitHub Pages for a newer release (can be turned off), and uploads an image after
  checking it against the release. The upload is signed with the settings password and the
  feeder checks the image's SHA-256 before switching, then restarts once the tray is still.
- New Firmware Variant sensor: `full`, `no-ble` or `yaml` (built from your own YAML).
- CI: the BLE session text buffer is sized for GCC's format checks (no behaviour change).

## 1.1.0: local first (2026-10-15)

The built-in web app, so the feeder is fully usable from a browser with no Home
Assistant and no internet ([docs/plans/1.1.0-local-first.md](docs/plans/1.1.0-local-first.md)).

- New read-only entities, worked out by the same code that runs the feeder:
  - `Today's Feeds`: today's feeds in time order, each fed, skipped, due, next,
    later or held.
  - `Feed Guard`: what a one-portion feed would get right now ("ready" or the
    reason it would be refused), without feeding.
  - `Device Time` (diagnostic): the feeder's local time and UTC offset.
  - `Schedule Data` (diagnostic, disabled by default in Home Assistant): all
    three profiles in one compact line for the web app (format in DESIGN.md
    section 7).
- New optional package `network_info.yaml` (in both examples): Wi-Fi network,
  IP addresses and signal strength, from ESPHome's own sensors.
- `tools/mock_device.py`: a fake feeder that speaks the web server's HTTP, for
  building the web app without hardware.
- The web app's foundation replaces ESPHome's stock dashboard (`packages/web_server.yaml`), which loaded its
  script from the internet:
  - Plain JavaScript and CSS in `web/src/`, built into `web/dist/` by `tools/build_web.py` and embedded in
    the firmware (about 18 KB gzipped).
  - Pages and sections appear only for the entities a build contains.
  - Light and dark follow the browser; phones get a bottom tab bar.
  - A warning banner for the most important problem, with what to do.
  - A Diagnostics page: system, network, memory, live log, and every entity with a control.
  - Dashboard:
    - the tray as a dial of its seven slots, the next feed and runway;
    - Feed now, with the guard's current answer, reporting the device's own result;
    - today's feeds, status, recent feeds;
    - hold with durations, resume, tray reload, and stop while moving.
  - Recovering the tray position after a power cut: pick the slot you see, from the banner.
  - Schedule:
    - all three profiles at a glance, read from Schedule Data, so looking never moves the editor that Home
      Assistant shares;
    - make a profile active; the schedule on or off;
    - an event editor (clock time or sunrise / sunset with an offset, portions, days, on or off);
    - cadence and fast days, with "start the cycle today";
    - one-shot feeds in the feeder's own time, and clearing them.
    Each save writes the editor fields in the documented order, each confirmed by the device. A live event is
    switched off before it moves, so a half-applied change can never fire. The end result is checked against
    Schedule Data.
  - Configuration: the feeding safety limits, reload pause and refill interval, location, and the device's time
    zone, clock and network, each with the device's own limits and units, confirmed when saved.
  - Maintenance:
    - a calibration walkthrough (start; pick a slot and the servo goes there; nudge it by 0.1 or 1 degree;
      next slot; finish and save);
    - the servo pulse range;
    - declaring the tray position or reloading;
    - the 24 h count edit for debugging, behind its explanation.
  - Bluetooth (only in builds with BLE):
    - status, the client (the last two bytes of its address, connected for, last activity, MTU);
    - protocol facts and counters;
    - the activity log of the last 32 connections and writes with their results (from `/feeder/ble`),
      fetched again only when a connection or write happens.
    Read-only; nothing secret is shown, because the device never reports it.
  - Maintenance also appears in builds without the calibration package (tray reload and the 24 h count).
- IPv6 alongside IPv4 (`enable_ipv6` substitution in core.yaml, on by default): the web app, Home Assistant
  and mDNS answer on both, and Diagnostics lists each address with its kind (IPv4, IPv6, link-local,
  private). Wi-Fi still counts as connected on IPv4 alone, so IPv4-only networks are unaffected. IPv6-only
  networks need a static IPv4 address (an ESPHome limit; see the README).
- The web app waits for the device's entities to settle before showing a page, so a link straight to a page
  (for example `#diagnostics`) no longer misses the parts of the build that arrive last.
- Web app hardening:
  - After the feeder restarts with a different app (an update), an open page offers a reload. The build stamps
    each app with an id; a page checks it only when the device's uptime goes backwards.
  - A page hidden for 30 seconds gives its connection back (the web server has only a few) and reconnects when
    shown.
  - Schedule saves re-point the shared editor before every write, and say by name if another client's use of the
    editor changed a different event.
  - Page headings and titles for screen readers, focus moves to the page on navigation, and every Set, Press and
    switch control is named after its setting.
- Docs: the README leads with the local-first idea; new [docs/WEB_UI.md](docs/WEB_UI.md); bench results for the
  web app, BLE diagnostics, IPv6 and web auth in docs/BENCH.md.
- BLE diagnostics (no protocol change, still protocol 2):
  - New entities `BLE Info` and `BLE Session` (diagnostic, disabled by default in
    Home Assistant): the connection, the last two bytes of the client's address,
    the MTU, and counters since boot.
  - The web server serves the last 32 BLE connections and writes at
    `/feeder/ble`.
  - The device log names opcodes ("Command FEED seq 5 -> ok") and logs
    connections.

## Examples: clone into the config folder (2026-10-15, no firmware change)

- The examples now expect the repository cloned into the ESPHome config folder
  as `precision-feeder/`, with a copy of an example next to it
  (`precision-feeder/...` paths). `git pull` updates the firmware; the user's
  file and `secrets.yaml` stay outside the repository. This replaces
  `examples/local.yaml`. `tools/check.sh` validates the examples in that layout.

## 1.0.1: quieter log (2026-10-15)

- "Clock set by NTP / Home Assistant / BLE" is logged at DEBUG instead of INFO.
  It was added to diagnose the Home Assistant time source; at INFO it appeared
  twice every 15 minutes. Turn on debug logging (`logger: level: DEBUG`) to see it.

## 1.0.0: first release (2026-10-14)

The first release: everything in this changelog, bench-tested on one complete
feeder (docs/BENCH.md). Firmware behaviour is the same as 0.5.2; since then
only internal refactors, all covered by the host tests:

- The BLE protocol runs in a host-tested engine; setting limits have one home;
  the scheduler and the projection share one source of events; the feed
  history is handled as records; the Python client is a package. Details below.
- `tools/check.sh` and a GitHub workflow run every check.

BLE protocol: version 2 (authenticated). Saved settings from 0.3.0 onwards are
migrated on update.

### Internal changes since 0.5.2

- Refactor (no behaviour change): the feed history is handled as records
  (`HistoryEntry`, newest first) through four helpers, instead of five parallel
  arrays shifted and indexed by hand; the saved layout is unchanged. The rule for
  which feeds the daily cap counts lives in one place (`counts_today_`).
- Python client split into the `precision_feeder_client` package: `protocol`
  (pure Python, standard library only, for reuse in other clients such as a PLC
  bridge), `client` (bleak), `selftest` and `cli`. `tools/precision_feeder_ble.py`
  stays the entry point and `import precision_feeder_ble` still works. A
  duplicate `PROTOCOL_VERSION` definition is gone.

## 0.5.2: refactors, tests and CI (2026-10-14)

- Settings limits have one home: `feeder_core.h`, enforced by FeederCore's
  setters, which Home Assistant, BLE and loading from flash all use. The range
  table in `protocol.h` is gone; `tests/test_consistency.py` checks the YAML
  min/max and the component schema against the C++ limits. BLE locations are
  checked as integers before conversion (as a float, 180.00001 degrees rounds to
  180.0).

- Refactor (no protocol change): the BLE protocol moved into `BleEngine`
  (`engine.h/.cpp`), plain C++ on top of FeederCore that talks to the radio only
  through a small `BleTransport` interface. `PrecisionFeederBle` is now a thin
  adapter (characteristics, hardware RNG, log, connect events). Record handlers
  return their answer instead of passing it through member variables. New host
  test `tests/test_ble_engine.cpp` drives the whole protocol against the
  simulated feeder (AUTH, retries, replay after a repeated AUTH, signing rules,
  every command, records, SET_TIME policy, disconnect); with today's two BLE
  bugs put back, it fails. The fake feeder is shared in `tests/fake_feeder.h`.

- Tests and CI: a daylight-saving test (a US Eastern fake clock: the skipped and
  repeated hours, daily times, and a sunset event across the change);
  `tools/check.sh` runs every host test (C++ now with -Werror) and validates the
  examples with ESPHome; `.github/workflows/ci.yml` runs it on every push (a full
  compile on manual dispatch).

- Refactor (no behaviour change): the scheduler and the projection get their
  events from one function, `for_each_occurrence_`, so what fires and what Next
  Feed / runway / Needs Food predict cannot drift apart again (the cause of two
  earlier bugs). New test `test_projection_matches_firing`: 25 random schedules
  over 3 simulated days, every scheduled feed must be in the predicted minute and
  no prediction may silently vanish.

## 0.5.1: bug sweep (2026-10-14)

- `selftest` (Python client) no longer cancels an existing hold: it tests
  retries by re-selecting the active profile, and checks that nothing changed.
- Next Feed (and Needs Food, runway) skip events inside the reload pause, as the
  scheduler does; they used to be shown and then skipped.
- Settings saves are flushed to flash (ESPHome otherwise waits up to 60 s, and a
  power cut in that time lost the edit).
- BLE: a signed command too short to hold an opcode is answered INVALID_ARG
  (the length arithmetic underflowed; harmless, but wrong).
- A multi-portion feed that is stopped part-way records only the portions that
  dropped (the step in progress counts; later ones do not).
- Guard settings are range-checked when loaded.
- Python client: day names are strict ("sunshine" is not Sunday), and bad input
  gives a one-line error instead of a traceback.

## 0.5.0: review fixes, publish on change, settings migration (2026-10-14)

- **BLE fix (found by the owner):** repeating a valid AUTH on an authenticated
  connection reset the last accepted sequence number, and the handler cleared
  the retry cache, so a write already executed in that session (a FEED, say)
  could run again if the guard allowed it. A repeated AUTH is now answered OK and
  changes nothing; the retry cache is only cleared on connect and disconnect.
  Regression test in tests/test_protocol.cpp (fails without the fix).
- Sun events that resolve to the evening before their day (for example
  "sunrise - 10 h") now feed; the scheduler only looked at today's and
  yesterday's events, so they never did, while Next Feed showed them.
- Setting and cancelling a hold is written to flash at once (a power cut within
  ESPHome's write interval could lose it).
- Settings layout version 4: the servo calibration (pulse range and offsets) is
  its own saved record, so a future settings change can never lose it. Version 3
  settings (firmware 0.3.0 to 0.4.x) are **migrated, not discarded**: schedule,
  guard settings, location and calibration are kept. Each record layout now has
  its own preference key, so an old record stays loadable for migration.
- Entities publish only when their value changes: the status, control, editor
  and calibration entities have no polling of their own; one 1-second interval
  per package compares and publishes (`publish_number/text/bool/option` in
  `precision_feeder.h`). Home Assistant and the log no longer see the same value
  every 2 to 30 seconds.
- Scheduler work: day contexts are cached for an hour, the sun-time cache covers
  the whole 30-day projection, and the projection refreshes every minute (and on
  any change) instead of every 30 seconds.
- Day names must be three letters or the full name (plural allowed); "sunshine"
  is no longer Sunday. Profile summaries list events in time order. An ordinary
  boot no longer rewrites the settings.
- `feeder_core.cpp` split into `feeder_core.cpp` (tray, guard, clock),
  `feeder_schedule.cpp` (scheduler, projection) and `feeder_text.cpp` (editor and
  status texts), with shared constants in `feeder_internal.h`.
- Python client: `config get/set`, `profile show/header/event`,
  `calibration get/set`, and `selftest` (checks a feeder's protocol without
  feeding).
- Docs: DESIGN.md is the current design only; this changelog and
  docs/BENCH.md hold the history and the bench results. README: how to put a
  password on the web dashboard.

## 0.4.1: HA time on by default, 2026-10-14

- `examples/basic.yaml` and `examples/ble.yaml` include the
  `time_homeassistant` package (it was commented out). No code change.

## 0.4.0: BLE protocol 2: authentication, 2026-10-14

- Every BLE write must be authenticated with the shared `ble_key` (section 10,
  docs/PROTOCOL.md). New Auth characteristic (000A), AUTH opcode (0x0D), result
  16 NOT_AUTHORIZED, capability bit 6. Profile, Config and Calibration writes
  gained a leading sequence number and are answered with it. **Not compatible
  with protocol 1 clients**: the PLC-side driver must implement protocol 2.
- `esp32_ble`: `auth_req_mode: sc_bond`.
- Retry cache entries carry the characteristic (channel), and the cache is
  cleared on every connect and disconnect.
- Each clock sync is logged ("Clock set by NTP" / "Home Assistant" / "BLE").
- Python client: `--key` / PRECISION_FEEDER_BLE_KEY; authenticates on connect
  and signs every write.

## 0.3.5: polar days, 2026-10-14

- On a day without a sunrise or sunset at the set location, sun events use
  06:00 (sunrise) or 18:00 (sunset) plus their offset instead of being skipped.
  Faults: "no sunrise or sunset today: sun events use 06:00 / 18:00"; BLE flag
  bit 6 stays set (it means a sun event could not use the real sun). No
  location set: still skipped. Test: `test_polar_fallback` (89 degrees N).

## 0.3.4: source label, 2026-10-14

- The feed source for the device's own feed buttons (pressed in Home Assistant
  or the web UI) is labelled "ESPHome" instead of "home assistant" in logs,
  "Last Feed" and "Recent Feeds" (owner request: it is an ESPHome entity call).
  Code name `Source::ESPHOME`. The BLE value is still 1, so the protocol is
  unchanged; only the label in PROTOCOL.md and the Python client changed. The
  clock source "Home Assistant" (HA time) keeps its name.

## 0.3.3: Needs Food, 2026-10-14

- New "Needs Food" binary sensor (status package, device class problem): on
  when the tray is empty, or the next scheduled or one-shot feed needs more
  portions than remain (the firmware would feed what is left, so the meal would
  come up short). Off while the position is unknown. Meant as an automation
  trigger. Also BLE Status flag bit 12 (an addition within protocol 1) and a
  Faults line. The projection now keeps the next feed's portions.

## 0.3.2: scheduling clarity, 2026-10-14

- New docs/SCHEDULING.md: building a profile step by step, field reference,
  rules and worked examples. Linked from the README and the editor package.
- New "Edited Profile" text sensor (editor package): the summary of the
  profile being edited, marked [active] / [not active], with a warning when its
  busiest day exceeds the daily cap. A separate sensor rather than part of
  "Active Schedule", which could then exceed Home Assistant's 255-character
  limit for a text sensor.
- Over-cap warning: when the active profile's busiest weekday (enabled events,
  fast days excluded) needs more portions than the daily cap, Faults says so
  and BLE Status flag bit 11 is set (an addition within protocol 1). The check
  is per weekday; the cap itself is a rolling 24 h window.

## 0.3.1: bench fixes, 2026-10-14

- "Next Feed" and the runway no longer include an event that already fed today
  (it could be moved later in the day, then shown as due while it would not
  feed). Test: `test_next_feed_skips_fired_event`.
- A clock kept by the RTC across a soft reset is no longer reported as an NTP
  sync (section 6). It shows as "kept across restart" (BLE: source 0 with the
  clock-valid flag) and does not block BLE `SET_TIME`. The sntp hook now calls
  `note_sntp_sync()`, which checks ESP-IDF's SNTP sync status.
- New Home Assistant diagnostic number "Portions Counted (24 h)": shows and
  sets the count the daily cap uses (decision log, 2026-10-14). New history
  source "adjustment" (internal; never sent over BLE).
- Docs: MG90S datasheet values and the deadband in docs/CALIBRATION.md;
  `examples/local*.yaml` for personal overrides (git-ignored).

## 0.3.0: servo pulse range settable at run time, 2026-10-14

- First bench result: the MG90S turned about 12% less per microsecond than the
  544 to 2400 us range assumes (slot 0 needed +11 degrees, slot 1 +14.5, and an
  uncalibrated position 6 stopped just past slot 5). The +-15 degree per-slot
  offsets cannot absorb that, and fixing it in YAML needs a reflash per try.
- The pulse range is now part of the saved settings and can be changed at run
  time: Home Assistant "Servo Min Pulse" / "Servo Max Pulse" (calibration
  package), BLE Config keys 10 and 11, capability bit 5. In calibration mode the
  servo moves to the selected position after each change. The YAML
  `min_pulse` / `max_pulse` are the defaults for a fresh device; saved values win.
  Limits 300 to 1500 / 1500 to 3000 us, at least 500 us apart.
- Settings version 3: **saved settings from earlier versions are discarded** on
  the first boot (schedule, guard settings, calibration offsets, location). The
  tray state is kept.
- BLE: an addition within protocol 1 (new keys and capability bit); nothing
  existing changed.

## 0.2.2: servo default, 2026-10-13

- Default `min_pulse` is 544 us (was 500), the pulse range the original firmware
  drives (ESP32Servo's default, 544 to 2400 us), so a fresh build starts the tray
  where the original did. The original was tuned on an SG92R; this build uses an
  MG90S, so the range still needs the bench check. Existing builds that set
  `min_pulse` themselves are unaffected; others will see every position shift by
  up to about 4 degrees and should re-check calibration.
- Docs: the tray geometry measured from the printed model (section 3).

## 0.2.1: review fixes, 2026-10-14

- Refactor: all logic moved into `FeederCore` (`feeder_core.h/.cpp`), plain C++
  with the clock, servo, storage and log behind `hal_*` hooks. `PrecisionFeeder`
  is now a thin ESPHome adapter. `tests/test_core.cpp` runs the tray, guard and
  scheduler against a simulated clock (13 test groups).
- The push button reloads the tray when its position is unknown, not only when
  empty, so a button-only build can recover from a power cut mid-feed.
- Scheduled and one-shot feeds refused with BUSY / TOO_SOON / hold are retried
  silently; the outcome is reported once (section 5.5). Previously every retry
  logged, changed "Last Result" and sent a BLE event-log notification.
- The BLE retry cache matches the whole command, not just the sequence number.
- Daily cap after a reboot without a clock (CHANGELOG.md deviations).
- One-shot feeds run with the Scheduler switch off; "Next Feed", runway and the
  over-capacity flag no longer count recurring events while it is off.
- The sunrise/sunset fault is evaluated every tick, and clears while the
  scheduler is off.
- The reload pause survives a reboot when the clock was known (state version 2;
  the saved tray state from earlier versions is discarded).
- "Minutes Since Clock Sync" is unknown until the clock has been set.
- Python client: answers matched on (seq, opcode); commands never use seq 0.

## 0.2.0: BLE protocol, and the two warnings fixed

- BLE protocol v1 implemented (section 8, `docs/PROTOCOL.md`).
- OTA authenticates with the API encryption key (no separate OTA password), and
  the web server's own `/update` page is disabled (`ota: false`), because it would
  accept an image in plaintext. Firmware uploads go through the encrypted ESPHome
  OTA only.
- `rgb_order` replaced by `channel_colors` (deprecated in ESPHome).
- The clock source is tracked and shown in Home Assistant ("Time Source",
  "Minutes Since Clock Sync").

## 0.1.1: cleanup after reviewing the original firmware's behaviour

- Late feeds removed: the "Late Start Grace" setting is gone and events fire only
  in the minute they are due (section 5.5). Settings version bumped to 2.
- Hold has no maximum: "Hold Duration" is in hours, 0 means until cancelled, and
  an indefinite hold needs no clock. A "Hold" status sensor shows what is active.
- The cancel-backlog step on tray reload is gone (nothing to cancel).
