# Mechanisms: what moves the tray

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

Since firmware 2.1.0 the feeder's logic (the feed guard, the schedule, the history, the tray position) does not know
what motor it drives. That is a **mechanism**: a motor, its driver and the tray or disc it turns. The servo fan tray
of the Precision Feeder Rev2 is the reference mechanism, and the only one on `main`. This guide is for building your
own: another motor, another tray, another number of slots.

The rules are in [AGENTS.md](../AGENTS.md), "Adding a mechanism", and the design in [DESIGN.md](../DESIGN.md)
section 3.0. This page is the walkthrough and the checklist.

## How the feeder uses a mechanism

The core asks for one thing at a time and waits for the answer:

| The core calls | When | The mechanism |
|---|---|---|
| `capacity()` | always | says how many portions one load holds |
| `feed_one(from)` | the guard allowed a feed | moves from position `from` to `from + 1`, wiggle included |
| `poll()` | every loop while it moves | answers BUSY, DONE, or FAULT (it stopped and does not know where the tray is) |
| `reload()` | Tray Reload | gets the tray ready to load; afterwards it is at position 0. It may move nothing |
| `restore(p)` | boot, Declare Tray Position, end of calibration | puts or keeps the tray at `p`; it may move nothing |
| `stop()` | Stop, or a fault | stops at once |
| `release()` | after `settle_ms()` of stillness | lets go of the motor |

Positions are portions fed since the last reload: 0 is a full load, `capacity()` is empty. The core saves its intent
before every move, so a power cut mid-move leaves the position unknown and feeding stops until someone reloads or
declares the position. A mechanism cannot change that, and must not try to.

## What you write

Take the servo as the pattern: `components/precision_feeder/mechanism_servo.{h,cpp}` and
`packages/mechanism_servo.yaml`.

1. **The mechanism**, `components/precision_feeder/mechanism_<name>.{h,cpp}`: a class implementing `Mechanism`
   (`mechanism.h`), plain C++ with no ESPHome includes. Give it a unique `name()`.
2. **Its tuning table**, if it has values a user should tune: a `TuningParam` per value (key, name, unit, limits,
   default, scale) and `apply_tuning()`. The core checks every change against the table and saves the values in the
   Tuning record. Change `tuning_layout()` whenever the table's entries or order change.
3. **Its hardware hooks**, in the ESPHome adapter (`precision_feeder.cpp` or a component of its own): the pins, the
   PWM, the encoder. If it needs a control loop faster than ESPHome's main loop (a few milliseconds), run it in its
   own task, and keep the logic in a pure function the host tests can call with a time and the inputs.
4. **Its package**, `packages/mechanism_<name>.yaml`: `tray_capacity` (its capacity), its pins as substitutions with
   comments, its outputs, its tuning and calibration entities (config category, listed in the settings password's
   protected entities). It registers the mechanism with the feeder before the feeder starts (`set_mechanism()`) and
   reserves its pins (`reserve_pin()`), so the button and LED cannot be moved onto them.
5. **The one-line switch**: in `packages/core.yaml`, `mechanism: !include mechanism_<name>.yaml` instead of the
   servo's.
6. **Its web description**: an entry in `MECHANISMS` in `web/src/mechanisms.js` under its `name()`, with its tray
   drawing (`dial`), its wording and its calibration cards, and its entities in `web/src/entities.js`. Then
   `python3 tools/build_web.py`.
7. **Tests**: host tests with a simulated motor (moves, a full load fed to empty, each fault, a stop mid-move), in
   `tests/`; code you tuned the motor with can go in `tests/reference/`. `tools/check.sh` must pass, including the
   servo's golden trace.
8. **Docs**: a subsection of DESIGN.md section 3, a section in docs/CALIBRATION.md, a CHANGELOG entry, bench results
   in docs/BENCH.md.

## Safety, which is not optional

A servo cannot turn past its end stop. Most other motors can, and a motor that keeps turning empties the tray into
the tank. So a mechanism whose motor can keep going must:

- stop and answer FAULT when it stalls, runs the wrong way, or takes too long (a timeout worked out from the
  distance, not a fixed minute);
- never move more than the planned distance plus a small margin;
- watch its own control loop, and stop the motor if the loop falls behind;
- never retry a feed by itself, and never reverse past the slot it started from (a small, bounded wiggle aside).

The core already counts a faulted portion as fed (the safe side for the daily cap) and stops feeding until the
position is known again. A missed meal beats an overfeed.

## Where it lives

A mechanism for your own build lives on a branch, `variant/<name>`: not tagged, no prebuilt binaries, its own
`fw_variant` (so the web app's update check never offers it main's firmware) and a version suffix
(`2.1.0-<name>.1`). Rebase it onto main after each release; the mechanism interface keeps that small. With the owner's
decision it moves to main, where it becomes a second drive beside the servo, chosen on the device (DESIGN.md section
3.4). The first one did: a direct-drive DC motor with an encoder turning a slotted disc, built on
`variant/n20-direct-drive` and on main since 2.1.0 (`packages/mechanism_n20.yaml`;
[docs/plans/drive-decoupling.md](plans/drive-decoupling.md), stage 2).

## Checklist

- [ ] `mechanism_<name>.{h,cpp}`: implements `Mechanism`, no ESPHome includes, unique `name()`
- [ ] faults: stall, wrong direction, timeout from the distance, travel limit, loop watchdog (if it can keep turning)
- [ ] tuning table with limits and a `tuning_layout()`; YAML min/max match it (`tests/test_consistency.py`)
- [ ] `packages/mechanism_<name>.yaml`: `tray_capacity`, pins as documented substitutions, `reserve_pin()`
- [ ] added beside the servo in a build's YAML (an example), offered as a drive with `offer_drive()`; the servo stays the
      prebuilt firmware's default
- [ ] `web/src/mechanisms.js` description, entities in `web/src/entities.js`, `web/dist/` rebuilt
- [ ] host tests with a simulated motor; `tools/check.sh` passes, servo golden trace included
- [ ] DESIGN.md, CALIBRATION.md, CHANGELOG.md, BENCH.md
- [ ] on its branch: `fw_variant` and version suffix set, README banner; on main: CI and the examples
