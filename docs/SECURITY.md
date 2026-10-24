# Security and permissions

The feeder is a local-network device. This guide explains which controls are open, which require credentials, and
what those credentials do not protect. Configure certificates with [HTTPS.md](HTTPS.md); BLE's wire authentication
is specified in [PROTOCOL.md](PROTOCOL.md).

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

## Set up access

Set an access-point password and a settings password in Configuration. Use HTTPS for the web app. If you use Home
Assistant, adopt the feeder or provision its API key promptly. If you use BLE, generate its separate key; if you use
MQTT, configure broker credentials and access rules.

The settings password protects sensitive changes. It is **not a login for the whole app**: anyone who can reach the
web interface can still request feeds, reload, hold, stop and edit schedules. The feed guard applies to every feed.
A YAML build can put the entire app behind an additional web login; see [BUILDING.md](BUILDING.md#keeping-visitors-out-of-the-web-app).

## Who can do what

| Path | Authentication | Access |
|---|---|---|
| Web app without a settings password | None | All available controls and settings |
| Web app with a settings password | Password proof for protected changes | Feeding, schedules, hold, reload, location, time zone and NTP remain open; guard limits, calibration, pins, keys, network, password and firmware changes are protected |
| Home Assistant / ESPHome API | API encryption key once provisioned | Everyday feeding controls and status; no protected settings or schedule editor |
| MQTT | Broker credentials and access rules | Everyday controls and status; schedule and one-shot editing; guard limits read only |
| BLE | Shared BLE key; authenticated session and tagged writes | Feeding, schedule, clock and position recovery; guard changes and servo calibration only when their permission switches allow them |
| ESPHome OTA | API key for encrypted upload; plaintext OTA refused | Firmware updates |
| USB | Physical access | Wi-Fi setup through Improv and firmware replacement |
| BOOT held 10 seconds | Physical access | Clear the settings password, leaving other settings intact |

BLE does not configure network, pins or keys. MQTT does not expose calibration, pins, drive, keys, network, firmware
or the diagnostic 24-hour count. That count can be edited only in the protected web Maintenance control, with a log
warning. See [MQTT.md](MQTT.md) for exact topics.

## The settings password

The browser derives a key from the password using PBKDF2-HMAC-SHA256, 20,000 rounds and a random salt supplied by the
feeder. The device stores the salt and derived key; the password itself is not sent.

Each protected request includes an HMAC-SHA256 proof covering a one-time challenge, path and value. A challenge lasts
one minute and can authorize one request. Wrong proofs impose a delay starting at one second and doubling to one
minute. Another website cannot construct a protected request without that proof; the custom header also requires
a CORS preflight the feeder does not grant.

New password-derived keys, API/BLE keys, Wi-Fi/AP passwords, MQTT passwords and uploaded HTTPS private keys travel
masked using the current settings password's derived key. With no settings password set, they travel without that
mask. **Use HTTPS**: masking and request proofs do not replace transport encryption, and HTTP traffic permits offline
password guessing. A long password makes guessing harder.

Firmware uploads authenticate the request and cover the image's SHA-256. The feeder checks integrity and rejects a
factory image before switching firmware. This does not establish a trusted publisher or prevent an authorized
owner from installing a different firmware.

## Keys

- **API key:** prebuilt firmware ships unprovisioned. Until a key is set, it accepts provisioning with the well-known
  all-zero Noise key and also permits plaintext API access. The first client that reaches it can set a key. Home
  Assistant can provision one on adoption; Configuration, Keys can also generate or enter one. Once provisioned,
  plaintext and other keys are refused. In a YAML build, a compiled API key is changed in YAML.
- **BLE key:** prebuilt firmware has none, so AUTH is refused until you set one. A device-saved key overrides a YAML
  key. Clearing it returns to the YAML key if present, otherwise refuses clients again; changing the key invalidates
  the session. The two BLE permission switches are on by default and can restrict guard changes and calibration.
- **Bluetooth bonds:** pairing encrypts the radio link but does not authorize commands; the BLE key does that.
  Pairing is optional, uses Just Works and keeps at most three bonds. **Forget pairings** removes bonds; change the
  BLE key as well when removing a client that knew it.
- Generated keys are made in the browser or by Home Assistant and shown once. The feeder never returns a stored key.

## Known limits

| Boundary | What to understand |
|---|---|
| Setup access point | A fresh install has no AP password. When the AP is up, anyone in range can join unless you set one; the captive portal and setup access remain plain HTTP. |
| Unprotected web controls | Another website can cause requests to ESPHome's open controls, including feeding. The guard limits the feed; the settings password protects only its own listed changes. |
| Nearby-network list | Anyone reaching the web interface can scan and read nearby Wi-Fi names. Joining or forgetting a network is protected. |
| Default HTTPS identity | The self-signed certificate is not automatically trusted. Verify the feeder's identity before accepting it, or install a certificate your clients trust. |
| HTTP redirect | Redirecting the app does not disable other HTTP routes. **HTTPS only** refuses them while HTTPS is running, except for the redirect and ACME challenge; it is suspended while the setup AP is up. |
| API provisioning | Trust on first use: provision a new feeder before an untrusted client can. |
| MQTT | Anyone your broker permits to publish commands can feed and edit schedules. The web settings password does not apply to MQTT. |
| BLE identity | Writes are authenticated; answers are not signed and the feeder does not prove its identity to the client. Pairing can encrypt the link but is not an independent identity guarantee. |
| Physical access | Flash is not encrypted. A person holding the board can read stored credentials, clear the settings password or replace firmware over USB. |
| Shared password challenge | Two clients making protected changes simultaneously can invalidate each other's challenge. Retry the change. |

**ESPHome's web upload (`/update`) is refused in every build.** Use the feeder's authenticated browser upload,
encrypted ESPHome OTA or USB instead.

## HTTPS, MQTT TLS and ACME

HTTPS encrypts browser traffic. The default certificate is self-signed; Configuration accepts an uploaded certificate
or a certificate issued and renewed by your own ACME server. HTTPS allows three simultaneous sessions by default.

MQTT TLS verifies the broker using the uploaded CA trust store and its host name, or pins a certificate on first
contact. Pinning assumes that first contact was genuine. With TLS enabled, a verification failure never falls back
to plaintext. The trust store also verifies outgoing requests to the ACME server; without a CA there, ACME does not
run. ACME uses an http-01 challenge on port 80, and generates its keys on the feeder. See [HTTPS.md](HTTPS.md).

## What the feeder talks to

The feeder contacts NTP servers and, if configured, your MQTT broker and ACME server. Home Assistant connects to it.
The browser performs the optional GitHub Pages release check and downloads; the feeder does not contact an update
service. A feeder without internet access can use a local clock source and local integrations.

Architecture and implementation details are in [DESIGN.md](../DESIGN.md), sections 9 and 10. Known bench gaps are
listed separately in [BENCH.md](BENCH.md) and DESIGN's open items; these protections are not a claim that every
failure mode has been tested on hardware.
