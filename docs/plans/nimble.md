# Plan: BLE on NimBLE instead of Bluedroid (experiment)

Status: **built as firmware 2.4.0**, advertising fixed in 2.4.4 (D42: the controller trim had stopped it; not tagged; M4,
the bench, still to do), on branch `experiment/https-tls`, from main
at 2.3.1. It follows proposal E9 of [https-tls.md](https-tls.md), which left the NimBLE port to its own plan. The
owner decided (2026-10-23, DESIGN.md decisions D33 to D35): NimBLE first, before TLS; non-breaking, so
`ble_gatt.yaml` itself moves to NimBLE and there is no Bluedroid overlap release (replaces B1); no phones, the clients
are a PLC bridge and scripts, and the Python client is enough for the bench (M2's phone pairing and "Forget BLE
bonds" are dropped or wait for the configuration page; "Forget BLE bonds" came in 2.4.3, in Configuration, Keys, D41). B2, B3, B5, B6, B7 and B9 are as proposed; B4 without the
button for now; B8 and B10 are overtaken by the new order (the controller trim is part of the port).

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude); reviewed by a human, but it may still
contain mistakes.

## Premise

BLE is the largest single item in the full build: about 318 KB of flash and 31 KB of static RAM (docs/BENCH.md,
"BLE size benchmark"), plus the heap Bluedroid takes when it starts. ESP-IDF ships a second BLE host, NimBLE, which
is smaller. ESPHome only supports Bluedroid, so using NimBLE means writing our own radio adapter. The owner wants to
keep BLE and would only move to an 8 MB part if forced. NimBLE is the way to give back room on the 4 MB part
without dropping the feature.

The port changes only the radio side. The protocol (docs/PROTOCOL.md), BleEngine and the authentication stay as
they are, byte for byte.

## What the experiment found

### How it was measured

- ESPHome 2026.10.0 (ESP-IDF 5.5.6), shaped like `firmware/full.yaml`, app slot 1,835,008 bytes.
- A throwaway NimBLE peripheral in a scratch folder, outside the repo. It had the same service UUID, 10
  characteristics with the same properties, Secure Connections with bonding, "Just Works" and bonds stored in NVS.
  It took the place of `esp32_ble`, `esp32_ble_server` and our adapter. It had no BleEngine, so it linked
  everything the radio needs and none of the protocol.
- Compiled only at first; QEMU has no Bluetooth radio. Heap at run time was measured later on the board (M0, "On
  the board" below).

### Facts from ESPHome (2026.10.0)

| Fact | Where |
|---|---|
| `esp32_ble` is Bluedroid only: it sets `CONFIG_BT_BLUEDROID_ENABLED` and calls Bluedroid's API (`esp_ble_gap_*`, `esp_gatts_*`). There is no NimBLE option | `esp32_ble/__init__.py`, `ble.cpp` |
| Everything Bluetooth in ESPHome depends on `esp32_ble`: `esp32_ble_server`, `esp32_ble_tracker`, `ble_client`, `bluetooth_proxy`, `esp32_ble_beacon`, `improv_ble`. None of them is available with NimBLE. The feeder uses only the server, and DESIGN.md section 8 already rules out `bluetooth_proxy` | `components/*/__init__.py` |
| A component can turn Bluetooth on without `esp32_ble` (`request_bluetooth()`) and set the stack's options (`add_idf_sdkconfig_option`) | `esp32/__init__.py` |
| `esp32_ble_server` adds a Device Information Service (manufacturer, model, firmware version) from `ble_gatt.yaml` | `ble_server.cpp` |
| After a connection, the server restarts advertising only while it has fewer clients than `max_clients` (1 here), and again after a disconnect | `ble_server.cpp` |
| The advertising carries the name, and the name gets the MAC suffix when `name_add_mac_suffix` is on | `ble_advertising.cpp`, `ble.cpp` |
| `esp32_ble_server` gives every characteristic plain read and write permissions. NimBLE can mark a characteristic "encryption required" (`BLE_GATT_CHR_F_READ_ENC`, `_WRITE_ENC`) | DESIGN.md section 10; NimBLE `ble_gatt.h` |
| ESPHome delivers Bluedroid's events on the main loop. NimBLE's host runs in its own FreeRTOS task, and its callbacks run there | `ble.cpp`; ESP-IDF `nimble_port_freertos` |

### What our code depends on

The adapter (`components/precision_feeder_ble/precision_feeder_ble.*`, about 230 lines) is the only code that
touches the stack:

- `BleTransport` (`engine.h`) is the whole interface BleEngine needs: `set_value(channel, data, n, notify)`,
  `random()` and `log()`. The engine also needs to hear about connects, disconnects, writes, reads, the peer address
  and the MTU. None of this is Bluedroid-specific.
- The Bluedroid-specific parts are `esp32_ble_server::BLECharacteristic` (values, `on_write`, `on_read`, `notify`),
  `BLEServer::on_connect/on_disconnect`, and the GATT event handler that reads the peer address
  (`ESP_GATTS_CONNECT_EVT`) and the MTU (`ESP_GATTS_MTU_EVT`).
- The GATT table (UUIDs, properties, initial values) is YAML in `packages/ble_gatt.yaml`.
- `tools/precision_feeder_client` finds the feeder by its service UUID in the advertising (`cli.py`,
  `BleakScanner.discover(service_uuids=...)`).

### Flash and static RAM

| Build | Flash | % of slot | Static RAM | Against `full` |
|---|---|---|---|---|
| `full` today (Bluedroid) | 1,634,067 | 89.0% | 164,447 | |
| NimBLE stub instead of Bluedroid | 1,479,999 | 80.7% | 158,171 | -154,068 flash, -6,276 RAM |
| `no-ble` | 1,284,431 | 70.0% | 133,231 | -349,636 flash, -31,216 RAM |

- NimBLE costs about 196 KB of flash where Bluedroid costs about 350 KB. Once our adapter and BleEngine are back in,
  the net saving should be about 140 KB. That is an estimate: BleEngine and the protocol were about 5 to 10 KB on
  Bluedroid, and the NimBLE adapter will be about the size of today's.
- Static RAM drops by only about 6 KB net. By section (`xtensa-esp32s3-elf-size -A` on both images):

  | Section | Bluedroid | NimBLE | Change |
  |---|---|---|---|
  | `.dram0.bss` | 67,608 | 56,328 | -11,280 |
  | `.dram0.data` | 19,984 | 20,084 | +100 |
  | `.iram0.text` | 92,051 | 96,955 | +4,904 |

  NimBLE puts more of its code in IRAM. On the S3, IRAM and DRAM are carved from the same 512 KB of SRAM, so the
  extra IRAM takes back most of the DRAM saving.
- **Heap at run time is the number that matters for TLS** ([https-tls.md](https-tls.md), "Heap"). It was measured
  in M0 ("On the board" below). Figures quoted on Espressif's forums (owner, 2026-10-23; **unverified**, the configuration and chip
  behind them not known) put NimBLE at about 13 to 14 KB of static DRAM and 47 KB of static IRAM. They put its
  total RAM at about 60 to 88 KB against 150 to 200 KB or more for Bluedroid, a saving of about 100 KB. Our static
  figures agree in shape. So nearly all of that saving would have to be heap Bluedroid takes at start: its task
  stacks, its GATT database and its buffers. Those comparisons are often for the classic ESP32, sometimes with
  Bluetooth Classic on. ESPHome already trims Bluedroid to BLE and a GATT server only, so the saving here may be
  smaller. M0 measures it on the board.
- With NimBLE and the whole TLS set (TLS 1.2 only, ACME, upload checks), the image would be about 1.53 MB (83%)
  instead of about 1.68 MB (92%).

### On the board (M0, 2026-10-23)

Measured on the disc rig with five scratch images, all with the same TLS lab code; the details are in
[docs/BENCH.md](../BENCH.md), "HTTPS, ACME and the BLE stacks on the disc rig". Heap free at idle:

| Image | Free at idle | BLE costs |
|---|---|---|
| No BLE | about 201 KB | |
| Bluedroid, as ESPHome builds it | about 110 KB | about 91 KB |
| NimBLE, ESP-IDF's defaults | about 110 KB | about 91 KB |
| Bluedroid, controller trimmed | about 125 KB | about 76 KB |
| NimBLE trimmed | about 143 KB | about 58 KB |

- **NimBLE saves nothing as it comes.** ESP-IDF's NimBLE defaults (3 connections, 24 ACL and 30 event buffers,
  BLE 5 features, 6 controller activities) cost as much heap as ESPHome's Bluedroid.
- **Most of the gain is the controller, which both stacks share.** One connection, 2 controller activities and no
  scanning save about 15 KB on Bluedroid with three YAML lines (`esp32_ble: max_connections: 1` and
  `sdkconfig_options`).
- **Trimmed NimBLE saves about 18 KB more than trimmed Bluedroid**, about 33 KB more than today's build, and about
  180 KB of flash. Its total, about 58 KB, is in line with the forum figures. The forum's 150 to 200 KB for Bluedroid
  is not what this build pays: ESPHome already trims it to about 91 KB.
- With four TLS sessions, the lowest free heap was 11 to 33 KB on today's Bluedroid (3 KB once), 51 to 52 KB on
  trimmed Bluedroid and 68 to 73 KB on trimmed NimBLE.
- The stub advertised its name only (no service UUID) and had one UUID byte out of order: scratch bugs, which B5 and
  the consistency test (B2) are there to catch. macOS kept the same device identifier across the stacks.

## Proposed design

### Shape

- A second transport inside `precision_feeder_ble`: `nimble_transport.{h,cpp}`, a `BleTransport` on NimBLE's host
  API. It is chosen by package. `packages/ble_nimble.yaml` replaces `ble_gatt.yaml` and does not include `esp32_ble`
  or `esp32_ble_server`. The component's YAML keys (key, time policy, staleness) stay the same.
- The component's `to_code` sets the stack's options: Bluedroid off, NimBLE on, peripheral role only (no central,
  no observer), one connection, a bond limit, bonds in NVS, and NimBLE's log level down. Validation refuses
  `esp32_ble` in the same build, because the two stacks cannot both be linked.
- The GATT table moves from YAML to C++ (B2): one table in the adapter, built from `protocol.h`'s channels. It has
  the same UUIDs, the same properties and the same initial values. `tests/test_consistency.py` checks the UUIDs and
  properties against `ble_gatt.yaml` while both packages exist, and against PROTOCOL.md after that.
- NimBLE's own Device Information Service (`ble_svc_dis`) carries the same manufacturer, model and firmware version
  as today.

### Threads

NimBLE calls us from its host task; BleEngine and FeederCore are main-loop only. So the adapter:

- **Writes:** copies each write (channel, bytes, connection) into a small fixed queue under a lock and returns at
  once. `loop()` drains the queue into `engine_->on_write()`. A write that does not fit in the queue is answered
  with an ATT error ("insufficient resources"), never dropped silently. The client retries, and the retry cache
  makes an exact retry safe.
- **Reads:** reads are answered on the host task from a per-channel value buffer. `set_value()` updates that buffer
  on the main loop under the same lock, so a read never sees half a value. Read counts are queued the same way as
  writes.
- **Connect, disconnect and MTU:** the GAP events are queued too. They give the peer address
  (`ble_gap_conn_find`) and the MTU (`BLE_GAP_EVENT_MTU`), which replace today's GATT event handler.
- **Notify:** `set_value(..., notify=true)` calls `ble_gatts_notify_custom()` from the main loop. NimBLE locks its
  host for this. The adapter notifies only while a client is connected and subscribed (the CCCD), as today.

### Advertising and connection

- The advertising stays as it is today (B5): flags, the 128-bit service UUID, and the name with the MAC suffix
  rules of `full.yaml`. The service UUID and the name together may not fit in 31 bytes, so M0 records exactly what
  Bluedroid sends today (main packet and scan response), and NimBLE copies it.
- One client. Advertising stops while it is connected and starts again on disconnect or on an advertising
  timeout, as today.
- Security Manager: LE Secure Connections with bonding, no input or output ("Just Works"), the same as
  `io_capability: none` and `auth_req_mode: sc_bond`. Bonds are kept in NVS (`ble_store_config`). When a peer that
  is already bonded pairs again, its old bond is deleted and pairing retries.
- Characteristic permissions stay plain (B3). Access control stays the protocol 2 key, which is unchanged.

## Decisions (proposed)

| # | Proposal |
|---|---|
| B1 | **Replace Bluedroid, with one overlap release.** The release that ships NimBLE keeps `ble_gatt.yaml` (Bluedroid) for YAML builds and makes `ble_nimble.yaml` the default in the examples and the `full` binary. The next minor release removes the Bluedroid package. Keeping both for good doubles the radio code we maintain |
| B2 | **The GATT table moves from YAML to C++.** DESIGN.md section 8 says the table is YAML; with our own adapter, keeping it as YAML would mean writing a YAML schema for something that never changes. Its UUIDs are fixed by the protocol |
| B3 | **No "encryption required" on the characteristics.** NimBLE can require it, but with "Just Works" it does not keep a stranger out. It would also break clients that do not pair (PLC bridges). It could become an option later ("BLE requires pairing", off by default), with its own decision |
| B4 | **At most 3 bonds.** When a fourth peer pairs, the oldest bond is replaced. The web app's Maintenance section gets "Forget BLE bonds", behind the settings password |
| B5 | **The advertising is identical to today's**, measured in M0, so scanners and `tools/precision_feeder_client` find the feeder as before |
| B6 | **The protocol stays at version 2.** Nothing a client sees changes apart from the stack. The firmware gets a minor version, and the CHANGELOG says that existing bonds must be made again |
| B7 | **NimBLE's memory stays in internal RAM.** PSRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`) is a lever, used only if the board's heap needs it and only once it is shown to work there (as E10 in [https-tls.md](https-tls.md)) |
| B8 | **Order with the TLS plan: after MQTT over TLS, before HTTPS** (see "Order" below). The HTTPS server is what needs the heap, and this plan is the largest way to free it short of PSRAM |
| B9 | **The port ships trimmed** (one connection, small buffer pools, no BLE 5 extras, the controller trim): ESP-IDF's NimBLE defaults save nothing (M0). The values are measured on the board and kept in the component |
| B10 | **Trim Bluedroid's controller first, on its own** (`esp32_ble: max_connections: 1`, 2 controller activities, no scanning): about 15 KB for three YAML lines, useful whether or not this port happens. It checks that nothing on the feeder needs scanning or a second link |

## Order (with the TLS plan)

1. **One bench run for both plans' M0.** Done 2026-10-23 (BENCH.md): trimmed NimBLE frees about 33 KB over
   today's build, of which about 15 KB comes from the controller trim alone (B10).
2. **The controller trim on Bluedroid** (B10), then **MQTT over TLS** (TLS plan M1). It is the audit finding, it costs about 1 KB of flash and one TLS session, and
   it fits with either BLE stack, so it does not wait for this port.
3. **This plan, M1 to M5.** M0 says it is worth it: about 18 KB of heap over trimmed Bluedroid and about 180 KB
   of flash, enough for four TLS sessions with 65 KB or more to spare.
4. **HTTPS** (TLS plan M2 to M6), tuned once, on the BLE stack that ships.

What M0 can change:
- NimBLE frees little and Bluedroid has room for HTTPS: HTTPS follows MQTT directly, and this plan is done later,
  for flash only, or not at all.
- Neither stack leaves room for HTTPS: PSRAM for the TLS buffers (E10 in the TLS plan, B7 here) becomes the
  question, whichever BLE stack ships.
- NimBLE frees about as much as the forum figures say: TLS 1.3 (E1 in the TLS plan) may be affordable again,
  provided the TLS 1.2 stall seen in the experiment is fixed.

## Steps (once the decisions are made)

Each step is one commit, with `tools/check.sh` green and DESIGN.md and the guides updated in the same change.

- **M0 Facts on the board (bench, the owner's OK to flash; one run with the TLS plan's M0).** No repo code. First the current Bluedroid build:
  - free heap with Wi-Fi, the web page open, MQTT, and a BLE client connected and authenticated;
  - the advertising packets and scan response (nRF Connect or a bleak scan);
  - the GATT table as a client sees it (handles, properties, descriptors);
  - pairing and reconnecting from macOS, iOS and Android;
  - `selftest` timings.

  Then the scratch NimBLE build, measuring the same heap. M0 also answers three questions:
  - Does the board keep its BLE address across the switch?
  - Where does Bluedroid keep its bonds in NVS, and should the new firmware erase them?
  - Does NimBLE's Service Changed indication reach bonded clients?
- **M1 The adapter.** `nimble_transport.*`, the thread queue and value buffers, the GATT table in C++,
  `ble_nimble.yaml`, the sdkconfig options and the conflict check. Host tests for the queue and value buffer (pure
  code, no NimBLE includes, like BleEngine). The consistency test for the table.
- **M2 Security and bonds.** The Security Manager settings, bonds in NVS, re-pairing, the bond limit (B4), "Forget
  BLE bonds", and the Service Changed indication on the first boot after the switch.
- **M3 Diagnostics parity.** The Device Information Service, BLE Info and BLE Session, and the activity log at
  `/feeder/ble`, all with the same contents as on Bluedroid.
- **M4 Bench.** `tools/precision_feeder_ble.py selftest` from macOS (the owner's Terminal panel; this session's
  Python cannot use Bluetooth), iOS and Android with nRF Connect, a PLC bridge if one is at hand. Tests with and
  without pairing, 100 connect and disconnect cycles, a retry storm (the same write sent repeatedly; it must feed
  once), BLE while the web page is open and MQTT is busy, a 24 hour soak, and heap against M0. Then an upgrade from
  2.3.x over the air, and a downgrade.
- **M5 Switch the defaults.** The examples and `firmware/full.yaml` move to `ble_nimble.yaml`, and `no-ble` is
  unchanged. Docs: DESIGN.md sections 8, 10 and 16 and the decisions log, BUILDING.md, PROTOCOL.md (one line: the
  stack changed and the protocol did not), SECURITY.md (bonds), BENCH.md. Version and CHANGELOG.
- **Later (the next minor release): remove `ble_gatt.yaml`** and the Bluedroid adapter (B1).

## What the port does not touch

- BleEngine, `protocol.h`, `crypto.h` and their host tests (`test_protocol.cpp`, `test_ble_engine.cpp`) and the
  golden byte vectors shared with the Python client.
- Authentication, sequence numbers, the retry cache and the two BLE permissions. BLE feeds still go through the
  feed guard, because BleEngine calls FeederCore exactly as before.
- Home Assistant, the ESPHome API, MQTT, the web server and OTA.

## Not in this plan

- "BLE requires pairing" (B3): its own decision if wanted.
- Anything else on top of NimBLE (a central role, scanning, a beacon, BLE provisioning).
- Bluetooth on/off at run time. NimBLE can be stopped and started (`nimble_port_deinit`), but nothing asks for it
  today.

## Risks

| Risk | Mitigation |
|---|---|
| The heap gain is smaller than hoped | Measured in M0: about 18 KB over trimmed Bluedroid, about 33 KB over today's, but only when NimBLE is trimmed (B9). M4 measures the real adapter against these figures |
| A thread bug: an engine call from NimBLE's task, or a read that sees half a value | Every NimBLE callback only copies into a queue or reads a locked buffer; BleEngine is called only from `loop()`; host tests for the queue; the M4 soak and retry storm |
| A write is lost when the queue is full | The client gets an ATT error, not silence, and retries; the retry cache makes an exact retry safe; the queue is sized from the protocol (one client, one write at a time) |
| Bonded clients cached the old GATT handles, and a write lands on the wrong characteristic after the update | Every write's tag is bound to its characteristic (DESIGN.md section 10), so a write on the wrong one is refused and cannot act; Service Changed tells bonded clients to discover again (checked in M0) |
| Existing bonds stop working after the update (phones, PLC bridges) | Expected; the CHANGELOG and USING.md say to forget the feeder and pair again. Pairing is optional, so a client that does not pair is unaffected. M0 decides whether the old Bluedroid bonds are erased from NVS |
| Scanners or the client no longer find the feeder | The advertising is copied byte for byte from M0's capture (B5); M4 runs the client's scan |
| Small behaviour differences between the stacks (long reads, error codes, notifications before subscribing, MTU) | M0 records how Bluedroid behaves, M4 compares; every record fits 20 bytes, so the MTU does not matter to the protocol |
| We own radio code that ESPHome used to maintain, and an ESPHome upgrade changes how Bluetooth is set up (`request_bluetooth()`, the sdkconfig options) | The adapter is small and written to ESP-IDF's NimBLE API, not to ESPHome's; `check.sh` compiles it on every change; if ESPHome adds NimBLE to `esp32_ble` itself, reconsider and move to it |
| Licence: ESPHome's Bluetooth code is GPLv3 | Written fresh against NimBLE's API (Apache-2.0, part of ESP-IDF); nothing is copied from ESPHome or from ESP-IDF's examples |
| Wi-Fi and BLE share the radio, and the coexistence timing differs from Bluedroid's | M4 runs BLE while the web page and MQTT are busy, and checks feed timing and the disc's control loop |
| Two BLE packages drift during the overlap release | The consistency test checks both tables; the overlap is one release (B1) |
