// Precision Feeder: SHA-256 and HMAC-SHA256 (for the BLE protocol and the settings password), and the BLE
// protocol's authentication helpers.
//
// Pure C++ with no ESPHome or ESP-IDF dependencies, so the exact code that runs on the device is tested on a host
// (tests/test_protocol.cpp: FIPS 180-4 and RFC 4231 vectors, plus golden vectors shared with the Python client).
// The amounts hashed are tiny (a few dozen bytes per command), so speed does not matter.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace precision_feeder {
namespace crypto {

// ---- SHA-256 (FIPS 180-4) --------------------------------------------------------------------------

class Sha256 {
 public:
  static constexpr size_t DIGEST_SIZE = 32;
  static constexpr size_t BLOCK_SIZE = 64;

  Sha256() { this->reset(); }

  void reset() {
    static constexpr uint32_t INIT[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(this->h_, INIT, sizeof(INIT));
    this->len_ = 0;
    this->used_ = 0;
  }

  void update(const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
      this->block_[this->used_++] = data[i];
      if (this->used_ == BLOCK_SIZE) {
        this->compress_();
        this->used_ = 0;
      }
    }
    this->len_ += static_cast<uint64_t>(n) * 8;
  }

  void finish(uint8_t out[DIGEST_SIZE]) {
    const uint64_t bits = this->len_;
    const uint8_t pad = 0x80;
    this->update(&pad, 1);
    const uint8_t zero = 0;
    while (this->used_ != BLOCK_SIZE - 8)
      this->update(&zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++)
      len_be[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    this->update(len_be, 8);
    for (int i = 0; i < 8; i++) {
      out[4 * i] = static_cast<uint8_t>(this->h_[i] >> 24);
      out[4 * i + 1] = static_cast<uint8_t>(this->h_[i] >> 16);
      out[4 * i + 2] = static_cast<uint8_t>(this->h_[i] >> 8);
      out[4 * i + 3] = static_cast<uint8_t>(this->h_[i]);
    }
  }

 protected:
  static uint32_t rotr_(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void compress_() {
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (static_cast<uint32_t>(this->block_[4 * i]) << 24) | (static_cast<uint32_t>(this->block_[4 * i + 1]) << 16) |
             (static_cast<uint32_t>(this->block_[4 * i + 2]) << 8) | static_cast<uint32_t>(this->block_[4 * i + 3]);
    }
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = rotr_(w[i - 15], 7) ^ rotr_(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr_(w[i - 2], 17) ^ rotr_(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = this->h_[0], b = this->h_[1], c = this->h_[2], d = this->h_[3];
    uint32_t e = this->h_[4], f = this->h_[5], g = this->h_[6], h = this->h_[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t s1 = rotr_(e, 6) ^ rotr_(e, 11) ^ rotr_(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + s1 + ch + K[i] + w[i];
      const uint32_t s0 = rotr_(a, 2) ^ rotr_(a, 13) ^ rotr_(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    this->h_[0] += a;
    this->h_[1] += b;
    this->h_[2] += c;
    this->h_[3] += d;
    this->h_[4] += e;
    this->h_[5] += f;
    this->h_[6] += g;
    this->h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t block_[BLOCK_SIZE];
  size_t used_;
  uint64_t len_;
};

inline void sha256(const uint8_t *data, size_t n, uint8_t out[Sha256::DIGEST_SIZE]) {
  Sha256 s;
  s.update(data, n);
  s.finish(out);
}

// ---- HMAC-SHA256 (RFC 2104) ----------------------------------------------------------------------------

/// HMAC-SHA256 over the concatenation of up to three parts (any may be empty), so callers need no buffer.
inline void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *a, size_t a_len, const uint8_t *b,
                        size_t b_len, const uint8_t *c, size_t c_len, uint8_t out[Sha256::DIGEST_SIZE]) {
  uint8_t k[Sha256::BLOCK_SIZE] = {};
  if (key_len > Sha256::BLOCK_SIZE) {
    sha256(key, key_len, k);
  } else {
    std::memcpy(k, key, key_len);
  }
  uint8_t pad[Sha256::BLOCK_SIZE];
  for (size_t i = 0; i < Sha256::BLOCK_SIZE; i++)
    pad[i] = k[i] ^ 0x36;
  Sha256 inner;
  inner.update(pad, sizeof(pad));
  inner.update(a, a_len);
  inner.update(b, b_len);
  inner.update(c, c_len);
  uint8_t inner_digest[Sha256::DIGEST_SIZE];
  inner.finish(inner_digest);
  for (size_t i = 0; i < Sha256::BLOCK_SIZE; i++)
    pad[i] = k[i] ^ 0x5c;
  Sha256 outer;
  outer.update(pad, sizeof(pad));
  outer.update(inner_digest, sizeof(inner_digest));
  outer.finish(out);
}

inline void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                        uint8_t out[Sha256::DIGEST_SIZE]) {
  hmac_sha256(key, key_len, msg, msg_len, nullptr, 0, nullptr, 0, out);
}

/// Compare without an early exit, so the time taken does not reveal where the first difference is.
inline bool equal_ct(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++)
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

// ---- Protocol 2 authentication (docs/PROTOCOL.md, "Authentication") ------------------------------------

constexpr size_t KEY_SIZE = 32;     // the shared secret, `ble_key` in secrets.yaml
constexpr size_t NONCE_SIZE = 16;   // the device's challenge, fresh for every connection and after a failed AUTH
constexpr size_t PROOF_SIZE = 16;   // what AUTH carries
constexpr size_t TAG_SIZE = 8;      // appended to every authenticated write
constexpr size_t SESSION_KEY_SIZE = 32;

/// AUTH proof: HMAC-SHA256(key, "PF2 auth" || nonce), first 16 bytes.
inline void auth_proof(const uint8_t key[KEY_SIZE], const uint8_t nonce[NONCE_SIZE], uint8_t out[PROOF_SIZE]) {
  static constexpr char LABEL[] = "PF2 auth";
  uint8_t mac[Sha256::DIGEST_SIZE];
  hmac_sha256(key, KEY_SIZE, reinterpret_cast<const uint8_t *>(LABEL), sizeof(LABEL) - 1, nonce, NONCE_SIZE, nullptr,
              0, mac);
  std::memcpy(out, mac, PROOF_SIZE);
}

/// Session key: HMAC-SHA256(key, "PF2 session" || nonce). Used only for the tags of this connection.
inline void session_key(const uint8_t key[KEY_SIZE], const uint8_t nonce[NONCE_SIZE], uint8_t out[SESSION_KEY_SIZE]) {
  static constexpr char LABEL[] = "PF2 session";
  hmac_sha256(key, KEY_SIZE, reinterpret_cast<const uint8_t *>(LABEL), sizeof(LABEL) - 1, nonce, NONCE_SIZE, nullptr,
              0, out);
}

/// Tag of an authenticated write: HMAC-SHA256(session key, channel || body), first 8 bytes. `channel` is the low
/// byte of the characteristic's UUID (0x03 Command, 0x06 Profile, 0x07 Config, 0x08 Calibration), so a write cannot
/// be moved to another characteristic; `body` is everything written before the tag (it starts with the sequence
/// number).
inline void write_tag(const uint8_t session[SESSION_KEY_SIZE], uint8_t channel, const uint8_t *body, size_t n,
                      uint8_t out[TAG_SIZE]) {
  uint8_t mac[Sha256::DIGEST_SIZE];
  hmac_sha256(session, SESSION_KEY_SIZE, &channel, 1, body, n, nullptr, 0, mac);
  std::memcpy(out, mac, TAG_SIZE);
}

/// A new authenticated write must use a sequence number after the last accepted one (serial-number arithmetic,
/// window 127, so it wraps from 255 to 1; 0 is never used). An exact repeat of a recent write is a retry and is
/// answered from the retry cache instead.
inline bool seq_is_newer(uint8_t last, uint8_t seq) {
  if (seq == 0)
    return false;
  // Sequence numbers run 1..255; map them to 0..254 so the arithmetic is mod 255.
  const int diff = ((static_cast<int>(seq) - 1) - (static_cast<int>(last == 0 ? 255 : last) - 1) + 255) % 255;
  return diff >= 1 && diff <= 127;
}

}  // namespace crypto
}  // namespace precision_feeder
}  // namespace esphome
