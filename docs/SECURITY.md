# Security model

What protects the feeder, from whom, and where the limits are (firmware 2.0). The design and the reasons are in
[DESIGN.md](../DESIGN.md) sections 9.5, 10 and 10.1; the BLE protocol's authentication is in
[PROTOCOL.md](PROTOCOL.md).

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

## The short version

- **Feeding is always open** to anyone who can reach the web app: feed, hold, schedule, tray reload, location, time
  zone and NTP servers. The
  feed guard (minimum gap, daily cap, portions per command) limits what any source can do, whoever asks.
- **Everything that could hurt the animals or take the feeder over** (the guard limits, calibration, pins, keys,
  Wi-Fi and name, the password itself, firmware) can be protected with a **settings password**. None is set by
  default: then anyone on your network can change them, as with most home devices (WLED, for example).
- **Home Assistant and Bluetooth have their own keys** and are not affected by the settings password.
- **Physical access is the final trust boundary.** Holding BOOT clears the password; USB reflashes the board.

## Who can do what

| Path | Authenticated by | Can do |
|---|---|---|
| Web app, no settings password | nothing (being on the network) | everything |
| Web app, settings password set | the password, for protected changes | everything else without it |
| Home Assistant (ESPHome API) | the API encryption key (Noise) | since 2.2.0, the everyday controls and status only: feeding, reload, stop, hold, the scheduler and active profile, and the status (DESIGN.md section 7); nothing protected |
| MQTT (with the `mqtt` package, 2.2.0) | the broker's login only | the same as Home Assistant; editing the schedule and one-shot feeds; the guard limits only while MQTT Can Change Guard Limits is on (off by default). Never calibration, pins, keys or network ([MQTT.md](MQTT.md)) |
| ESPHome OTA | the API key (encrypted); plaintext refused | firmware |
| Bluetooth (`full` only) | the BLE key; every write signed | feeding, schedule, clock; guard limits and calibration only while their two switches are on. Never pins, keys or network |
| USB cable | physical access | Wi-Fi over Improv, a full reflash |
| BOOT button, held 10 s | physical access | clears the settings password, nothing else |

## The settings password

- **Never crosses the network.** Your browser turns it into a key (PBKDF2-HMAC-SHA256, 20 000 rounds, a random salt
  kept on the feeder); the feeder stores only the salt and that key.
- **Every protected change is signed** over a one-time challenge from the feeder (HMAC-SHA256 of the challenge, the
  path and the value), so a recorded request cannot be replayed or moved to another value. A challenge lasts a
  minute and answers one request.
- **Wrong guesses wait:** 1 s after the first failure, doubling to 60 s.
- **New secrets travel masked:** a new password key, an API or BLE key, the Wi-Fi and access-point passwords are
  masked with a key derived from the current password. With no password set they cross the network as they are
  (anyone there could set their own anyway).
- **Another website cannot use your browser** to make a protected change: the signature header needs the browser's
  permission (a CORS preflight), which the feeder does not grant.
- **Firmware uploads** are signed over the image's SHA-256; the feeder checks the image against it, and refuses a
  first-flash (`.factory.bin`) image, before switching.

## Keys

- **API key (Home Assistant):** the prebuilt binaries ship with none. Until one is set, the feeder accepts the
  well-known all-zero Noise key (and, deprecated, plaintext), so Home Assistant can give it one on adoption. **Trust
  on first use:** whoever reaches it first can set the key, so adopt a new feeder promptly. You can also make one in
  the web app. Once set, plaintext and other keys are refused.
- **BLE key:** none in the binaries; with no key every BLE client is refused (detail 6). Never an all-zero key. Two
  protected switches decide whether BLE may change the guard limits and the calibration (both on by default).
- **Keys are made in your browser** (or by Home Assistant) and shown once; the feeder never sends a key back.

## Known limits

- **The fallback access point has no password on a fresh install.** Whenever it is up (before Wi-Fi is set, or when
  Wi-Fi is lost), anyone in range can join it, reach the web app and the captive portal, and, with no settings
  password, change anything. Set an access-point password in the setup wizard.
- **Plain HTTP.** The feeder serves no HTTPS. Someone recording your network can see feeding commands and settings
  being read, and can try to guess the settings password offline from a challenge and its answer; PBKDF2 slows that
  down, but a short password falls. Use a long one.
- **Unprotected controls can be pressed from other websites.** ESPHome's web server allows cross-site requests, so a
  page you visit could, for example, press Feed One Portion on the feeder. The feed guard still applies; protected
  settings still need the password (DESIGN.md section 14, item 12).
- **No flash encryption.** Keys and the settings password's derived key are in flash in the clear; anyone holding
  the board can read them.
- **One challenge at a time** for the whole feeder: two pages making protected changes in the same second can
  spoil each other's challenge; the app's next try works.
- **MQTT is as safe as your broker.** With the `mqtt` package set up, anyone your broker lets publish to the
  feeder's topics can feed (through the feed guard) and edit the schedule, and, with MQTT Can Change Guard Limits on,
  change the safety limits. Nothing else: calibration, pins, keys and the network are never on MQTT (2.2.0). The
  settings password does not apply over MQTT. Give the broker accounts and access rules, and do not use an open
  broker. The MQTT settings themselves (broker, login) can only be changed from the web app, and the password is never
  sent back.
- **ESPHome's own web upload (`/update`) is off.** Firmware goes only through the signed upload, ESPHome's encrypted
  OTA, or USB.

## What the feeder talks to

Only NTP (time) and, if you use them, Home Assistant and your MQTT broker. The update check runs in your browser, which reads a manifest
from GitHub Pages; the feeder itself never contacts the internet for updates, so a feeder blocked from the internet
loses nothing.
