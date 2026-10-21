// What moves the tray (firmware 2.1.0). The device names its mechanism in the Mechanism entity; each one has a
// description here: its tray drawing, its wording, and its own calibration cards, as data. Layout and wording only:
// limits and behaviour come from the device. Adding a mechanism: AGENTS.md, "Adding a mechanism".

/** Portions per load, as the device reports it (6 from a firmware before 2.1.0, which has no Capacity). */
function capacity() {
  const c = store.num(E.capacity);
  return isNaN(c) || c < 1 ? 6 : c;
}

function svgEl(tag, attrs) {
  const e = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  return e;
}

/** The middle of a dial: portions left, large, and a line under it. */
function dialCentre(svg, unknown, left, cap) {
  const big = svgEl('text', { x: 100, y: 102, class: 'dial-big', 'text-anchor': 'middle' });
  big.textContent = unknown ? '?' : String(left);
  const small = svgEl('text', { x: 100, y: 126, class: 'dial-small', 'text-anchor': 'middle' });
  small.textContent = unknown ? 'position unknown' : `of ${cap} left`;
  svg.appendChild(big);
  svg.appendChild(small);
}

const MECHANISMS = {
  servo: {
    /** The tray as seven slots on an arc: closed slot 0, slots already used, slots with food, and the current one. */
    dial(position, unknown) {
      const cap = capacity();
      const left = unknown ? null : Math.max(0, cap - position);
      const svg = svgEl('svg', { viewBox: '0 0 200 170', class: 'dial', role: 'img' });
      const title = svgEl('title', {});
      title.textContent = unknown ? 'Tray position unknown' : `Tray at slot ${position}: ${left} of ${cap} portions left`;
      svg.appendChild(title);
      svg.appendChild(svgEl('path', { d: 'M 37 136 A 74 74 0 1 1 163 136', class: 'dial-track' }));
      for (let i = 0; i <= cap; i++) {
        const a = (150 + i * 240 / cap) * Math.PI / 180;
        const x = 100 + 74 * Math.cos(a), y = 100 + 74 * Math.sin(a);
        let cls = 'slot-food';
        if (unknown) cls = 'slot-unknown';
        else if (i === 0) cls = 'slot-closed';
        else if (i <= position) cls = 'slot-used';
        if (!unknown && i === position) svg.appendChild(svgEl('circle', { cx: x, cy: y, r: 15, class: 'slot-ring' }));
        svg.appendChild(svgEl('circle', { cx: x, cy: y, r: i === 0 ? 8 : 10, class: `slot ${cls}` }));
      }
      dialCentre(svg, unknown, left, cap);
      return svg;
    },
    position: p => `Slot ${p}`,
    reloadText: cap => 'Press this before you load food. The tray turns back to the closed slot, so nothing can fall ' +
                       `out while you fill it, and all ${cap} portions count as available again.`,
    reloadDone: 'Tray going back to the closed slot',
    recoverText: cap => 'Look at the feeder and choose the slot that is over the opening. Slot 0 is the closed one; ' +
                        `each feed moves one slot further, up to ${cap}.`,
    recoverFallback: 'Not sure? Cancel and use Tray reload instead: it returns to the closed slot, which is always safe.',
    recoveryHint: 'Tell it which slot is over the opening, or reload the tray (it turns back to the closed slot, which ' +
                  'is always safe).',
    // The servo's calibration (slots, offsets, pulse range) is the Calibration card, shown whenever the build has a
    // Calibration Position. Cards a mechanism adds: {title, hint, settings: [[label, hint, E.x]], buttons: [[label,
    // E.x, done]], readings: [[label, E.x]]}.
    cards: [],
  },
};

MECHANISMS['n20-direct'] = {
  /** The disc as a ring: the slot over the chute at the bottom (always empty), slots with food, and slots used. */
  dial(position, unknown) {
    const cap = capacity(), slots = cap + 1;
    const left = unknown ? null : Math.max(0, cap - position);
    const svg = svgEl('svg', { viewBox: '0 0 200 170', class: 'dial', role: 'img' });
    const title = svgEl('title', {});
    title.textContent = unknown ? 'Disc position unknown' : `${left} of ${cap} portions left`;
    svg.appendChild(title);
    svg.appendChild(svgEl('circle', { cx: 100, cy: 85, r: 70, class: 'dial-track', fill: 'none' }));
    for (let i = 0; i < slots; i++) {
      // Slot 0 is over the chute; the next feed brings slot 1 there, and so on round the disc.
      const a = (90 + i * 360 / slots) * Math.PI / 180;
      const x = 100 + 70 * Math.cos(a), y = 85 + 70 * Math.sin(a);
      let cls = 'slot-used';
      if (unknown) cls = 'slot-unknown';
      else if (i === 0) cls = 'slot-closed';
      else if (i <= left) cls = 'slot-food';
      if (i === 0) svg.appendChild(svgEl('circle', { cx: x, cy: y, r: 12, class: 'slot-ring' }));
      svg.appendChild(svgEl('circle', { cx: x, cy: y, r: 7, class: `slot ${cls}` }));
    }
    dialCentre(svg, unknown, left, cap);
    return svg;
  },
  position: p => `${p} fed since reload`,
  reloadText: cap => `Fill every slot except the one over the chute, then press this: all ${cap} portions count again.`,
  reloadDone: 'Disc reloaded',
  recoverText: cap => `Portions fed since the disc was filled (0 to ${cap}). Centre a slot over the hole first.`,
  recoverFallback: 'Not sure? Cancel, refill the disc, then Tray reload.',
  recoveryHint: 'In calibration mode, nudge a slot over the hole, then declare the portions fed, or refill and reload.',
  cards: [
    { title: 'Calibration',
      hint: 'Turn Calibration mode on and follow Next step: it names each button. Empty the disc first.',
      settings: [['Calibration mode', 'Feeding is refused while it is on.', E.calibrationMode],
                 ['Disc slots', 'Slots on your disc, all open (a load holds one fewer). Calibration mode only.',
                  E.discSlots],
                 ['Slot trim', 'Set by Trim here (step 4): + further forward, − further back.',
                  E.slotTrim]],
      readings: [['Next step', E.calStep]],
      buttons: [['Start', E.calStart], ['Yes', E.calYes], ['No', E.calNo], ['Nudge 0.1°', E.calNudgeSmall],
                ['Nudge 1°', E.calNudgeLarge], ['Overshot', E.calOvershot], ['Zero', E.calZero],
                ['Run turns', E.calTurns], ['Measured', E.calMeasured], ['Next slot', E.calNext],
                ['Trim here', E.calTrimHere]] },
    { title: 'Encoder',
      settings: [['Ticks per turn', 'Encoder ticks for one turn of the disc; the calibration measures it.', E.ticksPerTurn],
                 ['Calibration turns', 'Turns the measurement spans: more is more accurate.', E.calibrationTurns],
                 ['Reverse motor', 'Set by the direction test.', E.reverseMotor],
                 ['Reverse encoder', 'Set by the direction test.', E.reverseEncoder]] },
    { title: 'Motion',
      settings: [['Drive power', '', E.drivePower],
                 ['Braking distance', 'Where it starts braking and creeping before each slot.', E.brakingDistance]] },
    { title: 'Wiggle',
      hint: 'Part of every feed: it shakes food loose and centres the slot.',
      settings: [['Amplitude', 'How far each side of the slot.', E.wiggleAmplitude],
                 ['Cycles', '', E.wiggleCycles]] },
    { title: 'Motor status',
      readings: [['Last fault', E.motorFault], ['Encoder ticks', E.encoderTicks], ['Longest control gap', E.motorLoopGap]] },
  ],
};

/** The description of the mechanism the device reports (the servo's for a firmware that reports none). */
function mechanism() {
  return MECHANISMS[store.text(E.mechanism)] || MECHANISMS.servo;
}

/** The mechanism's own calibration cards. Each control is the entity's own (setting()); a card none of whose entities
 *  this build has is left out. Pushes every row that needs updating into `rows` (each has ._update). */
function mechanismCards(rows) {
  const out = [];
  for (const c of mechanism().cards) {
    const items = (c.settings || []).map(([label, hint, id]) => setting(label, hint, id)).filter(Boolean);
    const reads = (c.readings || []).filter(([, id]) => store.has(id)).map(([label, id]) => {
      const r = row(label);
      r.row._update = () => setText(r.value, store.text(id));
      return r.row;
    });
    const buttons = (c.buttons || []).filter(([, id]) => store.has(id)).map(([label, id, done]) =>
      h('button', { class: 'btn', onclick: () => api.press(id).then(() => toast(done || label, 'ok'), toastError) }, label));
    if (!items.length && !reads.length && !buttons.length) continue;
    rows.push(...items, ...reads);
    out.push(card(c.title, c.hint ? h('p', { class: 'hint' }, c.hint) : null, ...items, ...reads,
      buttons.length ? h('div', { class: 'actions' }, buttons) : null));
  }
  return out;
}
