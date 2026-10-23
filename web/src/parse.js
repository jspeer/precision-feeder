// The device's text formats, read into plain values. Pure functions with no DOM, so tests/test_web.mjs can run them
// under Node. Formats: DESIGN.md section 7 and components/precision_feeder/feeder_text.cpp. Every parser returns
// null (or an empty list) for text it does not recognise; the caller then shows the raw text instead.

const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

/** GET /feeder/wifi/networks (2.3.0) -> {scan, scanning, networks: [{hex, name, rssi, secured, signal}]}. Names come
 *  as hex (any bytes), shown as UTF-8 or with "?" for other bytes; joining sends the hex back unchanged. */
function parseWifiNetworks(json) {
  const out = { scan: !!(json && json.scan), scanning: !!(json && json.scanning), networks: [] };
  for (const n of (json && Array.isArray(json.networks)) ? json.networks : []) {
    const hex = String(n && n.ssid || '');
    if (!/^([0-9a-f]{2}){1,32}$/i.test(hex)) continue;
    let name;
    try {
      name = decodeURIComponent(hex.replace(/../g, '%$&'));
    } catch (e) {
      name = hex.replace(/../g, b => { const c = parseInt(b, 16); return c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : '?'; });
    }
    const rssi = Number(n.rssi);
    out.networks.push({ hex: hex.toLowerCase(), name, rssi, secured: n.secured !== false,
                        signal: rssi >= -60 ? 'good' : rssi >= -70 ? 'fair' : 'weak' });
  }
  return out;
}

/** Wi-Fi list after a scan (2.3.1): keeps what the user picked ('' is Other), listed if the scan missed it. */
function wifiChoice(shown, found, value, touched) {
  const has = found.some(n => n.hex === value), old = !has && shown.find(n => n.hex === value);
  if (!touched) return { networks: found, value: has ? value : found.length ? found[0].hex : '' };
  return { networks: old ? found.concat(old) : found, value };
}

/** "a=1;b=x" -> {a: '1', b: 'x'} (BLE Info, BLE Session). */
function parseKV(text) {
  const out = {};
  for (const part of String(text || '').split(';')) {
    const i = part.indexOf('=');
    if (i > 0) out[part.slice(0, i).trim()] = part.slice(i + 1).trim();
  }
  return out;
}

/** A "; "-separated list where "none" means empty (Faults, Recent Feeds, One-shot Feeds). */
function parseList(text) {
  const t = String(text || '').trim();
  if (!t || t === 'none') return [];
  return t.split(';').map(s => s.trim()).filter(Boolean);
}

/** "Thu 2026-10-15 17:30" (the device's local time, as format_local_ writes it) -> parts, or null. */
function parseLocalDateTime(text) {
  const m = /^(?:(\w{3}) )?(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2})/.exec(String(text || '').trim());
  if (!m) return null;
  const [, weekday, y, mo, d, hh, mm] = m;
  return { weekday: weekday || null, year: +y, month: +mo, day: +d, hour: +hh, minute: +mm,
           date: `${y}-${mo}-${d}`, time: `${hh}:${mm}` };
}

/** "Thu 2026-10-15 17:30 -05:00" (Device Time) -> parts plus the UTC offset in minutes, or null. */
function parseDeviceTime(text) {
  const t = String(text || '').trim();
  const base = parseLocalDateTime(t);
  const m = /([+-])(\d{2}):(\d{2})$/.exec(t);
  if (!base || !m) return null;
  const offsetMin = (m[1] === '-' ? -1 : 1) * (+m[2] * 60 + +m[3]);
  return Object.assign(base, { offset: `${m[1]}${m[2]}:${m[3]}`, offsetMin });
}

/** A wall-clock time on the device as the browser's locale writes times (17:30 or 5:30 PM). No timezone maths:
 *  the device already gives local time, so it is formatted as if it were UTC. */
function formatClock(hour, minute, locale) {
  const d = new Date(Date.UTC(2000, 0, 1, hour, minute));
  return d.toLocaleTimeString(locale, { hour: 'numeric', minute: '2-digit', timeZone: 'UTC' });
}

/** A date on the device as "Thu 15 Oct" in the browser's language. */
function formatDate(parts, locale) {
  const d = new Date(Date.UTC(parts.year, parts.month - 1, parts.day, 12));
  return d.toLocaleDateString(locale, { weekday: 'short', day: 'numeric', month: 'short', timeZone: 'UTC' });
}

/** Seconds as "3 d 4 h", "2 h 5 min", "12 min" or "40 s". */
function formatDuration(seconds) {
  if (seconds == null || !isFinite(seconds) || seconds < 0) return '';
  const s = Math.floor(seconds);
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  if (d) return `${d} d ${h} h`;
  if (h) return `${h} h ${m} min`;
  if (m) return `${m} min`;
  return `${s} s`;
}

/** A log line from /events ("\x1b[0;32m[I][tag:042]: text\x1b[0m") -> {level, tag, text}. */
function parseLogLine(line) {
  const clean = String(line || '').replace(/\x1b\[[0-9;]*m/g, '');
  const m = /^\[([EWICDV])\]\[([^\]]*)\]:?\s?(.*)$/s.exec(clean);
  if (!m) return { level: '', tag: '', text: clean };
  return { level: m[1], tag: m[2].replace(/:\d+$/, ''), text: m[3] };
}

/** Today's Feeds: "08:30 x1 fed; 17:30 x2 next one-shot" -> {items: [{hour, minute, time, portions, status,
 *  oneshot}], note}. With none, items is empty and note is the reason ("fast day", ...) or ''. null if unreadable. */
function parseTodaysFeeds(text) {
  const t = String(text || '').trim();
  const none = /^none(?: \((.*)\))?$/.exec(t);
  if (none) return { items: [], note: none[1] || '' };
  if (t === 'clock not set') return { items: [], note: 'clock not set' };
  const items = [];
  for (const part of t.split(';').map(s => s.trim()).filter(Boolean)) {
    if (part === '...') continue;
    const m = /^(\d{2}):(\d{2}) x(\d+) (fed|skipped|due|next|later|held)( one-shot)?$/.exec(part);
    if (!m) return null;
    items.push({ hour: +m[1], minute: +m[2], time: `${m[1]}:${m[2]}`, portions: +m[3], status: m[4], oneshot: !!m[5] });
  }
  return { items, note: '' };
}

/** Recent Feeds: "10-15 08:30 x1 schedule; unknown time x1 button" -> [{month, day, hour, minute, portions,
 *  source}] (month null for "unknown time"). Unreadable entries are skipped. */
function parseRecentFeeds(text) {
  const out = [];
  for (const part of parseList(text)) {
    const m = /^(?:(\d{2})-(\d{2}) (\d{2}):(\d{2})|unknown time) x(\d+) (.+)$/.exec(part);
    if (!m) continue;
    out.push({ month: m[1] ? +m[1] : null, day: m[2] ? +m[2] : null, hour: m[3] ? +m[3] : null,
               minute: m[4] ? +m[4] : null, portions: +m[5], source: m[6] });
  }
  return out;
}

/** Last Feed: "Thu 2026-10-15 09:28 (ESPHome)" -> {when: parts, source}; "never" / "clock was not set" -> {when:
 *  null, source: null, note}. */
function parseLastFeed(text) {
  const t = String(text || '').trim();
  const when = parseLocalDateTime(t);
  if (!when) return { when: null, source: null, note: t };
  const m = /\(([^)]*)\)$/.exec(t);
  return { when, source: m ? m[1] : null, note: '' };
}

/** The words a feed source has in the device's texts, for people. */
function sourceLabel(source) {
  return { schedule: 'schedule', ESPHome: 'web or Home Assistant', ble: 'Bluetooth', button: 'the button',
           'one-shot': 'one-shot', adjustment: 'count adjusted' }[source] || source || '';
}

/** "today", "tomorrow", "yesterday" or null, for a device date against the device's today. */
function relativeDay(parts, today) {
  if (!parts || !today) return null;
  const a = Date.UTC(parts.year, parts.month - 1, parts.day), b = Date.UTC(today.year, today.month - 1, today.day);
  const diff = Math.round((a - b) / 86400000);
  return { 0: 'today', 1: 'tomorrow', '-1': 'yesterday' }[diff] || null;
}

/** Hold: "none" -> null; "until cancelled" -> {until: null}; "until Thu 2026-10-15 17:30" -> {until: parts};
 *  "active (clock not set)" -> {until: null, note}. */
function parseHold(text) {
  const t = String(text || '').trim();
  if (!t || t === 'none') return null;
  if (t === 'until cancelled') return { until: null, note: '' };
  const until = parseLocalDateTime(t.replace(/^until /, ''));
  return until ? { until, note: '' } : { until: null, note: t };
}

const ANCHORS = { c: 'clock', r: 'sunrise', s: 'sunset' };
const DAY_KEYS = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];  // bit 0 = Sunday, as the firmware

/** Schedule Data: "1|<A>|<B>|<C>", each profile "every_n,fast,ev1,ev2,ev3,ev4", each event
 *  "<0|1><c|r|s><offset>/<portions>/<days>" (DESIGN.md section 7) -> [{everyN, fast, events: [{enabled, anchor,
 *  offset, portions, days}]}], or null if it is not that format (a newer version, say). */
function parseScheduleData(text) {
  const parts = String(text || '').trim().split('|');
  if (parts[0] !== '1' || parts.length < 2) return null;
  const profiles = [];
  for (const p of parts.slice(1)) {
    const f = p.split(',');
    if (f.length !== 6) return null;
    const events = [];
    for (const ev of f.slice(2)) {
      const m = /^([01])([crs])(-?\d+)\/(\d+)\/([0-9a-f]{2})$/.exec(ev);
      if (!m) return null;
      events.push({ enabled: m[1] === '1', anchor: ANCHORS[m[2]], offset: +m[3], portions: +m[4], days: parseInt(m[5], 16) });
    }
    profiles.push({ everyN: +f[0], fast: parseInt(f[1], 16), events });
  }
  return profiles;
}

/** A weekday mask as the firmware's Event Days text: "all", "none" or "sun,mon,...". */
function daysText(mask) {
  mask &= 0x7f;
  if (mask === 0x7f) return 'all';
  if (mask === 0) return 'none';
  return DAY_KEYS.filter((_, i) => mask & (1 << i)).join(',');
}

/** An event's time as the firmware's Event Time text: "08:30" (clock) or "+01:30" / "-00:45" (sun). */
function eventTimeText(anchor, offset) {
  const a = Math.abs(offset), hh = String(Math.floor(a / 60)).padStart(2, '0'), mm = String(a % 60).padStart(2, '0');
  return anchor === 'clock' ? `${hh}:${mm}` : `${offset < 0 ? '-' : '+'}${hh}:${mm}`;
}

/** One-shot Feeds: "Thu 2026-10-20 18:00 x1; ..." -> [{when: parts, portions}]. */
function parseOneshots(text) {
  const out = [];
  for (const part of parseList(text)) {
    const m = /^(.*) x(\d+)$/.exec(part);
    const when = m && parseLocalDateTime(m[1]);
    if (when) out.push({ when, portions: +m[2] });
  }
  return out;
}

/** What kind of address a wifi_info address is: "IPv4", "IPv6 link-local" (fe80::/10, this network only),
 *  "IPv6 private" (fc00::/7, unique local), "IPv6"; '' for an empty or unset one. */
function addressKind(addr) {
  const a = String(addr || '').trim().toLowerCase();
  if (!a || a === '0.0.0.0' || a === '::') return '';
  if (/^\d{1,3}(\.\d{1,3}){3}$/.test(a)) return 'IPv4';
  if (!a.includes(':')) return '';
  if (/^fe[89ab]/.test(a)) return 'IPv6 link-local';
  if (/^f[cd]/.test(a)) return 'IPv6 private';
  return 'IPv6';
}

/** Compare two "major.minor.revision" versions: negative if a is older, 0 if the same, positive if newer. Missing
 *  parts count as 0; anything after a number in a part ("2.0.0-dev") is ignored. */
function compareVersions(a, b) {
  // A branch build's suffix ("2.1.0-n20.1": a variant on top of 2.1.0) is not part of the release number.
  const parts = v => String(v || '').split('-')[0].split('.').map(p => parseInt(p, 10) || 0);
  const x = parts(a), y = parts(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] || 0) - (y[i] || 0);
    if (d) return d;
  }
  return 0;
}

/** The update check (DESIGN.md section 9.5): the release manifest against the feeder's Firmware Version and
 *  Firmware Variant. {state: 'current' | 'update' | 'yaml' | 'unlisted' | 'unknown', latest}. 'yaml': built from its
 *  own YAML, updated with ESPHome; 'unlisted': a variant the releases do not publish (a branch build, firmware 2.1.0),
 *  so no image is ever offered to it; 'unknown': no manifest. */
function updateState(manifest, version, variant) {
  const latest = manifest && manifest.version;
  if (!latest) return { state: 'unknown', latest: '' };
  if (variant === 'yaml' || !variant) return { state: 'yaml', latest };
  if (!manifest.variants || !manifest.variants[variant]) return { state: 'unlisted', latest };
  return { state: compareVersions(latest, version) > 0 ? 'update' : 'current', latest };
}

/** Which released image a file is, by its SHA-256 (hex): {variant, kind: 'ota' | 'factory', version}, or null. */
function identifyImage(manifest, sha256hex) {
  const variants = (manifest && manifest.variants) || {};
  for (const variant of Object.keys(variants)) {
    for (const kind of ['ota', 'factory']) {
      const image = variants[variant][kind];
      if (image && String(image.sha256).toLowerCase() === sha256hex) return { variant, kind, version: manifest.version };
    }
  }
  return null;
}
