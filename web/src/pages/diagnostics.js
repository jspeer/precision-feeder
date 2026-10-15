// Diagnostics: always present. System and clock, network and memory (when those packages are built in), the live
// log, and every entity the device reports with a control for each: the debug view, and the fallback for anything
// no other page shows.

const DOMAIN_TITLES = {
  sensor: 'Sensors', binary_sensor: 'Binary sensors', text_sensor: 'Text sensors', button: 'Buttons',
  switch: 'Switches', select: 'Selects', number: 'Numbers', text: 'Text inputs',
};
const LEVELS = { E: 0, W: 1, I: 2, C: 2, D: 3, V: 4 };

PAGES.diagnostics = {
  mount(root) {
    this.updaters = [];
    this.timer = setInterval(() => this.updateSystem(), 1000);

    // System
    const sys = card('System');
    const add = (label, fn) => this.updaters.push(boundRow(sys, label, fn));
    add('Firmware', () => store.text(E.firmware) || 'unknown');
    add('Device name', () => store.title || location.hostname);
    add('Running for', () => formatDuration(store.uptime()));
    if (store.has(E.deviceTime)) {
      add('Device time', () => {
        const t = parseDeviceTime(store.text(E.deviceTime));
        return t ? `${formatDate(t)} ${formatClock(t.hour, t.minute)} (UTC${t.offset})` : store.text(E.deviceTime);
      });
    }
    add('Clock', () => {
      if (store.bool(E.clockValid) === false) return 'not set';
      const age = store.num(E.clockAge);
      return [store.text(E.timeSource), isNaN(age) ? '' : `synced ${formatDuration(age * 60) || 'just now'} ago`]
        .filter(Boolean).join(', ');
    });
    this.sysUpdaters = this.updaters.slice();

    // Network (network_info package)
    const net = store.has(E.wifiNetwork) || store.has(E.ipAddress1) ? card('Network') : null;
    if (net) {
      const n = (label, fn) => this.updaters.push(boundRow(net, label, fn));
      n('Wi-Fi network', () => store.text(E.wifiNetwork));
      n('Addresses', () => [E.ipAddress1, E.ipAddress2, E.ipAddress3, E.ipAddress4]
        .map(id => store.text(id)).filter(a => a && a !== '0.0.0.0').join(', '));
      if (store.has(E.wifiSignal)) {
        n('Signal', () => {
          const s = store.num(E.wifiSignal);
          if (isNaN(s)) return '';
          return `${s} dBm (${s >= -60 ? 'good' : s >= -70 ? 'fair' : 'weak'})`;
        });
      }
    }

    // Memory (diagnostics package)
    const mem = store.has(E.freeHeap) ? card('Memory') : null;
    if (mem) {
      const kb = id => () => isNaN(store.num(id)) ? '' : `${Math.round(store.num(id) / 1024)} KB`;
      const m = (label, fn) => this.updaters.push(boundRow(mem, label, fn));
      m('Free heap', kb(E.freeHeap));
      m('Largest free block', kb(E.largestBlock));
      m('Fragmentation', () => isNaN(store.num(E.fragmentation)) ? '' : `${Math.round(store.num(E.fragmentation))} %`);
      m('Longest loop', () => isNaN(store.num(E.loopTime)) ? '' : `${Math.round(store.num(E.loopTime))} ms`);
    }

    // Log
    this.paused = false;
    this.minLevel = 2;
    this.shownCount = Math.max(0, store.logCount - store.logLines.length);
    this.logBox = h('div', { class: 'log', role: 'log', 'aria-live': 'off', tabindex: '0' });
    const level = h('select', { class: 'input input-small', 'aria-label': 'Log level', onchange: () => {
      this.minLevel = +level.value;
      this.redrawLog();
    } }, h('option', { value: '4' }, 'Everything'), h('option', { value: '2', selected: true }, 'Info and above'),
         h('option', { value: '1' }, 'Warnings and errors'));
    const pause = h('button', { class: 'btn btn-small', onclick: () => {
      this.paused = !this.paused;
      setText(pause, this.paused ? 'Resume' : 'Pause');
      if (!this.paused) this.redrawLog();
    } }, 'Pause');
    const log = card('Log',
      h('div', { class: 'toolbar' }, level, pause,
        h('button', { class: 'btn btn-small', onclick: () => { this.clearedAt = store.logCount; this.redrawLog(); } }, 'Clear'),
        h('button', { class: 'btn btn-small', onclick: () => this.copyLog() }, icon('copy', 16), 'Copy')),
      this.logBox,
      h('p', { class: 'hint' }, 'Lines appear from when this page connected. The logger level in your YAML decides ' +
                                 'what the feeder sends (INFO by default).'));
    this.clearedAt = 0;

    // All entities
    this.filter = '';
    this.entityBox = h('div', { class: 'entities' });
    const search = h('input', { class: 'input', type: 'search', placeholder: 'Filter by name', 'aria-label': 'Filter entities',
                                oninput: () => { this.filter = search.value.trim().toLowerCase(); this.buildEntities(); } });
    const all = card('All entities', h('p', { class: 'hint' },
      'Everything this feeder reports, with a control for each. Changes act on the feeder straight away, ' +
      'through the same checks as everywhere else.'), search, this.entityBox);

    append(root, h('div', { class: 'grid' }, sys, net, mem), log, all);
    this.entityCount = -1;
    this.redrawLog();
    this.update(new Set(['*all']));
  },

  updateSystem() {
    for (const u of this.sysUpdaters) u();
  },

  update(changed) {
    for (const u of this.updaters) u();
    if (changed.has('*log') && !this.paused) this.appendLog();
    if (store.entities.size !== this.entityCount) this.buildEntities();
    else for (const id of changed) this.updateEntity(id);
  },

  unmount() {
    clearInterval(this.timer);
  },

  // ---- log ----
  logLine(entry) {
    const p = parseLogLine(entry.text);
    if ((LEVELS[p.level] ?? 2) > this.minLevel) return null;
    const time = new Date(entry.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    return h('div', { class: `log-line log-${p.level || 'I'}` },
      h('span', { class: 'log-time' }, time), h('span', { class: 'log-level' }, p.level || '·'),
      p.tag ? h('span', { class: 'log-tag' }, p.tag) : null, h('span', { class: 'log-text' }, p.text));
  },
  visibleLines() {
    const first = store.logCount - store.logLines.length;  // count of the oldest line kept
    return store.logLines.filter((_, i) => first + i >= this.clearedAt);
  },
  redrawLog() {
    clear(this.logBox);
    const lines = this.visibleLines().map(l => this.logLine(l)).filter(Boolean);
    if (!lines.length) this.logBox.appendChild(h('p', { class: 'log-empty' }, 'No log lines yet.'));
    append(this.logBox, lines);
    this.shownCount = store.logCount;
    this.logBox.scrollTop = this.logBox.scrollHeight;
  },
  appendLog() {
    const atBottom = this.logBox.scrollHeight - this.logBox.scrollTop - this.logBox.clientHeight < 30;
    const first = store.logCount - store.logLines.length;
    const fresh = store.logLines.filter((_, i) => first + i >= Math.max(this.shownCount, this.clearedAt));
    this.shownCount = store.logCount;
    const empty = this.logBox.querySelector('.log-empty');
    for (const l of fresh) {
      const el = this.logLine(l);
      if (!el) continue;
      if (empty) empty.remove();
      this.logBox.appendChild(el);
    }
    while (this.logBox.childElementCount > 300) this.logBox.firstElementChild.remove();
    if (atBottom) this.logBox.scrollTop = this.logBox.scrollHeight;
  },
  copyLog() {
    const text = this.visibleLines().map(l => {
      const p = parseLogLine(l.text);
      return `${new Date(l.at).toLocaleTimeString()} [${p.level}][${p.tag}] ${p.text}`;
    }).join('\n');
    const done = () => toast('Log copied', 'ok');
    // The Clipboard API needs HTTPS; the feeder serves plain HTTP, so fall back to a hidden text box.
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done, toastError);
      return;
    }
    const area = h('textarea', { class: 'offscreen' });
    area.value = text;
    document.body.appendChild(area);
    area.select();
    try {
      document.execCommand('copy');
      done();
    } catch (err) {
      toast('Could not copy: select the log text instead', 'error');
    }
    area.remove();
  },

  // ---- all entities ----
  buildEntities() {
    this.entityCount = store.entities.size;
    this.rows = new Map();
    clear(this.entityBox);
    const groups = new Map();
    for (const [id, e] of store.entities) {
      const name = e.name || id;
      if (this.filter && !name.toLowerCase().includes(this.filter)) continue;
      const domain = e.domain || id.slice(0, id.indexOf('/'));
      if (!groups.has(domain)) groups.set(domain, []);
      groups.get(domain).push([id, e]);
    }
    for (const domain of Object.keys(DOMAIN_TITLES).concat([...groups.keys()].filter(d => !DOMAIN_TITLES[d]))) {
      const items = groups.get(domain);
      if (!items) continue;
      items.sort((a, b) => (a[1].name || a[0]).localeCompare(b[1].name || b[0]));
      const list = h('div', { class: 'entity-list' });
      for (const [id, e] of items) {
        const value = h('span', { class: 'entity-state' });
        const control = entityControl(id);
        const tags = [e.entity_category === 1 ? 'config' : null, e.entity_category === 2 ? 'diagnostic' : null,
                      e.is_disabled_by_default ? 'hidden in Home Assistant' : null].filter(Boolean);
        list.appendChild(h('div', { class: 'entity-row' },
          h('div', { class: 'entity-name' }, e.name || id, tags.length ? h('span', { class: 'tags' }, tags.join(' · ')) : null),
          domain === 'button' || domain === 'switch' ? null : value,
          control));
        this.rows.set(id, { value, control });
        this.updateEntity(id);
      }
      this.entityBox.appendChild(h('details', { class: 'entity-group', open: true },
        h('summary', {}, `${DOMAIN_TITLES[domain] || domain} (${items.length})`), list));
    }
    if (!groups.size) this.entityBox.appendChild(h('p', { class: 'hint' }, this.filter ? 'Nothing matches.' : 'Waiting for the feeder…'));
  },
  updateEntity(id) {
    const r = this.rows && this.rows.get(id);
    if (!r) return;
    const e = store.get(id);
    setText(r.value, e.state === undefined ? '' : e.state);
    if (r.control && r.control._update) r.control._update();
  },
};
