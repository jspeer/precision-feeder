# Security model

What protects the feeder, from whom, and where the limits are (firmware 2.0 to 2.4). The design and the reasons are in
[DESIGN.md](../DESIGN.md) sections 9.5, 9.7, 10 and 10.1; the BLE protocol's authentication is in
[PROTOCOL.md](PROTOCOL.md); setting up HTTPS and certificates is in [HTTPS.md](HTTPS.md).

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
| MQTT (with the `mqtt` package, 2.2.0) | the broker's login only | the same as Home Assistant; editing the schedule and one-shot feeds; the guard limits read only (since 2.3.0). Never calibration, pins, keys or network ([MQTT.md](MQTT.md)) |
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
  masked with a key derived from the current password, and since 2.4.1 so is an uploaded HTTPS private key. With no password set they cross the network as they are
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
- **BLE key:** none in the binaries; with no key every BLE client is refused (detail 6). Never an all-zero key. A key
  saved on the feeder can be cleared in the web app (2.3.0): clients are then refused again (a YAML build goes back to
  its own key), and a connected one is cut off. Two
  protected switches decide whether BLE may change the guard limits and the calibration (both on by default).
- **Bluetooth pairing** (bonding) only encrypts the link; the key is what lets a client act. Up to 3 bonds are kept.
  Forget pairings (Configuration, Keys, 2.4.3; protected) unpairs them all, for a lost or sold phone; change the BLE
  key too if that client knew it.
- **Keys are made in your browser** (or by Home Assistant) and shown once; the feeder never sends a key back.

## Known limits

- **The fallback access point has no password on a fresh install.** Whenever it is up (before Wi-Fi is set, or when
  Wi-Fi is lost), anyone in range can join it, reach the web app and the captive portal, and, with no settings
  password, change anything. Set an access-point password in the setup wizard.
- **The list of nearby Wi-Fi networks is not protected** (2.3.0): anyone who can reach the web app can see the names
  the feeder found and ask it to scan again, as ESPHome's captive portal shows them on the access point. Forgetting
  the network is protected.
- **HTTPS, but self-signed by default** (2.4.1). With the `tls` package (in the prebuilt firmware) the web app is
  at `https://<name>.local`, and port 80 sends the app's page there (HTTPS Redirect). The feeder's certificate is
  one it made itself, so your browser warns until you accept it; upload a certificate from your own CA, or let the
  feeder get one from your ACME server (2.4.2, below), to avoid that. Until you turn on **HTTPS Only**, the rest of port 80 still answers in plain HTTP (an app opened there, older
  bookmarks, scripts): someone recording your network can see what crosses it, and can try to guess the settings
  password offline from a challenge and its answer; PBKDF2 slows that down, but a short password falls. Use a long
  one. HTTPS Only never applies while the access point is up, so the setup wizard and captive portal stay plain HTTP.
- **Home Assistant's "Visit" link is http.** Home Assistant builds it as `http://<address>:80` itself; the redirect
  takes the browser on to HTTPS.
- **MQTT over TLS checks the broker** (2.4.1, `mqtt_tls`): against a CA certificate you upload, or against the
  certificate the broker showed the first time ("Pin on first contact"; like SSH, safe if nobody impersonates the
  broker on that first contact). With TLS on, MQTT never falls back to plain text.
- **ACME** (2.4.2, `acme`, in the prebuilt firmware; off until a directory is set): the feeder asks the ACME server
  you name (step-ca on your LAN, say) for its certificate and renews it by itself. It is the feeder's only outgoing
  HTTPS, and the server is always checked against the trust store; without a CA there, ACME does not run. Its keys
  (the account's and the certificate's) are made on the feeder and never leave it. The challenge is answered on port
  80, so the CA must reach the feeder's port 80; the answer is served only for the one request being made.
- **Unprotected controls can be pressed from other websites.** ESPHome's web server allows cross-site requests, so a
  page you visit could, for example, press Feed One Portion on the feeder. The feed guard still applies; protected
  settings still need the password (DESIGN.md section 14, item 12).
- **No flash encryption.** Keys, HTTPS's private key and the settings password's derived key are in flash in the
  clear; anyone holding the board can read them.
- **One challenge at a time** for the whole feeder: two pages making protected changes in the same second can
  spoil each other's challenge; the app's next try works.
- **MQTT is as safe as your broker.** With the `mqtt` package set up, anyone your broker lets publish to the
  feeder's topics can feed (through the feed guard) and edit the schedule. Nothing else: the safety limits are only
  read (2.3.0), and calibration, pins, keys and the network are never on MQTT (2.2.0). The
  settings password does not apply over MQTT. Give the broker accounts and access rules, and do not use an open
  broker. The MQTT settings themselves (broker, login) can only be changed from the web app, and the password is never
  sent back. Since 2.4.5 it travels masked by the settings password, like the access point's password.
- **ESPHome's own web upload (`/update`) is refused.** Before 2.4.5 the captive portal still took a firmware image
  there while the access point was up, with no settings password; it is refused in every build now. Firmware goes
  only through the signed upload, ESPHome's encrypted OTA, or USB.

## What the feeder talks to

Only NTP (time) and, if you set them up, your MQTT broker and your ACME server; Home Assistant connects to the
feeder. The update check runs in your browser, which reads a manifest
from GitHub Pages; the feeder itself never contacts the internet for updates, so a feeder blocked from the internet
loses nothing.
