# The web app

The feeder carries its own web app (firmware 1.1.0 and later; this guide describes 2.0). It runs the whole feeder
from any browser on your network, on a computer or a phone, with no Home Assistant, no
internet and nothing to install. It is stored in the firmware, so nothing is loaded
from the internet.

*Written with the assistance of an AI (Anthropic's Claude); check it against your own feeder.*

## Opening it

- Browse to `http://<name>.local` (the prebuilt firmware: `precision-feeder-` and the end of
  the board's MAC address, for example `precision-feeder-a1b2c3.local`; a YAML build: its
  `name`), or to the feeder's IP address. IPv4 and IPv6 both work; Diagnostics lists the
  addresses. A new feeder opens the setup wizard.
- It is in the prebuilt firmware, and in a YAML build with the `web_server` package (on in
  both examples).
- **Passwords:** none by default. A **settings password** (Configuration) protects the
  safety limits, calibration, pins, keys, network and firmware ([SECURITY.md](SECURITY.md)).
  A YAML build can also put the whole app behind a login ([BUILDING.md](BUILDING.md)).
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
  - the tray as a dial of its slots (seven on the servo tray, the disc's own count on the disc), the next feed and how long the food lasts;
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
  - the time zone, chosen by place or typed as a POSIX string (empty means the YAML's, or
    Home Assistant's), and the NTP servers (2.0.2; up to three, empty means the build's);
  - network: the name (used after a restart), joining another Wi-Fi network (at once; 2.3.0:
    picked from the networks the feeder sees, or typed),
    forgetting it (2.3.0, builds with no network in their YAML: it restarts into the access
    point), and the fallback access point's password (used after a restart). Passwords are never shown
    or sent back;
  - drive, board and pins (with the `hardware` package): the drive (servo or direct-drive
    disc, in a build with the disc's package), a board preset or each pin (the servo's
    while the servo is the drive, the disc's five motor pins while the disc is), saved at
    once and used after a restart (Restart is offered when one is waiting); only pins
    the chip can spare are listed, and "none" for a button or LED that is not fitted;
  - the settings password: set, unlock (for this page, until it is reloaded), lock,
    change or remove. Locked, changes to the safety limits, the calibration, the
    pins, the keys and the network are refused with a message saying so;
  - keys: where the Home Assistant and Bluetooth keys come from (none, your YAML, or
    saved on the feeder); generate one (made in the browser and shown once), enter
    your own, or clear a key saved on the feeder (the Home Assistant key, and since 2.3.0 the Bluetooth key),
    where the build allows it; and the two
    switches for what Bluetooth may change. The feeder never sends a key back;
  - MQTT (with the `mqtt` package, 2.1.0): on or off, the broker, port, username,
    password and topic prefix, whether it is connected and the prefix in use; and (2.2.0) whether MQTT may
    change the safety limits. Saved at once; once MQTT has connected, changes are used after a restart.
    The password is never shown or sent back.
- **Setup** (`#setup`, not a tab; 2.0.0): opened by itself on a new feeder, and from
  Configuration (Device). Steps: welcome, network, time zone (by place) and location,
  board and pins, keys, settings password, calibration. Each saves as it goes and can be
  skipped; Finish (or skipping) tells the feeder setup is done. On a prebuilt binary whose
  access point has no password yet, the network step strongly recommends setting one.
- **Firmware** (`#firmware`, not a tab; 2.0.0): linked from Diagnostics. The installed version
  and variant; the update check (your browser reads the latest release's manifest from GitHub
  Pages when the page opens: up to date, an update with its release notes, or "Couldn't
  check"; it can be turned off for that browser); and the upload: choose an `.ota.bin`, the
  page names it from the release (or warns that it is not one), uploads it signed with the
  settings password, and the feeder checks it again before restarting into it.
- **Maintenance:**
  - a calibration walkthrough (start, pick a slot, nudge it, next slot, finish and save)
    and the servo pulse range; on the direct-drive disc, its step-by-step wizard (slots and
    direction, zero, run turns, check every slot, trim) and its tuning;
  - declaring the tray position after a power cut, or reloading;
  - the 24 h count, for debugging.
- **Bluetooth** (BLE builds): the connection, the client (only the last two bytes of
  its address), counters, and the last 32 connections and writes.
- **Diagnostics:** system, clock, network addresses, memory, the live log, and every
  entity the feeder reports, each with a control. Anything no other page shows is here.
  System links to the Firmware page.

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
