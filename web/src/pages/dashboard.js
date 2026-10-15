// Dashboard: the tray, the next feed, Feed now, today's feeds, status and the everyday actions. Everything shown
// is what the device reports; Feed now asks the device, whose guard decides, and reports the device's answer.

/** The tray as seven slots on an arc: closed slot 0, slots already used, slots with food, and the current one. */
function trayDial(position, unknown) {
  const ns = 'http://www.w3.org/2000/svg';
  const el = (tag, attrs) => {
    const e = document.createElementNS(ns, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    return e;
  };
  const left = unknown ? null : Math.max(0, 6 - position);
  const svg = el('svg', { viewBox: '0 0 200 170', class: 'dial', role: 'img' });
  const title = el('title', {});
  title.textContent = unknown ? 'Tray position unknown' : `Tray at slot ${position}: ${left} of 6 portions left`;
  svg.appendChild(title);
  svg.appendChild(el('path', { d: 'M 37 136 A 74 74 0 1 1 163 136', class: 'dial-track' }));
  for (let i = 0; i <= 6; i++) {
    const a = (150 + i * 40) * Math.PI / 180;
    const x = 100 + 74 * Math.cos(a), y = 100 + 74 * Math.sin(a);
    let cls = 'slot-food';
    if (unknown) cls = 'slot-unknown';
    else if (i === 0) cls = 'slot-closed';
    else if (i <= position) cls = 'slot-used';
    if (!unknown && i === position) svg.appendChild(el('circle', { cx: x, cy: y, r: 15, class: 'slot-ring' }));
    svg.appendChild(el('circle', { cx: x, cy: y, r: i === 0 ? 8 : 10, class: `slot ${cls}` }));
  }
  const big = el('text', { x: 100, y: 102, class: 'dial-big', 'text-anchor': 'middle' });
  big.textContent = unknown ? '?' : String(left);
  const small = el('text', { x: 100, y: 126, class: 'dial-small', 'text-anchor': 'middle' });
  small.textContent = unknown ? 'position unknown' : 'of 6 left';
  svg.appendChild(big);
  svg.appendChild(small);
  return svg;
}

const STATUS_WORDS = {
  fed: ['check', 'Fed'], skipped: ['x', 'Skipped'], due: ['clock', 'Due now'], next: ['clock', 'Next'],
  later: ['clock', 'Later'], held: ['pause', 'On hold'],
};
const HOLD_CHOICES = [[2, '2 hours'], [12, '12 hours'], [24, '1 day'], [72, '3 days'], [168, '1 week'], [0, 'Until I resume it']];

PAGES.dashboard = {
  mount(root) {
    this.portions = 1;
    this.updaters = [];
    this.keys = {};

    // ---- hero: tray, next feed, runway, Feed now ----
    this.dialBox = h('div', { class: 'dial-box' });
    this.nextTime = h('div', { class: 'stat-big' });
    this.nextSub = h('div', { class: 'stat-sub' });
    this.runwayBig = h('div', { class: 'stat-big' });
    this.runwaySub = h('div', { class: 'stat-sub' });
    this.guardPill = h('span', { class: 'pill' });
    this.stepper = h('div', { class: 'stepper', hidden: true },
      h('button', { class: 'btn btn-small', 'aria-label': 'One portion less', onclick: () => this.step(-1) }, '−'),
      this.stepValue = h('span', { class: 'stepper-value', 'aria-live': 'polite' }, '1'),
      h('button', { class: 'btn btn-small', 'aria-label': 'One portion more', onclick: () => this.step(1) }, '+'));
    this.feedBtn = h('button', { class: 'btn btn-primary btn-feed', onclick: () => this.feedNow() }, icon('fish', 20), 'Feed now');
    const hero = h('section', { class: 'card hero' },
      this.dialBox,
      h('div', { class: 'hero-side' },
        h('div', { class: 'stats' },
          h('div', {}, h('div', { class: 'stat-label' }, 'Next feed'), this.nextTime, this.nextSub),
          h('div', {}, h('div', { class: 'stat-label' }, 'Runway'), this.runwayBig, this.runwaySub)),
        h('div', { class: 'feed-row' }, this.stepper, this.feedBtn),
        h('div', { class: 'guard' }, this.guardPill)));

    // ---- today ----
    this.todayTitle = h('h2', { class: 'card-title' }, 'Today');
    this.todayList = h('div', { class: 'today' });
    this.scheduleLink = h('a', { class: 'card-link', href: '#schedule', hidden: true }, 'Schedule');
    const today = h('section', { class: 'card' }, h('div', { class: 'card-head' }, this.todayTitle, this.scheduleLink),
                    this.todayList);

    // ---- status ----
    const status = card('Status');
    const add = (label, fn) => this.updaters.push(boundRow(status, label, fn));
    add('Schedule', () => {
      if (store.has(E.scheduler) && store.bool(E.scheduler) === false) return 'Off';
      if (parseHold(store.text(E.hold))) return 'On hold';
      return 'Running';
    });
    add('Tray', () => {
      const state = store.text(E.trayState);
      return state === 'unknown' ? 'Position unknown' : `Slot ${store.num(E.trayPosition)}, ${state}`;
    });
    add('Fed in the last 24 h', () => {
      const n = store.num(E.portions24h), cap = store.num(E.dailyCap);
      if (isNaN(n)) return `${store.num(E.fedToday)} today`;
      return isNaN(cap) ? `${n}` : `${n} of ${cap} allowed`;
    });
    add('Last feed', () => {
      const l = parseLastFeed(store.text(E.lastFeed));
      if (!l.when) return l.note || '';
      return `${this.when(l.when)}${l.source ? `, ${sourceLabel(l.source)}` : ''}`;
    });
    add('Clock', () => {
      if (store.bool(E.clockValid) === false) return 'Not set';
      const age = store.num(E.clockAge);
      return [store.text(E.timeSource), isNaN(age) ? '' : `synced ${formatDuration(age * 60) || 'just now'} ago`]
        .filter(Boolean).join(', ');
    });

    // ---- recent feeds ----
    this.recentList = h('div', { class: 'recent' });
    const recent = card('Recent feeds', this.recentList);

    // ---- actions ----
    this.holdText = h('p', { class: 'hint' });
    this.holdBtn = h('button', { class: 'btn', onclick: () => this.holdOrResume() });
    this.stopBtn = h('button', { class: 'btn btn-danger', hidden: true, onclick: () => api.press(E.stop)
      .then(() => toast('Stopped', 'ok'), toastError) }, 'Stop');
    const actions = card('Actions',
      h('div', { class: 'actions' },
        h('button', { class: 'btn', onclick: trayReloadFlow }, 'Tray reload'),
        store.has(E.holdFeeding) ? this.holdBtn : null,
        this.stopBtn),
      this.holdText);

    append(root, hero, h('div', { class: 'grid' }, today, status, recent, actions));
    this.update(new Set(['*all']));
  },

  /** The device's "today" (from Device Time), so "today" and "tomorrow" follow the feeder's clock, not the browser's. */
  today() {
    return parseDeviceTime(store.text(E.deviceTime));
  },
  /** A device date and time as "17:30", "tomorrow 08:00" or "Thu 15 Oct 08:00". */
  when(parts) {
    const rel = relativeDay(parts, this.today());
    const clock = formatClock(parts.hour, parts.minute);
    if (rel === 'today') return clock;
    return `${rel || formatDate(parts)} ${clock}`;
  },

  update() {
    for (const u of this.updaters) u();
    this.updateHero();
    this.updateToday();
    this.updateRecent();
    this.updateActions();
  },

  changed(name, value) {
    const key = JSON.stringify(value);
    if (this.keys[name] === key) return false;
    this.keys[name] = key;
    return true;
  },

  updateHero() {
    const state = store.text(E.trayState);
    const position = store.num(E.trayPosition);
    if (this.changed('dial', [state, position])) {
      clear(this.dialBox);
      this.dialBox.appendChild(trayDial(position, state === 'unknown' || isNaN(position)));
    }
    // Next feed
    const next = parseLocalDateTime(store.text(E.nextFeed));
    if (next) {
      setText(this.nextTime, formatClock(next.hour, next.minute));
      const rel = relativeDay(next, this.today());
      const today = parseTodaysFeeds(store.text(E.todaysFeeds));
      const item = rel === 'today' && today ? today.items.find(i => i.time === next.time && i.status !== 'fed') : null;
      const portions = item ? `${item.portions} portion${item.portions === 1 ? '' : 's'}` : '';
      setText(this.nextSub, [rel ? rel[0].toUpperCase() + rel.slice(1) : formatDate(next), portions].filter(Boolean).join(', '));
    } else {
      const raw = store.text(E.nextFeed);
      setText(this.nextTime, '—');
      setText(this.nextSub, raw === 'none' ? 'Nothing scheduled' : raw === 'clock not set' ? 'Clock not set' : raw);
    }
    // Runway
    const runway = store.num(E.runway);
    const emptyOn = store.text(E.emptyOn);
    const empty = parseLocalDateTime(emptyOn);
    if (store.num(E.portionsRemaining) === 0) {
      setText(this.runwayBig, 'Empty');
      setText(this.runwaySub, 'Reload the tray');
    } else if (!isNaN(runway)) {
      setText(this.runwayBig, runway < 1 ? 'Under a day' : `${runway} day${runway === 1 ? '' : 's'}`);
      setText(this.runwaySub, empty ? `Empty ${this.when(empty)}` : '');
    } else {
      setText(this.runwayBig, emptyOn === 'not within 30 days' ? '30+ days' : '—');
      setText(this.runwaySub, emptyOn === 'clock not set' ? 'Clock not set' : '');
    }
    // Portions stepper, only when one command may feed more than one portion
    const max = store.num(E.maxPerCommand);
    const multi = max > 1 && store.has(E.feedSelected) && store.has(E.manualPortions);
    this.stepper.hidden = !multi;
    if (!multi) this.portions = 1;
    else this.portions = Math.min(this.portions, max);
    setText(this.stepValue, String(this.portions));
    setText(this.feedBtn.lastChild, this.portions > 1 ? `Feed ${this.portions} portions` : 'Feed now');
    // What the guard would say right now
    const guard = store.text(E.feedGuard);
    if (!store.has(E.feedGuard)) {
      this.guardPill.hidden = true;
    } else {
      this.guardPill.hidden = false;
      const ready = guard === 'ready';
      setText(this.guardPill, ready ? 'Ready to feed' : guard ? guard[0].toUpperCase() + guard.slice(1) : '');
      this.guardPill.className = `pill ${ready ? 'pill-ok' : 'pill-warn'}`;
    }
  },

  step(delta) {
    const max = store.num(E.maxPerCommand) || 1;
    this.portions = Math.min(max, Math.max(1, this.portions + delta));
    this.updateHero();
  },

  async feedNow() {
    const portions = this.portions;
    const guard = store.text(E.feedGuard);
    const ok = await confirmDialog({
      title: portions > 1 ? `Feed ${portions} portions now?` : 'Feed one portion now?',
      body: guard && guard !== 'ready'
        ? `Right now the feeder would refuse it: ${guard}. You can still ask; it checks again.`
        : 'The feeder checks it against its safety limits first.',
      ok: 'Feed',
    });
    if (!ok) return;
    const before = [store.text(E.lastResult), store.text(E.recentFeeds)];
    try {
      if (portions > 1) {
        await api.setAndConfirm(E.manualPortions, portions);
        await api.press(E.feedSelected);
      } else {
        await api.press(E.feedOne);
      }
    } catch (err) {
      toastError(err);
      return;
    }
    // The device reports its answer as Last Result (a refusal) or a new Recent Feeds entry (a feed). A refusal
    // repeated unchanged produces no update, so after a while its current Last Result is the answer.
    await api.waitForChange([E.lastResult, E.recentFeeds],
      () => store.text(E.lastResult) !== before[0] || store.text(E.recentFeeds) !== before[1], 3500);
    const result = store.text(E.lastResult);
    if (store.text(E.recentFeeds) !== before[1] || result === 'ok') toast(portions > 1 ? `Feeding ${portions} portions` : 'Feeding one portion', 'ok');
    else toast(`The feeder refused: ${result}`, 'error', 6000);
  },

  updateToday() {
    const profile = store.text(E.activeProfile);
    setText(this.todayTitle, profile ? `Today · Profile ${profile}` : 'Today');
    this.scheduleLink.hidden = !(PAGES.schedule && store.has(E.editProfile));
    const raw = store.text(E.todaysFeeds);
    if (!this.changed('today', raw)) return;
    clear(this.todayList);
    if (!store.has(E.todaysFeeds)) return;
    const t = parseTodaysFeeds(raw);
    if (!t) {
      this.todayList.appendChild(h('p', { class: 'hint' }, raw));
      return;
    }
    if (!t.items.length) {
      const notes = { 'fast day': 'No feeds today: it is a fast day.', 'not a feeding day': 'No feeds today: not a feeding day in this cycle.',
                      'scheduler off': 'The schedule is off.', 'clock not set': 'Waiting for the clock.' };
      this.todayList.appendChild(h('p', { class: 'hint' }, notes[t.note] || 'Nothing scheduled today.'));
      return;
    }
    for (const it of t.items) {
      const [ic, word] = STATUS_WORDS[it.status] || ['clock', it.status];
      this.todayList.appendChild(h('div', { class: `today-item today-${it.status}` },
        h('span', { class: 'today-icon' }, icon(ic, 18)),
        h('span', { class: 'today-time' }, formatClock(it.hour, it.minute)),
        h('span', { class: 'today-what' }, `${it.portions} portion${it.portions === 1 ? '' : 's'}`,
          it.oneshot ? h('span', { class: 'tag' }, 'one-shot') : null),
        h('span', { class: 'today-status' }, word)));
    }
  },

  updateRecent() {
    const raw = store.text(E.recentFeeds);
    if (!this.changed('recent', raw)) return;
    clear(this.recentList);
    const feeds = parseRecentFeeds(raw);
    if (!feeds.length) {
      this.recentList.appendChild(h('p', { class: 'hint' }, raw && raw !== 'none' ? raw : 'No feeds yet.'));
      return;
    }
    const year = (this.today() || { year: new Date().getFullYear() }).year;
    for (const f of feeds) {
      const when = f.month === null ? 'Time unknown'
        : this.when({ year, month: f.month, day: f.day, hour: f.hour, minute: f.minute });
      this.recentList.appendChild(h('div', { class: 'row' },
        h('span', { class: 'row-label' }, when),
        h('span', { class: 'row-value' }, `${f.portions} × ${sourceLabel(f.source)}`)));
    }
  },

  updateActions() {
    const hold = parseHold(store.text(E.hold));
    setText(this.holdBtn, hold ? 'Resume schedule' : 'Hold feeding');
    setText(this.holdText, hold ? `On hold: ${holdWords(hold).toLowerCase()}. Feeding by hand still works.` : '');
    this.stopBtn.hidden = store.text(E.trayState) !== 'moving';
  },

  async holdOrResume() {
    if (parseHold(store.text(E.hold))) {
      api.press(E.cancelHold).then(() => toast('Scheduled feeding resumed', 'ok'), toastError);
      return;
    }
    const choices = HOLD_CHOICES.map(([hours, label], i) => h('label', { class: 'choice' },
      h('input', { type: 'radio', name: 'hold', value: String(hours), checked: i === 2 }), label));
    const ok = await confirmDialog({
      title: 'Hold scheduled feeding',
      body: h('div', {}, h('p', {}, 'Scheduled and one-shot feeds are skipped while on hold, and not made up afterwards. ' +
                                   'Feeding by hand still works.'), h('div', { class: 'choices' }, choices)),
      ok: 'Hold',
    });
    if (!ok) return;
    const hours = +choices.map(c => c.firstChild).find(r => r.checked).value;
    try {
      await api.setAndConfirm(E.holdDuration, hours);
      await api.press(E.holdFeeding);
      await api.waitFor(E.hold, e => e.value && e.value !== 'none', 3500);
      toast('Scheduled feeding on hold', 'ok');
    } catch (err) {
      toast(store.bool(E.clockValid) === false && hours
        ? 'A timed hold needs the clock. Choose "Until I resume it" instead.' : err.message, 'error', 6000);
    }
  },
};
