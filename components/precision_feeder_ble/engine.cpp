// Precision Feeder BLE protocol engine (see engine.h).
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
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

// The protocol's layout constants must agree with the feeder.
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
  uint16_t caps = proto::CAP_SOLAR | proto::CAP_PROFILE_EDIT | proto::CAP_CALIBRATION | proto::CAP_HOLD_FOREVER |
                  proto::CAP_SERVO_PULSE | proto::CAP_AUTH;
  if (this->time_policy_ != proto::TimePolicy::OFF)
    caps |= proto::CAP_TIME_PUSH;
  return caps;
}

void BleEngine::start() {
  // Info: protocol version, capability bits, positions, profiles, events per profile.
  uint8_t info[6];
  info[0] = proto::PROTOCOL_VERSION;
  proto::put_u16(info + 1, this->capabilities());
  info[3] = proto::POSITIONS;
  info[4] = proto::PROFILES;
  info[5] = proto::EVENTS_PER_PROFILE;
  this->transport_.set_value(proto::CHANNEL_INFO, info, sizeof(info), false);
  // Nothing but Info and Auth is readable until a client has authenticated.
  this->clear_reads_();
  this->new_nonce_();
}

void BleEngine::on_connect() {
  this->connected_ = true;
  this->replay_.clear();
  this->clear_reads_();
  this->new_nonce_();  // each connection gets a new challenge and starts unauthenticated
}

void BleEngine::on_disconnect() {
  this->connected_ = false;
  this->session_.end();
  this->replay_.clear();
  this->clear_reads_();
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
  this->transport_.set_value(proto::CHANNEL_RESPONSE, response, sizeof(response), true);
}

// ---------------------------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------------------------

void BleEngine::on_write(uint8_t channel, const uint8_t *data, size_t n) {
  if (n == 0)
    return;
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
  uint8_t detail = 0;
  const bool was_authenticated = this->session_.authenticated();
  const uint8_t result = this->session_.authenticate(seq, proof, n, this->now_(), detail);
  if (result == proto::RES_OK) {
    // The retry cache is NOT cleared here: connect and disconnect already start every session with an empty one,
    // and clearing it on a repeated AUTH would let a retried write execute a second time.
    if (!was_authenticated) {
      this->log_(LogLevel::kInfo, "BLE client authenticated");
      this->force_status_ = true;  // send fresh status straight away
    }
  } else {
    this->log_(LogLevel::kWarn, "BLE authentication refused (detail %u)", detail);
    if (detail == proto::AUTH_DETAIL_BAD_PROOF)
      this->new_nonce_();  // a guess is never tried against the same challenge twice
  }
  this->respond_(seq, proto::OP_AUTH, result, detail);
}

void BleEngine::on_command_(const uint8_t *command, size_t n) {
  const uint8_t seq = command[0];
  if (n < 2) {
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
    this->respond_(response[0], response[1], response[2], response[3]);
    return;
  }
  uint8_t detail = 0;
  const uint8_t check = this->session_.check(proto::CHANNEL_COMMAND, command, n, detail);
  if (check != proto::RES_OK) {
    this->log_(LogLevel::kWarn, "Command 0x%02X seq %u refused: not authorized (detail %u)", opcode, seq, detail);
    this->respond_(seq, opcode, check, detail);
    return;
  }
  this->session_.accept(seq);

  Answer a{proto::RES_INVALID_ARG, 0};
  if (n >= 2 + crypto::TAG_SIZE)  // otherwise there is no room for an opcode: "opcode" is the tag's first byte
    a = this->execute_(opcode, command + 2, n - 2 - crypto::TAG_SIZE);
  const uint8_t answer[proto::RESPONSE_SIZE] = {seq, opcode, a.result, a.detail};
  this->replay_.store(proto::CHANNEL_COMMAND, command, n, answer, now);
  this->log_(LogLevel::kInfo, "Command 0x%02X seq %u -> %s", opcode, seq, result_text(a.result));
  this->respond_(seq, opcode, a.result, a.detail);
}

BleEngine::Answer BleEngine::execute_(uint8_t opcode, const uint8_t *args, size_t n) {
  using namespace proto;
  FeederCore &f = this->feeder_;
  auto res = [](Result r) { return Answer{static_cast<uint8_t>(r), 0}; };
  const Answer invalid{RES_INVALID_ARG, 0};

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
      if (n != 1 || args[0] > 1)
        return invalid;
      f.set_calibration_mode(args[0] == 1);
      return {RES_OK, 0};

    case OP_JOG: {
      if (n != 3)
        return invalid;
      const int16_t delta = get_i16(args + 1);
      if (delta < 2 * CAL_OFFSET_LO || delta > 2 * CAL_OFFSET_HI)
        return invalid;
      return res(f.jog_calibration(args[0], delta));
    }
    case OP_SAVE_CALIBRATION:
      if (n != 0)
        return invalid;
      f.save_calibration();
      return {RES_OK, 0};

    case OP_SET_POSITION:
      if (n != 1 || args[0] > MAX_PORTIONS)
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
    this->respond_(response[0], response[1], response[2], response[3]);  // a retry: same answer, not applied again
    return;
  }
  uint8_t detail = 0;
  const uint8_t check = this->session_.check(channel, write, n, detail);
  if (check != proto::RES_OK) {
    this->log_(LogLevel::kWarn, "Write to 0x%02X seq %u refused: not authorized (detail %u)", channel, seq, detail);
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
      if (!decode_profile_event(d, n, r) || !valid_profile_event(r))
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
  if (r != Result::OK)
    return {RES_INVALID_ARG, key};
  this->set_config_read_(key);
  return {RES_OK, key};
}

// ---------------------------------------------------------------------------------------------
// Calibration characteristic
// ---------------------------------------------------------------------------------------------

void BleEngine::set_calibration_read_(uint8_t index) {
  if (index >= proto::POSITIONS)
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
    if (index >= POSITIONS)
      return {RES_INVALID_ARG, index};
    this->set_calibration_read_(index);
    return {RES_OK, index};
  }
  if (n != CALIBRATION_SET_SIZE)
    return {RES_INVALID_ARG, d[0]};
  const uint8_t index = d[0];
  const int16_t tenths = get_i16(d + 1);
  if (index >= POSITIONS || tenths < CAL_OFFSET_LO || tenths > CAL_OFFSET_HI)
    return {RES_INVALID_ARG, index};
  this->feeder_.set_calibration_offset_at(index, tenths);
  this->set_calibration_read_(index);
  return {RES_OK, index};
}

// ---------------------------------------------------------------------------------------------
// Status and event log
// ---------------------------------------------------------------------------------------------

void BleEngine::loop(uint32_t now_ms) {
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
  this->transport_.set_value(proto::CHANNEL_STATUS, buf, sizeof(buf), true);
}

void BleEngine::on_feed(const FeedRecord &record) {
  if (!this->authorized())
    return;
  uint8_t buf[proto::EVENT_LOG_SIZE];
  proto::encode_event_log(record.epoch, record.source, record.portions, record.result, buf);
  this->transport_.set_value(proto::CHANNEL_EVENT_LOG, buf, sizeof(buf), true);
  this->force_status_ = true;  // the tray moved on or a refusal changed "last result": send fresh status
}

}  // namespace precision_feeder
}  // namespace esphome
