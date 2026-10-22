# Installing the prebuilt firmware

From firmware 2.0 the feeder is an appliance: flash a release from a web page, then set it up in its own web app.
Nothing to build. If you would rather build your own from YAML (the advanced path, with Home Assistant's ESPHome
add-on or ESPHome on a computer), see [BUILDING.md](BUILDING.md).

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

## What you need

- An ESP32-S3 board with 4 MB of flash or more (the Waveshare ESP32-S3-Zero is the reference; an
  ESP32-S3-DevKitC-1 works too). No PSRAM needed.
- An MG90S servo (or similar) and the printed Precision Feeder Rev2; the push button is optional.
- A USB cable that carries data, and Chrome or Edge on a computer (Web Serial). Phones and Safari or Firefox
  cannot flash.
- A Wi-Fi network that hands out IPv4 addresses (DHCP). Dual-stack and IPv4-only networks are fine; an IPv6-only
  network is not (see Limits).

## Choosing a variant

| Variant | What it has | App slot used (of 1.8 MB) | RAM used at boot |
|---|---|---|---|
| `full` | everything, including the BLE control plane for a PLC or a phone app | 88.5% (1,623,115 bytes) | 48.2% |
| `no-ble` | Wi-Fi, the web app, Home Assistant and MQTT; no Bluetooth | 70.4% (1,291,243 bytes) | 39.0% |

Both carry both drives (the servo tray, which they start with, and the direct-drive disc: choose it in Configuration,
Drive) and MQTT (off until you set a broker in Configuration, MQTT). Measured for 2.1.0 with ESPHome 2026.9.1 on the
4 MB layout (static RAM; 2.0.2 used 79.1% and 61.1%). Both images run on larger flash too. Each comes as
`precision-feeder-<variant>-<version>.factory.bin` (the first flash, written from address 0: bootloader, partition
table and app) and `.ota.bin` (updates through the web app). You can switch variants later by uploading the other
one's `.ota.bin`; settings are kept.

## Install

1. Open the install page, **<https://jspeer.github.io/precision-feeder/>**.
2. Plug the board in. The S3-Zero has no USB-serial chip: if it does not show up, hold **BOOT** while plugging it
   in, then let go.
3. Click **Connect** under the variant you want, pick the board's port (often "USB JTAG/serial debug unit"), then
   **Install**.
4. **Erase or not?** On a new board, erase. On a feeder that already runs this firmware (or a 1.x YAML build), do not
   erase if you want to keep its settings, calibration, schedule and tray state.
5. After flashing, the page offers to **set up Wi-Fi** over the same cable (Improv). Choose your network; the page
   then links to the feeder.

If you skip the Wi-Fi step, or the feeder cannot join, it opens its own access point,
`precision-feeder-<6 hex digits>-setup`, **with no password**. Join it from a phone and the setup page opens; pick
your network there.

## Setup wizard

The feeder's address (`http://precision-feeder-<6 hex digits>.local`, or the IP address the installer or your router
shows) opens a setup wizard on a new feeder. Each step saves as it goes and can be skipped; Configuration and
Maintenance hold the same settings later, and Configuration (Device) starts the wizard again.

1. **Network:** the name (`precision-feeder-a1b2c3` until you change it; used after a restart), another Wi-Fi network
   if needed, and the **access point password. Set one.** Until you do, anyone in range can join the access point
   whenever it is up (no Wi-Fi yet, or Wi-Fi lost) and change the feeder's settings.
2. **Time zone and location:** choose your place; the location is needed for sunrise and sunset events. Optionally,
   a time (NTP) server on your network (2.0.2); otherwise the public pool servers are used.
3. **Board and pins:** a preset (Waveshare ESP32-S3-Zero, ESP32-S3-DevKitC-1) or each pin; "none" for a button or LED
   that is not fitted. Pins apply after a restart.
4. **Keys:** the Home Assistant (API) key and the Bluetooth key (`full` only). See below.
5. **Settings password:** optional; protects the safety limits, calibration, pins, keys, network and firmware from
   changes through the web app. See [SECURITY.md](SECURITY.md).
6. **Calibration:** the tray walkthrough ([CALIBRATION.md](CALIBRATION.md)).

## Home Assistant and Bluetooth

- **Home Assistant** finds the feeder (ESPHome integration) and gives it an encryption key when you add it ("adopt"
  it). Adopt it promptly: until a key is set, whoever reaches it first can set one. You can also make or enter a key
  in the web app (Configuration, Keys) and give that key to Home Assistant. Updates over ESPHome's own OTA then use
  this key; plaintext OTA is refused.
- **What Home Assistant sees** (2.2.0): the everyday controls and status (feed, reload, stop, hold, scheduler, active
  profile, portions remaining, tray state, next and last feed, faults, needs food, firmware version). Settings,
  calibration, pins and the schedule editor are in the web app. A feeder upgraded from 2.1 leaves the other entities
  behind in Home Assistant as unavailable: remove them, or delete the device and adopt it again.
- **MQTT** (2.1.0; 2.2.0 scopes): instead of or beside the ESPHome integration, set your broker in the web app
  (Configuration, MQTT); Home Assistant's MQTT integration then discovers the same entities, and the schedule has its
  own topics ([MQTT.md](MQTT.md)). Use a broker with accounts ([SECURITY.md](SECURITY.md)).
- **Bluetooth** (`full`): the feeder refuses every BLE client until a key is made in the web app (Configuration,
  Keys; shown once). Give that key to your PLC bridge or the Python client. The BLE name is `Feeder-<6 hex digits>`.

## Updates

The web app's Diagnostics page links to **Updates and firmware upload**. Opening it, your browser (not the feeder)
asks GitHub Pages for the latest release and says "up to date", "version x is available" (with the release notes),
or "couldn't check". The check can be turned off on that page, per browser.

To update, download the `.ota.bin` for your variant from the
[releases page](https://github.com/jspeer/precision-feeder/releases) and choose it there. The page checks the file
against the release and refuses a `.factory.bin`; with a settings password set it asks for it. The feeder checks the
file's SHA-256 again before switching, and restarts once the tray is not moving. Settings and calibration are kept.
A failed or cut upload leaves the running firmware in place.

## Moving from your own YAML build

Flash a binary over it **without erasing** (install page, or an upload from a 2.0 web app). Settings, calibration,
schedule and tray state are kept. What your YAML held does not come along:

- the Wi-Fi network: set it again (the installer asks; or the access point);
- the Home Assistant key: let Home Assistant adopt the feeder again, or enter your old key in the web app;
- the Bluetooth key: make a new one, or enter your old one, in the web app.

The feeder's name changes to `precision-feeder-<6 hex digits>` unless you set one in the web app.

## Recovery

| Problem | Fix |
|---|---|
| Forgot the settings password | Hold **BOOT** for 10 seconds while the feeder runs. It clears the password and nothing else. |
| Joined the wrong Wi-Fi, or the network changed | Wait: when it cannot join, it opens its access point (`...-setup`) after a minute or two; join it and pick the network. Or plug it into a computer and use the install page's Connect, then "Change Wi-Fi" (Improv), no reflash needed. |
| Forgot the access point password | Plug it into a computer and set Wi-Fi over USB (as above); then change the password in the web app. |
| An update will not start | The feeder falls back to the firmware it had (ESPHome's rollback). If it does not come back, flash the factory image from the install page; do not erase, to keep your settings. |
| Start over completely | Flash from the install page **with erase**. Everything is wiped: settings, calibration, Wi-Fi, keys, password. |
| Tray position unknown after a power cut mid-feed | Maintenance: declare the position, or Tray Reload. See [USING.md](USING.md#safety-behaviour). |

There is no factory reset in the web app yet; erase-and-flash is the way to wipe a feeder.

## Limits

- **IPv6-only networks:** the binaries cannot join one. ESPHome counts Wi-Fi as connected only with an IPv4 address,
  and a static IPv4 address can only be set in a YAML build (`manual_ip`). Use a YAML build there.
- The update check needs the browser, not the feeder, to reach the internet: a feeder kept off the internet loses
  nothing, and downloads come from github.com, which has no IPv6.
- The feeder makes no outbound connections except NTP (and Home Assistant, if you use it).
