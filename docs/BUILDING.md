# Building your own firmware from YAML

The advanced path: build the firmware yourself with ESPHome, choosing exactly which features are compiled in. Most
people should flash the prebuilt firmware instead ([INSTALL.md](INSTALL.md)); a YAML build has the same web app,
settings and safety behaviour, plus whatever you change in your YAML.

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

## How a build is put together

The firmware is built from **packages**: one YAML file per feature in [`packages/`](../packages/). Your configuration
file lists the packages it wants, so a feature is switched on or off by one line.

| Package | What it gives you | Include it? |
|---|---|---|
| `core` | the feeder itself: the servo tray, Calibration Mode and position recovery, Wi-Fi with a fallback access point, Home Assistant API, OTA, NTP time | **always** |
| `status` | sensors: tray position, portions left, next feed, runway, Needs Food, faults, time source, firmware version and variant | **always** |
| `controls` | buttons and settings: feed, tray reload, hold, active profile, guard limits, location, time zone, NTP servers | **always** |
| `schedule_editor` | editing profiles and events, one-shot feeds | **always** (unless something else, like a PLC over BLE, owns the schedule) |
| `calibration` | the servo's calibration: pulse range and per-slot offsets | **always** (the disc's calibration is in its own package) |
| `mechanism_n20` | the direct-drive disc as a second drive, with its tuning, calibration wizard, motor pins and **Drive** (servo or disc, chosen on the device) | only for a disc feeder ([`disc.yaml`](../examples/disc.yaml)) |
| `web_server` | the built-in web app at `http://<name>.local`, and the settings password | recommended (both examples); leave it out to save memory |
| `hardware` | Board and Servo Pin chosen on the device, Pins In Use, Restart Needed, Restart | recommended (both examples); harmless if you never use it |
| `status_led` | colour-coded state on the board's RGB LED; LED Pin on the device | if your board has the LED (both examples) |
| `button` | a push button: short press feeds (or reloads when the tray is empty or its position unknown), a 3 s press toggles calibration; Button Pin on the device | only if you fitted one |
| `time_homeassistant` | Home Assistant as a second clock source (keeps time when the internet is down but Home Assistant is up) | both examples; harmless without Home Assistant. It also uses Home Assistant's time zone unless you pin yours (see its comment) |
| `network_info` | the Wi-Fi network, IP addresses and signal strength | both examples; leave it out to save a little flash |
| `improv_serial` | Wi-Fi set over the USB cable from an install page ([Improv](https://www.improv-wifi.com/)) | both examples; leave it out if you never set Wi-Fi over USB |
| `ble_gatt` | the Bluetooth control interface ([PROTOCOL.md](PROTOCOL.md)) and two switches for what BLE may change | only if a PLC, app or script will use BLE. Costs about 330 KB of flash and 30 KB of RAM |
| `api_key_on_device` | no API key in your YAML: Home Assistant sets one when it adopts the feeder, or you make one in the web app (as the prebuilt firmware does) | optional; add it after `core`. Plaintext OTA is then refused, and OTA needs the key once one is set |
| `mqtt` | MQTT with Home Assistant discovery for every entity, beside the API; broker, login and topic prefix set in the web app (Configuration, MQTT), off until then | if you use MQTT. Costs about 132 KB of flash |
| `diagnostics` | free memory and loop time | for troubleshooting (on in `ble.yaml`) |

**Three ready-made configurations** are in [`examples/`](../examples/):

- [`basic.yaml`](../examples/basic.yaml): Home Assistant (including its time) and the web app, no BLE. The right start
  for most people.
- [`ble.yaml`](../examples/ble.yaml): the same plus the BLE interface and diagnostics, for a feeder driven by a PLC or
  another BLE controller.
- [`disc.yaml`](../examples/disc.yaml): `basic.yaml` for the direct-drive disc (a GA12-N20 gear motor behind a DRV8833),
  with the button on GPIO3. The firmware carries the servo too; it starts with the disc.

## Setting it up

1. **Install ESPHome 2026.10.0** (the version this is built and tested with; newer versions may need small fixes).
   Use the ESPHome add-on in Home Assistant, or on a computer (Python 3.12 to 3.14):
   ```sh
   python3 -m venv .venv
   .venv/bin/pip install esphome==2026.10.0
   ```
2. **Clone this repository into your ESPHome config folder** (the add-on's folder is `/config/esphome/`), so it sits
   there as `precision-feeder/`:
   ```sh
   cd /config/esphome
   git clone https://github.com/jspeer/precision-feeder.git
   ```
3. **Copy an example up next to the clone** and rename it, for example `precision-feeder.yaml`:
   ```
   /config/esphome/
     precision-feeder/          <- this repository (update it with git pull)
     precision-feeder.yaml      <- your copy of examples/basic.yaml or ble.yaml
     secrets.yaml               <- your keys (see examples/secrets.example.yaml)
   ```
   Your copy reads the components and packages from the clone, so updating the repository never touches your file
   or your secrets.
4. **Secrets.** In the config folder (not in the clone), add the entries from
   `precision-feeder/examples/secrets.example.yaml` to your `secrets.yaml`: the Wi-Fi name and password, a password
   for the fallback access point, and an API key (make it with `openssl rand -base64 32`; Home Assistant asks for
   it). With the BLE package, also a `ble_key`, made the same way, which your BLE client needs. Both keys can instead
   be made in the web app (Configuration, Keys); see [Keys](#keys-in-a-yaml-build).
5. **Adjust your copy.** At least set `timezone` (an IANA name such as `Europe/London`, or a POSIX string such as
   `<-06>6` for a fixed UTC−6), and comment packages in or out:
   ```yaml
   packages:
     core: !include precision-feeder/packages/core.yaml
     # ...
     button: !include precision-feeder/packages/button.yaml        # enabled
     # time_ha: !include precision-feeder/packages/time_homeassistant.yaml   # disabled
   ```
6. **First flash over USB.** If the S3-Zero does not show up, hold **BOOT** while plugging in the USB-C cable, then
   let go. From the config folder:
   ```sh
   esphome run precision-feeder.yaml --device /dev/ttyACM0
   ```
   (macOS: `/dev/cu.usbmodem…`; Windows: `COM3` or similar; in the add-on, Install → Plug into this computer.) The
   first build downloads the toolchain (1 to 2 GB). Later updates go over Wi-Fi: `--device precision-feeder.local`.
7. **Add it to Home Assistant.** It is usually discovered (Settings → Devices & services). On another network segment
   (an IoT VLAN, for example) discovery does not reach it: add the **ESPHome** integration by hand with the feeder's
   IP address, port 6053, and your API key as the encryption key.

Then calibrate, load and schedule it as in the README's [Getting started](../README.md#getting-started).

**Updating:** `git pull` inside `precision-feeder/`, then `esphome run precision-feeder.yaml` again. Read the
[CHANGELOG](../CHANGELOG.md) first. A YAML build reports Firmware Variant `yaml`, and the web app's update check
reminds you to update with ESPHome rather than offering the prebuilt images.

## Settings in your YAML

Set these in your copy's `substitutions:` (defaults in [`packages/core.yaml`](../packages/core.yaml), the servo's in
[`packages/mechanism_servo.yaml`](../packages/mechanism_servo.yaml), which core.yaml includes, and
[`packages/ble_gatt.yaml`](../packages/ble_gatt.yaml)). A feeder with another motor uses another mechanism package;
see [MECHANISMS.md](MECHANISMS.md). The direct-drive disc's
[`packages/mechanism_n20.yaml`](../packages/mechanism_n20.yaml), added beside the servo, has its own: `disc_slots`,
`disc_build_default` (`"true"`: the disc until Drive is changed on the feeder; `"false"`: the servo),
`motor_in1_pin`, `motor_in2_pin`, `motor_standby_pin`, `encoder_a_pin`, `encoder_b_pin` and
`motor_pwm_frequency` (pins chosen on the feeder win); set `tray_capacity: "59"` and `button_pin: GPIO3` with it, as
`disc.yaml` does. Many are only **starting values**: the same setting changed
on the feeder (web app or Home Assistant) is saved there and wins, as the table says.

| Substitution | Default | What it is |
|---|---|---|
| `name` / `friendly_name` | `precision-feeder` / `Precision Feeder` | the device's name (network, Home Assistant). Give each feeder its own. A name set in the web app wins. |
| `name_add_mac_suffix` | `"false"` | `"true"` adds the end of the board's MAC address (`precision-feeder-a1b2c3`), so feeders built from one YAML do not clash |
| `timezone` | `America/New_York` | IANA name or POSIX string; daylight saving and sun times depend on it. One set on the feeder (or sent by Home Assistant) wins. |
| `ntp_server_1` to `_3` | `0.pool.ntp.org` to `2.pool.ntp.org` | the NTP servers. Servers set on the feeder (NTP Servers) win. `2.pool.ntp.org` also answers over IPv6. |
| `servo_pin` | `GPIO1` | servo signal. With the `hardware` package, a pin chosen on the feeder wins. |
| `button_pin` | `GPIO2` | push button (`button` package); Button Pin on the feeder wins |
| `status_led_pin` | `GPIO21` | onboard RGB LED (`status_led` package); LED Pin on the feeder wins |
| `boot_pin` | `GPIO0` | the board's BOOT button: held 10 s, clears the settings password |
| `enable_ipv6` | `"true"` | IPv6 alongside IPv4: the web app, Home Assistant and mDNS answer on both. `"false"` for IPv4 only. |
| `min_pulse` / `max_pulse` | `544us` / `2400us` | starting servo pulse range; calibrated values win. An MG90S is closer to 500 / 2500. |
| `servo_invert` | `"false"` | `"true"` if the servo is mounted the other way round |
| `sweep_degrees` | `"175"` | the angle the 7 slots span (25° each). Leave it for the Rev2 print. |
| `step_time` | `1s` | the time given to each of the three moves of a feed |
| `wiggle` | `"true"` | forward, back, forward on each feed to shake food loose |
| `detach_delay` | `1s` | the servo is released this long after the last move (no buzzing, no heat) |
| `tray_capacity` | `"6"` | portions per load, set by the mechanism package (6 for the servo tray; `"59"` with the disc, the most any disc holds); the portion input boxes go up to it. Leave it. |
| `ble_name` | `Precision Feeder` | the name shown to BLE scanners. At most 20 characters; at most 13 with `name_add_mac_suffix` (ESPHome adds the suffix there too). |
| `ble_time_policy` | `fallback_only` | a clock pushed over BLE: accepted only when network time is stale (recommended), `always`, or `off` |
| `ble_network_time_stale_after` | `24h` | how long after the last NTP or Home Assistant sync a BLE clock is still refused (`fallback_only`) |

`fw_version` and `fw_variant` are the firmware's own; leave them. Everything else (the schedule, guard limits,
calibration, location, hold) is set on the feeder and saved there: see [USING.md](USING.md#settings).

## Wi-Fi, the name and keys in a YAML build

The web app's Configuration page can change the network settings and keys of any build; in a YAML build, this is
how they interact with your YAML:

- **Name:** one set in the web app wins over `name` (and any MAC suffix), used after the next restart. Home Assistant
  may need the feeder added again under the new name.
- **Wi-Fi network:** one joined from the web app, the captive portal or Improv replaces the YAML's. ESPHome keys that
  saved network to the build, so with a network in your YAML, the YAML's comes back after the next update built from
  a changed YAML.
- **Access point password:** one set in the web app wins over `fallback_ap_password`, used after the next restart.
- <a id="keys-in-a-yaml-build"></a>**Home Assistant key:** with the key in your YAML, change it there; the web app
  cannot. With the `api_key_on_device` package there is none in the build: Home Assistant sets it on adoption, or the
  web app makes, takes or clears one.
- **Bluetooth key:** one made or entered in the web app wins over `ble_key` (which is optional). With neither, every
  BLE client is refused.

## Keeping visitors out of the web app

The web app has no login by default; the [settings password](SECURITY.md) protects what matters. To keep visitors
out of the app altogether, uncomment the `auth` lines in [`packages/web_server.yaml`](../packages/web_server.yaml) and
add `web_username` and `web_password` to your `secrets.yaml`. That is HTTP authentication over plain HTTP: the
password crosses the network on every request, so it keeps casual visitors out and no more. If you never use the web
app, leave the `web_server` package out instead.
