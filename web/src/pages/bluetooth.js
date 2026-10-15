// Bluetooth: the BLE control plane as the device sees it. BLE Info (static facts) and BLE Session (the connection
// and counters since boot) are entities; the activity log comes from GET /feeder/ble, the last 32 connections and
// writes kept on the device. Read-only: nothing here acts on BLE, and nothing secret (key, nonce, tag) is shown,
// because the device never reports it.

const ACTIVITY_WORDS = {
  connect: ['SYS', 'Client connected'], disconnect: ['SYS', 'Client disconnected'],
  auth: ['RX', 'AUTH'], command: ['RX', null], record: ['RX', null], replay: ['RX', null],
};

PAGES.bluetooth = {
  mount(root) {
    this.updaters = [];
    this.entries = [];
    this.fetchKey = '';
    this.clearedId = 0;
    try { this.clearedId = +localStorage.getItem('pf-ble-cleared') || 0; } catch (err) { /* private mode: not kept */ }

    const status = card('Status');
    const s = (label, fn) => this.updaters.push(boundRow(status, label, fn));
    s('Bluetooth', () => 'On');
    s('Connection', () => (this.kv().connected === '1' ? 'Client connected' : 'Waiting for a client (advertising)'));
    s('Authentication', () => {
      const kv = this.kv();
      if (kv.connected !== '1') return '—';
      return kv.auth === '1' ? 'Verified' : 'Not yet';
    });
    s('Connections since start', () => this.kv().conns || '0');

    const client = card('Client');
    const c = (label, fn) => this.updaters.push(boundRow(client, label, fn));
    c('Address', () => (this.kv().peer ? `··:··:··:··:${this.kv().peer}` : '—'));
    c('Connected for', () => {
      const kv = this.kv();
      return kv.connected === '1' ? this.ago(kv.since, '') : '—';
    });
    c('Last activity', () => this.ago(this.kv().last, ' ago'));
    c('MTU', () => this.kv().mtu || '—');
    client.appendChild(h('p', { class: 'hint' }, 'Only the last two bytes of the address are kept. Phones change ' +
                                                 'their address from time to time.'));

    const proto = card('Protocol');
    this.facts = h('div', { class: 'metrics' });
    proto.appendChild(this.facts);
    const p = (label, fn) => this.updaters.push(boundRow(proto, label, fn));
    p('Service', () => this.info().service || '');
    p('Time from Bluetooth', () => ({ fallback_only: 'Only when network time is stale', always: 'Always accepted',
                                      off: 'Refused' }[this.info().time] || this.info().time || ''));

    this.activityBox = h('div', { class: 'activity', role: 'log' });
    const activity = card('Activity',
      h('div', { class: 'toolbar' },
        h('button', { class: 'btn btn-small', onclick: () => this.clearView() }, 'Clear view'),
        h('button', { class: 'btn btn-small', onclick: () => this.loadActivity(true) }, 'Refresh')),
      this.activityBox,
      h('p', { class: 'hint' }, 'The last 32 connections and writes, kept by the feeder until it restarts. Reads ' +
        'and status notifications are only counted. Clear view only hides lines in this browser.'));

    append(root, h('div', { class: 'grid' }, status, client), proto, activity);
    this.timer = setInterval(() => this.update(), 1000);
    this.update();
  },

  unmount() {
    clearInterval(this.timer);
  },

  kv() {
    return parseKV(store.text(E.bleSession));
  },
  info() {
    return parseKV(store.text(E.bleInfo));
  },

  /** A device uptime (seconds) as time before now: "3 min ago". */
  ago(uptimeText, suffix) {
    const t = parseInt(uptimeText, 10), now = store.uptime();
    if (isNaN(t) || t < 0 || now === null) return '—';
    const d = Math.max(0, now - t);
    return d < 2 && suffix ? 'just now' : `${formatDuration(d)}${suffix}`;
  },

  update() {
    for (const u of this.updaters) u();
    const kv = this.kv(), info = this.info();
    const metrics = [
      ['Protocol', info.protocol ? `version ${info.protocol}` : '—'],
      ['Authentication', (info.auth || '').toUpperCase() || '—'],
      ['Writes in', kv.wr || '0', kv.wrb ? `${kv.wrb} bytes` : ''],
      ['Reads', kv.rd || '0', kv.rdb ? `${kv.rdb} bytes` : ''],
      ['Notifications out', kv.nt || '0', kv.ntb ? `${kv.ntb} bytes` : ''],
      ['AUTH accepted', kv.authok || '0'],
      ['AUTH refused', kv.authfail || '0', '', +kv.authfail > 0],
      ['Refused writes', kv.refused || '0', 'not authorized', +kv.refused > 0],
      ['Invalid writes', kv.invalid || '0', '', +kv.invalid > 0],
    ];
    if (this.changed !== JSON.stringify(metrics)) {
      this.changed = JSON.stringify(metrics);
      clear(this.facts);
      for (const [label, value, sub, warn] of metrics) {
        this.facts.appendChild(h('div', { class: warn ? 'metric metric-warn' : 'metric' },
          h('div', { class: 'metric-label' }, label), h('div', { class: 'metric-value' }, value),
          sub ? h('div', { class: 'metric-sub' }, sub) : null));
      }
    }
    // The log only changes with a connection or a write, so fetch it again only then.
    const key = `${kv.conns}|${kv.wr}|${kv.connected}`;
    if (key !== this.fetchKey) {
      this.fetchKey = key;
      this.loadActivity(false);
    }
  },

  async loadActivity(byHand) {
    try {
      const r = await fetch(`${location.origin}/feeder/ble`);
      if (!r.ok) throw new Error(`the feeder answered ${r.status}`);
      this.entries = (await r.json()).entries || [];
      this.renderActivity();
      if (byHand) toast('Activity refreshed', 'ok');
    } catch (err) {
      clear(this.activityBox);
      this.activityBox.appendChild(h('p', { class: 'hint' }, `The activity log could not be read (${err.message}).`));
    }
  },

  renderActivity() {
    clear(this.activityBox);
    // Ids start again at 1 when the feeder restarts: a remembered "cleared up to" beyond the newest is stale.
    const newest = this.entries.length ? this.entries[this.entries.length - 1].id : 0;
    if (newest < this.clearedId) {
      this.clearedId = 0;
      try { localStorage.removeItem('pf-ble-cleared'); } catch (err) { /* not kept */ }
    }
    const shown = this.entries.filter(e => e.id > this.clearedId);
    if (!shown.length) {
      this.activityBox.appendChild(h('p', { class: 'hint' }, 'Nothing yet.'));
      return;
    }
    const now = store.uptime();
    for (const e of shown.slice().reverse()) {  // newest first
      const [dir, fixed] = ACTIVITY_WORDS[e.kind] || ['?', e.kind];
      const at = now === null ? null : new Date(Date.now() - Math.max(0, now - e.t) * 1000);
      const what = fixed || e.name || '';
      const ok = e.result === 'ok';
      this.activityBox.appendChild(h('div', { class: 'act' },
        h('span', { class: 'act-time' }, at ? at.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : ''),
        h('span', { class: `act-dir act-${dir.toLowerCase()}` }, dir),
        h('span', { class: 'act-what' }, what, e.kind === 'replay' ? h('span', { class: 'tag' }, 'retry') : null,
          e.seq !== undefined ? h('span', { class: 'act-seq' }, `seq ${e.seq}`) : null),
        e.result !== undefined
          ? h('span', { class: ok ? 'pill pill-ok' : 'pill pill-warn' }, ok ? 'ok' : e.result + (e.detail ? ` (${e.detail})` : ''))
          : h('span', {})));
    }
  },

  clearView() {
    this.clearedId = this.entries.length ? this.entries[this.entries.length - 1].id : this.clearedId;
    try { localStorage.setItem('pf-ble-cleared', String(this.clearedId)); } catch (err) { /* not kept */ }
    this.renderActivity();
  },
};
