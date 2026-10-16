# Using the feeder

Everyday use, the settings, what the feeder does to stay safe, and what to do when something looks wrong. The same
for the prebuilt firmware and a YAML build.

*Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

## Everyday use

**In the web app** (`http://<name>.local`, or the feeder's IP address): the Dashboard has Feed now, Tray reload and
Hold; the Schedule page edits profiles, events and one-shot feeds; Maintenance calibrates and recovers the tray
position. [WEB_UI.md](WEB_UI.md) walks through every page.

**In Home Assistant** the same controls are entities:

| To | Use |
|---|---|
| Feed now | **Feed One Portion**, or **Manual Portions** + **Feed Selected Portions**, or a short press of the button |
| Refill | **Tray Reload**, then fill the tray. Scheduled feeding pauses for the **Reload Pause** (default 10 minutes) while you load. |
| Pause the schedule | **Hold Duration** (hours; 0 = until cancelled) + **Hold Scheduled Feeding**; **Cancel Hold** ends it. Manual feeds still work. |
| Switch plans | **Active Profile** |
| Feed once at a set time | **One-shot Portions** + **Add One-shot Feed** (`2026-10-20 18:00`) |
| After a power cut mid-feed | look at the tray, set **Declared Tray Position**, press **Declare Tray Position**; or press **Tray Reload** |
| Stop a feed in progress | **Stop** (the position becomes unknown, as above) |

**Get told when it needs food.** **Needs Food** turns on when the tray is empty or the next feed needs more portions
than are left. A Home Assistant automation (check the entity ID on your system):

```yaml
alias: Fish feeder needs food
triggers:
  - trigger: state
    entity_id: binary_sensor.precision_feeder_needs_food
    to: "on"
actions:
  - action: notify.notify
    data:
      message: "The fish feeder needs refilling."
```

**Status LED** (on the S3-Zero, or wherever LED Pin points): dim green all fine; yellow clock not set; red tray
empty; magenta position unknown; blue moving; white calibration mode; dim cyan hold or reload pause.

**The button** (if fitted): a short press feeds one portion, or reloads the tray when it is empty or its position is
unknown. Hold it 3 seconds to turn calibration mode on or off.

## Settings

Saved on the feeder, changed in the web app or Home Assistant, kept across updates:

| Setting | Default | Where |
|---|---|---|
| The schedule: profiles, events, cadence, fast days, one-shot feeds; Active Profile; Scheduler | three profiles of four events, all off | Schedule ([SCHEDULING.md](SCHEDULING.md)) |
| Minimum Gap Between Feeds | 60 s | Configuration (protected) |
| Gap Between Portions | 5 s | Configuration (protected) |
| Daily Portion Cap (rolling 24 h) | 4 | Configuration (protected) |
| Max Portions Per Command | 1 | Configuration (protected) |
| Reload Pause | 10 min | Configuration |
| Refill Interval (for the runway warning) | 3 days | Configuration |
| Latitude, Longitude | not set | Configuration, setup wizard |
| Time zone | the build's (UTC in the prebuilt firmware) | Configuration, setup wizard |
| NTP servers (up to three) | the build's (public pool) | Configuration, setup wizard (2.0.2) |
| Servo pulse range, calibration offsets | 544 to 2400 µs, 0° | Maintenance (protected) |
| Board and pins | the build's | Configuration (protected; after a restart) |
| Name, Wi-Fi network, access point password | the build's | Configuration, Network (protected; name and password after a restart) |
| Home Assistant and Bluetooth keys; what Bluetooth may change | none; both allowed | Configuration, Keys (protected) |
| Settings password | none | Configuration ([SECURITY.md](SECURITY.md)) |

**Protected** settings need the settings password in the web app when one is set; Home Assistant and BLE use their
own keys instead. The time zone and NTP servers are not protected. Pins, the name and the access point password take
effect at the next restart: **Restart Needed** says so, and nothing restarts by itself. The pin lists offer only pins
the ESP32-S3 can spare (no flash, PSRAM, USB or strapping pins), and "none" for a button or LED that is not fitted.

## Safety behaviour

These are deliberate, so a fault ends in a missed meal rather than an over-fed tank:

- **Every feed passes the same guard**, whatever asked for it. The daily cap counts every portion in the last 24
  hours. (For debugging and recovery, the count it uses can be edited: "Portions Counted (24 h)", logged.)
- **No catching up.** Events feed only in the minute they are due.
- **Each event feeds at most once a day**, also across reboots and time zone changes.
- **Power cut mid-feed → position unknown → feeding stops** until you confirm the tray position.
- **The clock is never restored from memory after a power cut** (it would be wrong by the length of the outage).
  Scheduled feeding waits for a real time source; manual feeds still work. After a restart *without* a power cut (an
  update, a crash) the chip keeps good time, shown as "kept across restart".
- **No automatic reboots.** Losing Wi-Fi or Home Assistant does not reboot it. A firmware update restarts it once,
  after waiting for the tray to stop.

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| Nothing is fed on schedule | Check **Faults** and **Next Feed**: the clock not set, the Scheduler switch off, a hold or reload pause, the wrong Active Profile, or the tray empty or its position unknown. |
| A meal was skipped | Missed events are never made up (feeder off, on hold, refused by the guard). **Recent Feeds** and **Last Result** say what happened. |
| "Feed refused: daily cap" | More portions in 24 hours than **Daily Portion Cap**. **Edited Profile** warns when a profile's day exceeds the cap. |
| Slots do not line up, worse towards slot 6 | The servo's pulse range: calibrate **Servo Max Pulse** first ([CALIBRATION.md](CALIBRATION.md)). |
| Servo buzzes or strains at one end | The pulse range is too wide for your servo; narrow **Servo Min/Max Pulse**. |
| Tray position "unknown" | Power was cut or Stop pressed during a move. Declare the position or press Tray Reload. |
| The clock is not set | No NTP answer: check the network, or set **NTP Servers** to one your network can reach. **Time Source** and the clock's age (Diagnostics) show when it last synced. |
| Wrong times (an hour off) | The time zone: the web app's (Configuration), otherwise the build's, or Home Assistant's with the `time_homeassistant` package unless the build pins one. |
| Home Assistant does not find it | Another network segment: add the ESPHome integration by IP with the API key (port 6053). |
| Lost the web app's address | Your router's client list (`precision-feeder-...`), or the install page's Connect over USB, which links to it. |
| Forgot a password | [INSTALL.md, Recovery](INSTALL.md#recovery). |
| Food sticks | Use pellets; flake food is not advised. |

## Limits

- **Six portions per load.** Schedules that feed several times a day need frequent refills; the runway sensors tell
  you how long a load lasts.
- **After a power cut with no network,** scheduled feeding waits until the clock is set again (NTP, Home Assistant,
  or BLE). There is no battery-backed clock.
- **Sunrise and sunset events** need Latitude and Longitude. On days with no sunrise or sunset (far north or south)
  they count from 06:00 and 18:00 instead, and Faults says so.
- **The Timezone entity takes a POSIX TZ string** (`CET-1CEST,M3.5.0,M10.5.0/3`), not an IANA name like
  `Europe/Berlin`; the web app picks it for you from a place name.
- **IPv6-only networks.** ESPHome counts Wi-Fi as connected only with an IPv4 address. A YAML build can use a static
  one (`wifi: manual_ip:`); the prebuilt firmware cannot join such a network. Dual-stack and IPv4-only networks are
  fine.
- **A few browsers at a time.** The feeder's web server has only a handful of connections; with more than about four
  pages open they take turns and reconnect. A page left in the background gives its connection back after 30 seconds
  and takes it again when you return.
- **BLE trusts anyone with the key, and does not prove itself to the client.** Writes need the key; answers are not
  signed, so a client should pair (an encrypted link) if it must trust what it reads. Details:
  [PROTOCOL.md](PROTOCOL.md), Authentication.
- **No factory reset in the web app yet:** wiping a feeder is an erase-and-flash from the install page.
