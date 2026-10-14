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
If it buzzes or stalls at the ends of its travel, reduce `sweep_degrees` or
adjust `min_pulse` / `max_pulse`. The defaults, 544 to 2400 µs, are what the
original firmware drove its TowerPro SG92R with; an MG90S fits the same mount but
may turn a slightly different amount per microsecond, which shows up as an error
that grows from slot 1 to slot 6.

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
