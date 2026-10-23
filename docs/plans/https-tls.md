# Plan: HTTPS, MQTT over TLS and certificates (experiment)

Status: **HTTPS, the trust store and MQTT over TLS built in firmware 2.4.1, ACME in 2.4.2** (DESIGN.md section 9.7,
D36 to D40; compiled, not yet on hardware); the web app's page is 2.4.3. Branch `experiment/https-tls`, from main at
2.3.1. This document records what a measurement experiment of 2026-10-23 found and the design proposed from it; what
was built and decided is in [DESIGN.md](../../DESIGN.md).

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude); reviewed by a human, but it may still
contain mistakes.

## Premise

Two security audits found that MQTT is plain text. [SECURITY.md](../SECURITY.md) also lists plain HTTP as a known
limit: someone recording the network sees feeding commands and settings, and can try to guess the settings password
offline from a challenge and its answer. Both can be fixed with TLS, which raises three questions: does it fit in
the 4 MB flash with the full build, does it run out of memory, and does anything else break (Home Assistant, the
ESPHome API, OTA, the captive portal).

The experiment answered the first question with numbers, and the second for the extra memory TLS itself takes, in an
emulator. Free heap on the board itself, with Wi-Fi and BLE running, still needs the bench.

## What the experiment found

### How it was measured

- ESPHome 2026.10.0 (ESP-IDF 5.5.6, mbedTLS 3.6.7), shaped like `firmware/full.yaml`. App slot 1,835,008 bytes.
- Throwaway components outside the repo (a scratch folder): an HTTPS server, a TLS proxy, MQTT over TLS, upload
  checks, an ACME client and a NimBLE peripheral. Each was realistic enough to link what the feature needs, and
  switched on by a flag, so every step was its own build.
- Heap: Espressif's QEMU (`qemu-xtensa` esp_develop_9.2.2_20260417, machine `esp32s3`) running the full image with
  Wi-Fi swapped for the emulated Ethernet (`open_eth`), a task sampling the internal heap every 50 ms, and
  `heap_caps_get_minimum_free_size()` for the exact low point. A script on the Mac loaded the web app over HTTPS
  through forwarded ports. QEMU has no Wi-Fi or BLE radio, so it shows **what TLS adds**, not the board's total.
  Its timings are not the chip's.

### Facts from ESPHome (2026.10.0)

| Fact | Where |
|---|---|
| ESPHome's IDF web server is built on ESP-IDF's `esp_http_server`; `esp_https_server` is a thin TLS layer over the same daemon | `web_server_idf.cpp`, `httpd_start` |
| ESPHome leaves `esp_https_server`, `esp-tls` and `esp_http_client` out of the build by default; a component brings them back with `include_builtin_idf_component()`, `request_tls()`, `request_http_client()` | `esp32/__init__.py` |
| ESPHome builds mbedTLS lean on purpose (client only, some features off); a component widens it with `require_mbedtls_tls_server()`, `require_mbedtls_tls_extras()`, `require_mbedtls_peer_cert()` | `esp32/__init__.py` |
| The MQTT component already links the TLS client ("esp-mqtt links transport_ssl.c (esp_tls) even for plain MQTT") | `mqtt/__init__.py` |
| ESPHome's MQTT uses TLS only when a CA is set; with none it falls back to plain TCP. It has no "skip verification" option, only `skip_cert_common_name_check`. The CA can be set at run time (`set_ca_certificate()`) | `mqtt_backend_esp32.cpp` |
| ESPHome's `esp32_ble` is Bluedroid only: it calls Bluedroid's API (`esp_ble_gap_*`, `esp_gatts_*`). NimBLE means our own adapter | `esp32_ble/` |
| Any ESP-IDF option can be set from the YAML (`esp32: framework: sdkconfig_options:`), as `core.yaml` already does for SNTP | `packages/core.yaml` |
| The sockets are one pool for the whole device, sized from what components declare (`socket.consume_sockets()`); today 14 | `socket/__init__.py` |
| `ethernet: type: OPENETH` does not compile for the ESP32-S3 (EMAC types in the non-SPI branch). Only matters for emulation | `ethernet_component.h` |

### Flash

| Build | Flash (bytes) | Slot | Step |
|---|---|---|---|
| `full` today (Bluedroid) | 1,634,067 | 89.0% | |
| `no-ble` | 1,284,431 | 70.0% | BLE as a whole: 349,636 |
| NimBLE stub instead of Bluedroid (10 characteristics, Secure Connections bonding, bonds in NVS) | 1,479,999 | 80.7% | -154,068 (about -140 KB once our adapter and BleEngine are back in) |
| + HTTPS server, self-signed certificate made on the device | 1,499,195 | 81.7% | +19,196 |
| + MQTT over TLS (CA from NVS), upload checks (certificate, key, chain) | 1,499,855 | 81.7% | +660 |
| + ACME client (account key, JWS ES256, CSR, http-01, chain check) | 1,520,959 | 82.9% | +21,104 |
| + session tickets, lean buffers | 1,527,547 | 83.2% | +6,588 |
| + TLS 1.3 | 1,568,631 | 85.5% | +41,084 |
| + common CA bundle | 1,588,331 | 86.6% | +19,700 |
| full CA bundle instead | 1,639,395 | 89.3% | +51,064 more |
| TLS proxy instead of `esp_https_server` | 1,585,975 | 86.4% | -2,356 |
| Bluedroid + everything (TLS 1.3, common bundle) | 1,741,523 | 94.9% | static RAM 165,327 |

Flash is not the wall: everything fits on 4 MB, even with Bluedroid. With NimBLE and TLS 1.2 only, the whole set
leaves about 290 KB of the slot. An 8 MB board is not needed for this.

### Heap (QEMU, what TLS adds)

| | TLS 1.3 build | TLS 1.2-only build |
|---|---|---|
| One session, peak | about 37 KB | about 24 KB |
| Heaviest parallel phase | about 112 KB (4 sessions, the cap) | about 61 KB |
| 100 handshakes in a row | 100/100, heap back to its level | 100/100, heap back to its level |
| Session resumption (tickets) | did not resume | resumes |
| A TLS 1.2 client | handshake stalls until the client gives up | works |
| A TLS 1.3 client | works (`TLS_AES_256_GCM_SHA384`) | clean refusal |

Against the board's last measured free heap (about 145 KB with the page open, Bluedroid; docs/BENCH.md, "Several
browsers at once"):
TLS 1.3 with four sessions would leave about 33 KB, too little; TLS 1.2 about 80 KB. NimBLE should give some RAM
back (static RAM 6 KB less; its heap use is not measured).

### On the board (M0, 2026-10-23)

The disc rig, scratch images with the TLS proxy (TLS 1.2 only, tickets, lean buffers, at most 4 sessions), each
session holding `/events` as an open page does. Details in [docs/BENCH.md](../BENCH.md), "HTTPS, ACME and the BLE
stacks on the disc rig".

| BLE in the image | Free at idle | Lowest, 3 sessions | Lowest, 4 sessions |
|---|---|---|---|
| None | about 201 KB | 137 KB | 127 to 129 KB |
| Bluedroid, today | about 110 KB | 44 to 52 KB | 11 to 33 KB (3 KB once) |
| Bluedroid, controller trimmed | about 125 KB | 67 to 69 KB | 51 to 52 KB |
| NimBLE, trimmed | about 143 KB | 86 to 88 KB | 68 to 73 KB |

- A TLS session costs about 15 KB on top of the plain connection it relays, close to QEMU's figure. BLE costs much
  more (58 to 91 KB, see [nimble.md](nimble.md)).
- **Four sessions are not safe on today's build; three are.** With the controller trim or NimBLE, four are.
- Handshakes take 0.33 to 0.6 s (0.7 to 1.4 s with four at once); resumption works (0.1 s); 100 handshakes leak
  nothing.
- **ACME works against a local step-ca** (http-01, the board's mDNS name): 23.7 s to a certificate the board then
  serves with its intermediate, verified by curl against the root alone. It needed a firewall rule from the IoT
  network to the CA, and mDNS on the CA host. step-ca's default lifetime is 24 hours, so renewal runs about every 16
  hours and must be cheap and quiet.
- Not measured: MQTT over TLS, PSRAM for the TLS buffers.

### Other findings

- **Parallel requests are reset on plain HTTP too.** Six or ten at once to ESPHome's own server on port 80: some are
  reset, and its log shows `httpd_accept_conn: error in accept`. Not caused by TLS. The board behaves alike:
  ESPHome closes the least recently used connection when its sockets run out, and about four pages stay connected
  at once (docs/BENCH.md, "Several browsers at once"). TLS sessions come out of the same budget.
- **The proxy costs two sockets per session** (the browser's, and its own to 127.0.0.1:80). Declared with
  `socket.consume_sockets()`, the pool went from 14 to 24.
- **Idle connections hold TLS slots.** A browser keeps connections open; with a session cap, an idle one blocks the
  next until it times out. The real version needs a short idle timeout.
- **A certificate serial number needs care.** Random bytes with only the top bit cleared made an invalid certificate
  when the first byte came out zero (DER forbids the leading zero byte); OpenSSL refused it. The real code forces a
  nonzero top byte and has a test.
- **PSRAM for TLS buffers is inconclusive.** With `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` the buffers moved to PSRAM, but
  every handshake then hung in QEMU. It may be QEMU's emulation of the crypto hardware; only the board can tell.
- **Making the self-signed certificate** (P-256 key and certificate, SAN with the name) took 0.2 to 0.4 s of
  emulated time; on the chip it should be shorter (hardware ECC and SHA).
- **A latent bug in our code:** `precision_feeder.cpp` includes `nvs.h` only under `USE_WIFI` but calls NVS outside it.
  Harmless while every build has Wi-Fi; worth fixing in passing.

### What TLS does not touch

- **Home Assistant** uses the native API (port 6053, Noise encryption) and MQTT, not the web server.
- **ESPHome OTA** (port 3232, encrypted) is separate. The web app's firmware upload (`POST /feeder/firmware`) works
  unchanged through the proxy.
- **The captive portal** must stay plain HTTP: phones probe for it over HTTP.

## Proposed design

### Trust store (whom the feeder trusts as a client)

- **One custom CA slot**, uploaded, replaced or cleared in the web app, signed with the settings password like the
  keys. Saved in NVS.
- **Optionally the common public CA bundle** (ESP-IDF's, +19.7 KB), for an ACME server or broker with a public
  certificate.
- Used by MQTT (checking the broker) and the ACME client (checking the ACME server).

### MQTT over TLS

- New settings beside the broker: TLS on or off, the port (8883 by default with TLS), and **Allow insecure TLS**.
- With TLS on, the broker is checked against the store (CA slot, then the bundle if built in).
- **Allow insecure TLS** encrypts without checking the broker. ESPHome's MQTT has no such option (see the facts); it
  needs either a small change upstream, or our own transport. Proposed instead (E5): pin the broker's certificate on
  first contact, so "insecure" means "trust what you saw first", not "trust anything".

### HTTPS identity (who the feeder is to browsers)

One of three, chosen in the web app (Configuration, Certificates):

1. **Self-signed** (the default and the fallback): a P-256 key and certificate made on the device on first boot,
   with the feeder's name (`<name>.local`) in the certificate's names. Made again when the name changes, or on
   request. Fixed long validity, because the feeder has no clock at first boot.
2. **Uploaded**: certificate, key and chain. The key travels masked with the settings password (as the API key and
   Wi-Fi password do, `password_unmask_text()`) and signed. The feeder checks the key matches the certificate and the
   chain verifies against the CA slot before using it.
3. **ACME** (RFC 8555), for example step-ca on the LAN: an account key and a certificate key made on the device (they
   never leave it), the http-01 challenge served on port 80 at `/.well-known/acme-challenge/<token>`, the chain
   checked against the store. Renewal starts at about two thirds of the lifetime, only with a valid clock. A failed
   renewal keeps the current certificate and raises a fault; an expired one falls back to the self-signed one. Never
   a reboot.

The step-ca server has to resolve the feeder's name: the owner's step-ca VM resolves `.local` (mDNS) and its policy
can allow `*.local`. For development, a local step-ca can run on the Mac and be stopped afterwards.

### Serving HTTPS

- **The TLS proxy** (route B of the experiment): our own component listens on 443, does TLS with `esp-tls`, and passes
  plain bytes to ESPHome's server on 127.0.0.1:80. ESPHome is untouched; every handler (the app, `/events`, the
  settings guard, the firmware upload) works as it is. Fresh code, MIT.
- At most 3 or 4 sessions at once (a semaphore; a connection beyond waits), a short idle timeout, sockets declared.
- **Port 80 stays** for the captive portal and the ACME challenge. Optional **HTTPS only**: the settings guard refuses
  requests to port 80 that do not come from 127.0.0.1, except the captive portal while the access point is open and
  the ACME challenge path.
- Considered and not proposed: a modified copy of ESPHome's `web_server_idf` started with `httpd_ssl_start` (one
  socket per session, no relay). ESPHome's C++ is GPLv3, so the copy cannot live in this MIT repo. A change upstream
  (TLS as an option of ESPHome's web server) would be the cleaner end state; the proxy works without waiting for it.

### TLS settings

- TLS 1.2 only, ECDHE-ECDSA with P-256 and AES-GCM, session tickets on.
- Receive buffer 16 KB (browsers do not negotiate smaller records), send buffer 4 KB, dynamic buffers.
- No RSA keys.

### Packaging

- A package, `packages/tls.yaml`, that compiles out cleanly: HTTPS, the trust store and MQTT over TLS. ACME and the CA
  bundle as options of it.
- New saved records (certificate, key, CA, ACME account) under new preference keys; none of today's records change.
- The keys are in flash in the clear, as every key is today (no flash encryption, SECURITY.md).

### The web app

- A Certificates section in Configuration: the identity in use (source, names, expiry), upload, ACME (directory URL,
  status, renew now), the CA slot, and the MQTT TLS settings.
- Over HTTPS the page is a secure context, which gives it `crypto.subtle` and the browser's geolocation (the setup
  wizard types the location in because of HTTP; DESIGN.md section 13, 2.0.0 N6). The app keeps its own SHA-256 and
  PBKDF2 for HTTP.

## Decisions for the owner

| # | Proposal |
|---|---|
| E1 | TLS 1.2 only. TLS 1.3 costs 41 KB of flash and about 13 KB more heap per session, and in the experiment it broke TLS 1.2 and resumption. Every current browser supports TLS 1.2 with ECDHE and AES-GCM |
| E2 | The TLS proxy, not a copy of ESPHome's server. Raise TLS in ESPHome's web server upstream separately |
| E3 | Port 80 stays (captive portal, ACME challenge). "HTTPS only" is an option, off by default |
| E4 | One custom CA slot, shared by MQTT and ACME. The common CA bundle: in or out (+19.7 KB)? |
| E5 | "Allow insecure TLS" for MQTT pins the broker's certificate on first contact instead of trusting anything. Which way it is built (upstream change or our own transport) is decided in M0 |
| E6 | HTTPS identity: self-signed by default, uploaded, or ACME (http-01 only; no dns-01, which would store DNS credentials on the feeder) |
| E7 | **ACME reverses part of a logged decision.** The 2.0.0 appliance decision (DESIGN.md section 13) says the feeder "makes no update requests and has no HTTPS client". ACME needs an HTTPS client, talking only to the ACME server the owner configures. The no-update-requests part stays |
| E8 | At most 3 TLS sessions on Bluedroid as it is today (4 went down to 3 KB free on the board), 4 with the controller trim or trimmed NimBLE; a short idle timeout |
| E9 | NimBLE: port the BLE adapter to NimBLE in its own plan, [nimble.md](nimble.md) (BleEngine and the protocol stay; only the radio side changes). Not needed for TLS on flash, but it gives back about 140 KB of flash, and forum figures (unverified) claim about 100 KB of RAM, nearly all heap. It comes after M1 and before M2 if M0 confirms the heap gain (see "Order") |
| E10 | PSRAM for TLS buffers only if the board's heap needs it, and only after it is shown to work on the board |

## Order (with the NimBLE plan)

1. **M0 of both plans in one bench run.** Done 2026-10-23 ("On the board" above).
2. **The Bluedroid controller trim** ([nimble.md](nimble.md), B10), then **M1, MQTT over TLS.** The audit finding; about 1 KB of flash and one session, so it fits with either BLE stack.
3. **The NimBLE port** ([nimble.md](nimble.md), M1 to M5): M0 shows it is worth it, trimmed.
4. **M2 to M6, HTTPS**, tuned once on the BLE stack that ships.

If NimBLE frees little and Bluedroid has room, M2 follows M1 directly. If neither leaves room, PSRAM (E10) is the
question. If NimBLE frees about what the forum figures say, TLS 1.3 (E1) may be affordable, once its TLS 1.2 stall
is fixed.

## Steps (once the decisions are made)

Each step one commit, `tools/check.sh` green, DESIGN.md and the guides updated in the same change.

- **M0 Facts on the board (bench, the owner's OK to flash; run with [nimble.md](nimble.md)'s M0).** The lab builds of this experiment on the bench board:
  free heap with Wi-Fi, BLE, MQTT and the page open, then with 1, 3 and 4 HTTPS sessions; parallel requests on plain
  HTTP; PSRAM for the TLS buffers. How MQTT's "insecure" can be done (E5). No repo code.
- **M1 Trust store and MQTT over TLS.** The CA slot (upload, clear, signed), MQTT TLS on, port, pinning. A test
  broker with TLS (Mosquitto with a scratch CA). Host tests for the certificate checks.
- **M2 Self-signed HTTPS.** The certificate made on the device, the TLS proxy, the session cap and idle timeout, the
  serial number fix with a test. Browsers: Chrome, Firefox, Safari on macOS and iOS, Chrome on Android.
- **M3 Uploaded certificate.** Upload with the key masked; the checks (key matches, chain verifies).
- **M4 ACME.** Against a local step-ca; renewal, failure, expiry and fallback. ACME logic in pure code, host-tested
  against recorded server answers.
- **M5 HTTPS only (option), the web app's Certificates section, docs** (SECURITY.md, WEB_UI.md, MQTT.md, DESIGN.md),
  version and CHANGELOG.
- **M6 Bench.** The whole set on the board, a 24 hour soak with the page open and MQTT over TLS.

## Not in this plan

- The NimBLE port (E9): its own plan, [nimble.md](nimble.md).
- dns-01 and any other ACME challenge.
- Client certificates for MQTT (mutual TLS): the backend supports them; add only if a broker needs it.
- A different MCU: not needed on flash; revisit only if M0 shows the heap cannot hold it.

## Risks

| Risk | Mitigation |
|---|---|
| Not enough heap on the board with Wi-Fi, BLE, MQTT TLS and HTTPS together | M0 measures before anything is built; TLS 1.2 only; the session cap; PSRAM (E10) and NimBLE (E9) as fallbacks |
| A TLS handshake starves the feeder's work | The proxy and ACME run in their own tasks at low priority; M6 checks feed timing and the disc's control loop |
| Browsers warn on the self-signed certificate | Expected; the docs say how to trust it, and ACME or an upload remove it |
| A certificate or key that does not work locks the owner out of HTTPS | Port 80 stays unless "HTTPS only" is on; a failed check refuses the upload; the self-signed certificate is always the fallback |
| Renewal fails silently | A fault and a status entity; the current certificate stays in use until it expires, then the self-signed one |
| Too few connections for a browser over TLS | The board already closes the least recently used connection on HTTP (docs/BENCH.md); M0 measures how many TLS sessions fit beside it, and the app's hidden-page release (30 s) frees them |
