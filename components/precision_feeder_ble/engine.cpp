// Precision Feeder BLE protocol engine (see engine.h).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace precision_feeder {

// The protocol's layout constants must agree with the feeder. POSITIONS is the servo's slots, which the calibration
// channel is indexed by; Info reports the mechanism's positions (capacity + 1), which is the same for the servo.
static_assert(proto::POSITIONS == NUM_POSITIONS, "protocol and feeder disagree on the number of positions");
static_assert(proto::PROFILES == NUM_PROFILES, "protocol and feeder disagree on the number of profiles");
static_assert(proto::EVENTS_PER_PROFILE == EVENTS_PER_PROFILE, "protocol and feeder disagree on events per profile");

static const char *result_text(uint8_t result) {
  return result == proto::RES_NOT_AUTHORIZED ? "not authorized" : FeederCore::result_name(static_cast<Result>(result));
}

void BleEngine::log_(LogLevel level, const char *fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  this->transport_.log(level, buf);
}

// ---------------------------------------------------------------------------------------------
// Connection life cycle
// ---------------------------------------------------------------------------------------------

uint16_t BleEngine::capabilities() const {
  uint16_t caps = proto::CAP_SOLAR | proto::CAP_PROFILE_EDIT | proto::CAP_HOLD_FOREVER | proto::CAP_AUTH;
  // Calibration offsets and the pulse range are the servo's; another mechanism does not offer them over BLE.
  if (this->feeder_.mechanism().calibration_points() > 0)
    caps |= proto::CAP_CALIBRATION;
  if (this->feeder_.is_servo())
    caps |= proto::CAP_SERVO_PULSE;
  if (this->time_policy_ != proto::TimePolicy::OFF)
    caps |= proto::CAP_TIME_PUSH;
  return caps;
}

void BleEngine::start() {
  // Info: protocol version, capability bits, positions, profiles, events per profile.
  uint8_t info[6];
  info[0] = proto::PROTOCOL_VERSION;
  proto::put_u16(info + 1, this->capabilities());
  info[3] = static_cast<uint8_t>(this->feeder_.capacity() + 1);  // positions 0 to capacity
  info[4] = proto::PROFILES;
  info[5] = proto::EVENTS_PER_PROFILE;
  this->transport_.set_value(proto::CHANNEL_INFO, info, sizeof(info), false);
  // Nothing but Info and Auth is readable until a client has authenticated.
  this->clear_reads_();
  this->new_nonce_();
  this->key_serial_ = this->feeder_.ble_key_serial();
}

void BleEngine::on_connect() {
  this->connected_ = true;
  this->replay_.clear();
  this->clear_reads_();
  this->new_nonce_();  // each connection gets a new challenge and starts unauthenticated
  this->stats_.connections++;
  this->connected_ms_ = this->now_();
  this->mtu_ = DEFAULT_MTU;
  this->add_activity_(ActivityKind::CONNECT);
  this->log_(LogLevel::kInfo, "BLE client connected");
}

void BleEngine::on_disconnect() {
  this->connected_ = false;
  this->session_.end();
  this->replay_.clear();
  this->clear_reads_();
  this->have_peer_ = false;
  this->add_activity_(ActivityKind::DISCONNECT);
  this->log_(LogLevel::kInfo, "BLE client disconnected");
}

void BleEngine::new_nonce_() {
  uint8_t nonce[crypto::NONCE_SIZE];
  this->transport_.random(nonce, sizeof(nonce));
  this->session_.start(nonce);
  this->transport_.set_value(proto::CHANNEL_AUTH, nonce, sizeof(nonce), false);
}

void BleEngine::clear_reads_() {
  static constexpr uint8_t ZEROS[proto::STATUS_SIZE] = {};
  this->transport_.set_value(proto::CHANNEL_STATUS, ZEROS, proto::STATUS_SIZE, false);
  this->transport_.set_value(proto::CHANNEL_PROFILE, ZEROS, proto::PROFILE_EVENT_SIZE, false);
  this->transport_.set_value(proto::CHANNEL_CONFIG, ZEROS, proto::CONFIG_SET_SIZE, false);
  this->transport_.set_value(proto::CHANNEL_CALIBRATION, ZEROS, proto::CALIBRATION_SET_SIZE, false);
  this->transport_.set_value(proto::CHANNEL_RESPONSE, ZEROS, proto::RESPONSE_SIZE, false);
  std::memset(this->last_status_, 0, sizeof(this->last_status_));
}

void BleEngine::respond_(uint8_t seq, uint8_t opcode, uint8_t result, uint8_t detail) {
  const uint8_t response[proto::RESPONSE_SIZE] = {seq, opcode, result, detail};
  // Also left readable, in case the notification is missed.
  this->notify_(proto::CHANNEL_RESPONSE, response, sizeof(response));
}

// ---------------------------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------------------------

void BleEngine::on_write(uint8_t channel, const uint8_t *data, size_t n) {
  if (n == 0)
    return;
  this->stats_.writes++;
  this->stats_.write_bytes += static_cast<uint32_t>(n);
  this->last_activity_ms_ = this->now_();
  // Copy first: answering sets characteristic values, and on the device `data` points into one of them.
  uint8_t write[proto::ReplayCache::MAX_COMMAND];
  n = std::min(n, proto::ReplayCache::MAX_COMMAND);
  std::memcpy(write, data, n);
  if (channel == proto::CHANNEL_COMMAND)
    this->on_command_(write, n);
  else if (channel == proto::CHANNEL_PROFILE || channel == proto::CHANNEL_CONFIG || channel == proto::CHANNEL_CALIBRATION)
    this->on_record_(channel, write, n);
}

void BleEngine::handle_auth_(uint8_t seq, const uint8_t *proof, size_t n) {
  uint8_t detail = proto::AUTH_DETAIL_NO_KEY;
  const bool was_authenticated = this->session_.authenticated();
  uint8_t result = proto::RES_NOT_AUTHORIZED;
  uint8_t key[crypto::KEY_SIZE];
  // No key at all: refused outright. Never fall back to an all-zero key, which anyone could compute with.
  if (this->feeder_.ble_key(key)) {
    this->session_.set_key(key);
    result = this->session_.authenticate(seq, proof, n, this->now_(), detail);
  }
  std::memset(key, 0, sizeof(key));
  if (result == proto::RES_OK) {
    // The retry cache is NOT cleared here: connect and disconnect already start every session with an empty one,
    // and clearing it on a repeated AUTH would let a retried write execute a second time.
    this->stats_.auth_ok++;
    if (!was_authenticated) {
      this->log_(LogLevel::kInfo, "BLE client authenticated");
      this->force_status_ = true;  // send fresh status straight away
    }
  } else {
    this->stats_.auth_failures++;
    this->log_(LogLevel::kWarn, "BLE authentication refused (detail %u)", detail);
    if (detail == proto::AUTH_DETAIL_BAD_PROOF)
      this->new_nonce_();  // a guess is never tried against the same challenge twice
    if (!this->session_.authenticated())
      this->clear_reads_();  // no longer authenticated: nothing published for the session stays readable (2.1.2)
  }
  this->add_activity_(ActivityKind::AUTH, proto::OP_AUTH, seq, result, detail);
  this->respond_(seq, proto::OP_AUTH, result, detail);
}

void BleEngine::on_command_(const uint8_t *command, size_t n) {
  const uint8_t seq = command[0];
  if (n < 2) {
    this->count_answer_(proto::RES_INVALID_ARG);
    this->add_activity_(ActivityKind::COMMAND, 0, seq, proto::RES_INVALID_ARG, 0);
    this->respond_(seq, 0, proto::RES_INVALID_ARG, 0);
    return;
  }
  const uint8_t opcode = command[1];
  if (opcode == proto::OP_AUTH) {
    this->handle_auth_(seq, command + 2, n - 2);
    return;
  }
  const uint64_t now = this->now_();
  uint8_t response[proto::RESPONSE_SIZE];
  if (this->replay_.find(proto::CHANNEL_COMMAND, command, n, now, response)) {
    // A retry of a command already answered: repeat the answer, do not act again.
    this->log_(LogLevel::kInfo, "Command seq %u repeated, replaying the earlier answer", seq);
    this->add_activity_(ActivityKind::REPLAY, response[1], response[0], response[2], response[3]);
    this->respond_(response[0], response[1], response[2], response[3]);
    return;
  }
  uint8_t detail = 0;
  const uint8_t check = this->session_.check(proto::CHANNEL_COMMAND, command, n, detail);
  if (check != proto::RES_OK) {
    this->log_(LogLevel::kWarn, "Command %s seq %u refused: not authorized (detail %u)", opcode_name(opcode).c_str(),
               seq, detail);
    this->stats_.refused++;
    this->add_activity_(ActivityKind::COMMAND, opcode, seq, check, detail);
    this->respond_(seq, opcode, check, detail);
    return;
  }
  this->session_.accept(seq);

  Answer a{proto::RES_INVALID_ARG, 0};
  if (n >= 2 + crypto::TAG_SIZE)  // otherwise there is no room for an opcode: "opcode" is the tag's first byte
    a = this->execute_(opcode, command + 2, n - 2 - crypto::TAG_SIZE);
  const uint8_t answer[proto::RESPONSE_SIZE] = {seq, opcode, a.result, a.detail};
  this->replay_.store(proto::CHANNEL_COMMAND, command, n, answer, now);
  this->log_(LogLevel::kInfo, "Command %s seq %u -> %s", opcode_name(opcode).c_str(), seq, result_text(a.result));
  this->count_answer_(a.result);
  this->add_activity_(ActivityKind::COMMAND, opcode, seq, a.result, a.detail);
  this->respond_(seq, opcode, a.result, a.detail);
}

BleEngine::Answer BleEngine::execute_(uint8_t opcode, const uint8_t *args, size_t n) {
  using namespace proto;
  FeederCore &f = this->feeder_;
  auto res = [](Result r) { return Answer{static_cast<uint8_t>(r), 0}; };
  const Answer invalid{RES_INVALID_ARG, 0};
  const Answer off{RES_NOT_AUTHORIZED, AUTH_DETAIL_SETTING_OFF};  // "BLE can change calibration" is off

  switch (opcode) {
    case OP_FEED: {
      if (n != 1)
        return invalid;
      uint8_t accepted = 0;
      const Result r = f.request_feed(args[0], Source::BLE, &accepted);
      return {static_cast<uint8_t>(r), r == Result::OK ? accepted : f.remaining()};
    }
    case OP_TRAY_RELOAD:
      if (n != 0)
        return invalid;
      if (f.calibration_mode())
        return res(Result::CALIBRATING);
      f.tray_reload();
      return {RES_OK, f.remaining()};

    case OP_SET_PROFILE:
      if (n != 1 || f.set_active_profile(args[0]) != Result::OK)
        return invalid;
      return {RES_OK, args[0]};

    case OP_HOLD: {
      if (n != 4)
        return invalid;
      const uint32_t minutes = get_u32(args);
      if (minutes == 0) {
        f.cancel_hold();
        return {RES_OK, 0};
      }
      if (minutes == 0xFFFFFFFFu) {
        f.set_hold_indefinite();
        return {RES_OK, 0};
      }
      return res(f.set_hold_minutes(minutes));
    }
    case OP_STOP:
      if (n != 0)
        return invalid;
      f.stop();
      return {RES_OK, 0};

    case OP_SET_TIME: {
      if (n != 6)
        return invalid;
      const uint32_t epoch = get_u32(args);
      // args[4..5] is the sender's UTC offset in minutes. The device's timezone is fixed at build time
      // (DESIGN.md section 6), so it is accepted for forward compatibility and not used.
      const uint8_t decision =
          decide_ble_time(this->time_policy_, epoch, f.network_time_age_ms(), this->network_stale_ms_);
      if (decision != RES_OK) {
        this->log_(LogLevel::kInfo, "Clock from BLE not applied: %s", result_text(decision));
        return {decision, 0};
      }
      f.set_system_time(epoch);
      return {RES_OK, 0};
    }
    case OP_SET_LOCATION: {
      if (n != 8)
        return invalid;
      return res(f.set_location_e5(get_i32(args), get_i32(args + 4)));
    }
    case OP_CALIBRATE:
      if (!f.ble_may_change_calibration())
        return off;
      if (n != 1 || args[0] > 1)
        return invalid;
      f.set_calibration_mode(args[0] == 1);
      return {RES_OK, 0};

    case OP_JOG: {
      if (!f.ble_may_change_calibration())
        return off;
      if (n != 3)
        return invalid;
      const int16_t delta = get_i16(args + 1);
      if (delta < 2 * CAL_OFFSET_LO || delta > 2 * CAL_OFFSET_HI)
        return invalid;
      return res(f.jog_calibration(args[0], delta));
    }
    case OP_SAVE_CALIBRATION:
      if (!f.ble_may_change_calibration())
        return off;
      if (n != 0)
        return invalid;
      f.save_calibration();
      return {RES_OK, 0};

    case OP_SET_POSITION:
      if (n != 1 || args[0] > f.capacity())
        return invalid;
      if (f.calibration_mode())
        return res(Result::CALIBRATING);
      f.declare_position(args[0]);
      return {RES_OK, f.remaining()};

    case OP_SET_TIMEZONE:  // reserved: the timezone is set at build time
    default:
      return {RES_UNSUPPORTED, 0};
  }
}

void BleEngine::on_record_(uint8_t channel, const uint8_t *write, size_t n) {
  const uint8_t opcode = channel == proto::CHANNEL_PROFILE  ? proto::OP_PROFILE_WRITE
                         : channel == proto::CHANNEL_CONFIG ? proto::OP_CONFIG_WRITE
                                                            : proto::OP_CALIBRATION_WRITE;
  const uint8_t seq = write[0];
  const uint64_t now = this->now_();
  uint8_t response[proto::RESPONSE_SIZE];
  if (this->replay_.find(channel, write, n, now, response)) {
    this->add_activity_(ActivityKind::REPLAY, response[1], response[0], response[2], response[3]);
    this->respond_(response[0], response[1], response[2], response[3]);  // a retry: same answer, not applied again
    return;
  }
  uint8_t detail = 0;
  const uint8_t check = this->session_.check(channel, write, n, detail);
  if (check != proto::RES_OK) {
    this->log_(LogLevel::kWarn, "%s seq %u refused: not authorized (detail %u)", opcode_name(opcode).c_str(), seq,
               detail);
    this->stats_.refused++;
    this->add_activity_(ActivityKind::RECORD, opcode, seq, check, detail);
    this->respond_(seq, opcode, check, detail);
    return;
  }
  this->session_.accept(seq);

  // The record sits between the sequence number and the tag.
  const uint8_t *record = write + 1;
  const size_t len = n - 1 - crypto::TAG_SIZE;
  Answer a{proto::RES_INVALID_ARG, 0};
  if (len > 0) {
    if (channel == proto::CHANNEL_PROFILE)
      a = this->handle_profile_(record, len);
    else if (channel == proto::CHANNEL_CONFIG)
      a = this->handle_config_(record, len);
    else
      a = this->handle_calibration_(record, len);
  }
  const uint8_t answer[proto::RESPONSE_SIZE] = {seq, opcode, a.result, a.detail};
  this->replay_.store(channel, write, n, answer, now);
  this->count_answer_(a.result);
  this->add_activity_(ActivityKind::RECORD, opcode, seq, a.result, a.detail);
  this->respond_(seq, opcode, a.result, a.detail);
}

// ---------------------------------------------------------------------------------------------
// Profile characteristic
// ---------------------------------------------------------------------------------------------

void BleEngine::set_profile_read_(uint8_t profile, uint8_t event) {
  const Profile &p = this->feeder_.cfg.profiles[profile];
  if (event == proto::QUERY_HEADER) {
    proto::ProfileHeaderRec r;
    r.profile = profile;
    r.every_n = p.every_n;
    r.fast_mask = p.fast_days;
    r.anchor_day = static_cast<uint16_t>(p.anchor_day);
    uint8_t out[proto::PROFILE_HEADER_SIZE];
    proto::encode_profile_header(r, out);
    this->transport_.set_value(proto::CHANNEL_PROFILE, out, sizeof(out), false);
  } else {
    const Event &e = p.ev[event];
    proto::ProfileEventRec r;
    r.profile = profile;
    r.event = event;
    r.enabled = e.enabled;
    r.anchor = e.anchor;
    r.offset_min = e.offset_min;
    r.portions = e.portions;
    r.days = e.days;
    uint8_t out[proto::PROFILE_EVENT_SIZE];
    proto::encode_profile_event(r, out);
    this->transport_.set_value(proto::CHANNEL_PROFILE, out, sizeof(out), false);
  }
}

BleEngine::Answer BleEngine::handle_profile_(const uint8_t *d, size_t n) {
  using namespace proto;
  FeederCore &f = this->feeder_;
  const uint8_t profile_hint = n > 1 ? d[1] : 0;
  switch (d[0]) {
    case PROFILE_TAG_HEADER: {
      ProfileHeaderRec r;
      if (!decode_profile_header(d, n, r) || !valid_profile_header(r))
        return {RES_INVALID_ARG, profile_hint};
      Profile &p = f.cfg.profiles[r.profile];
      p.every_n = r.every_n;
      p.fast_days = r.fast_mask;
      p.anchor_day = r.anchor_day;
      if (r.anchor_day == 0)
        f.restart_cadence_today(r.profile);  // 0 means "start the cycle today"
      f.cfg_changed();
      this->set_profile_read_(r.profile, QUERY_HEADER);
      return {RES_OK, r.profile};
    }
    case PROFILE_TAG_EVENT: {
      ProfileEventRec r;
      if (!decode_profile_event(d, n, r) || !valid_profile_event(r, f.capacity()))
        return {RES_INVALID_ARG, profile_hint};
      Event &e = f.cfg.profiles[r.profile].ev[r.event];
      e.enabled = r.enabled;
      e.anchor = r.anchor;
      e.offset_min = r.offset_min;
      e.portions = r.portions;
      e.days = r.days;
      f.cfg_changed();
      this->set_profile_read_(r.profile, r.event);
      return {RES_OK, r.profile};
    }
    case PROFILE_TAG_QUERY:
      if (n != PROFILE_QUERY_SIZE || d[1] >= PROFILES || (d[2] != QUERY_HEADER && d[2] >= proto::EVENTS_PER_PROFILE))
        return {RES_INVALID_ARG, profile_hint};
      this->set_profile_read_(d[1], d[2]);
      return {RES_OK, d[1]};
    default:
      return {RES_INVALID_ARG, profile_hint};
  }
}

// ---------------------------------------------------------------------------------------------
// Config characteristic
// ---------------------------------------------------------------------------------------------

bool BleEngine::get_config_(uint8_t key, int32_t &value) const {
  using namespace proto;
  const Settings &c = this->feeder_.cfg;
  switch (key) {
    case CFG_MIN_GAP_S:
      value = c.min_gap_s;
      return true;
    case CFG_PORTION_GAP_S:
      value = c.portion_gap_s;
      return true;
    case CFG_RELOAD_PAUSE_MIN:
      value = c.reload_pause_min;
      return true;
    case CFG_REFILL_INTERVAL_DAYS:
      value = c.refill_interval_days;
      return true;
    case CFG_DAILY_CAP:
      value = c.daily_cap;
      return true;
    case CFG_MAX_PORTIONS_CMD:
      value = c.max_portions_cmd;
      return true;
    case CFG_SCHEDULER_ENABLED:
      value = c.scheduler_enabled;
      return true;
    case CFG_LATITUDE_E5:
      value = static_cast<int32_t>(std::lround(c.latitude * 100000.0f));
      return true;
    case CFG_LONGITUDE_E5:
      value = static_cast<int32_t>(std::lround(c.longitude * 100000.0f));
      return true;
    case CFG_MIN_PULSE_US:
      value = this->feeder_.min_pulse_us();
      return true;
    case CFG_MAX_PULSE_US:
      value = this->feeder_.max_pulse_us();
      return true;
    default:
      return false;
  }
}

void BleEngine::set_config_read_(uint8_t key) {
  int32_t value = 0;
  if (!this->get_config_(key, value))
    return;
  uint8_t out[proto::CONFIG_SET_SIZE];
  out[0] = key;
  proto::put_i32(out + 1, value);
  this->transport_.set_value(proto::CHANNEL_CONFIG, out, sizeof(out), false);
}

BleEngine::Answer BleEngine::handle_config_(const uint8_t *d, size_t n) {
  using namespace proto;
  FeederCore &f = this->feeder_;
  if (n == QUERY_SIZE && (d[0] & QUERY_FLAG)) {
    const uint8_t key = d[0] & ~QUERY_FLAG;
    if (!config_key_known(key))
      return {RES_INVALID_ARG, key};
    this->set_config_read_(key);
    return {RES_OK, key};
  }
  const uint8_t key = d[0];
  if (n != CONFIG_SET_SIZE || !config_key_known(key))
    return {RES_INVALID_ARG, key};
  // The owner's two BLE permissions (DESIGN.md section 10). Queries above are never refused.
  const bool guard = key == CFG_MIN_GAP_S || key == CFG_PORTION_GAP_S || key == CFG_DAILY_CAP ||
                     key == CFG_MAX_PORTIONS_CMD;
  const bool pulse = key == CFG_MIN_PULSE_US || key == CFG_MAX_PULSE_US;
  if ((guard && !f.ble_may_change_guard()) || (pulse && !f.ble_may_change_calibration()))
    return {RES_NOT_AUTHORIZED, AUTH_DETAIL_SETTING_OFF};
  const int32_t v = get_i32(d + 1);
  Result r = Result::INVALID_ARG;
  switch (key) {  // the setters check the value against the limits in feeder_core.h
    case CFG_MIN_GAP_S:
      r = f.set_min_gap_s(v);
      break;
    case CFG_PORTION_GAP_S:
      r = f.set_portion_gap_s(v);
      break;
    case CFG_RELOAD_PAUSE_MIN:
      r = f.set_reload_pause_min(v);
      break;
    case CFG_REFILL_INTERVAL_DAYS:
      r = f.set_refill_interval_days(v);
      break;
    case CFG_DAILY_CAP:
      r = f.set_daily_cap(v);
      break;
    case CFG_MAX_PORTIONS_CMD:
      r = f.set_max_portions_cmd(v);
      break;
    case CFG_SCHEDULER_ENABLED:
      if (v == 0 || v == 1)
        r = f.set_scheduler_enabled(v == 1);
      break;
    case CFG_LATITUDE_E5:
      r = f.set_location_e5(v, static_cast<int32_t>(std::lround(f.cfg.longitude * 100000.0f)));
      break;
    case CFG_LONGITUDE_E5:
      r = f.set_location_e5(static_cast<int32_t>(std::lround(f.cfg.latitude * 100000.0f)), v);
      break;
    case CFG_MIN_PULSE_US:
    case CFG_MAX_PULSE_US: {
      // The pair must stay at least 500 us apart; the core checks it and moves the servo in calibration mode.
      const int32_t lo = key == CFG_MIN_PULSE_US ? v : f.min_pulse_us();
      const int32_t hi = key == CFG_MAX_PULSE_US ? v : f.max_pulse_us();
      if (lo >= 0 && lo <= 0xFFFF && hi >= 0 && hi <= 0xFFFF)
        r = f.set_pulse_range(static_cast<uint16_t>(lo), static_cast<uint16_t>(hi));
      break;
    }
  }
  if (r == Result::UNSUPPORTED)  // the pulse range, on a mechanism that is not the servo
    return {RES_UNSUPPORTED, key};
  if (r != Result::OK)
    return {RES_INVALID_ARG, key};
  this->set_config_read_(key);
  return {RES_OK, key};
}

// ---------------------------------------------------------------------------------------------
// Calibration characteristic
// ---------------------------------------------------------------------------------------------

void BleEngine::set_calibration_read_(uint8_t index) {
  if (index >= proto::POSITIONS || index >= this->feeder_.mechanism().calibration_points())
    return;
  uint8_t out[proto::CALIBRATION_SET_SIZE];
  out[0] = index;
  proto::put_i16(out + 1, this->feeder_.calib.offsets[index]);
  this->transport_.set_value(proto::CHANNEL_CALIBRATION, out, sizeof(out), false);
}

BleEngine::Answer BleEngine::handle_calibration_(const uint8_t *d, size_t n) {
  using namespace proto;
  if (n == QUERY_SIZE && (d[0] & QUERY_FLAG)) {
    const uint8_t index = d[0] & ~QUERY_FLAG;
    if (index >= POSITIONS || index >= this->feeder_.mechanism().calibration_points())
      return {RES_INVALID_ARG, index};
    this->set_calibration_read_(index);
    return {RES_OK, index};
  }
  if (n != CALIBRATION_SET_SIZE)
    return {RES_INVALID_ARG, d[0]};
  if (!this->feeder_.ble_may_change_calibration())
    return {RES_NOT_AUTHORIZED, AUTH_DETAIL_SETTING_OFF};
  const uint8_t index = d[0];
  const int16_t tenths = get_i16(d + 1);
  if (index >= POSITIONS || index >= this->feeder_.mechanism().calibration_points() || tenths < CAL_OFFSET_LO ||
      tenths > CAL_OFFSET_HI)
    return {RES_INVALID_ARG, index};
  this->feeder_.set_calibration_offset_at(index, tenths);
  this->set_calibration_read_(index);
  return {RES_OK, index};
}

// ---------------------------------------------------------------------------------------------
// Status and event log
// ---------------------------------------------------------------------------------------------

void BleEngine::loop(uint32_t now_ms) {
  if (this->feeder_.ble_key_serial() != this->key_serial_) {
    // The key changed: a session proven with the old one ends. The client reads the new nonce and authenticates
    // again, with the new key.
    this->key_serial_ = this->feeder_.ble_key_serial();
    if (this->session_.authenticated()) {
      this->log_(LogLevel::kInfo, "BLE key changed: the client must authenticate again");
      this->replay_.clear();
      this->clear_reads_();
      this->new_nonce_();
    }
  }
  if (!this->authorized())
    return;
  if (!this->force_status_ && now_ms - this->last_status_ms_ < STATUS_PERIOD_MS)
    return;
  this->last_status_ms_ = now_ms;
  this->publish_status_(this->force_status_);
  this->force_status_ = false;
}

proto::StatusSnapshot BleEngine::snapshot_() {
  using namespace proto;
  FeederCore &f = this->feeder_;
  StatusSnapshot s;
  s.tray_position = f.position();
  s.remaining = f.remaining();
  s.tray_state = static_cast<uint8_t>(f.tray_state());
  s.active_profile = f.cfg.active_profile;

  uint16_t flags = 0;
  if (f.time_valid())
    flags |= FLAG_TIME_VALID;
  if (f.hold_active())
    flags |= FLAG_HOLD;
  if (f.calibration_mode())
    flags |= FLAG_CALIBRATING;
  if (f.tray_state() == TrayState::MOVING)
    flags |= FLAG_MOVING;
  if (f.cfg.scheduler_enabled)
    flags |= FLAG_SCHEDULER_ENABLED;
  if (f.reload_pause_active())
    flags |= FLAG_RELOAD_PAUSE;
  if (f.solar_fault())
    flags |= FLAG_SOLAR_FAULT;
  if (f.schedule_oversubscribed())
    flags |= FLAG_OVERSUBSCRIBED;
  if (f.time_stale())
    flags |= FLAG_TIME_STALE;
  if (f.position_unknown())
    flags |= FLAG_POSITION_UNKNOWN;
  if (f.remaining() == 0)
    flags |= FLAG_TRAY_EMPTY;
  if (f.schedule_over_cap())
    flags |= FLAG_OVER_CAP;
  if (f.needs_food())
    flags |= FLAG_NEEDS_FOOD;
  s.flags = flags;

  s.last_feed_epoch = f.last_feed_epoch();
  s.last_feed_source = f.last_feed_source();
  s.feeds_today = static_cast<uint8_t>(std::min<uint16_t>(f.feeds_today(), 255));
  s.next_event_epoch = f.next_event_epoch();
  s.runway_days = f.runway_days();
  s.time_source = static_cast<uint8_t>(f.time_source());
  s.time_age_min = f.time_age_min();
  return s;
}

void BleEngine::publish_status_(bool force) {
  uint8_t buf[proto::STATUS_SIZE];
  proto::encode_status(this->snapshot_(), buf);
  if (!force && std::memcmp(buf, this->last_status_, sizeof(buf)) == 0)
    return;
  std::memcpy(this->last_status_, buf, sizeof(buf));
  this->notify_(proto::CHANNEL_STATUS, buf, sizeof(buf));
}

void BleEngine::on_feed(const FeedRecord &record) {
  if (!this->authorized())
    return;
  uint8_t buf[proto::EVENT_LOG_SIZE];
  proto::encode_event_log(record.epoch, record.source, record.portions, record.result, buf);
  this->notify_(proto::CHANNEL_EVENT_LOG, buf, sizeof(buf));
  this->force_status_ = true;  // the tray moved on or a refusal changed "last result": send fresh status
}

// ---------------------------------------------------------------------------------------------
// Diagnostics: counters, the session and the activity log (read-only for the protocol)
// ---------------------------------------------------------------------------------------------

void BleEngine::notify_(uint8_t channel, const uint8_t *data, size_t n) {
  this->transport_.set_value(channel, data, n, true);
  this->stats_.notifications++;
  this->stats_.notify_bytes += static_cast<uint32_t>(n);
}

void BleEngine::count_answer_(uint8_t result) {
  if (result == proto::RES_INVALID_ARG || result == proto::RES_UNSUPPORTED)
    this->stats_.invalid++;
  else if (result == proto::RES_NOT_AUTHORIZED)
    this->stats_.refused++;  // a setting turned off for BLE
}

void BleEngine::set_peer(const uint8_t address[6]) {
  std::memcpy(this->peer_, address, sizeof(this->peer_));
  this->have_peer_ = true;
}

void BleEngine::on_read(uint8_t /*channel*/, size_t size) {
  this->stats_.reads++;
  this->stats_.read_bytes += static_cast<uint32_t>(size);
  this->last_activity_ms_ = this->now_();
}

void BleEngine::add_activity_(ActivityKind kind, uint8_t code, uint8_t seq, uint8_t result, uint8_t detail) {
  BleActivity &a = this->activity_[this->activity_head_];
  a.id = this->next_activity_id_++;
  a.uptime_s = static_cast<uint32_t>(this->now_() / 1000);
  a.kind = kind;
  a.code = code;
  a.seq = seq;
  a.result = result;
  a.detail = detail;
  this->activity_head_ = static_cast<uint8_t>((this->activity_head_ + 1) % ACTIVITY_SIZE);
  if (this->activity_count_ < ACTIVITY_SIZE)
    this->activity_count_++;
}

const BleActivity &BleEngine::activity(uint8_t i) const {
  const uint8_t oldest = static_cast<uint8_t>((this->activity_head_ + ACTIVITY_SIZE - this->activity_count_) % ACTIVITY_SIZE);
  return this->activity_[(oldest + i) % ACTIVITY_SIZE];
}

std::string BleEngine::opcode_name(uint8_t opcode) {
  using namespace proto;
  switch (opcode) {
    case OP_FEED:
      return "FEED";
    case OP_TRAY_RELOAD:
      return "TRAY_RELOAD";
    case OP_SET_PROFILE:
      return "SET_PROFILE";
    case OP_HOLD:
      return "HOLD";
    case OP_STOP:
      return "STOP";
    case OP_SET_TIME:
      return "SET_TIME";
    case OP_SET_LOCATION:
      return "SET_LOCATION";
    case OP_CALIBRATE:
      return "CALIBRATE";
    case OP_JOG:
      return "JOG";
    case OP_SAVE_CALIBRATION:
      return "SAVE_CALIBRATION";
    case OP_SET_POSITION:
      return "SET_POSITION";
    case OP_SET_TIMEZONE:
      return "SET_TIMEZONE";
    case OP_AUTH:
      return "AUTH";
    case OP_PROFILE_WRITE:
      return "PROFILE_WRITE";
    case OP_CONFIG_WRITE:
      return "CONFIG_WRITE";
    case OP_CALIBRATION_WRITE:
      return "CALIBRATION_WRITE";
    default: {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "0x%02X", opcode);
      return buf;
    }
  }
}

std::string BleEngine::info_text() const {
  const char *policy = "fallback_only";
  if (this->time_policy_ == proto::TimePolicy::ALWAYS)
    policy = "always";
  else if (this->time_policy_ == proto::TimePolicy::OFF)
    policy = "off";
  static const char *const KEYS[] = {"none", "build", "device"};
  char buf[128];
  std::snprintf(buf, sizeof(buf), "protocol=%u;auth=hmac-sha256;time=%s;stale_h=%u;key=%s;guard=%d;calibration=%d",
                static_cast<unsigned>(proto::PROTOCOL_VERSION), policy,
                static_cast<unsigned>(this->network_stale_ms_ / 3600000u),
                KEYS[static_cast<uint8_t>(this->feeder_.ble_key_source())], this->feeder_.ble_may_change_guard() ? 1 : 0,
                this->feeder_.ble_may_change_calibration() ? 1 : 0);
  return buf;
}

std::string BleEngine::session_text() const {
  char peer[8] = "";
  if (this->have_peer_)
    std::snprintf(peer, sizeof(peer), "%02X:%02X", this->peer_[4], this->peer_[5]);
  auto seconds = [](int64_t ms) { return ms < 0 ? -1L : static_cast<long>(ms / 1000); };
  const BleStats &s = this->stats_;
  // Room for every field at its widest on a 64-bit host (430 bytes), so GCC's -Wformat-truncation stays quiet.
  char buf[432];
  std::snprintf(buf, sizeof(buf),
                "connected=%d;auth=%d;peer=%s;mtu=%u;since=%ld;last=%ld;conns=%lu;wr=%lu;wrb=%lu;rd=%lu;rdb=%lu;"
                "nt=%lu;ntb=%lu;authok=%lu;authfail=%lu;refused=%lu;invalid=%lu",
                this->connected_ ? 1 : 0, this->authorized() ? 1 : 0, peer, static_cast<unsigned>(this->mtu_),
                seconds(this->connected_ms_), seconds(this->last_activity_ms_), static_cast<unsigned long>(s.connections),
                static_cast<unsigned long>(s.writes), static_cast<unsigned long>(s.write_bytes),
                static_cast<unsigned long>(s.reads), static_cast<unsigned long>(s.read_bytes),
                static_cast<unsigned long>(s.notifications), static_cast<unsigned long>(s.notify_bytes),
                static_cast<unsigned long>(s.auth_ok), static_cast<unsigned long>(s.auth_failures),
                static_cast<unsigned long>(s.refused), static_cast<unsigned long>(s.invalid));
  return buf;
}

std::string BleEngine::activity_json() const {
  static const char *const KINDS[] = {"connect", "disconnect", "auth", "command", "record", "replay"};
  std::string out = "{\"entries\":[";
  char buf[200];
  for (uint8_t i = 0; i < this->activity_count_; i++) {
    const BleActivity &a = this->activity(i);
    const uint8_t kind = static_cast<uint8_t>(a.kind);
    if (kind <= static_cast<uint8_t>(ActivityKind::DISCONNECT)) {
      std::snprintf(buf, sizeof(buf), "%s{\"id\":%lu,\"t\":%lu,\"kind\":\"%s\"}", i ? "," : "",
                    static_cast<unsigned long>(a.id), static_cast<unsigned long>(a.uptime_s), KINDS[kind]);
    } else {
      std::snprintf(buf, sizeof(buf),
                    "%s{\"id\":%lu,\"t\":%lu,\"kind\":\"%s\",\"name\":\"%s\",\"seq\":%u,\"result\":\"%s\",\"detail\":%u}",
                    i ? "," : "", static_cast<unsigned long>(a.id), static_cast<unsigned long>(a.uptime_s),
                    kind < sizeof(KINDS) / sizeof(KINDS[0]) ? KINDS[kind] : "?", opcode_name(a.code).c_str(),
                    static_cast<unsigned>(a.seq), result_text(a.result), static_cast<unsigned>(a.detail));
    }
    out += buf;
  }
  out += "]}";
  return out;
}

}  // namespace precision_feeder
}  // namespace esphome
