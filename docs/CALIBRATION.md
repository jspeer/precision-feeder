# Calibrate the tray or disc

Calibrate with **no food in the mechanism**. Open **Maintenance → Calibration** in the web app and follow the section
for your drive. A settings password, when set, is required. Calibration blocks feeds; finish it before loading.
Home Assistant does not expose calibration or position-recovery controls.

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); verify against your own build.*

## Choose the mechanism

Prebuilt firmware starts as a servo feeder. For the disc, select **Configuration → Drive → Direct-drive disc**,
check the motor pins and restart. Saved drive and pin changes apply after a restart.

- [Servo fan tray](#servo-fan-tray): pulse range first, then the seven slot offsets.
- [Direct-drive disc](#the-direct-drive-disc): direction, zero, ticks per turn and a shared slot trim.

## Servo fan tray

There are seven positions, 25° apart: position 0 is closed; positions 1–6 hold food. The servo moves 150° between
the first and last positions. The YAML value `sweep_degrees: 175` describes seven slot widths, not that travel.

### First-time setup

1. Check the wiring and motor supply: servo signal to the selected pin (GPIO1 by default), power to a suitable
   5 V supply and a shared ground. Do not power the servo from the board's 3.3 V regulator.
2. Follow the print's mounting geometry: the clockwise end corresponds to the closed slot and its alignment mark.
   Seat the horn so small calibration corrections can line up slot 0 without straining the servo.
3. The firmware commands the saved position at boot (position 0 on a fresh feeder), then releases the servo after
   the detach delay. Keep the tray empty during setup.

### Fine-tuning each position

Start calibration in Maintenance. The walkthrough selects a position and nudges its offset by 0.1° or 1°.
Use **Servo pulse range (advanced)** to correct the overall travel before applying individual offsets.

#### 1. Pulse range

1. Turn **Calibration Mode** on and select **Calibration Position 0**.
2. Adjust **Servo Min Pulse** until the closed slot lines up with the mark. Each change moves the servo again.
3. Select **position 6** and adjust **Servo Max Pulse** until slot 6 lines up with the outlet. Increasing the span
   makes each step longer. Recheck position 0, then position 6, until both align.
4. Check positions 1–5. If the error grows steadily with each slot, correct the pulse range before using offsets.

The default range is 544–2400 µs. It is a starting value, not a calibration for every MG90S or SG92R. Servo units
and horn placement differ; do not copy another feeder's final values. Listen for buzzing or strain and narrow the
range if necessary. The allowed minimum is 300–1500 µs, the maximum 1500–3000 µs, with at least 500 µs between them.
Saved calibration overrides the YAML's starting range and survives an update without erasing.

A useful scale check: if the additional correction grows by *d* degrees per slot, the required pulse span is roughly
its current span multiplied by (25 + *d*) / 25. Recheck physical alignment after any change.

#### 2. Per-slot offsets

1. With Calibration Mode on, select position 0 and adjust **Calibration Offset** until it aligns.
2. Select positions 1–6 and adjust each until its opening lines up with the base.
3. Check each position after approaching it from the lower position: select the previous slot, then the one being
   checked. Servo deadband and backlash can make the approach direction matter; feeding ends with a forward move.
4. Turn Calibration Mode off. Offsets are saved and the servo returns to its current tracked position.

Offsets are limited to ±15°. Position 0 starts at 2.5°, and commands cannot go below 0°, so its negative correction
is limited by that boundary. Re-seat the horn or adjust Min Pulse if needed. A reversed servo mounting can use
`servo_invert: "true"` in a YAML build.

### Loading and the tray position

Press **Tray Reload before filling**. Wait for the tray to reach its closed position, then fill the six open slots.
The reload pause (10 minutes by default) skips scheduled meals while you load; it does not block manual feeds.

After an interrupted move or Stop, the position becomes unknown. Inspect the tray and use Maintenance to declare
the observed position, or reload and refill. A short press of the optional button also reloads when position is
unknown. Do not declare a position just to clear the warning without checking it.

Bench measurements and observations are in [BENCH.md](BENCH.md). Calibration must be checked on your own print and
servo, including whether the loaded tray stays aligned after the servo releases.

## The direct-drive disc

The disc uses encoder ticks, not servo angles or per-slot offsets. Its calibration measures ticks per turn and
checks the slot grid. Empty it first: calibration turns every opening over the chute.

In **Maintenance → Calibration**, turn Calibration mode on and follow **Next step**:

1. **Slots and direction.** Set **Disc slots** to the actual count (3–60; 15 by default), then press **Start** for a
   short forward push. Answer Yes or No to the direction question. The firmware also checks the encoder count and
   sets Reverse Motor or Reverse Encoder as needed; press Start again if requested. With no counts, check the
   encoder's C1, C2 and 3.3 V wiring.
2. **Zero.** Centre an empty slot over the hole with **Nudge 0.1°** and **Nudge 1°**. Nudges go forward to take up
   gearbox play consistently. **Overshot** backs up about 5° and comes forward 4°, then lets you nudge forward again.
   Press **Zero** when centred.
3. **Run turns.** Run one full turn, or set Calibration Turns to three for a longer measurement. Nudge the original
   slot back to the centre and press **Measured**. The measured count becomes Ticks Per Turn. A measurement more
   than 20% off is refused; check for a slipped disc and zero again.
4. **Check every slot.** **Next slot** moves as a feed does, including the wiggle. If aligned, continue with Next
   slot or Yes. Otherwise nudge to the centre, using Overshot if needed, and press **Trim here**. That correction
   shifts the entire slot grid; continue checking the rest. Different errors at different slots cannot be corrected
   by this shared trim: per-slot disc offsets are not supported.
5. Turn Calibration mode off. Movement has made the food position unknown. Fill every slot **except the one over
   the chute**, then press **Tray reload** to mark a full load. Reloading the disc does not move it.

Motion and Wiggle tuning starts at Drive Power 100%, Braking Distance 160 ticks and Wiggle 2° × 4. Keep those values
unless the mechanism needs tuning. Slots, ticks per turn, trim and directions can only be changed in calibration
mode. The mechanical check is alignment by eye, including on a printed disc.

After interrupted motion, use calibration mode to centre an empty slot, leave calibration mode, then declare the
portions fed since the last reload. If you cannot establish which slots still hold food, refill and reload instead.
See [position recovery](USING.md#recovering-an-unknown-position).
