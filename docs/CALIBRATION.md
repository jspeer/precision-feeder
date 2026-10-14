# Calibration

The tray is moved by a servo to absolute angles: 7 positions, 25 degrees apart
across a 175 degree sweep (both are configurable). Mechanical tolerances mean
each position needs a small correction.

*Written with the assistance of an AI (Anthropic's Claude); verify against your
own build.*

## First-time setup

1. Wire the servo (signal to `servo_pin`, 5 V, GND) and power the board.
2. Turn the tray fully **clockwise** to its end stop and fit it so slot 0, the
   closed slot, lines up with the alignment mark on the base. This matches the
   original firmware's instructions.
3. Flash the firmware. At boot the servo moves to position 0 (a freshly flashed
   device assumes a freshly loaded tray) and then goes limp after a second.

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

The YAML `min_pulse` / `max_pulse` are only the starting values for a fresh
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

## What still needs bench confirmation

- The MG90S's real end stops against the 2.5 degree margin at position 0, and
  the default 544 to 2400 µs pulse range (the original's, tuned on an SG92R).
- Whether the tray stays put when the servo is released after a move.
- Whether the servo twitches at power-up (and whether the 10 kΩ pull-down is needed).
