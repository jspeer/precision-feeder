// Maintenance: calibrating the tray positions, the servo's pulse range, recovering the tray position, and the
// owner-approved debugging edit of the 24 h count (DESIGN.md section 13, 2026-10-14). Every step is a request the
// device carries out; feeds are refused while calibration mode is on (the device's own rule).

PAGES.maintenance = {
  mount(root) {
    this.keys = {};
    this.rows = [];
    this.busy = false;
    const parts = [];

    // The servo's slot calibration: whenever the build has its Calibration Position (packages/calibration.yaml).
    if (store.has(E.calibrationMode) && store.has(E.calibrationPosition)) {
      this.calStatus = h('p', { class: 'cal-status' });
      this.calStart = h('button', { class: 'btn btn-primary', onclick: () => this.startCalibration() }, 'Start calibration');
      this.calFinish = h('button', { class: 'btn btn-primary', onclick: finishCalibration }, 'Finish and save');
      this.slotCount = (store.get(E.calibrationPosition).option || []).length || 7;
      this.slots = h('div', { class: 'slot-picker', role: 'group', 'aria-label': 'Slot to adjust' },
        Array.from({ length: this.slotCount }, (_, i) => i).map(i => h('button', { type: 'button', class: 'btn slot-pick', 'aria-pressed': 'false',
          onclick: () => this.pickSlot(i) }, String(i))));
      this.offsetValue = h('span', { class: 'offset-value', 'aria-live': 'polite' });
      const nudge = (delta, text) => h('button', { class: 'btn', 'aria-label': `Offset ${text}`, onclick: () => this.nudge(delta) }, text);
      this.workArea = h('div', { class: 'cal-work' },
        h('div', { class: 'form-label' }, 'Slot'), this.slots,
        h('div', { class: 'form-label' }, 'Offset of this slot'),
        h('div', { class: 'nudge' }, nudge(-1, '−1°'), nudge(-0.1, '−0.1°'), this.offsetValue,
          nudge(0.1, '+0.1°'), nudge(1, '+1°')),
        h('div', { class: 'actions' },
          this.nextBtn = h('button', { class: 'btn', onclick: () => this.pickSlot(this.slot() + 1) }, 'Next slot'),
          this.calFinish));
      const pulses = [setting('Servo min pulse', 'Pulse width for 0°.', E.servoMinPulse),
                      setting('Servo max pulse', 'Pulse width for 180°. Bigger means longer steps.', E.servoMaxPulse)].filter(Boolean);
      this.rows.push(...pulses);
      parts.push(card('Calibration',
        h('p', { class: 'hint' }, 'Line each slot up with the opening in the base. Fit the tray turned fully clockwise, ' +
          'with slot 0 (the closed one) on the mark. Feeding is refused until you finish.'),
        this.calStatus,
        h('div', { class: 'actions' }, this.calStart),
        this.workArea,
        pulses.length ? h('details', { class: 'advanced' },
          h('summary', {}, 'Servo pulse range (advanced)'),
          h('p', { class: 'hint' }, 'If the error grows from slot to slot (each needs a bigger offset than the one ' +
            'before), fix that here first: Servo max pulse changes the step length, Servo min pulse lines up slot 0. ' +
            'Then fine-tune each slot. The two must stay at least 500 µs apart.'), pulses) : null));
    }

    parts.push(...mechanismCards(this.rows));

    // Recovery
    this.posRow = row('Tray', '');
    parts.push(card('Tray position',
      this.posRow.row,
      h('p', { class: 'hint' }, 'After a power cut in the middle of a feed the feeder no longer knows where the tray ' +
        'is and stops feeding. ' + mechanism().recoveryHint),
      h('div', { class: 'actions' },
        store.has(E.declarePosition) && store.has(E.declaredPosition)
          ? h('button', { class: 'btn', onclick: recoverPositionFlow }, 'Declare position') : null,
        h('button', { class: 'btn', onclick: trayReloadFlow }, 'Tray reload'))));

    // The 24 h count (owner decision 2026-10-14: debugging and recovery only)
    if (store.has(E.portions24h)) {
      this.countRow = row('Portions counted in the last 24 h', '');
      parts.push(card('Daily cap count',
        this.countRow.row,
        h('p', { class: 'hint' }, 'The number of portions the daily cap is counting right now. Changing it is for ' +
          'debugging and recovery: it never feeds or moves the tray, and every change is logged.'),
        h('div', { class: 'actions' }, h('button', { class: 'btn', onclick: () => this.editCount() }, 'Change the count'))));
    }

    append(root, ...parts);
    this.update();
  },

  slot() {
    return parseInt(store.text(E.calibrationPosition), 10) || 0;
  },

  update() {
    for (const r of this.rows) r._update();
    const state = store.text(E.trayState);
    setText(this.posRow.value, state === 'unknown' ? 'Position unknown'
      : `${mechanism().position(store.num(E.trayPosition))}, ${state}`);
    if (this.countRow) {
      const cap = store.num(E.dailyCap);
      setText(this.countRow.value, `${store.num(E.portions24h)}${isNaN(cap) ? '' : ` (cap ${cap})`}`);
    }
    if (!this.calStatus) return;
    const on = store.bool(E.calibrationMode);
    setText(this.calStatus, on ? 'Calibration mode is on: the servo follows the slot below.' : 'Calibration mode is off.');
    this.calStatus.className = on ? 'cal-status cal-on' : 'cal-status';
    this.calStart.hidden = !!on;
    this.workArea.hidden = !on;
    const slot = this.slot();
    this.slots.querySelectorAll('button').forEach((b, i) => b.setAttribute('aria-pressed', i === slot ? 'true' : 'false'));
    this.nextBtn.hidden = slot >= this.slotCount - 1;
    const off = parseFloat(store.get(E.calibrationOffset) ? store.get(E.calibrationOffset).value : NaN);
    setText(this.offsetValue, isNaN(off) ? '' : `${off > 0 ? '+' : ''}${off.toFixed(1)}°`);
  },

  async run(task, done) {
    if (this.busy) return;
    this.busy = true;
    try {
      await task();
      if (done) toast(done, 'ok');
    } catch (err) {
      toastError(err);
    } finally {
      this.busy = false;
    }
  },

  async startCalibration() {
    if (!await confirmDialog({ title: 'Start calibration?',
      body: 'The servo moves to the tray’s current slot, and every feed is refused until you finish. Scheduled ' +
            'feeds due meanwhile are skipped.', ok: 'Start' })) return;
    this.run(() => api.turnAndConfirm(E.calibrationMode, true));
  },

  pickSlot(i) {
    if (i < 0 || i >= this.slotCount) return;
    this.run(() => api.setAndConfirm(E.calibrationPosition, String(i)));
  },

  nudge(delta) {
    const e = store.get(E.calibrationOffset);
    if (!e) return;
    const lo = parseFloat(e.min_value), hi = parseFloat(e.max_value);
    const next = Math.round(Math.min(hi, Math.max(lo, parseFloat(e.value) + delta)) * 10) / 10;
    this.run(() => api.setAndConfirm(E.calibrationOffset, next));
  },

  async editCount() {
    const e = store.get(E.portions24h);
    const input = h('input', { type: 'number', class: 'input input-num', min: e.min_value, max: e.max_value, step: 1,
                               value: store.num(E.portions24h), 'aria-label': 'Portions counted' });
    const ok = await formDialog({
      title: 'Change the 24 h count',
      body: [h('p', {}, 'Lower: the oldest counted feeds are forgotten, so more feeds are allowed. Higher: an ' +
                        '"adjustment" is added to the feed history, so fewer are allowed. Nothing is fed.'),
             field('Portions counted', input)],
      ok: 'Change',
      validate: () => {
        const v = Number(input.value);
        return Number.isInteger(v) && v >= +e.min_value && v <= +e.max_value ? '' : `A whole number from ${e.min_value} to ${e.max_value}.`;
      },
    });
    if (!ok) return;
    this.run(() => api.setAndConfirm(E.portions24h, Number(input.value)), 'Count changed');
  },
};
