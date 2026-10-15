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
  formatDuration, parseLogLine };`, ctx);
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
