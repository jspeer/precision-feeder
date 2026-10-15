// The connection to the device and everything it has told us. One EventSource on /events: ESPHome's web server
// sends a "ping" with the device's title and uptime, then every entity of the build with full detail (name,
// limits, options, ...), then a "state" event whenever one changes, "log" lines, and a "ping" every 10 seconds.
// The app reads the device only through here, and changes it only through api.js.

const LOG_MAX = 300;
const STALE_MS = 25000;  // no ping for this long: the connection is dead even if the browser has not noticed
const HIDDEN_RELEASE_MS = 30000;  // a page hidden this long gives its connection back (see start())

const store = {
  entities: new Map(),  // "domain/Name" -> the latest JSON the device sent for it (full detail kept)
  title: '',
  status: 'connecting',  // connecting | online | offline | paused (page hidden, connection given back)
  everOnline: false,
  lastNewEntity: 0,      // Date.now() when an entity was last seen for the first time
  logLines: [],          // {at, text}, newest last
  logCount: 0,           // lines received in total (so a view can tell which ones are new)
  uptimeS: null,
  uptimeAt: 0,
  _subs: new Set(),
  _watchers: new Set(),
  _changed: new Set(),
  _timer: 0,
  _es: null,
  _retry: 0,
  _lastMessage: 0,

  get(id) { return this.entities.get(id); },
  has(id) { return this.entities.has(id); },
  /** The value as text (text sensors, selects, texts), '' if unknown. */
  text(id) {
    const e = this.entities.get(id);
    return e ? String(e.value ?? e.state ?? '') : '';
  },
  /** The value as a number (sensors, numbers), NaN if unknown. */
  num(id) {
    const e = this.entities.get(id);
    if (!e || e.value === null || e.value === undefined) return NaN;
    return typeof e.value === 'number' ? e.value : parseFloat(e.value);
  },
  /** Binary sensors and switches: true, false, or undefined if not in this build. */
  bool(id) {
    const e = this.entities.get(id);
    return e ? e.value === true : undefined;
  },
  /** Seconds since the device booted, counted on from its last ping. */
  uptime() {
    return this.uptimeS === null ? null : this.uptimeS + (Date.now() - this.uptimeAt) / 1000;
  },

  /** fn(changed: Set of ids, plus "*status", "*ping", "*log") after each burst of updates. Returns unsubscribe. */
  subscribe(fn) {
    this._subs.add(fn);
    return () => this._subs.delete(fn);
  },
  /** fn(id) at once for every update (for code waiting on one entity). Returns unwatch. */
  watch(fn) {
    this._watchers.add(fn);
    return () => this._watchers.delete(fn);
  },

  _mark(id) {
    this._changed.add(id);
    for (const w of [...this._watchers]) w(id);
    if (!this._timer) this._timer = setTimeout(() => this._flush(), 40);
  },
  _flush() {
    this._timer = 0;
    const changed = this._changed;
    this._changed = new Set();
    for (const fn of [...this._subs]) fn(changed);
  },
  _setStatus(status) {
    if (this.status === status) return;
    this.status = status;
    if (status === 'online') this.everOnline = true;
    this._mark('*status');
  },

  connect() {
    if (this._es) this._es.close();
    const es = new EventSource('/events');
    this._es = es;
    this._lastMessage = Date.now();
    const alive = () => { this._lastMessage = Date.now(); this._retry = 0; this._setStatus('online'); };
    es.onopen = alive;
    es.onerror = () => {
      if (this._es !== es) return;
      // Reconnect ourselves: ESPHome asks browsers to wait 30 s, too long for someone looking at the page.
      es.close();
      this._es = null;
      this._setStatus('offline');
      setTimeout(() => this.connect(), Math.min(10000, 1000 * 2 ** this._retry++));
    };
    es.addEventListener('ping', ev => {
      alive();
      try {
        const d = JSON.parse(ev.data);
        if (typeof d.title === 'string') this.title = d.title;
        if (typeof d.uptime === 'number') {
          // Uptime going backwards means the feeder restarted (a power cut, or an update).
          if (this.uptimeS !== null && d.uptime + 5 < this.uptime()) this._mark('*restart');
          this.uptimeS = d.uptime;
          this.uptimeAt = Date.now();
        }
      } catch (err) { /* a ping we cannot read changes nothing */ }
      this._mark('*ping');
    });
    es.addEventListener('state', ev => {
      alive();
      let d;
      try { d = JSON.parse(ev.data); } catch (err) { return; }
      // ESPHome 2026.x ids are "domain/Name"; build one from the detail if an older format ever arrives.
      let id = d.id;
      if (typeof id !== 'string' || !id.includes('/')) id = d.domain && d.name ? `${d.domain}/${d.name}` : null;
      if (!id) return;
      const prev = this.entities.get(id);
      if (prev) Object.assign(prev, d);
      else {
        this.entities.set(id, d);
        this.lastNewEntity = Date.now();
      }
      this._mark(id);
    });
    es.addEventListener('log', ev => {
      alive();
      this.logLines.push({ at: Date.now(), text: ev.data });
      this.logCount++;
      if (this.logLines.length > LOG_MAX) this.logLines.shift();
      this._mark('*log');
    });
  },

  start() {
    this.connect();
    setInterval(() => {
      if (this._es && this.status === 'online' && Date.now() - this._lastMessage > STALE_MS) {
        this._es.onerror();  // treat silence as a dropped connection
      }
    }, 5000);
    // The feeder's web server has only a few sockets, and drops the least recently used connection when they run
    // out (measured: about four open pages at once). So a page hidden for a while (a background tab, a phone in a
    // pocket) lets go of its connection, and takes a new one, with everything current, as soon as it is shown.
    let hiddenTimer = 0;
    document.addEventListener('visibilitychange', () => {
      clearTimeout(hiddenTimer);
      if (document.hidden) {
        hiddenTimer = setTimeout(() => {
          if (!this._es) return;
          this._es.close();
          this._es = null;
          this.status = 'paused';
          this._mark('*status');
        }, HIDDEN_RELEASE_MS);
      } else if (!this._es) {
        this._retry = 0;
        this.connect();
      }
    });
  },
};
