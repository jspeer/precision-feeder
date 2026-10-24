# Scheduling

Set up the on-board schedule in the built-in web app's **Schedule** page. Home Assistant can switch
Scheduler and Active Profile, but cannot edit profiles, events or one-shot feeds. MQTT's own topics and BLE can
also edit profiles ([MQTT.md](MQTT.md), [PROTOCOL.md](PROTOCOL.md)). The schedule runs on the feeder itself: once
set, feeding carries on with no Home Assistant and no network, as long as the clock is set.

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); check it against your own feeder.*

## The idea in one minute

- There are **three profiles**, A, B and C. Only one runs at a time: the
  **Active Profile**. Keep different plans in them, for example "normal",
  "holiday" and "sunset only", and switch with one click.
- Each profile has **four events** (1 to 4). An event is one feeding: *when* (a
  clock time, or so long before or after sunrise or sunset), *how much* (1 to the mechanism's capacity: 6 on the servo, 14 on the default 15-slot disc) and *on which weekdays*.
- Each profile also has two whole-profile settings: **every N days** (feed every
  day, every second day, ...) and **fast days** (weekdays with no feeding at all).
- A full servo tray holds **6 portions**; a disc holds **its slot count minus one**. One portion is one step of the tray. Use
  hard food (pellets): flake food can stick in the slots and is not advised.

**Editing and running are separate.** Looking at a profile in the Schedule page does not activate it. You can
build profile B while A keeps feeding. The raw **Edit Profile** selector chooses which profile the shared editor
controls change; its **Edited Profile** sensor shows `[active]` or `[not active]`. **Active Profile** chooses which
plan runs.

**In the Schedule page**, choose a profile, set its cadence and fast days, then press **Edit** beside an event.
Choose its time or sun anchor, portions and weekdays, then save the dialog. **Make active** selects the plan to run.
The app disables a live event before changing its time or days, writes the fields, and enables it last. It checks
what the device saved. Use the one-shot card for a dated feed.

**The underlying entities apply each write immediately**, with flash saving a couple of seconds later. The raw
controls in Diagnostics have no combined Save button. The reference below describes those controls; use the
Schedule page for normal editing.

## Before you start

- **The clock must be set.** "Clock Valid" must be on. It is as soon as the
  feeder gets internet time (NTP), or Home Assistant time if that package is
  enabled, or a clock pushed over BLE. Without a clock nothing scheduled
  runs; manual feeds still work.
- **For sunrise or sunset events, set Latitude and Longitude** (decimal degrees,
  for example 51.5074 and -0.1278 for London; west and south are negative). While both are
  0 the location counts as not set, those events are skipped, and Faults says
  "sunrise/sunset unavailable".
- **Check the Daily Portion Cap** (default 4). It is a safety limit on portions in
  any 24 hours, and it also applies to the schedule (see the rules below).
- The **Scheduler** switch must be on for profiles to run.

## Examples

These examples use the servo tray's 6 portions; plan around how often you will refill.

**A: two meals a day, no feeding on Sundays.**
Every N Days 1, Fast Days `sun`.
Event 1: clock, `08:00`, `all`, 1 portion, on.
Event 2: clock, `18:00`, `all`, 1 portion, on.
Events 3 and 4: off.
Edited Profile: `A: 08:00 x1, 18:00 x1; fast: sun`. Two portions a feeding day:
a full tray lasts three feeding days.

**B: holiday, one meal every second day.**
Every N Days 2, Fast Days `none`, then press "Restart Cycle Today" if the first
meal should be today.
Event 1: clock, `18:00`, `all`, 1 portion, on. Others off.
`B: 18:00 x1; every 2 days`. A full tray lasts six meals: day 1, 3, 5, 7, 9
and 11, so about 11 days.
Before you leave: Tray Reload, fill the tray, set Active Profile to B.

**C: half an hour before sunset.**
Set Latitude and Longitude first. Every N Days 1, Fast Days `none`.
Event 1: anchor `sunset` (choose it first), time `-00:30`, `all`, 1 portion, on.
Others off. `C: sunset-00:30 x1`. The time follows the sunset through the year
(and daylight saving, if your timezone has it).

## Field reference

| Control | What it does | Accepted values |
|---|---|---|
| Edit Profile | which profile the controls below change | A, B, C |
| Profile Every N Days | feed only every N-th day | 1 to 14 (1 = every day) |
| Restart Cycle Today | makes today day 1 of the every-N cycle (for the edited profile) | button |
| Profile Fast Days | weekdays with no feeding in this profile | days, as below |
| Edit Event | which of the profile's four events the controls below change | 1 to 4 |
| Event Anchor | what the time is measured from | clock, sunrise, sunset |
| Event Time | clock: time of day. Sunrise/sunset: offset from the sun event | clock: `HH:MM`, 00:00 to 23:59 (`8:30` is fine). Sun: `+HH:MM` or `-HH:MM`, up to 12 hours either way; `+90` means plus 90 minutes; no sign means plus. Nothing may follow the time (`08:00 PM` is refused) |
| Event Days | weekdays this event feeds on | `all` (or `daily`), `none`, `weekdays` (Mon to Fri), `weekend`, or day names separated by commas or spaces: `mon,wed,fri` (`monday` works too) |
| Event Portions | portions this event feeds | 1 to the mechanism's capacity (6 on the servo) |
| Event Enabled | whether the event runs | on / off |
| Active Profile | the profile the feeder runs | A, B, C |
| Scheduler | runs the active profile's events | on / off |

Careful with Event Days: `none` (or an empty box) means the event never feeds,
even while it is switched on.

## Rules worth knowing

- **Missed means skipped.** An event feeds only in the minute it is due. If the
  feeder was off, on hold, in the reload pause, or the guard refused it for that
  whole minute, it is skipped and not made up later. A missed meal is safer than
  a double one.
- **Once a day per event.** Each event feeds at most once a day. If you move an
  event that already fed today to later today, it next feeds tomorrow (Next Feed
  shows that).
- **Switching profiles mid-day:** events of the new profile that are still due
  today do run. The daily cap still applies.
- **The daily cap applies to the schedule.** If a profile's events add up to more
  portions in a day than the Daily Portion Cap, the later ones are refused every
  day. The **Edited Profile** sensor warns while you edit (`! 5 portions/day >
  cap 4`), and **Faults** warns for the active profile. The check adds up each
  weekday's events; the cap itself is a rolling 24 hours, so a late feed followed
  by an early one the next morning can also meet it.
- **Keep events a few minutes apart.** After any feed, the next one waits at
  least the Minimum Gap Between Feeds (default 60 s). Two events in the same
  minute: the second is refused. An event with several portions takes a little
  while (each portion is a tray step, Gap Between Portions apart).
- **Every N days counts calendar days; fast days are simply skipped.** With
  "every 2 days" and a Sunday fast, a feeding day that falls on Sunday is missed,
  not moved to Monday. The cycle starts the day the clock is first set; "Restart
  Cycle Today" moves it.
- **Sun events after midnight.** "Sunset + 5 h" can land after midnight. It
  belongs to the day of the sunset: its weekday (and fast day and every-N day) is
  the evening's, even though it feeds the next morning.
- **Sun events and location.** With no location set, sun events are skipped and
  Faults says so. On a day with no sunrise or no sunset (far north or south:
  polar night or midnight sun), a sunrise event counts from 06:00 and a sunset
  event from 18:00 instead, so it still feeds; Faults says when that happens.
- **Hold and reload pause stop the schedule, not you.** "Hold Scheduled Feeding"
  (for "Hold Duration" hours, or until "Cancel Hold" when it is 0) and the
  pause after "Tray Reload" skip scheduled events; manual feeds still work.
- **One-shot feeds** ("Add One-shot Feed", format `2026-10-20 18:00`, with
  "One-shot Portions") are single dated feeds outside the profiles. They run
  even with the Scheduler switch off; a hold still stops them.

## Checking your work

| Sensor | Shows |
|---|---|
| Edited Profile | the profile being edited, `[active]` or `[not active]`, and a cap warning |
| Active Schedule | the profile being run |
| Next Feed | when the next scheduled or one-shot feed is due |
| Needs Food | on when the tray is empty, or the next scheduled or one-shot feed needs more portions than remain |
| Runway / Tray Empty On | how long the tray lasts under the active profile |
| Schedule Over Capacity | the profile needs more portions than the tray holds before the next refill ("Refill Interval") |
| Faults | anything stopping the schedule: clock, location, tray empty or unknown, cap |
| Recent Feeds / Last Result | what actually happened |

**Needs Food as a trigger.** If the next feed asks for more portions than are
left, the feeder still feeds what is left rather than nothing, so that meal comes
up short. "Needs Food" turns on as soon as that is the case (or the tray is
empty), which makes it a good trigger for a Home Assistant notification, for
example "notify me when Needs Food turns on". It stays off while the tray
position is unknown (Faults reports that instead). Over BLE the same signal is
Status flag bit 12.

## Over BLE

A controller can read and write the same profiles through the Profile
characteristic ([PROTOCOL.md](PROTOCOL.md)). There, profiles are 0 to 2 and events
0 to 3 (the web app shows A to C and 1 to 4), and days are a bit mask with bit
0 = Sunday.

## Raw editor controls, step by step

1. **Edit Profile**: pick the profile to work on (A, B or C).
2. **Profile Every N Days**: 1 for every day, 2 for every second day, and so on
   (up to 14).
3. **Profile Fast Days**: weekdays with no feeding, for example `sun`, or `none`.
4. For each meal, pick an event with **Edit Event** (1 to 4), then, in this order:
   1. **Event Anchor**: `clock`, `sunrise` or `sunset`. Choose this *first*:
      changing the anchor resets the time (to 12:00 for clock, to +00:00 for
      sunrise and sunset).
   2. **Event Time**: `08:30` for a clock event; `+01:30` or `-00:45` for a
      sun event (see the field reference).
   3. **Event Days**: `all`, `weekdays`, `mon,wed,fri`, ...
   4. **Event Portions**: 1 to the mechanism's capacity (6 on the servo).
   5. **Event Enabled**: switch it on last. The event is live from now on.
5. Switch off any event you do not want (all four start switched off, at 08:00,
   12:00, 18:00 and 20:00 with 1 portion every day).
6. Read **Edited Profile** to check it, for example
   `A: 08:00 x1, 18:00 x1; fast: sun [not active]`.
7. When it is right, set **Active Profile** to it. **Next Feed**, **Runway**
   and **Tray Empty On** now follow this profile.

If a field does not accept what you typed, it goes back to its old value and the
feeder's log says why (for example `Event time '25:00' not understood`).
