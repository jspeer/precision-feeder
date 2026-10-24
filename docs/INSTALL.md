# Install, update and recover

Use the [browser installer](https://jspeer.github.io/precision-feeder/) for prebuilt firmware. No compiler or
Home Assistant is needed. To choose your own compiled features, follow [BUILDING.md](BUILDING.md).

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

## What you need

- An ESP32-S3 board with at least 4 MB flash; the reference board is the Waveshare ESP32-S3-Zero. No PSRAM required.
- A wired servo tray or direct-drive disc, with its own suitable motor supply. See [wiring](../README.md#wiring).
- A USB data cable and Chrome or Edge on a computer for the browser installer.
- Wi-Fi with IPv4 addresses. Dual-stack works; see [Limits](#limits) for IPv6-only networks.

## Choose a variant

| Variant | Included |
|---|---|
| `full` | Both drives, web app, HTTPS, Home Assistant, MQTT, MQTT TLS, ACME and Bluetooth |
| `no-ble` | The same features, without Bluetooth |

Both start with the servo drive. MQTT is off until configured; ACME is off until you set a directory.
Choose `no-ble` if you do not need a BLE controller. You can switch variants later without erasing saved settings.

Images have two forms:

- **`.factory.bin`:** the USB installation image, including the bootloader and partition table; written at address 0.
- **`.ota.bin`:** the update image for the feeder's Firmware page. Do not upload a factory image there.

The documented current source builds occupy about 85% (`full`) and 75% (`no-ble`) of the 1.8 MB app slot.
Exact build measurements belong in [DESIGN.md](../DESIGN.md#15-implementation-status-v245), rather than in the
installation steps; they vary with the source and ESPHome version.

## Install over USB

1. Open the [installer](https://jspeer.github.io/precision-feeder/) and connect the board with the USB data cable.
   If the S3-Zero does not appear, hold **BOOT** while plugging it in, then release it.
2. Click **Connect** under your variant and select the board's port, often called USB JTAG/serial debug unit.
3. Choose **Install**. Erase a new board; **do not erase an existing feeder if you want to preserve its settings,
   calibration, schedule and tray state**. See [Upgrading an existing feeder](#upgrading-an-existing-feeder).
4. Use the installer's Wi-Fi setup over USB (Improv) to select your network. It then links to the feeder.

Without a usable network, the feeder opens `precision-feeder-<6 hex digits>-setup`. A fresh installation has no
access-point password. Join it and choose the network in the captive portal, then set an access-point password
in the feeder's configuration.

## Setup wizard

Open `http://precision-feeder-<6 hex digits>.local`, or the IP shown by the installer or router. A new feeder opens
the wizard. Each step saves immediately; you can return to the settings later in Configuration or Maintenance.

1. **Network:** choose a name, Wi-Fi network and access-point password. The name and AP password apply after a
   restart; joining Wi-Fi happens at once.
2. **Time and place:** choose the time zone. Set latitude and longitude for sun-based events. You can also choose
   local NTP servers; otherwise the public pool is used.
3. **Board and pins:** choose a preset or individual pins. Select None for an unfitted button or LED. Restart when
   requested. For a disc feeder, choose **Direct-drive disc** in Configuration, Drive, and restart before calibrating.
4. **Keys:** configure Home Assistant or Bluetooth if you use them. Save generated keys when shown; the feeder
   cannot show them again.
5. **Settings password:** set one to protect safety limits, calibration, pins, network, keys and firmware updates.
6. **Calibration:** continue to [the guide for your mechanism](CALIBRATION.md), with no food loaded.

Finish with [loading and daily use](USING.md#everyday-use), then [set a schedule](SCHEDULING.md).

## Open the app securely

On your network, open `https://<name>.local` or the feeder's IP over HTTPS. HTTP redirects the app to HTTPS by
default. The initial certificate is self-signed: verify that you reached your feeder, then accept the browser's
warning. An uploaded certificate or your own ACME server can remove that warning; see [HTTPS.md](HTTPS.md).
The captive portal and setup access point continue to allow plain HTTP.

## Optional connections

| Connection | Setup |
|---|---|
| Home Assistant | Add the discovered ESPHome device. It can provision an API encryption key, or you can generate or enter one in Configuration, Keys and give it to Home Assistant. Adopt promptly: the first client can provision the key. |
| MQTT | Configure the broker, login and prefix in Configuration, MQTT. Home Assistant discovery exposes everyday entities; schedule editing has its own topics. See [MQTT.md](MQTT.md). |
| Bluetooth (`full`) | Generate or enter a BLE key in Configuration, Keys; without one, AUTH is refused. Give it to the controller or Python client. The prebuilt BLE name is `Feeder-<6 hex digits>`. See [PROTOCOL.md](PROTOCOL.md). |

Home Assistant exposes everyday controls and status. Configuration, calibration, pins and schedule editing live in
the web app. MQTT also edits schedules; BLE has its own documented permissions. See [SECURITY.md](SECURITY.md).

## Updates

In the web app, open **Diagnostics → Updates and firmware upload**. The browser can check the latest release on
GitHub Pages; the feeder itself does not contact GitHub. You can disable this check per browser.

1. Read the release notes and download the appropriate `.ota.bin` from the
   [releases page](https://github.com/jspeer/precision-feeder/releases).
2. Select it on the Firmware page. The app identifies known release images and warns about an unrecognized file.
   A settings password, when set, is required for the upload.
3. Wait for the image check and restart. The feeder waits until motion stops before restarting. A failed or
   interrupted upload leaves the running firmware in place.
4. Reload the app when offered, then check configuration, tray position and the next feed.

The upload authenticates the request when a settings password is set and checks image integrity with SHA-256.
This is not a manufacturer-signed firmware scheme; choose the source of the image you install.

## Upgrading an existing feeder

Saved settings, calibration, schedule and tray state have migration paths. **Preserving flash is essential**:
erasing it bypasses those migrations by removing the old data. Record your settings and calibration before a large
upgrade, and check them afterward before loading food.

When moving from a YAML build to prebuilt firmware, values compiled only into your old YAML are not saved settings:

- Set Wi-Fi again through the installer or setup access point.
- Re-adopt in Home Assistant, or enter the previous API key in the web app.
- Enter the previous BLE key or generate a new one and update the clients.
- The name becomes `precision-feeder-<6 hex digits>` unless a name was saved on the device.

A web upload needs firmware that already has the feeder's Firmware page; otherwise use USB without erasing.
If a BLE client bonded with firmware using the older Bluedroid stack (2.3 or earlier), delete that pairing on the
client before reconnecting to NimBLE firmware. The BLE protocol key is separate from the bond.

## Recovery

| Problem | What to do |
|---|---|
| Forgot the settings password | Hold the board's configured **BOOT** button for 10 seconds while running; only the settings password is cleared. |
| Wrong or unavailable Wi-Fi | Wait for the setup access point, then join it and select the network. Or use the installer's **Change Wi-Fi** over USB without reflashing. |
| Forgot the access-point password | Set Wi-Fi over USB, reconnect to the web app and change the AP password. |
| Need to leave a network | Configuration, Network, **Forget network** restarts into the setup access point and keeps other settings. Available in prebuilt firmware and builds with no YAML network. |
| Firmware fails to start | ESPHome can roll back a failed update. If the feeder does not return, install the factory image over USB without erasing to preserve settings. |
| HTTPS will not open | Use `https://`. Plain HTTP is allowed while HTTPS is unavailable and while the setup AP is up. See [HTTPS troubleshooting](HTTPS.md#troubleshooting). |
| Remove a Bluetooth client | Configuration, Keys, **Forget pairings**. Change the BLE key too if that client knew it. |
| Unknown tray position | Recover or reload through Maintenance; see [daily use](USING.md#recovering-an-unknown-position). |
| Wipe everything | Use the installer with **erase**. This removes settings, calibration, network, keys and password. There is no web factory-reset control. |

## Limits

- Prebuilt firmware needs IPv4 to join Wi-Fi. On an IPv6-only network, a YAML build can supply a static IPv4 address
  using `manual_ip`; see [BUILDING.md](BUILDING.md).
- Release checks and downloads need internet access from the browser. Normal operation needs no cloud connection.
- The feeder contacts its time servers and, when configured, its MQTT broker and ACME server. Home Assistant
  connects to the feeder.
