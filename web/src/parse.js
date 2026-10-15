// The device's text formats, read into plain values. Pure functions with no DOM, so tests/test_web.mjs can run them
// under Node. Formats: DESIGN.md section 7 and components/precision_feeder/feeder_text.cpp. Every parser returns
// null (or an empty list) for text it does not recognise; the caller then shows the raw text instead.

const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

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
