// Configuration: the feeder's settings, each with the device's own limits and unit, saved when you press Set (or
// Enter) and confirmed by the device. The feed guard's limits are the device's: this page only changes them.

PAGES.configuration = {
  mount(root) {
    this.rows = [];
    const group = (title, intro, items) => {
      const rows = items.filter(Boolean);
      if (!rows.length) return null;
      this.rows.push(...rows);
      return card(title, intro ? h('p', { class: 'hint' }, intro) : null, rows);
    };

    const safety = group('Feeding safety',
      'Every feed, from the schedule, this app, Home Assistant, Bluetooth or the button, is checked against these.', [
        setting('Minimum gap between feeds', 'No feed starts sooner than this after the last one.', E.minGap),
        setting('Daily portion cap', 'Most portions in any 24 hours, scheduled ones included.', E.dailyCap),
        setting('Most portions per command', 'For feeds by hand or remote; 1 means one portion per press.', E.maxPerCommand),
        setting('Gap between portions', 'Pause between the portions of one feed.', E.portionGap),
      ]);
    const tray = group('Tray and refills', null, [
      setting('Reload pause', 'After Tray reload, scheduled feeding waits this long so you can load food. Feeds due ' +
              'in that time are skipped.', E.reloadPause),
      setting('Refill interval', 'How often you expect to refill. The schedule is flagged when it needs more than the ' +
              'tray holds before then.', E.refillInterval),
    ]);

    // Location and time
    const loc = card('Location and time',
      h('p', { class: 'hint' }, 'The location is only needed for sunrise and sunset events. Decimal degrees: north and ' +
                                 'east are positive, south and west negative. Both 0 means not set.'));
    const tz = setting('Time zone', 'A POSIX TZ string, for example CET-1CEST,M3.5.0,M10.5.0/3 (not a name like ' +
                       'Europe/Berlin). Empty: the one in your YAML, or Home Assistant\'s.', E.timezone);
    for (const r of [setting('Latitude', null, E.latitude), setting('Longitude', null, E.longitude), tz].filter(Boolean)) {
      this.rows.push(r);
      loc.appendChild(r);
    }
    this.timeRows = [];
    const add = (label, fn) => this.timeRows.push(boundRow(loc, label, fn));
    add('Time zone in use', () => {
      const t = parseDeviceTime(store.text(E.deviceTime));
      const from = store.text(E.timezone) ? 'set here' : 'from the YAML (or Home Assistant)';
      return t ? `UTC${t.offset}, ${from}` : from;
    });
    add('Clock', () => {
      if (store.bool(E.clockValid) === false) return 'Not set';
      const age = store.num(E.clockAge);
      return [store.text(E.timeSource), isNaN(age) ? '' : `synced ${formatDuration(age * 60) || 'just now'} ago`]
        .filter(Boolean).join(', ');
    });

    // Device
    const dev = card('Device');
    const d = (label, fn) => this.timeRows.push(boundRow(dev, label, fn));
    d('Name', () => store.title || location.hostname);
    d('Address', () => location.host);
    if (store.has(E.wifiNetwork)) d('Wi-Fi network', () => store.text(E.wifiNetwork));
    if (store.has(E.wifiSignal)) {
      d('Signal', () => {
        const s = store.num(E.wifiSignal);
        return isNaN(s) ? '' : `${s} dBm (${s >= -60 ? 'good' : s >= -70 ? 'fair' : 'weak'})`;
      });
    }
    d('Firmware', () => store.text(E.firmware));
    dev.appendChild(h('p', { class: 'hint' }, 'Wi-Fi and the name are set in your YAML configuration. ' +
                                              'Diagnostics shows every address and the live log.'));

    this.pw = card('Settings password');
    this.pw.hidden = true;
    auth.onChange(() => this.updatePassword());
    auth.refresh();

    append(root, h('div', { class: 'grid' }, safety, tray), h('div', { class: 'grid' }, loc, dev),
           h('div', { class: 'grid' }, this.pw));
    this.update();
  },

  /** The password card: what is protected, and lock, unlock, set, change or remove. */
  updatePassword() {
    const pw = this.pw;
    if (!pw || !pw.isConnected) return;
    pw.hidden = !auth.available;
    while (pw.children.length > 1) pw.lastChild.remove();
    const what = 'Changing the feeding safety limits or the calibration from this app';
    const btn = (label, onclick, cls = 'btn') => h('button', { class: cls, onclick }, label);
    const run = (p, ok) => p.then(() => ok && toast(ok, 'ok'), toastError);
    const forgot = h('p', { class: 'hint' }, 'Forgotten? Hold the BOOT button on the board for 10 seconds while the ' +
                     'feeder runs: that clears the password and nothing else. Home Assistant and Bluetooth do not ' +
                     'use it; they have their own keys.');
    let text, actions;
    if (!auth.set) {
      text = `No settings password. ${what} needs none: anyone who can open this page can change them.`;
      actions = [btn('Set a password', () => this.passwordDialog(false), 'btn btn-primary')];
    } else if (auth.locked()) {
      text = `${what} needs the settings password.`;
      actions = [btn('Unlock', () => this.unlockDialog(), 'btn btn-primary')];
    } else {
      text = 'Unlocked on this page until you reload it or lock it.';
      actions = [btn('Lock', () => auth.lock()), btn('Change', () => this.passwordDialog(true)),
                 btn('Remove', async () => {
                   if (!await confirmDialog({ title: 'Remove the settings password?', danger: true, ok: 'Remove',
                                              body: `${what} will need no password.` })) return;
                   run(auth.setPassword(''), 'Settings password removed');
                 }, 'btn btn-danger')];
    }
    append(pw, h('p', {}, text), h('div', { class: 'actions' }, actions), forgot);
  },

  async unlockDialog() {
    const input = h('input', { class: 'input', type: 'password', autocomplete: 'current-password' });
    if (!await formDialog({ title: 'Unlock settings', ok: 'Unlock', body: field('Settings password', input),
                            validate: () => (input.value ? '' : 'Enter the password') })) return;
    auth.unlock(input.value).then(() => toast('Unlocked', 'ok'), toastError);
  },

  async passwordDialog(change) {
    const a = h('input', { class: 'input', type: 'password', autocomplete: 'new-password' });
    const b = h('input', { class: 'input', type: 'password', autocomplete: 'new-password' });
    const body = [field('New password', a, 'It is never sent to the feeder; this page proves it knows it. A long ' +
                        'one is best: someone recording your network could try to guess it.'),
                  field('Again', b)];
    const ok = await formDialog({ title: change ? 'Change the settings password' : 'Set a settings password', body,
      validate: () => (a.value.length < 8 ? 'At least 8 characters' : a.value !== b.value ? 'The two do not match' : '') });
    if (!ok) return;
    auth.setPassword(a.value).then(() => toast(change ? 'Password changed' : 'Password set', 'ok'), toastError);
  },

  update() {
    for (const r of this.rows) r._update();
    for (const u of this.timeRows) u();
  },
};
