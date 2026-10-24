# The web app

Open the feeder in a browser to feed, schedule, configure and maintain it. The app lives in the firmware, works on
phones and computers, and loads no assets from the internet. Home Assistant is optional.

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

## Opening it

Browse to `https://<name>.local` or the feeder's IP address. Prebuilt firmware uses `precision-feeder-<6 hex digits>`
until renamed. HTTP redirects the app to HTTPS when enabled; the first visit warns about the self-signed certificate.
See [HTTPS.md](HTTPS.md) for certificates and [INSTALL.md](INSTALL.md) for first-time setup.

A phone puts the tabs along the bottom. You can add the page to its home screen as a shortcut. The app is included
in prebuilt firmware and source builds with the `web_server` package; HTTPS also needs `tls`.

## Start with the task

| Task | Where |
|---|---|
| Feed, refill, pause or see what happens next | Dashboard |
| Edit meal times, profiles or dated feeds | Schedule |
| Set safety limits, time, network, drive, pins, passwords or integrations | Configuration |
| Calibrate, recover position or adjust the diagnostic 24-hour count | Maintenance |
| Inspect a BLE session or command activity | Bluetooth |
| Inspect memory, clock, network, log or raw entities | Diagnostics |
| Install an update | Diagnostics → Updates and firmware upload |
| Run first-time setup again | Configuration → Device |

## Dashboard

The tray drawing follows the selected mechanism. It shows portions remaining, the next feed, projected food supply,
today's feeds and recent results. **Feed now** asks for confirmation, then displays the device's answer. **Hold**
pauses scheduled feeding; **Resume** cancels the hold. Manual feeding still passes through the guard.

**Tray reload** gives instructions for the current mechanism: close the servo tray before filling, or fill the disc
except for the slot over the chute before marking it reloaded. See [daily use](USING.md#everyday-use).

The banner below the header shows the most urgent reported issue, such as lost connection, unknown position,
calibration mode, empty tray, missing clock, insufficient food for the next meal or a hold.

## Schedule

View profiles A, B and C without changing which one runs. **Edit** sets an event's time or sun anchor, portions,
weekdays and enabled state. Set cadence and fast days for the profile; **Make active** chooses the plan to run.
One-shot feeds use the feeder's local time.

Saving disables a live event before changing its fields and enables it last, then checks what the feeder reports.
The editor is shared with other clients; avoid simultaneous edits from another browser, MQTT or BLE. A conflict
message names what to check. Details and examples: [SCHEDULING.md](SCHEDULING.md).

## Configuration

| Section | Controls |
|---|---|
| Feeding | Guard limits, reload pause, refill interval and location, using the device's limits and units |
| Time | Time zone by place or POSIX string, and up to three NTP servers; an empty value returns to the build's default |
| Network | Name, nearby networks or manual Wi-Fi entry, Forget network where supported, and setup AP password |
| Hardware | Drive, board preset and pins; only acceptable pins are listed, with None for an unfitted button or LED |
| Settings password | Set, unlock, lock, change or remove; unlocking lasts for the loaded page |
| Keys | Generate, enter or clear API/BLE keys where supported; BLE permissions and Forget pairings |
| MQTT | Enabled state, broker, port, login, prefix, connection status; TLS checking and stored pin where supported |
| HTTPS and certificates | Current certificate, fingerprint and expiry; upload/remove/regenerate, HTTP redirect, HTTPS only, trust store and ACME |
| Device | Restart when needed and reopen the setup wizard |

Changes save as you make them. Pins, drive, name and AP password need a restart. MQTT connection changes need one
once its client has started; turning MQTT off takes effect immediately. Certificate uploads and trust-store changes
need a restart; an ACME certificate is applied to new connections immediately. **Restart Needed** indicates pending
changes; nothing restarts automatically just because a setting changed.

Generated API and BLE keys are shown once. Passwords and private keys are never read back. A settings password
protects the listed sensitive changes; it does not lock everyday feeding or schedule editing. The available actions
depend on the build and on whether a key came from YAML. See [SECURITY.md](SECURITY.md), [MQTT.md](MQTT.md) and
[HTTPS.md](HTTPS.md) before setting up those connections.

## Maintenance

Calibrate an empty mechanism with the guided steps. The servo has pulse range and per-slot offsets; the disc has a
direction, zero, measured-turn and slot-trim walkthrough plus motion tuning. Follow [CALIBRATION.md](CALIBRATION.md).

After interrupted motion, declare the observed tray position or reload. **Portions Counted (24 h)** changes the
count used by the guard for debugging or recovery; it is protected and logged. It does not change the amount of food
physically in the feeder.

## Bluetooth

BLE builds show the connected client, authentication state, counters and the last 32 connections and writes. Only
the last two bytes of the client's address are shown. This page diagnoses the BLE interface; it does not connect
your browser to the feeder over Bluetooth. The app itself uses Wi-Fi.

## Diagnostics and firmware

Diagnostics shows the system, clock, network addresses, memory, live log and raw entities. Use those controls when
you need the exact firmware values; the normal pages provide the task-specific controls.

**Updates and firmware upload** shows the installed version and variant, an optional release check and an upload
control. The browser checks GitHub Pages for published releases. Use an `.ota.bin` and follow
[the update instructions](INSTALL.md#updates).

## What you see depends on your build

Missing packages hide their pages or cards: no BLE means no Bluetooth page; omitted diagnostics or network packages
remove those cards. The app follows what the device reports; settings are saved on the feeder, not in the browser.

A few open browsers can exhaust the device's connections. A background page releases its connection after 30
seconds and reconnects when opened again. HTTPS allows three simultaneous sessions by default. After a firmware
restart, an open page offers to reload so it uses the app bundled with the new firmware.

Calibration controls follow the running mechanism: the servo has its slot offsets and pulse range; the direct-drive
disc has its guided calibration and encoder settings. A combined image shows one calibration workflow.
