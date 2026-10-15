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
    for (const r of [setting('Latitude', null, E.latitude), setting('Longitude', null, E.longitude)].filter(Boolean)) {
      this.rows.push(r);
      loc.appendChild(r);
    }
    this.timeRows = [];
    const add = (label, fn) => this.timeRows.push(boundRow(loc, label, fn));
    add('Time zone', () => {
      const t = parseDeviceTime(store.text(E.deviceTime));
      return t ? `UTC${t.offset}, set in the YAML (or by Home Assistant)` : 'Set in the YAML (or by Home Assistant)';
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
    dev.appendChild(h('p', { class: 'hint' }, 'Wi-Fi, the name and the time zone are set in your YAML configuration. ' +
                                              'Diagnostics shows every address and the live log.'));

    append(root, h('div', { class: 'grid' }, safety, tray), h('div', { class: 'grid' }, loc, dev));
    this.update();
  },

  update() {
    for (const r of this.rows) r._update();
    for (const u of this.timeRows) u();
  },
};
