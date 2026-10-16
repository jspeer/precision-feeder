// Precision Feeder core: the settings password (DESIGN.md section 10.1), the BLE key saved on the device and the
// two BLE permissions (section 10).
//
// The device stores a salt and the key the browser derived from the password (PBKDF2-HMAC-SHA256), never the
// password. A protected request carries HMAC-SHA256(key, nonce || message) for a one-time challenge, so neither the
// password nor the key crosses plain HTTP, and a recorded answer cannot be replayed.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "feeder_internal.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace precision_feeder {

static const char NEW_KEY_LABEL[] = "new key";

void FeederCore::password_challenge(uint8_t nonce[AUTH_NONCE_SIZE]) {
  this->hal_random(this->auth_nonce_, AUTH_NONCE_SIZE);
  this->auth_nonce_live_ = true;
  this->auth_nonce_ms_ = this->hal_millis();
  std::memcpy(nonce, this->auth_nonce_, AUTH_NONCE_SIZE);
}

uint32_t FeederCore::password_wait_ms() {
  if (this->auth_wait_ms_ == 0)
    return 0;
  const uint32_t passed = this->hal_millis() - this->auth_wait_from_ms_;
  if (passed >= this->auth_wait_ms_) {
    this->auth_wait_ms_ = 0;
    return 0;
  }
  return this->auth_wait_ms_ - passed;
}

AuthResult FeederCore::password_check(const uint8_t nonce[AUTH_NONCE_SIZE], const uint8_t tag[AUTH_TAG_SIZE],
                                      const std::string &message, uint32_t &wait_ms) {
  wait_ms = this->password_wait_ms();
  if (!this->password_set())
    return AuthResult::OK;
  if (wait_ms != 0)
    return AuthResult::WAIT;  // the challenge stays: it may still be answered once the wait is over
  const bool live = this->auth_nonce_live_ && this->hal_millis() - this->auth_nonce_ms_ < AUTH_NONCE_TTL_MS &&
                    crypto::equal_ct(nonce, this->auth_nonce_, AUTH_NONCE_SIZE);
  this->auth_nonce_live_ = false;  // one answer per challenge, right or wrong
  if (!live)
    return AuthResult::NO_CHALLENGE;
  bool ok = false;
  if (this->dev_.password_scheme == PASSWORD_SCHEME_PBKDF2) {
    uint8_t expect[AUTH_TAG_SIZE];
    crypto::hmac_sha256(this->dev_.password_hash, PASSWORD_HASH_SIZE, nonce, AUTH_NONCE_SIZE,
                        reinterpret_cast<const uint8_t *>(message.data()), message.size(), nullptr, 0, expect);
    ok = crypto::equal_ct(expect, tag, AUTH_TAG_SIZE);
  }
  // An unknown scheme (saved by a newer firmware) refuses everything: only BOOT can clear it.
  if (ok) {
    this->auth_failures_ = 0;
    return AuthResult::OK;
  }
  this->auth_failures_ = static_cast<uint8_t>(std::min<int>(this->auth_failures_ + 1, 16));
  uint32_t wait = AUTH_WAIT_MIN_MS;
  for (uint8_t i = 1; i < this->auth_failures_ && wait < AUTH_WAIT_MAX_MS; i++)
    wait *= 2;
  this->auth_wait_ms_ = std::min(wait, AUTH_WAIT_MAX_MS);
  this->auth_wait_from_ms_ = this->hal_millis();
  wait_ms = this->auth_wait_ms_;
  return AuthResult::REFUSED;
}

void FeederCore::password_unmask(const uint8_t nonce[AUTH_NONCE_SIZE], const uint8_t masked[PASSWORD_HASH_SIZE],
                                 uint8_t key[PASSWORD_HASH_SIZE]) const {
  uint8_t mask[PASSWORD_HASH_SIZE]{};
  if (this->password_set())
    crypto::hmac_sha256(this->dev_.password_hash, PASSWORD_HASH_SIZE, nonce, AUTH_NONCE_SIZE,
                        reinterpret_cast<const uint8_t *>(NEW_KEY_LABEL), sizeof(NEW_KEY_LABEL) - 1, nullptr, 0,
                        mask);
  for (unsigned i = 0; i < PASSWORD_HASH_SIZE; i++)
    key[i] = masked[i] ^ mask[i];
}

void FeederCore::set_password(const uint8_t salt[PASSWORD_SALT_SIZE], const uint8_t key[PASSWORD_HASH_SIZE]) {
  const bool had = this->password_set();
  this->dev_.password_scheme = PASSWORD_SCHEME_PBKDF2;
  std::memcpy(this->dev_.password_salt, salt, PASSWORD_SALT_SIZE);
  std::memcpy(this->dev_.password_hash, key, PASSWORD_HASH_SIZE);
  this->auth_nonce_live_ = false;
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kInfo, had ? "Settings password changed" : "Settings password set");
}

void FeederCore::clear_password(const char *why) {
  this->dev_.password_scheme = PASSWORD_SCHEME_NONE;
  std::memset(this->dev_.password_salt, 0, PASSWORD_SALT_SIZE);
  std::memset(this->dev_.password_hash, 0, PASSWORD_HASH_SIZE);
  this->auth_nonce_live_ = false;
  this->auth_failures_ = 0;
  this->auth_wait_ms_ = 0;
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kWarn, "Settings password cleared (%s)", why);
}

// ---- BLE key and permissions ----

static bool all_zero(const uint8_t *key) {
  uint8_t any = 0;
  for (unsigned i = 0; i < DEVICE_KEY_SIZE; i++)
    any |= key[i];
  return any == 0;
}

void FeederCore::set_build_ble_key(const uint8_t key[DEVICE_KEY_SIZE]) {
  std::memcpy(this->build_ble_key_, key, DEVICE_KEY_SIZE);
  this->ble_built_ = true;
  this->ble_key_serial_++;
}

KeySource FeederCore::ble_key_source() const {
  if (!all_zero(this->dev_.ble_key))
    return KeySource::DEVICE;
  return all_zero(this->build_ble_key_) ? KeySource::NONE : KeySource::BUILD;
}

bool FeederCore::ble_key(uint8_t out[DEVICE_KEY_SIZE]) const {
  switch (this->ble_key_source()) {
    case KeySource::DEVICE:
      std::memcpy(out, this->dev_.ble_key, DEVICE_KEY_SIZE);
      return true;
    case KeySource::BUILD:
      std::memcpy(out, this->build_ble_key_, DEVICE_KEY_SIZE);
      return true;
    default:
      std::memset(out, 0, DEVICE_KEY_SIZE);
      return false;
  }
}

Result FeederCore::set_ble_key(const uint8_t key[DEVICE_KEY_SIZE]) {
  if (!this->ble_built_)
    return Result::UNSUPPORTED;
  if (all_zero(key))
    return Result::INVALID_ARG;
  if (std::memcmp(key, this->dev_.ble_key, DEVICE_KEY_SIZE) == 0)
    return Result::OK;
  std::memcpy(this->dev_.ble_key, key, DEVICE_KEY_SIZE);
  this->ble_key_serial_++;
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kWarn, "BLE key changed on the device: clients need the new key");
  return Result::OK;
}

void FeederCore::set_device_flag_(uint8_t flag, bool on, const char *what) {
  if (((this->dev_.flags & flag) != 0) == on)
    return;
  this->dev_.flags = static_cast<uint8_t>(on ? this->dev_.flags | flag : this->dev_.flags & ~flag);
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kInfo, "BLE may %s change the %s", on ? "now" : "no longer", what);
}

void FeederCore::boot_button(bool pressed) {
  if (pressed && !this->boot_pressed_) {
    this->boot_pressed_ms_ = this->hal_millis();
    this->boot_handled_ = false;
  }
  this->boot_pressed_ = pressed;
}

}  // namespace precision_feeder
}  // namespace esphome
