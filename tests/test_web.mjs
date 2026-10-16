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
  formatDuration, parseLogLine, parseTodaysFeeds, parseRecentFeeds, parseLastFeed, sourceLabel, relativeDay, parseHold, parseScheduleData, daysText, eventTimeText, parseOneshots, addressKind, compareVersions,
  updateState, identifyImage };`, ctx);
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

test('address kinds', () => {
  assert.equal(P.addressKind('172.16.50.162'), 'IPv4');
  assert.equal(P.addressKind('fe80::1234:56ff:fe78:9abc'), 'IPv6 link-local');
  assert.equal(P.addressKind('FE80::1'), 'IPv6 link-local');
  assert.equal(P.addressKind('fd00::1'), 'IPv6 private');
  assert.equal(P.addressKind('2001:db8::1'), 'IPv6');
  assert.equal(P.addressKind(''), '');
  assert.equal(P.addressKind('0.0.0.0'), '');
  assert.equal(P.addressKind('::'), '');
});

// ---- the settings password (web/src/auth.js): the same answers as the device (tests/test_core.cpp) ----
const authSrc = readFileSync(new URL('../web/src/auth.js', import.meta.url), 'utf8');
const actx = vm.createContext({ TextEncoder, atob, btoa });
vm.runInContext(`${authSrc}\nthis.A = { sha256, hmacSha256, pbkdf2Sha256, toHex, fromHex, utf8, authHeader, authMessage, maskKey, maskText,
                                       keyToBase64, keyFromBase64 };`, actx);
const A = actx.A;

test('SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 4231)', () => {
  assert.equal(A.toHex(A.sha256(A.utf8('abc'))), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.equal(A.toHex(A.sha256(A.utf8(''))), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(A.toHex(A.sha256(A.utf8('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'))),
               '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
  assert.equal(A.toHex(A.sha256(new Uint8Array(1000).fill(0x61))),
               '41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3');
  // RFC 4231 test case 2, and case 6 (a key longer than a block).
  assert.equal(A.toHex(A.hmacSha256(A.utf8('Jefe'), A.utf8('what do ya want for nothing?'))),
               '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843');
  assert.equal(A.toHex(A.hmacSha256(new Uint8Array(131).fill(0xaa),
                                    A.utf8('Test Using Larger Than Block-Size Key - Hash Key First'))),
               '60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54');
});

test('PBKDF2-HMAC-SHA256', () => {
  // RFC 7914 section 11.
  assert.equal(A.toHex(A.pbkdf2Sha256(A.utf8('passwd'), A.utf8('salt'), 1)),
               '55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc');
});

test('settings password: the device vectors', () => {
  const salt = Uint8Array.from({ length: 16 }, (_, i) => i);
  const nonce = Uint8Array.from({ length: 16 }, (_, i) => 16 + i);
  const key = A.pbkdf2Sha256(A.utf8('correct horse'), salt, 20000);
  assert.equal(A.toHex(key), 'f353b5100691ea4531a49c82821a088e2083178c2c6954a17a1ec53b3900da25');
  const message = A.authMessage('/number/Daily Portion Cap/set', 3);
  assert.equal(message, '/number/Daily Portion Cap/set\n3');
  assert.equal(A.authHeader(key, nonce, message),
               A.toHex(nonce) + '9b2b5e3df64049cfbdb3b02ebddb43277df22b4cc0fe8c6c96f61ac11ed79330');
  const newKey = A.pbkdf2Sha256(A.utf8('battery staple'), Uint8Array.from({ length: 16 }, (_, i) => 32 + i), 20000);
  assert.equal(A.toHex(newKey), '880ec4efdf8f0c832ce0d85cc76b3191b73014a0c3768c3bbd5a14354698425c');
  assert.equal(A.toHex(A.maskKey(newKey, key, nonce)), '2e33a9c5afad47f2f7547a0ab2ab9f9ff8f2acea70fc7609bdf3b4903cedea2f');
  assert.equal(A.toHex(A.maskKey(newKey, null, nonce)), A.toHex(newKey), 'no current password: not masked');
  assert.equal(A.authMessage('/switch/Calibration Mode/turn_on'), '/switch/Calibration Mode/turn_on\n');
});

test('network: secret texts masked as the device unmasks them (2.0.0 N5)', () => {
  const key = A.fromHex('f353b5100691ea4531a49c82821a088e2083178c2c6954a17a1ec53b3900da25');
  const nonce = Uint8Array.from({ length: 16 }, (_, i) => 16 + i);
  const text = 'a wifi password longer than thirty-two bytes';
  assert.equal(A.toHex(A.maskText(text, key, nonce)),
               'a8685e68331b4ef6464670da56f92f1fcdcfcab7533f75b6a2a34b3a2eed915476e0934605b6633c2ac93e3d');
  assert.equal(A.toHex(A.maskText('hello', null, nonce)), A.toHex(A.utf8('hello')), 'no password: not masked');
});

test('keys: base64 in and out (2.0.0 N4)', () => {
  const key = Uint8Array.from({ length: 32 }, (_, i) => i);
  const text = A.keyToBase64(key);
  assert.equal(text, 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=');
  assert.equal(A.toHex(A.keyFromBase64(` ${text}\n`)), A.toHex(key), 'spaces around it are ignored');
  assert.equal(A.keyFromBase64('AAEC'), null, 'too short');
  assert.equal(A.keyFromBase64('not base64!'), null, 'not base64');
  assert.equal(A.keyFromBase64(A.keyToBase64(new Uint8Array(32))), null, 'the all-zero key is refused');
});

// ---- time zones (web/src/tz_zones.js, 2.0.0 N6) ----
const tzSrc = readFileSync(new URL('../web/src/tz_zones.js', import.meta.url), 'utf8');
const tctx = vm.createContext({});
vm.runInContext(`${tzSrc}\nthis.T = { tzZones, tzPosix, tzNamesFor, TZ_RULES };`, tctx);
const T = tctx.T;

test('time zones: names to the POSIX strings ESPHome compiles', () => {
  assert.equal(T.tzPosix('Europe/Berlin'), 'CET-1CEST,M3.5.0,M10.5.0/3');
  assert.equal(T.tzPosix('America/New_York'), 'EST5EDT,M3.2.0,M11.1.0');
  assert.equal(T.tzPosix('Asia/Kolkata'), 'IST-5:30');
  assert.equal(T.tzPosix('Nowhere/Special'), '');
  assert.ok(T.tzNamesFor('CET-1CEST,M3.5.0,M10.5.0/3').includes('Europe/Paris'));
  const names = T.tzZones().map(([n]) => n);
  assert.ok(names.length > 400 && names.every(n => n.includes('/')), 'Area/Location names only');
});

test('time zones: every rule the app can send is one the device parser is tested on (tests/tz_golden.h)', () => {
  const golden = new Set([...readFileSync(new URL('./tz_golden.h', import.meta.url), 'utf8')
    .matchAll(/^\s*\{"([^"]+)"/gm)].map(m => m[1]));
  const missing = T.TZ_RULES.filter(r => !golden.has(r));
  assert.deepEqual([...missing], [], 'regenerate both with ESPHome\'s Python: tools/gen_tz_golden.py, gen_tz_zones.py');
});

test('firmware versions and the update check', () => {
  assert.ok(P.compareVersions('2.0.0', '1.1.0') > 0);
  assert.ok(P.compareVersions('2.0.0', '2.0.1') < 0);
  assert.equal(P.compareVersions('2.0', '2.0.0'), 0);
  assert.equal(P.compareVersions('2.0.0-dev', '2.0.0'), 0);
  assert.ok(P.compareVersions('2.10.0', '2.9.9') > 0);
  const m = { version: '2.1.0', variants: {
    full: { ota: { sha256: 'aa' }, factory: { sha256: 'bb' } }, 'no-ble': { ota: { sha256: 'cc' } } } };
  assert.deepEqual(plain(P.updateState(m, '2.0.0', 'full')), { state: 'update', latest: '2.1.0' });
  assert.deepEqual(plain(P.updateState(m, '2.1.0', 'no-ble')), { state: 'current', latest: '2.1.0' });
  assert.deepEqual(plain(P.updateState(m, '2.2.0', 'full')), { state: 'current', latest: '2.1.0' });
  assert.equal(P.updateState(m, '2.0.0', 'yaml').state, 'yaml');
  assert.equal(P.updateState(m, '2.0.0', '').state, 'yaml');
  assert.equal(P.updateState(m, '2.0.0', 'other').state, 'unknown');
  assert.equal(P.updateState(null, '2.0.0', 'full').state, 'unknown');
  assert.deepEqual(plain(P.identifyImage(m, 'aa')), { variant: 'full', kind: 'ota', version: '2.1.0' });
  assert.deepEqual(plain(P.identifyImage(m, 'bb')), { variant: 'full', kind: 'factory', version: '2.1.0' });
  assert.equal(P.identifyImage(m, 'dd'), null);
  assert.equal(P.identifyImage(null, 'aa'), null);
});
