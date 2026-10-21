# Calibration

This guide is for the servo fan tray, the mechanism `main` builds. A feeder with another mechanism (another
motor and tray, [MECHANISMS.md](MECHANISMS.md)) has its own section here or on its branch.

The tray is moved by a servo to absolute angles: 7 positions, 25 degrees apart
across a 175 degree sweep (both are configurable). Mechanical tolerances mean
each position needs a small correction.

*Written with the assistance of an AI (Anthropic's Claude); verify against your
own build.*

**In the built-in web app**, Maintenance has the same steps as a walkthrough (the setup
wizard's last step points there): start calibration, pick a slot, nudge its offset by 0.1
or 1 degree, go to the next slot, then finish and save. The servo pulse range is under
"Servo pulse range (advanced)" there. Changing the calibration needs the settings password
when one is set. The steps below use the entity names, as Home Assistant shows them.

## First-time setup

1. Wire the servo (signal to the servo pin, GPIO1 on the S3-Zero by default; 5 V; GND) and
   power the board.
2. Turn the tray fully **clockwise** to its end stop and fit it so slot 0, the
   closed slot, lines up with the alignment mark on the base. This matches the
   original firmware's instructions.
3. Flash the firmware. At boot the servo moves to the saved position (position 0 on a new
   feeder, which assumes a freshly loaded tray) and then goes limp after a second.

## Fine-tuning each position

Do this in two stages: first the servo's scale (pulse range), then the per-slot
offsets. The offsets are limited to ±15 degrees, enough for small corrections but
not for a servo that turns a different amount per microsecond than assumed.

### 1. Pulse range

1. Turn **Calibration Mode** on and pick **Calibration Position** `0`.
2. Adjust **Servo Min Pulse** (µs) until the closed slot lines up with the mark.
   The servo moves again after every change.
3. Pick position `6` and adjust **Servo Max Pulse** until slot 6 lines up with the
   opening. A bigger value makes every step longer. Changing it moves slot 0 a
   little too, so go back to position 0 and repeat once or twice.
4. Check positions `1` to `5`. If they are all close, go on to the offsets.

Rough rule: if the offsets you would need grow by *d* degrees per slot, multiply
the span (max − min) by (25 + *d*) / 25.

**MG90S:** its datasheet gives 500 to 2500 µs for 0 to 180 degrees (neutral
1500 µs, ±3 degrees), so start the range there rather than at the defaults. The
first bench build ended at 637 to 2675 µs: the same scale within 2%, shifted by
where the horn sits on its spline. Increasing pulse turns it counter-clockwise,
so slot 0 is at the clockwise end, as the setup above expects.

The build's `min_pulse` / `max_pulse` (544 / 2400 µs) are only the starting values for a new
device. Once you change the range here it is saved and used from then on (a
reflash does not reset it). The limits are 300 to 1500 µs for the minimum, 1500
to 3000 µs for the maximum, and at least 500 µs between them. The firmware never
commands more than about 155 degrees, so a large maximum does not by itself push
the servo into its end stop; still, listen for buzzing at position 6.

### 2. Per-slot offsets

Hobby servos have a deadband (3 degrees on the MG90S): within it the servo does
not correct, so where it stops depends a little on the direction it came from.
Feeds always arrive at a slot from the lower position (the wiggle ends with a
forward move), so check each position after arriving from position 0's side:
select the position below it first, then the position itself.

1. Turn **Calibration Mode** on. Feeds are refused while it is on, and the LED is
   white.
2. Pick **Calibration Position** `0`. The servo moves there. Adjust
   **Calibration Offset** (in degrees, 0.1 steps) until the closed slot lines up
   with the mark.
3. Pick positions `1` to `6` in turn and adjust until each open slot lines up
   with the opening in the base.
4. Turn **Calibration Mode** off. Offsets are saved and the tray returns to its
   current position.

If your servo moves the wrong way for your mounting, set `servo_invert: "true"`.
The default pulse range, 544 to 2400 µs, is what the original firmware drove its
TowerPro SG92R with. An MG90S fits the same mount but may turn a different amount
per microsecond: on the first bench build it needed about 3.5 degrees more per
step, which is what the pulse range stage above is for.

Position 0 is commanded to 2.5 degrees, and the firmware never goes below 0, so a
negative offset at slot 0 can only take off 2.5 degrees. Mount the servo horn so
that slot 0 needs a positive correction (or a higher Servo Min Pulse).

## Loading and the tray position

Press **Tray Reload** before filling the tray. That returns the tray to
position 0, marks all 6 portions as available, and pauses scheduled feeding for
the *Reload Pause* (10 minutes by default) so you can finish loading the tray.

If power is lost during a feed, or you press **Stop** mid-feed, the position is
**unknown** (LED magenta) and feeding is blocked. Look at the tray, set
**Declared Tray Position** to where it really is, and press **Declare Tray
Position**; or reload the tray. On a build with the push button, a short press
reloads the tray in this state (it moves to the closed slot), then load it.

## What the bench build showed

From the first calibrated feeder ([BENCH.md](BENCH.md), 2026-10-14):

- The default 544 to 2400 µs range does not fit an MG90S: it ended at 637 to 2675 µs, with
  offsets of at most +3.3 degrees. Calibrate the range per servo.
- The horn had to be re-seated once so that slot 0 needs a positive correction.
- No servo twitch at power-up, so the 10 kΩ pull-down was not needed there.
- The tray stays put once the servo is released (checked with an empty tray; recheck with
  food in it). Still to watch in daily use: whether the servo strains at position 6.

## The direct-drive disc (branch `variant/n20-direct-drive`)

A disc of open slots turned directly by a gear motor with an encoder (DESIGN.md section 3.3). The servo steps above do
not apply: there are no slot offsets and no pulse range. Instead the firmware measures how many encoder ticks one turn
of the disc takes, and checks every slot.

**Empty the disc first.** The calibration turns it, and every open slot passes over the hole.

In the web app: **Maintenance → Calibration** (or the Calibration buttons in Home Assistant). Turn **Calibration
mode** on (feeding is refused until you turn it off), then follow **Next step**: it says which step you are on and
which button to press.

1. **Slots and direction.** Check **Disc slots** matches your disc (15 on the bench print; 3 to 60). Change it here if
   not: it can only be changed in calibration mode, because it moves every slot. Then press **Start**: the disc gives
   a short push forward. Did it turn the way it should when it feeds? Press **Yes** or **No**. The firmware also
   checks the encoder counted up while it turned; if the motor or the encoder is the wrong way round it sets Reverse
   Motor or Reverse Encoder for you, and you press **Start** once more. If the encoder did not count at all, check
   its C1, C2 and 3V3 wires.
2. **Zero.** Centre one empty slot exactly over the hole with **Nudge 0.1°** and **Nudge 1°**. They only turn
   forward, so the gearbox's play never comes into it; if a slot goes past the centre, press **Overshot**: it backs
   up about 5° and comes forward 4°, and you nudge forward from there. Then press **Zero**.
3. **Run turns.** The disc makes one full turn (or Calibration Turns: three is more accurate) and stops close to the
   same slot. Nudge it until that slot is centred over the hole again, then press **Measured**. Ticks Per Turn is now
   what it really took. A measurement more than 20% off is refused (a slipped disc, not a calibration): zero again.
4. **Check every slot.** **Next slot** moves one slot exactly as a feed does, wiggle included. Look at the slot:
   - centred over the hole: press **Next slot** (or **Yes**) for the next one;
   - not centred: nudge it to the centre (or Overshot, then nudge, if it went past), then press **Trim here**. The
     firmware works out how far you moved it and shifts **every** slot by that much (Slot Trim: + is further
     forward, − further back). You never type the trim or work out its sign. Then carry on with **Next slot**.
   - If the slots are off by different amounts rather than all the same way, the print is uneven; per-slot offsets
     would be needed, and the firmware has none yet.
5. Turn **Calibration mode** off. The disc has turned, so the feeder no longer knows which slots hold food: fill
   every slot except the one over the hole and press **Tray reload**.

The other tuning values (Maintenance → Motion and Wiggle) start at the bench's: Drive Power 100%, Braking Distance 160
ticks, Wiggle 2° × 4. Leave them unless the disc overshoots, stalls or the food sticks.

**After a power cut mid-feed** the position is unknown and feeding stops. Turn calibration mode on, nudge until a slot
is centred over the hole, turn calibration mode off, then **Declare position** (portions fed since the last reload), or
refill the disc and reload.
