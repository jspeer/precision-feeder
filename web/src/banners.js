// The banner under the header: the most important thing wrong right now, with what you can do about it. One at a
// time, most severe first. Everything here comes from what the device reports (Tray State, Faults, Clock Valid,
// Hold, ...); the app never works out a fault for itself.

/** Before loading food: explain, then press Tray Reload. Shared with the Dashboard. */
async function trayReloadFlow() {
  const pause = store.num(E.reloadPause);
  const ok = await confirmDialog({
    title: 'Reload the tray?',
    body: h('div', {},
      h('p', {}, 'Press this before you load food. The tray turns back to the closed slot, so nothing can fall out ' +
                 'while you fill it, and all 6 portions count as available again.'),
      h('p', {}, isNaN(pause) || pause === 0
        ? 'Scheduled feeding carries on straight away.'
        : `Scheduled feeding pauses for ${pause} min while you load it. Feeds due in that time are skipped, not made up.`)),
    ok: 'Reload tray',
  });
  if (!ok) return;
  api.press(E.trayReload).then(() => toast('Tray going back to the closed slot', 'ok'), toastError);
}

/** After a power cut mid-feed: say which slot the tray is really on (Declared Tray Position, then Declare). */
async function recoverPositionFlow() {
  let chosen = null;
  const buttons = [0, 1, 2, 3, 4, 5, 6].map(i => h('button', { type: 'button', class: 'btn slot-pick', 'aria-pressed': 'false',
    onclick: () => {
      chosen = i;
      for (const b of buttons) b.setAttribute('aria-pressed', b === buttons[i] ? 'true' : 'false');
    } }, String(i)));
  const ok = await confirmDialog({
    title: 'Where is the tray?',
    body: h('div', {},
      h('p', {}, 'Look at the feeder and choose the slot that is over the opening. Slot 0 is the closed one; ' +
                 'each feed moves one slot further, up to 6.'),
      h('div', { class: 'slot-picker' }, buttons),
      h('p', {}, 'Not sure? Cancel and use Tray reload instead: it returns to the closed slot, which is always safe.')),
    ok: 'Set position',
  });
  if (!ok) return;
  if (chosen === null) {
    toast('Choose a slot first', 'error');
    return;
  }
  try {
    await api.setAndConfirm(E.declaredPosition, chosen);
    await api.press(E.declarePosition);
    await api.waitFor(E.trayState, e => e.value !== 'unknown', 3500);
    toast(`Tray set to slot ${chosen}`, 'ok');
  } catch (err) {
    toastError(err);
  }
}

/** Turn calibration mode off, which saves the offsets. */
function finishCalibration() {
  api.turn(E.calibrationMode, false)
    .then(() => api.waitFor(E.calibrationMode, x => x.value === false))
    .then(() => toast('Calibration saved', 'ok'), toastError);
}

/** A hold in words: "Until you resume it", "Until Sat, Oct 17 2:28 PM". */
function holdWords(hold) {
  if (hold.until) return `Until ${formatDate(hold.until)} ${formatClock(hold.until.hour, hold.until.minute)}`;
  if (hold.note) return hold.note[0].toUpperCase() + hold.note.slice(1);
  return 'Until you resume it';
}

/** The faults the banners below already explain, so they are not listed twice. */
const COVERED_FAULTS = ['clock not set', 'tray position unknown', 'tray empty', 'next feed needs more portions'];

function currentBanner() {
  if (store.everOnline && store.status !== 'online') {
    return { kind: 'neutral', icon: 'wifiOff', title: 'Connection lost',
             text: 'Reconnecting… The feeder keeps running on its own schedule.' };
  }
  if (!store.has(E.trayState)) return null;
  const tray = store.text(E.trayState);
  if (tray === 'unknown') {
    return { kind: 'danger', icon: 'alert', title: 'Tray position unknown',
             text: 'Feeding is off until you tell the feeder where the tray is, or reload it.',
             actions: [
               store.has(E.declarePosition) && store.has(E.declaredPosition)
                 ? { label: 'Recover position', run: recoverPositionFlow } : null,
               { label: 'Tray reload', run: trayReloadFlow },
             ] };
  }
  if (tray === 'calibrating') {
    return { kind: 'warning', icon: 'tune', title: 'Calibration mode is on', text: 'All feeds are refused until you finish.',
             actions: store.has(E.calibrationMode) ? [{ label: 'Finish and save', run: finishCalibration }] : [] };
  }
  if (tray === 'empty') {
    return { kind: 'danger', icon: 'bowl', title: 'Tray empty',
             text: 'Reload the tray, then load food.', actions: [{ label: 'Tray reload', run: trayReloadFlow }] };
  }
  if (store.bool(E.clockValid) === false) {
    return { kind: 'warning', icon: 'clock', title: 'Clock not set',
             text: 'Scheduled feeds wait until the feeder knows the time. Feeding by hand still works.' };
  }
  if (store.bool(E.needsFood)) {
    return { kind: 'warning', icon: 'fish', title: 'The next feed needs more than remains',
             text: `${store.num(E.portionsRemaining)} left. Refill before ${store.text(E.nextFeed)}.`,
             actions: [{ label: 'Tray reload', run: trayReloadFlow }] };
  }
  const hold = parseHold(store.text(E.hold));
  if (hold) {
    return { kind: 'info', icon: 'pause', title: 'Scheduled feeding on hold',
             text: `${holdWords(hold)}. Missed feeds are skipped, not made up.`,
             actions: store.has(E.cancelHold) ? [{ label: 'Resume now', run: () => api.press(E.cancelHold)
               .then(() => toast('Scheduled feeding resumed', 'ok'), toastError) }] : [] };
  }
  const other = parseList(store.text(E.faults)).filter(f => !COVERED_FAULTS.some(c => f.startsWith(c)));
  if (other.length) {
    return { kind: 'warning', icon: 'alert', title: other.length === 1 ? 'One thing to check' : `${other.length} things to check`,
             list: other.map(f => f[0].toUpperCase() + f.slice(1)) };
  }
  return null;
}

const banners = {
  el: null,
  key: '',
  mount(el) { this.el = el; },
  update() {
    const b = currentBanner();
    const key = b ? JSON.stringify([b.kind, b.title, b.text, b.list, (b.actions || []).filter(Boolean).map(a => a.label)]) : '';
    if (key === this.key) return;
    this.key = key;
    clear(this.el);
    if (!b) return;
    this.el.appendChild(h('div', { class: `banner banner-${b.kind}`, role: b.kind === 'danger' ? 'alert' : 'status' },
      icon(b.icon, 22),
      h('div', { class: 'banner-body' },
        h('strong', {}, b.title),
        b.text ? h('p', {}, b.text) : null,
        b.list ? h('ul', {}, b.list.map(f => h('li', {}, f))) : null,
        (b.actions || []).filter(Boolean).length
          ? h('div', { class: 'banner-actions' }, b.actions.filter(Boolean).map(a => h('button', { class: 'btn btn-small', onclick: a.run }, a.label)))
          : null)));
  },
};
