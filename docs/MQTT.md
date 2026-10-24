# MQTT

*Written with the assistance of an AI (Anthropic's Claude); verify against the source if something looks wrong.*
Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).

With the `mqtt` package (in the prebuilt firmware, off until set up), the feeder talks to your MQTT broker. Set it up
in the web app: Configuration, MQTT (broker, port, username, password, topic prefix).

## What MQTT reaches

| | Over MQTT |
|---|---|
| Everyday control and status | yes: the entities Home Assistant sees (below), with Home Assistant discovery |
| The schedule and one-shot feeds | yes: their own topics (below) |
| The feed guard's limits | read only: published, never changed over MQTT (change them in the web app) |
| Calibration, pins, drive, keys, network, firmware, the 24 h count | never: the web app (and the YAML) only |

The broker's login is MQTT's only protection: anyone your broker lets publish to these topics can use them. Give the
broker accounts and access rules, and never use an open broker. (Feeds always go through the feed guard.)

## Topics

`<prefix>` is the topic prefix in use: the one set in the web app, or else the feeder's name
(`precision-feeder-a1b2c3`). Configuration, MQTT shows it.

### The entities (ESPHome's own topics)

ESPHome publishes these, with Home Assistant discovery (`homeassistant/...`), under `<prefix>/<component>/<name>/`:
Feed One Portion, Feed Selected Portions and Manual Portions, Tray Reload, Stop, Hold Scheduled Feeding and Hold
Duration, Cancel Hold, Scheduler, Active Profile, Portions Remaining, Tray State, Next Feed, Last Feed, Last Result,
Recent Feeds, Today's Feeds, Faults, Needs Food and Firmware Version. `<prefix>/status` is `online` or `offline`.
ESPHome's device discovery answers on `esphome/discover`.

### The feeder's own topics

Under `<prefix>/feeder/`. Every value has a **state topic** (retained, published at connect and whenever it changes,
from MQTT, the web app, BLE or the schedule) and a **`/set`** topic to change it. One value per message.

| State topic | Payload | `/set` takes |
|---|---|---|
| `schedule/<A\|B\|C>/event/<1-4>/enabled` | `ON` or `OFF` | `ON`, `OFF`, `true`, `false`, `1`, `0` |
| `schedule/<P>/event/<N>/anchor` | `clock`, `sunrise` or `sunset` | the same; switching between clock and sun resets the time (12:00, or 0 from the sun) |
| `schedule/<P>/event/<N>/time` | `HH:MM` (clock), `+HH:MM` or `-HH:MM` (from the sun) | the same, or `+90` (minutes from the sun); at most 12 hours from the sun |
| `schedule/<P>/event/<N>/portions` | a number | 1 to the portions per load (6 on the servo tray) |
| `schedule/<P>/event/<N>/days` | `all`, `none` or `mon,wed,fri` | the same, or `weekdays`, `weekend`, full day names |
| `schedule/<P>/every_n_days` | a number | 1 to 14 (feed every N days) |
| `schedule/<P>/fast_days` | like `days` | like `days` (weekdays with no feeding) |
| `oneshot/list` | `none`, or `Thu 2026-12-24 08:00 x2; ...` | (read only) |
| `policy/daily_cap` | a number | (read only: `/set` is answered "not allowed" and the value in force) |
| `policy/min_gap_s` | seconds | (read only) |
| `policy/portion_gap_s` | seconds | (read only) |
| `policy/max_portions_cmd` | a number | (read only) |
| `policy/writable` | always `OFF` | (read only; clients can use this to discover that policy writes are unavailable) |

Commands without a state:

| Topic | Payload |
|---|---|
| `oneshot/add` | `YYYY-MM-DD HH:MM PORTIONS` in the feeder's time (portions optional, 1 by default; nothing else, so `08:00 PM` is refused). The same feed again is not added twice |
| `oneshot/clear` | anything: removes every one-shot feed |
| `schedule/<P>/restart_cycle` | anything: the every-N-days cycle starts today (needs the clock) |

After every command the feeder publishes:

- the value's state topic, **also when the value was refused**, so you always read what is in force;
- `<prefix>/feeder/result` (not retained): `<topic>: ok`, or the reason, as the web app's Last Result words it
  (`invalid argument`, `not allowed`, `clock not set`, ...).

Anything else under `<prefix>/feeder/` is ignored.

Example (a broker on `broker.lan`, the feeder named `precision-feeder-a1b2c3`):

```sh
mosquitto_pub -h broker.lan -u feeder -P '...' -t precision-feeder-a1b2c3/feeder/schedule/A/event/1/time/set -m 07:30
mosquitto_sub -h broker.lan -u feeder -P '...' -t 'precision-feeder-a1b2c3/feeder/#' -v
```

## Good to know

- **Never publish commands retained.** The broker would send them again at every reconnect. A retained `oneshot/add`
  is harmless (the same feed is not added twice), but a retained `restart_cycle` would restart the cycle each time.
- **One value at a time.** ESPHome's MQTT client queues incoming messages and drops some when its queue is full: send
  a burst with small pauses and check the state topics.
- **TLS** (the `mqtt_tls` package, in the prebuilt firmware): turn on MQTT TLS in the web app and use the
  broker's TLS port (usually 8883). MQTT TLS Check picks how the broker is checked: against a CA certificate you
  upload to the feeder's trust store (the broker's name is checked too), or "Pin on first contact": the feeder keeps
  the certificate the broker shows the first time (a self-signed one, or the CA at the top of its chain) and checks
  against it from then on. A broker that sends only its own certificate, signed by a CA it does not send, cannot be
  pinned: upload the CA instead. With TLS on, MQTT never connects in plain text: without a CA (or before the pin) it
  does not start. These settings apply after a restart. They, the trust store and the pin's Forget are in the web
  app's Configuration (MQTT, and HTTPS and certificates); setting them up: [HTTPS.md](HTTPS.md#mqtt-over-tls).
- **Settings of the MQTT connection itself** (broker, port, login, prefix) are web app only. Once MQTT has connected
  after a restart, a change to them is used after the next restart (Restart Needed), because ESPHome builds its MQTT
  client once per boot. Turning MQTT off takes effect at once.
- The schedule over MQTT is the same schedule as everywhere else: editing it here changes it in the web app and over
  BLE, and the other way round.
