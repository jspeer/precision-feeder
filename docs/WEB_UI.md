# The web app

The feeder carries its own web app (firmware 1.1.0 and later). It runs the whole feeder
from any browser on your network, on a computer or a phone, with no Home Assistant, no
internet and nothing to install. It is stored in the firmware, so nothing is loaded
from the internet.

*Written with the assistance of an AI (Anthropic's Claude); check it against your own feeder.*

## Opening it

- Browse to `http://precision-feeder.local` (your `name` substitution plus `.local`), or
  to the feeder's IP address. IPv4 and IPv6 both work; Diagnostics lists the addresses.
- It needs the `web_server` package (on in both examples).
- **Password:** none by default. Uncomment `auth` in `packages/web_server.yaml` and add
  `web_username` / `web_password` to `secrets.yaml`. It is HTTP authentication over
  plain HTTP: it keeps casual visitors out, but it is not encrypted.
- **Phones:** the tabs move to a bar at the bottom. Add the page to your home screen
  for an app-like shortcut.

## What you see depends on your build

Each page and section appears only if your build has the package behind it.
Without BLE there is no Bluetooth page; without the calibration package Maintenance
keeps only tray reload and the 24 h count; without `diagnostics` or `network_info`
those cards are missing. Nothing is configured in the app itself.

A banner under the header shows the most important problem, with what you can do:
connection lost, tray position unknown (recover or reload), calibration on, tray
empty, clock not set, the next feed needing more than remains, a hold, or anything
else the feeder lists under Faults.

## Pages

- **Dashboard:**
  - the tray as a dial of its seven slots, the next feed and how long the food lasts;
  - **Feed now**: it asks first, shows whether the feeder is ready (the feed guard's
    answer), and reports the feeder's own result ("too soon", "daily cap reached", ...);
  - today's feeds with their state, status, recent feeds;
  - Hold (a choice of durations) or Resume, and Tray reload with a short walkthrough.
- **Schedule:**
  - the three profiles side by side; looking at one never changes what runs, and
    "Make active" does that;
  - **Edit** on an event sets its time (clock, or before or after sunrise or sunset),
    portions, days and on or off. Saving writes the fields in a safe order and checks
    the result;
  - cadence (every N days) and fast days per profile;
  - one-shot feeds, in the feeder's own time.
- **Configuration:**
  - the feeding safety limits, reload pause, refill interval and location, each with
    the feeder's own limits and units;
  - the time zone (a POSIX string; empty means the YAML's, or Home Assistant's);
  - network: the name (used after a restart), joining another Wi-Fi network (at once), and
    the fallback access point's password (used after a restart). Passwords are never shown
    or sent back;
  - board and pins (with the `hardware` package): a board preset or each pin, saved at
    once and used after a restart (Restart is offered when one is waiting); only pins
    the chip can spare are listed, and "none" for a button or LED that is not fitted;
  - the settings password: set, unlock (for this page, until it is reloaded), lock,
    change or remove. Locked, changes to the safety limits, the calibration, the
    pins, the keys and the network are refused with a message saying so;
  - keys: where the Home Assistant and Bluetooth keys come from (none, your YAML, or
    saved on the feeder); generate one (made in the browser and shown once), enter
    your own, or clear the Home Assistant key, where the build allows it; and the two
    switches for what Bluetooth may change. The feeder never sends a key back.
- **Maintenance:**
  - a calibration walkthrough (start, pick a slot, nudge it, next slot, finish and save)
    and the servo pulse range;
  - declaring the tray position after a power cut, or reloading;
  - the 24 h count, for debugging.
- **Bluetooth** (BLE builds): the connection, the client (only the last two bytes of
  its address), counters, and the last 32 connections and writes.
- **Diagnostics:** system, clock, network addresses, memory, the live log, and every
  entity the feeder reports, each with a control. Anything no other page shows is here.

## Good to know

- **The feeder decides.** The app asks, and shows what the feeder answers; every feed goes
  through the same guard as the schedule, Home Assistant, BLE and the button.
- **A few browsers at a time.** The web server has only a handful of connections; a page
  in the background lets go of its connection after 30 seconds and takes it again when
  you return.
- **After a firmware update**, an open page offers to reload itself.
- **Shared with Home Assistant:** the schedule editor is one shared set of controls. If
  someone edits the schedule in Home Assistant at the same moment, the app may say so after
  saving, and names what to check.
