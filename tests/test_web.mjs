// Tests for the web app's text parsers (web/src/parse.js), with Node's built-in test runner and no packages:
//   node --test tests/test_web.mjs
// parse.js is a plain script (the app joins its sources into one module), so it is run in a fresh context here.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const src = readFileSync(new URL('../web/src/parse.js', import.meta.url), 'utf8');
const ctx = vm.createContext({});
vm.runInContext(`${src}\nthis.P = { parseKV, parseList, parseLocalDateTime, parseDeviceTime, formatClock, formatDate,
  formatDuration, parseLogLine, parseTodaysFeeds, parseRecentFeeds, parseLastFeed, sourceLabel, relativeDay, parseHold, parseScheduleData, daysText, eventTimeText, parseOneshots };`, ctx);
const P = ctx.P;
// Objects made inside the context have their own prototypes; compare them as plain data.
const plain = x => JSON.parse(JSON.stringify(x));

test('key=value lines (BLE Info, BLE Session)', () => {
  assert.deepEqual(plain(P.parseKV('connected=1;auth=0;peer=4A:21;mtu=23')),
                   { connected: '1', auth: '0', peer: '4A:21', mtu: '23' });
  assert.deepEqual(plain(P.parseKV('peer=;x')), { peer: '' });
  assert.deepEqual(plain(P.parseKV('')), {});
});

test('lists where "none" is empty (Faults)', () => {
  assert.deepEqual(plain(P.parseList('none')), []);
  assert.deepEqual(plain(P.parseList('clock not set; tray empty')), ['clock not set', 'tray empty']);
  assert.deepEqual(plain(P.parseList('')), []);
});

test('device local time and Device Time', () => {
  const t = P.parseLocalDateTime('Thu 2026-10-15 17:30');
  assert.equal(t.weekday, 'Thu');
  assert.equal(t.date, '2026-10-15');
  assert.equal(t.hour, 17);
  assert.equal(t.minute, 30);
  assert.equal(P.parseLocalDateTime('clock not set'), null);
  const d = P.parseDeviceTime('Thu 2026-10-15 17:30 -05:00');
  assert.equal(d.offsetMin, -300);
  assert.equal(d.offset, '-05:00');
  assert.equal(P.parseDeviceTime('Mon 2026-03-30 09:05 +05:30').offsetMin, 330);
  assert.equal(P.parseDeviceTime('Thu 2026-10-15 17:30'), null);
});

test('times are shown as the device gives them, in the browser locale', () => {
  assert.equal(P.formatClock(17, 30, 'en-GB'), '17:30');
  assert.match(P.formatClock(17, 30, 'en-US'), /^5:30\sPM$/);
  assert.match(P.formatDate({ year: 2026, month: 10, day: 15 }, 'en-GB'), /Thu/);
});

test('durations', () => {
  assert.equal(P.formatDuration(40), '40 s');
  assert.equal(P.formatDuration(12 * 60 + 5), '12 min');
  assert.equal(P.formatDuration(2 * 3600 + 5 * 60), '2 h 5 min');
  assert.equal(P.formatDuration(3 * 86400 + 4 * 3600 + 59), '3 d 4 h');
  assert.equal(P.formatDuration(null), '');
});

test('log lines', () => {
  const l = P.parseLogLine('\x1b[0;32m[I][precision_feeder:123]: Feeding 1 portion(s)\x1b[0m');
  assert.deepEqual(plain(l), { level: 'I', tag: 'precision_feeder', text: 'Feeding 1 portion(s)' });
  assert.equal(P.parseLogLine('[W][schedule_editor]: Days not understood').level, 'W');
  assert.deepEqual(plain(P.parseLogLine('plain text')), { level: '', tag: '', text: 'plain text' });
});

test("Today's Feeds", () => {
  const t = P.parseTodaysFeeds('08:00 x1 fed; 12:00 x1 next one-shot; 20:00 x2 later');
  assert.equal(t.items.length, 3);
  assert.deepEqual(plain(t.items[1]), { hour: 12, minute: 0, time: '12:00', portions: 1, status: 'next', oneshot: true });
  assert.equal(t.items[2].portions, 2);
  for (const s of ['fed', 'skipped', 'due', 'next', 'later', 'held']) assert.equal(P.parseTodaysFeeds(`09:15 x1 ${s}`).items[0].status, s);
  assert.deepEqual(plain(P.parseTodaysFeeds('none')), { items: [], note: '' });
  assert.equal(P.parseTodaysFeeds('none (fast day)').note, 'fast day');
  assert.equal(P.parseTodaysFeeds('none (not a feeding day)').note, 'not a feeding day');
  assert.equal(P.parseTodaysFeeds('clock not set').note, 'clock not set');
  assert.equal(P.parseTodaysFeeds('08:00 x1 fed; ...').items.length, 1);
  assert.equal(P.parseTodaysFeeds('something else'), null);
});

test('Recent Feeds and Last Feed', () => {
  const r = P.parseRecentFeeds('10-15 08:30 x1 schedule; unknown time x2 button; 10-14 17:00 x1 one-shot');
  assert.equal(r.length, 3);
  assert.deepEqual(plain(r[0]), { month: 10, day: 15, hour: 8, minute: 30, portions: 1, source: 'schedule' });
  assert.equal(r[1].month, null);
  assert.equal(r[1].source, 'button');
  assert.equal(r[2].source, 'one-shot');
  assert.deepEqual(plain(P.parseRecentFeeds('none')), []);
  const l = P.parseLastFeed('Thu 2026-10-15 09:28 (ESPHome)');
  assert.equal(l.source, 'ESPHome');
  assert.equal(l.when.time, '09:28');
  assert.equal(P.parseLastFeed('never').note, 'never');
  assert.equal(P.sourceLabel('ble'), 'Bluetooth');
});

test('relative days', () => {
  const today = { year: 2026, month: 12, day: 31 };
  assert.equal(P.relativeDay({ year: 2027, month: 1, day: 1 }, today), 'tomorrow');
  assert.equal(P.relativeDay({ year: 2026, month: 12, day: 31 }, today), 'today');
  assert.equal(P.relativeDay({ year: 2026, month: 12, day: 30 }, today), 'yesterday');
  assert.equal(P.relativeDay({ year: 2027, month: 1, day: 5 }, today), null);
});

test('Hold', () => {
  assert.equal(P.parseHold('none'), null);
  assert.deepEqual(plain(P.parseHold('until cancelled')), { until: null, note: '' });
  assert.equal(P.parseHold('until Sun 2026-10-18 14:28').until.hour, 14);
  assert.equal(P.parseHold('active (clock not set)').note, 'active (clock not set)');
});

test('Schedule Data', () => {
  const s = P.parseScheduleData('1|1,01,1c510/1/7e,1c1050/2/7e,0s-30/1/7f,0c720/1/7f|14,41,0c480/1/7f,0c720/1/7f,1s-720/6/3e,0c1200/1/7f|1,00,1r30/1/7f,1c720/1/7f,1s-60/1/7f,0c720/1/7f');
  assert.equal(s.length, 3);
  assert.equal(s[0].everyN, 1);
  assert.equal(s[0].fast, 1);
  assert.deepEqual(plain(s[0].events[1]), { enabled: true, anchor: 'clock', offset: 1050, portions: 2, days: 0x7e });
  assert.deepEqual(plain(s[1].events[2]), { enabled: true, anchor: 'sunset', offset: -720, portions: 6, days: 0x3e });
  assert.equal(s[1].everyN, 14);
  assert.equal(s[2].events[0].anchor, 'sunrise');
  assert.equal(P.parseScheduleData('2|...'), null);
  assert.equal(P.parseScheduleData('1|1,00,0c480/1/7f'), null);
  assert.equal(P.parseScheduleData(''), null);
});

test('event texts the firmware reads back the same way', () => {
  assert.equal(P.daysText(0x7f), 'all');
  assert.equal(P.daysText(0), 'none');
  assert.equal(P.daysText(0x2a), 'mon,wed,fri');
  assert.equal(P.daysText(0x41), 'sun,sat');
  assert.equal(P.eventTimeText('clock', 510), '08:30');
  assert.equal(P.eventTimeText('clock', 0), '00:00');
  assert.equal(P.eventTimeText('sunset', -30), '-00:30');
  assert.equal(P.eventTimeText('sunrise', 90), '+01:30');
  assert.equal(P.eventTimeText('sunrise', 0), '+00:00');
});

test('One-shot Feeds', () => {
  const o = P.parseOneshots('Tue 2026-10-20 18:00 x1; Wed 2026-10-21 07:30 x2');
  assert.equal(o.length, 2);
  assert.equal(o[1].portions, 2);
  assert.equal(o[1].when.time, '07:30');
  assert.deepEqual(plain(P.parseOneshots('none')), []);
});
