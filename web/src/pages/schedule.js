// Schedule: the three profiles and their events, the scheduler switch, and one-shot feeds. The whole schedule is
// read from Schedule Data, so looking at a profile never moves the editor that Home Assistant shares. Changes go
// through the firmware's editor entities in the order docs/SCHEDULING.md gives, each confirmed by the device's
// echo, and are checked against Schedule Data at the end.

const PROFILE_KEYS = ['A', 'B', 'C'];
const ANCHOR_WORDS = { clock: 'Clock time', sunrise: 'Sunrise', sunset: 'Sunset' };

/** "08:30", "Sunset − 0:30", "Sunrise + 1:15". */
function eventWhen(ev) {
  if (ev.anchor === 'clock') return formatClock(Math.floor(ev.offset / 60), ev.offset % 60);
  const a = Math.abs(ev.offset);
  const base = ANCHOR_WORDS[ev.anchor];
  if (a === 0) return `At ${base.toLowerCase()}`;
  return `${base} ${ev.offset < 0 ? '−' : '+'} ${Math.floor(a / 60)}:${String(a % 60).padStart(2, '0')}`;
}

function sameEvent(a, b) {
  return a && b && a.enabled === b.enabled && a.anchor === b.anchor && a.offset === b.offset &&
         a.portions === b.portions && (a.days & 0x7f) === (b.days & 0x7f);
}

/** Point the firmware's editor at profile p (0..2) and, if given, event e (0..3). Cheap when it already is, so it
 *  is called again before every field write: Home Assistant shares this editor and may have moved it meanwhile. */
async function editorAt(p, e) {
  if (store.text(E.editProfile) !== PROFILE_KEYS[p]) await api.setAndConfirm(E.editProfile, PROFILE_KEYS[p]);
  if (e !== undefined && store.text(E.editEvent) !== String(e + 1)) await api.setAndConfirm(E.editEvent, String(e + 1));
}

/** Everything other than event e of profile p (e = -1: profile p's own settings) that differs from `before`. */
function othersChanged(before, p, e) {
  const after = parseScheduleData(store.text(E.scheduleData));
  const moved = [];
  if (!before || !after) return moved;
  before.forEach((prof, pi) => {
    if (pi !== p && (prof.everyN !== after[pi].everyN || prof.fast !== after[pi].fast)) moved.push(`profile ${PROFILE_KEYS[pi]} settings`);
    prof.events.forEach((ev, ei) => {
      if ((pi !== p || ei !== e) && !sameEvent(ev, after[pi].events[ei])) moved.push(`profile ${PROFILE_KEYS[pi]} event ${ei + 1}`);
    });
  });
  return moved;
}

function collisionMessage(moved) {
  return `Someone else used the schedule editor at the same time, and ${moved.join(', ')} changed too. ` +
         'Check it before relying on the schedule.';
}

/** Run a schedule save, then make sure nothing else changed: Home Assistant shares the editor, and if it moved the
 *  editor in the middle of the save a write may have landed on another event. That is reported by name, also when
 *  the save itself failed (a write that landed elsewhere is not confirmed where it was meant to go). */
async function guardedSave(p, e, task) {
  const before = parseScheduleData(store.text(E.scheduleData));
  try {
    await task();
  } catch (err) {
    // Give a stray write's result time to show in Schedule Data (published within about a second).
    await api.waitForChange([E.scheduleData], () => othersChanged(before, p, e).length > 0, 1500);
    const moved = othersChanged(before, p, e);
    throw moved.length ? new Error(`${err.message} ${collisionMessage(moved)}`) : err;
  }
  const moved = othersChanged(before, p, e);
  if (moved.length) throw new Error(collisionMessage(moved));
}

PAGES.schedule = {
  mount(root, sub) {
    const want = PROFILE_KEYS.indexOf((sub || '').toUpperCase());
    const active = PROFILE_KEYS.indexOf(store.text(E.activeProfile));
    this.view = want >= 0 ? want : Math.max(0, active);
    this.busy = false;
    this.keys = {};

    this.tabs = h('div', { class: 'profile-tabs', role: 'tablist', 'aria-label': 'Profiles' });
    this.head = h('div', { class: 'profile-head' });
    this.events = h('div', { class: 'events' });
    this.warn = h('div', {});
    const profileCard = h('section', { class: 'card' }, this.tabs, this.head, this.warn, this.events);

    this.schedulerRow = h('div', { class: 'row' });
    const general = card('Schedule', this.schedulerRow,
      h('p', { class: 'hint' }, 'Changes reach the feeder straight away. The editor is shared with Home Assistant: its ' +
                                 'Edit Profile and Edit Event follow what you change here.'));

    this.oneshotList = h('div', { class: 'oneshots' });
    const oneshots = card('One-shot feeds',
      h('p', { class: 'hint' }, 'A single feed on a given date. These run even with the schedule off, but not during a hold.'),
      this.oneshotList,
      h('div', { class: 'actions' },
        h('button', { class: 'btn', onclick: () => this.addOneshot() }, 'Add one-shot feed'),
        this.clearBtn = h('button', { class: 'btn', onclick: () => this.clearOneshots() }, 'Clear all')));

    append(root, profileCard, h('div', { class: 'grid' }, general, oneshots));
    this.update(new Set(['*all']));
  },

  changed(name, value) {
    const key = JSON.stringify(value);
    if (this.keys[name] === key) return false;
    this.keys[name] = key;
    return true;
  },

  profiles() {
    return parseScheduleData(store.text(E.scheduleData));
  },

  update() {
    const profiles = this.profiles();
    const active = PROFILE_KEYS.indexOf(store.text(E.activeProfile));
    const scheduler = store.bool(E.scheduler);
    if (this.changed('profile', [store.text(E.scheduleData), active, scheduler, this.view, store.text(E.editedProfile),
                                 store.text(E.faults)])) {
      this.renderProfile(profiles, active, scheduler);
    }
    if (this.changed('scheduler', scheduler)) this.renderScheduler(scheduler);
    if (this.changed('oneshots', store.text(E.oneshots))) this.renderOneshots();
  },

  renderProfile(profiles, active, scheduler) {
    clear(this.tabs);
    PROFILE_KEYS.forEach((k, i) => {
      this.tabs.appendChild(h('button', { class: i === this.view ? 'ptab ptab-on' : 'ptab', role: 'tab',
        'aria-selected': i === this.view ? 'true' : 'false', onclick: () => this.show(i) },
        `Profile ${k}`, i === active ? h('span', { class: 'ptab-active' }, 'Active') : null));
    });
    clear(this.head);
    clear(this.warn);
    clear(this.events);
    if (!profiles) {
      this.events.appendChild(h('p', { class: 'hint' }, store.has(E.scheduleData)
        ? 'This firmware reports the schedule in a newer format than this page understands.'
        : 'Waiting for the feeder…'));
      return;
    }
    const p = profiles[this.view];
    const isActive = this.view === active;
    const cadence = p.everyN === 1 ? 'Every day' : p.everyN === 2 ? 'Every second day' : `Every ${p.everyN} days`;
    append(this.head,
      h('div', { class: 'profile-facts' },
        h('span', {}, cadence),
        h('span', {}, p.fast ? `Fast days: ${daysWords(p.fast)}` : 'No fast days')),
      h('div', { class: 'actions' },
        h('button', { class: 'btn btn-small', onclick: () => this.editProfile() }, 'Cadence and fast days'),
        isActive ? null : h('button', { class: 'btn btn-small btn-primary', onclick: () => this.makeActive() },
                              `Make profile ${PROFILE_KEYS[this.view]} active`)));
    // Warnings: the firmware's own, for this profile (Edited Profile when the editor is on it; Faults for the active one).
    const notes = [];
    const edited = store.text(E.editedProfile);
    if (store.text(E.editProfile) === PROFILE_KEYS[this.view] && edited.includes(' ! ')) notes.push(edited.split(' ! ')[1]);
    if (isActive) {
      for (const f of parseList(store.text(E.faults))) {
        if (f.startsWith('schedule ') || f.startsWith('sunrise') || f.startsWith('no sunrise')) notes.push(f);
      }
      if (scheduler === false) notes.push('the schedule is off, so this profile is not running');
    } else {
      notes.push('not running: only the active profile feeds');
    }
    if (notes.length) {
      this.warn.appendChild(h('ul', { class: isActive ? 'notes notes-warn' : 'notes' },
        notes.map(n => h('li', {}, n[0].toUpperCase() + n.slice(1)))));
    }
    p.events.forEach((ev, i) => {
      this.events.appendChild(h('div', { class: ev.enabled ? 'event' : 'event event-off' },
        h('span', { class: 'event-num' }, String(i + 1)),
        h('div', { class: 'event-main' },
          h('div', { class: 'event-when' }, eventWhen(ev)),
          h('div', { class: 'event-sub' }, `${ev.portions} portion${ev.portions === 1 ? '' : 's'}`,
            h('span', { class: 'sr-only' }, `, ${daysWords(ev.days)}`))),
        dayChips(ev.days, false, `Event ${i + 1} days`),
        h('button', { class: ev.enabled ? 'btn btn-small btn-on' : 'btn btn-small', role: 'switch',
                      'aria-checked': ev.enabled ? 'true' : 'false', 'aria-label': `Event ${i + 1} on`,
                      onclick: () => this.toggleEvent(i, !ev.enabled) }, ev.enabled ? 'On' : 'Off'),
        h('button', { class: 'btn btn-small', onclick: () => this.editEvent(i) }, 'Edit')));
    });
  },

  show(i) {
    this.view = i;
    history.replaceState(null, '', `#schedule/${PROFILE_KEYS[i].toLowerCase()}`);
    this.update();
  },

  renderScheduler(on) {
    clear(this.schedulerRow);
    append(this.schedulerRow,
      h('span', { class: 'row-label' }, 'Scheduled feeding'),
      h('button', { class: on ? 'btn btn-small btn-on' : 'btn btn-small', role: 'switch', 'aria-checked': on ? 'true' : 'false',
                    onclick: () => this.setScheduler(!on) }, on ? 'On' : 'Off'));
  },

  async setScheduler(on) {
    if (!on && !await confirmDialog({ title: 'Turn the schedule off?',
      body: 'No profile feeds until you turn it back on. One-shot feeds and feeding by hand still work.', ok: 'Turn off' })) return;
    this.run(() => api.turnAndConfirm(E.scheduler, on), on ? 'Schedule on' : 'Schedule off');
  },

  async makeActive() {
    const k = PROFILE_KEYS[this.view];
    if (!await confirmDialog({ title: `Make profile ${k} active?`,
      body: `The feeder follows profile ${k} from now on. Feeds already done today still count towards the daily cap.`,
      ok: `Use profile ${k}` })) return;
    this.run(() => api.setAndConfirm(E.activeProfile, k), `Profile ${k} is active`);
  },

  /** Run a change once at a time, reporting the outcome. */
  async run(task, done) {
    if (this.busy) {
      toast('Still saving the last change…');
      return;
    }
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

  async toggleEvent(i, on) {
    const p = this.view;
    this.run(() => guardedSave(p, i, async () => {
      await editorAt(p, i);
      await api.turnAndConfirm(E.eventEnabled, on);
      await api.waitFor(E.scheduleData, x => {
        const s = parseScheduleData(x.value);
        return s && s[p].events[i].enabled === on;
      }, 4000).catch(() => { throw new Error(`Event ${i + 1} did not end up ${on ? 'on' : 'off'}. Check it and try again.`); });
    }), `Event ${i + 1} ${on ? 'on' : 'off'}`);
  },

  async editEvent(i) {
    const profiles = this.profiles();
    if (!profiles) return;
    const p = this.view;
    const cur = profiles[p].events[i];
    const portionsEntity = store.get(E.eventPortions) || {};
    const lo = parseInt(portionsEntity.min_value, 10) || 1, hi = parseInt(portionsEntity.max_value, 10) || 6;

    const enabled = h('input', { type: 'checkbox', checked: cur.enabled });
    const clockTime = h('input', { type: 'time', class: 'input', step: '60', required: true,
      value: cur.anchor === 'clock' ? eventTimeText('clock', cur.offset) : '08:00' });
    const a = cur.anchor === 'clock' ? 0 : Math.abs(cur.offset);
    const dir = segmented([['before', 'Before'], ['after', 'After']], cur.anchor !== 'clock' && cur.offset < 0 ? 'before' : 'after', 'Before or after');
    const hours = h('input', { type: 'number', class: 'input input-num', min: 0, max: 12, step: 1, value: Math.floor(a / 60), 'aria-label': 'Hours' });
    const minutes = h('input', { type: 'number', class: 'input input-num', min: 0, max: 59, step: 1, value: a % 60, 'aria-label': 'Minutes' });
    const sunBox = h('div', { class: 'sun-offset' }, dir, hours, h('span', {}, 'h'), minutes, h('span', {}, 'min'));
    const clockField = field('Time', clockTime, 'The feeder’s local time.');
    const sunField = field('How long', sunBox, 'Up to 12 hours. Set the location under Configuration.');
    const showTime = anchor => {
      clockField.hidden = anchor !== 'clock';
      sunField.hidden = anchor === 'clock';
    };
    const anchor = segmented([['clock', 'Clock time'], ['sunrise', 'Sunrise'], ['sunset', 'Sunset']], cur.anchor, 'When', showTime);
    showTime(cur.anchor);
    const portions = stepper(cur.portions, lo, hi, 'Portions');
    const days = dayChips(cur.days, true, 'Days');

    const wanted = () => {
      let offset;
      if (anchor.value === 'clock') {
        const [hh, mm] = (clockTime.value || '').split(':').map(Number);
        offset = hh * 60 + mm;
      } else {
        offset = (parseInt(hours.value, 10) || 0) * 60 + (parseInt(minutes.value, 10) || 0);
        if (dir.value === 'before') offset = -offset;
      }
      return { enabled: enabled.checked, anchor: anchor.value, offset, portions: portions.value, days: days.mask };
    };
    const ok = await formDialog({
      title: `Profile ${PROFILE_KEYS[p]}, event ${i + 1}`,
      body: [
        h('label', { class: 'check' }, enabled, 'Feed at this time'),
        field('When', anchor), clockField, sunField,
        field('Portions', portions),
        field('Days', days)],
      validate: () => {
        const w = wanted();
        if (w.anchor === 'clock' && !/^\d{2}:\d{2}/.test(clockTime.value)) return 'Enter a time.';
        if (w.anchor !== 'clock') {
          const hh = parseInt(hours.value, 10) || 0, mm = parseInt(minutes.value, 10) || 0;
          if (hh < 0 || mm < 0 || mm > 59 || Math.abs(w.offset) > 720) return 'Up to 12 hours before or after.';
        }
        if (w.enabled && !w.days) return 'Choose at least one day, or turn the event off.';
        return '';
      },
    });
    if (!ok) return;
    const want = wanted();
    if (sameEvent(cur, want)) return;
    this.run(() => guardedSave(p, i, async () => {
      // Each write re-points the shared editor first (see editorAt).
      const write = async step => {
        await editorAt(p, i);
        await step();
      };
      // The editor applies each field at once. Switch a live event off before moving it, so it can never fire
      // half-changed; a failure part-way then leaves it off (the safe side), not wrong.
      const moves = cur.anchor !== want.anchor || cur.offset !== want.offset || cur.days !== want.days;
      if (cur.enabled && moves) await write(() => api.turnAndConfirm(E.eventEnabled, false));
      if (cur.anchor !== want.anchor) await write(() => api.setAndConfirm(E.eventAnchor, want.anchor));
      // A new anchor resets the time on the device, so write the time whenever either changed.
      if (cur.anchor !== want.anchor || cur.offset !== want.offset) {
        await write(() => api.setAndConfirm(E.eventTime, eventTimeText(want.anchor, want.offset)));
      }
      if (cur.days !== want.days) await write(() => api.setAndConfirm(E.eventDays, daysText(want.days)));
      if (cur.portions !== want.portions) await write(() => api.setAndConfirm(E.eventPortions, want.portions));
      await editorAt(p, i);
      if (store.bool(E.eventEnabled) !== want.enabled) await api.turnAndConfirm(E.eventEnabled, want.enabled);
      await api.waitFor(E.scheduleData, x => {
        const s = parseScheduleData(x.value);
        return s && sameEvent(s[p].events[i], want);
      }, 4000).catch(() => { throw new Error(`Event ${i + 1} did not end up as set. Check it and try again.`); });
    }), `Event ${i + 1} saved`);
  },

  async editProfile() {
    const profiles = this.profiles();
    if (!profiles) return;
    const p = this.view;
    const cur = profiles[p];
    const n = store.get(E.everyNDays) || {};
    const lo = parseInt(n.min_value, 10) || 1, hi = parseInt(n.max_value, 10) || 14;
    const every = h('select', { class: 'input', 'aria-label': 'How often' });
    for (let k = lo; k <= hi; k++) {
      every.appendChild(h('option', { value: String(k), selected: k === cur.everyN },
        k === 1 ? 'Every day' : k === 2 ? 'Every second day' : `Every ${k} days`));
    }
    const restart = h('input', { type: 'checkbox' });
    const restartField = h('label', { class: 'check' }, restart, 'Start the cycle today (today becomes a feeding day)');
    const showRestart = () => { restartField.hidden = +every.value === 1; };
    every.addEventListener('change', showRestart);
    showRestart();
    const fast = dayChips(cur.fast, true, 'Fast days');
    const ok = await formDialog({
      title: `Profile ${PROFILE_KEYS[p]}: cadence and fast days`,
      body: [field('How often', every), restartField,
             field('Fast days', fast, 'Weekdays with no feeding at all in this profile.')],
    });
    if (!ok) return;
    const everyN = +every.value;
    if (everyN === cur.everyN && fast.mask === cur.fast && !restart.checked) return;
    this.run(() => guardedSave(p, -1, async () => {
      if (everyN !== cur.everyN) {
        await editorAt(p);
        await api.setAndConfirm(E.everyNDays, everyN);
      }
      if (fast.mask !== cur.fast) {
        await editorAt(p);
        await api.setAndConfirm(E.fastDays, daysText(fast.mask));
      }
      if (restart.checked && everyN > 1) {
        await editorAt(p);
        await api.press(E.restartCycle);
      }
      await api.waitFor(E.scheduleData, x => {
        const s = parseScheduleData(x.value);
        return s && s[p].everyN === everyN && s[p].fast === fast.mask;
      }, 4000).catch(() => { throw new Error('The profile did not end up as set. Check it and try again.'); });
    }), `Profile ${PROFILE_KEYS[p]} saved`);
  },

  renderOneshots() {
    clear(this.oneshotList);
    const list = parseOneshots(store.text(E.oneshots));
    this.clearBtn.hidden = !list.length;
    if (!list.length) {
      this.oneshotList.appendChild(h('p', { class: 'hint' }, 'None waiting.'));
      return;
    }
    const today = parseDeviceTime(store.text(E.deviceTime));
    for (const o of list) {
      const rel = relativeDay(o.when, today);
      this.oneshotList.appendChild(h('div', { class: 'row' },
        h('span', { class: 'row-label' }, `${rel ? rel[0].toUpperCase() + rel.slice(1) : formatDate(o.when)} ${formatClock(o.when.hour, o.when.minute)}`),
        h('span', { class: 'row-value' }, `${o.portions} portion${o.portions === 1 ? '' : 's'}`)));
    }
  },

  async addOneshot() {
    const now = parseDeviceTime(store.text(E.deviceTime));
    const pe = store.get(E.oneshotPortions) || {};
    const date = h('input', { type: 'date', class: 'input', required: true, value: now ? now.date : '' });
    const time = h('input', { type: 'time', class: 'input', step: '60', required: true, value: '18:00' });
    const portions = stepper(1, parseInt(pe.min_value, 10) || 1, parseInt(pe.max_value, 10) || 6, 'Portions');
    const ok = await formDialog({
      title: 'Add a one-shot feed',
      body: [field('Date', date), field('Time', time,
               now ? `In the feeder’s time, now ${formatClock(now.hour, now.minute)} (UTC${now.offset}).` : 'In the feeder’s time.'),
             field('Portions', portions)],
      ok: 'Add',
      validate: () => (/^\d{4}-\d{2}-\d{2}$/.test(date.value) && /^\d{2}:\d{2}/.test(time.value) ? '' : 'Enter a date and a time.'),
    });
    if (!ok) return;
    const text = `${date.value} ${time.value.slice(0, 5)}`;
    const before = store.text(E.oneshots);
    this.run(async () => {
      await api.setAndConfirm(E.oneshotPortions, portions.value);
      await api.setText(E.addOneshot, text);
      const added = await api.waitForChange([E.oneshots], () => store.text(E.oneshots) !== before, 3500);
      if (!added) throw new Error('The feeder did not add it. It must be in the future (in the feeder’s time), and at most 8 can wait.');
    }, 'One-shot feed added');
  },

  async clearOneshots() {
    if (!await confirmDialog({ title: 'Clear all one-shot feeds?', body: 'Every one-shot feed still waiting is removed.',
                               ok: 'Clear all', danger: true })) return;
    this.run(() => api.press(E.clearOneshots).then(() => api.waitFor(E.oneshots, x => x.value === 'none')), 'One-shot feeds cleared');
  },
};
