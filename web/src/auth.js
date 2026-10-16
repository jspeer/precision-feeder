// The settings password (DESIGN.md section 10.1). The page is plain HTTP, where browsers offer no crypto.subtle, so
// SHA-256, HMAC and PBKDF2 are written out here (small, and tested against the device's vectors in
// tests/test_web.mjs). The password never leaves this page: it becomes a key (PBKDF2 with the device's salt), kept in
// memory only, and each protected change carries HMAC(key, one-time challenge || what is changed).

const SHA256_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
  0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
  0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
  0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
  0xc67178f2]);
const SHA256_IV = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];

/** One 64-byte block into state h (8 words), in place. */
function sha256Block(h, block, w) {
  for (let i = 0; i < 16; i++)
    w[i] = (block[4 * i] << 24) | (block[4 * i + 1] << 16) | (block[4 * i + 2] << 8) | block[4 * i + 3];
  for (let i = 16; i < 64; i++) {
    const a = w[i - 15], b = w[i - 2];
    const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
    const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
    w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
  }
  let [a, b, c, d, e, f, g, k] = h;
  for (let i = 0; i < 64; i++) {
    const t1 = (k + (((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7))) +
                ((e & f) ^ (~e & g)) + SHA256_K[i] + w[i]) | 0;
    const t2 = ((((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10))) +
                ((a & b) ^ (a & c) ^ (b & c))) | 0;
    k = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
  }
  h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
  h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + k) | 0;
}

/** SHA-256 of bytes, continuing from state `start` after `done` bytes (for HMAC's precomputed pads). */
function sha256From(start, done, bytes) {
  const h = start.slice(), w = new Int32Array(64);
  const total = done + bytes.length;
  const padded = new Uint8Array(((bytes.length + 9 + 63) >> 6) << 6);
  padded.set(bytes);
  padded[bytes.length] = 0x80;
  const bits = total * 8;
  for (let i = 0; i < 4; i++) padded[padded.length - 1 - i] = (bits / 2 ** (8 * i)) & 0xff;
  for (let i = 0; i < padded.length; i += 64) sha256Block(h, padded.subarray(i, i + 64), w);
  const out = new Uint8Array(32);
  for (let i = 0; i < 8; i++) {
    out[4 * i] = h[i] >>> 24; out[4 * i + 1] = h[i] >>> 16; out[4 * i + 2] = h[i] >>> 8; out[4 * i + 3] = h[i];
  }
  return out;
}

function sha256(bytes) { return sha256From(SHA256_IV, 0, bytes); }

function concatBytes(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

/** HMAC-SHA256 with the key's pads hashed once: returns (message bytes) => tag. */
function hmacSha256Keyed(key) {
  if (key.length > 64) key = sha256(key);
  const pad = (x) => {
    const block = new Uint8Array(64);
    for (let i = 0; i < 64; i++) block[i] = (key[i] || 0) ^ x;
    const h = SHA256_IV.slice();
    sha256Block(h, block, new Int32Array(64));
    return h;
  };
  const inner = pad(0x36), outer = pad(0x5c);
  return msg => sha256From(outer, 64, sha256From(inner, 64, msg));
}

function hmacSha256(key, msg) { return hmacSha256Keyed(key)(msg); }

/** PBKDF2-HMAC-SHA256, one 32-byte block (all the device needs). */
function pbkdf2Sha256(password, salt, iterations) {
  const mac = hmacSha256Keyed(password);
  let u = mac(concatBytes(salt, new Uint8Array([0, 0, 0, 1])));
  const out = u.slice();
  for (let i = 1; i < iterations; i++) {
    u = mac(u);
    for (let j = 0; j < 32; j++) out[j] ^= u[j];
  }
  return out;
}

const utf8 = s => new TextEncoder().encode(s);
const toHex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
function fromHex(hex) {
  const out = new Uint8Array(hex.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(2 * i, 2), 16);
  return out;
}

/** What a protected entity change is signed over: the path the device sees and the value written. */
function authMessage(path, value) { return `${path}\n${value === undefined || value === null ? '' : String(value)}`; }

/** The X-Feeder-Auth header: nonce then HMAC(key, nonce || message), in hex. */
function authHeader(key, nonce, message) { return toHex(nonce) + toHex(hmacSha256(key, concatBytes(nonce, utf8(message)))); }

/** A new key travels masked by the current one (none: unmasked), as FeederCore::password_unmask expects. */
function maskKey(newKey, currentKey, nonce) {
  if (!currentKey) return newKey.slice();
  const mask = hmacSha256(currentKey, concatBytes(nonce, utf8('new key')));
  return newKey.map((b, i) => b ^ mask[i]);
}

/** A 32-byte key as base64, as ESPHome (api: encryption: key) and the BLE clients write it. */
function keyToBase64(bytes) { return btoa(String.fromCharCode(...bytes)); }
/** A key typed in: base64 of exactly 32 bytes, not all zero; null otherwise. */
function keyFromBase64(text) {
  let raw;
  try { raw = atob(String(text).trim()); } catch (e) { return null; }
  if (raw.length !== 32) return null;
  const bytes = Uint8Array.from(raw, ch => ch.charCodeAt(0));
  return bytes.some(b => b !== 0) ? bytes : null;
}

const auth = {
  available: false,   // the device has the settings guard (the web server package with it)
  set: false,         // a settings password is set
  key: null,          // the derived key once unlocked (memory only: a reload locks again)
  protectedIds: new Set(),
  // Where the API (Home Assistant) and BLE keys come from: none, build (the YAML's), device (saved on it) or off
  // (this build cannot have one). The device never sends a key.
  keys: { api: 'off', ble: 'off' },
  _listeners: [],

  onChange(fn) { this._listeners.push(fn); },
  _changed() { for (const fn of this._listeners) fn(); },

  /** Ask the device for a challenge (and its current state). */
  async challenge() {
    const r = await fetch(`${location.origin}/feeder/auth`, { cache: 'no-store' });
    if (!r.ok) throw new Error(`The feeder answered ${r.status}.`);
    const c = await r.json();
    this.available = true;
    this.set = c.scheme !== 0;
    if (!this.set) this.key = null;
    this.protectedIds = new Set(c.protected || []);
    this.keys = Object.assign({ api: 'off', ble: 'off' }, c.keys || {});
    c.nonceBytes = fromHex(c.nonce);
    c.saltBytes = fromHex(c.salt);
    return c;
  },
  async refresh() {
    try { await this.challenge(); } catch (e) { this.available = false; }
    this._changed();
  },
  isProtected(id) { return this.protectedIds.has(id); },
  locked() { return this.set && !this.key; },

  /** Headers for a request to `path` writing `value`: {} without a password; throws while locked. */
  async headersFor(path, value) {
    const c = await this.challenge();
    if (!this.set) return {};
    if (!this.key) throw new Error('Locked: unlock with the settings password (Configuration) first.');
    return { 'X-Feeder-Auth': authHeader(this.key, c.nonceBytes, authMessage(path, value)) };
  },

  /** Turn a refused answer into a message. */
  async refusal(r) {
    let wait = 0;
    try { wait = (await r.json()).wait || 0; } catch (e) { /* no body */ }
    if (r.status === 429) return new Error(`Too many tries: wait ${Math.ceil(wait / 1000)} s.`);
    if (r.status === 403) {
      this.key = null;
      this._changed();
      return new Error('The settings password was not accepted. Unlock again.');
    }
    return new Error(`The feeder answered ${r.status}.`);
  },

  async unlock(password) {
    const c = await this.challenge();
    if (!this.set) return;
    const key = pbkdf2Sha256(utf8(password), c.saltBytes, c.iterations);
    const path = '/feeder/auth/check';
    const r = await fetch(location.origin + path, { method: 'POST',
      headers: { 'X-Feeder-Auth': authHeader(key, c.nonceBytes, authMessage(path, '')) } });
    if (!r.ok) {
      const err = await this.refusal(r);
      throw r.status === 403 ? new Error('That is not the settings password.') : err;
    }
    this.key = key;
    this._changed();
  },

  lock() { this.key = null; this._changed(); },

  /** Save a 32-byte key ('api' or 'ble') on the device, masked like a new password key, or clear the API key with
   *  null. Signed with the settings password when one is set. The device never sends a key back. */
  async setKey(kind, bytes) {
    const c = await this.challenge();
    if (this.set && !this.key) throw new Error('Locked: unlock with the settings password first.');
    const path = `/feeder/keys/${kind}`;
    const keyHex = bytes ? toHex(maskKey(bytes, this.set ? this.key : null, c.nonceBytes)) : '';
    const headers = this.set ? { 'X-Feeder-Auth': authHeader(this.key, c.nonceBytes, authMessage(path, keyHex)) } : {};
    let r;
    try {
      r = await fetch(location.origin + path + (keyHex ? `?key=${keyHex}` : ''), { method: 'POST', headers });
    } catch (e) {
      throw new Error('The feeder could not be reached.');
    }
    if (r.status === 403 || r.status === 429) throw await this.refusal(r);
    if (r.status === 409) throw new Error('This build cannot change that key: it comes from the YAML.');
    if (!r.ok) throw new Error(`The feeder answered ${r.status}.`);
    await this.refresh();
  },

  /** Set a new password (or change it: needs to be unlocked), or clear it with ''. */
  async setPassword(password) {
    const c = await this.challenge();
    if (this.set && !this.key) throw new Error('Unlock with the current password first.');
    const path = '/feeder/auth/password';
    let query = '', saltHex = '', keyHex = '', newKey = null;
    if (password) {
      const salt = crypto.getRandomValues(new Uint8Array(16));
      newKey = pbkdf2Sha256(utf8(password), salt, c.iterations);
      saltHex = toHex(salt);
      keyHex = toHex(maskKey(newKey, this.set ? this.key : null, c.nonceBytes));
      query = `?salt=${saltHex}&key=${keyHex}`;
    }
    const headers = this.set ? { 'X-Feeder-Auth': authHeader(this.key, c.nonceBytes, `${path}\n${saltHex}${keyHex}`) } : {};
    const r = await fetch(location.origin + path + query, { method: 'POST', headers });
    if (!r.ok) throw await this.refusal(r);
    this.key = newKey;
    this.set = !!password;
    this._changed();
  },
};
