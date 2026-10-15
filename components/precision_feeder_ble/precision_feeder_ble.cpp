// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder_ble.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder_ble";

// The protocol's layout constants must agree with the feeder component.
static_assert(proto::POSITIONS == NUM_POSITIONS, "protocol and feeder disagree on the number of positions");
static_assert(proto::PROFILES == NUM_PROFILES, "protocol and feeder disagree on the number of profiles");
static_assert(proto::EVENTS_PER_PROFILE == EVENTS_PER_PROFILE, "protocol and feeder disagree on events per profile");

static constexpr uint32_t STATUS_PERIOD_MS = 500;

static std::vector<uint8_t> to_vector(const uint8_t *data, size_t n) { return std::vector<uint8_t>(data, data + n); }

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void PrecisionFeederBle::setup() {
  // Info: protocol version, capability bits, positions, profiles, events per profile.
  uint16_t caps = proto::CAP_SOLAR | proto::CAP_PROFILE_EDIT | proto::CAP_CALIBRATION | proto::CAP_HOLD_FOREVER |
                  proto::CAP_SERVO_PULSE;
  if (this->time_policy_ != proto::TimePolicy::OFF)
    caps |= proto::CAP_TIME_PUSH;
  uint8_t info[6];
  info[0] = proto::PROTOCOL_VERSION;
  proto::put_u16(info + 1, caps);
  info[3] = proto::POSITIONS;
  info[4] = proto::PROFILES;
  info[5] = proto::EVENTS_PER_PROFILE;
  this->info_->set_value(to_vector(info, sizeof(info)));

  this->command_->on_write([this](std::span<const uint8_t> data, uint16_t) { this->on_command_(data); });
  this->profile_->on_write([this](std::span<const uint8_t> data, uint16_t) { this->on_profile_write_(data); });
  this->config_->on_write([this](std::span<const uint8_t> data, uint16_t) { this->on_config_write_(data); });
  this->calibration_->on_write([this](std::span<const uint8_t> data, uint16_t) { this->on_calibration_write_(data); });

  // Something sensible to read before the client has asked for anything.
  this->set_profile_read_(this->feeder_->cfg.active_profile, proto::QUERY_HEADER);
  this->set_config_read_(proto::CFG_MIN_GAP_S);
  this->set_calibration_read_(0);

  this->server_->on_connect([this](uint16_t) { this->force_status_ = true; });
  this->feeder_->set_feed_listener([this](const FeedRecord &record) { this->publish_event_(record); });
}

void PrecisionFeederBle::dump_config() {
  const char *policy = "fallback_only";
  if (this->time_policy_ == proto::TimePolicy::ALWAYS)
    policy = "always";
  else if (this->time_policy_ == proto::TimePolicy::OFF)
    policy = "off";
  ESP_LOGCONFIG(TAG, "Precision Feeder BLE:");
  ESP_LOGCONFIG(TAG, "  Protocol version: %u", proto::PROTOCOL_VERSION);
  ESP_LOGCONFIG(TAG, "  Time policy: %s (network time counts as fresh for %u h)", policy,
                (unsigned) (this->network_stale_ms_ / 3600000u));
}

void PrecisionFeederBle::loop() {
  if (!this->has_client_())
    return;
  const uint32_t now = millis();
  if (!this->force_status_ && now - this->last_status_ms_ < STATUS_PERIOD_MS)
    return;
  this->last_status_ms_ = now;
  this->publish_status_(this->force_status_);
  this->force_status_ = false;
}

// ---------------------------------------------------------------------------------------------
// Command characteristic
// ---------------------------------------------------------------------------------------------

void PrecisionFeederBle::send_response_(const uint8_t response[proto::RESPONSE_SIZE]) {
  this->response_->set_value(to_vector(response, proto::RESPONSE_SIZE));  // also readable if the notify is missed
  this->response_->notify();
}

void PrecisionFeederBle::respond_write_(uint8_t opcode, uint8_t result, uint8_t detail) {
  const uint8_t response[proto::RESPONSE_SIZE] = {0, opcode, result, detail};  // seq 0: writes carry no sequence number
  this->send_response_(response);
}

void PrecisionFeederBle::on_command_(std::span<const uint8_t> data) {
  if (data.empty())
    return;
  const uint8_t seq = data[0];
  if (data.size() < 2) {
    const uint8_t response[proto::RESPONSE_SIZE] = {seq, 0, proto::RES_INVALID_ARG, 0};
    this->send_response_(response);
    return;
  }
  const uint8_t opcode = data[1];
  const uint64_t now = static_cast<uint64_t>(this->feeder_->uptime());

  uint8_t response[proto::RESPONSE_SIZE];
  if (this->replay_.find(data.data(), data.size(), now, response)) {
    // A retry of a command we already answered: repeat the answer, do not act again.
    ESP_LOGI(TAG, "Command seq %u repeated, replaying the earlier answer", seq);
    this->send_response_(response);
    return;
  }

  // Copy the command first: executing it can notify and set values, and `data` points into ESPHome's buffer.
  uint8_t command[proto::ReplayCache::MAX_COMMAND];
  const size_t command_len = std::min(data.size(), proto::ReplayCache::MAX_COMMAND);
  std::memcpy(command, data.data(), command_len);

  uint8_t detail = 0;
  const uint8_t result = this->execute_(opcode, command + 2, command_len - 2, detail);
  response[0] = seq;
  response[1] = opcode;
  response[2] = result;
  response[3] = detail;
  this->replay_.store(command, command_len, response, now);
  ESP_LOGI(TAG, "Command 0x%02X seq %u -> %s", opcode, seq, PrecisionFeeder::result_name(static_cast<Result>(result)));
  this->send_response_(response);
}

uint8_t PrecisionFeederBle::execute_(uint8_t opcode, const uint8_t *args, size_t n, uint8_t &detail) {
  using namespace proto;
  PrecisionFeeder &f = *this->feeder_;
  auto res = [](Result r) { return static_cast<uint8_t>(r); };

  switch (opcode) {
    case OP_FEED: {
      if (n != 1)
        return RES_INVALID_ARG;
      uint8_t accepted = 0;
      const Result r = f.request_feed(args[0], Source::BLE, &accepted);
      detail = r == Result::OK ? accepted : f.remaining();
      return res(r);
    }
    case OP_TRAY_RELOAD:
      if (n != 0)
        return RES_INVALID_ARG;
      if (f.calibration_mode())
        return res(Result::CALIBRATING);
      f.tray_reload();
      detail = f.remaining();
      return RES_OK;

    case OP_SET_PROFILE:
      if (n != 1 || args[0] >= PROFILES)
        return RES_INVALID_ARG;
      f.cfg.active_profile = args[0];
      f.cfg_changed();
      detail = args[0];
      return RES_OK;

    case OP_HOLD: {
      if (n != 4)
        return RES_INVALID_ARG;
      const uint32_t minutes = get_u32(args);
      if (minutes == 0) {
        f.cancel_hold();
        return RES_OK;
      }
      if (minutes == 0xFFFFFFFFu) {
        f.set_hold_indefinite();
        return RES_OK;
      }
      return res(f.set_hold_minutes(minutes));
    }
    case OP_STOP:
      if (n != 0)
        return RES_INVALID_ARG;
      f.stop();
      return RES_OK;

    case OP_SET_TIME: {
      if (n != 6)
        return RES_INVALID_ARG;
      const uint32_t epoch = get_u32(args);
      // args[4..5] is the sender's UTC offset in minutes. The device's timezone is fixed at build time
      // (DESIGN.md section 6), so it is accepted for forward compatibility and not used.
      const uint8_t decision = decide_ble_time(this->time_policy_, epoch, f.network_time_age_ms(), this->network_stale_ms_);
      if (decision != RES_OK) {
        ESP_LOGI(TAG, "Clock from BLE not applied: %s", PrecisionFeeder::result_name(static_cast<Result>(decision)));
        return decision;
      }
      f.set_system_time(epoch);
      return RES_OK;
    }
    case OP_SET_LOCATION: {
      if (n != 8)
        return RES_INVALID_ARG;
      const int32_t lat = get_i32(args);
      const int32_t lon = get_i32(args + 4);
      if (!config_value_ok(CFG_LATITUDE_E5, lat) || !config_value_ok(CFG_LONGITUDE_E5, lon))
        return RES_INVALID_ARG;
      f.cfg.latitude = static_cast<float>(lat) / 100000.0f;
      f.cfg.longitude = static_cast<float>(lon) / 100000.0f;
      f.cfg_changed();
      return RES_OK;
    }
    case OP_CALIBRATE:
      if (n != 1 || args[0] > 1)
        return RES_INVALID_ARG;
      f.set_calibration_mode(args[0] == 1);
      return RES_OK;

    case OP_JOG: {
      if (n != 3)
        return RES_INVALID_ARG;
      const int16_t delta = get_i16(args + 1);
      if (delta < -2 * CAL_MAX || delta > 2 * CAL_MAX)
        return RES_INVALID_ARG;
      return res(f.jog_calibration(args[0], delta));
    }
    case OP_SAVE_CALIBRATION:
      if (n != 0)
        return RES_INVALID_ARG;
      f.save_calibration();
      return RES_OK;

    case OP_SET_POSITION:
      if (n != 1 || args[0] > MAX_PORTIONS)
        return RES_INVALID_ARG;
      if (f.calibration_mode())
        return res(Result::CALIBRATING);
      f.declare_position(args[0]);
      detail = f.remaining();
      return RES_OK;

    case OP_SET_TIMEZONE:  // reserved: the timezone is set at build time
    default:
      return RES_UNSUPPORTED;
  }
}

// ---------------------------------------------------------------------------------------------
// Profile characteristic
//
// Note for the three write handlers below (profile, config, calibration): ESPHome passes the written bytes as a
// span into the characteristic's own stored value, and the handlers replace that value (set_value) so that a
// following read returns the record the client asked for. Anything needed from the written bytes must therefore
// be copied before the first set_value call.
// ---------------------------------------------------------------------------------------------

void PrecisionFeederBle::set_profile_read_(uint8_t profile, uint8_t event) {
  const Profile &p = this->feeder_->cfg.profiles[profile];
  if (event == proto::QUERY_HEADER) {
    proto::ProfileHeaderRec r;
    r.profile = profile;
    r.every_n = p.every_n;
    r.fast_mask = p.fast_days;
    r.anchor_day = static_cast<uint16_t>(p.anchor_day);
    uint8_t out[proto::PROFILE_HEADER_SIZE];
    proto::encode_profile_header(r, out);
    this->profile_->set_value(to_vector(out, sizeof(out)));
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
    this->profile_->set_value(to_vector(out, sizeof(out)));
  }
}

void PrecisionFeederBle::on_profile_write_(std::span<const uint8_t> d) {
  using namespace proto;
  if (d.empty())
    return;
  PrecisionFeeder &f = *this->feeder_;
  const uint8_t profile_hint = d.size() > 1 ? d[1] : 0;

  switch (d[0]) {
    case PROFILE_TAG_HEADER: {
      ProfileHeaderRec r;
      if (!decode_profile_header(d.data(), d.size(), r) || !valid_profile_header(r)) {
        this->respond_write_(OP_PROFILE_WRITE, RES_INVALID_ARG, profile_hint);
        return;
      }
      Profile &p = f.cfg.profiles[r.profile];
      p.every_n = r.every_n;
      p.fast_days = r.fast_mask;
      p.anchor_day = r.anchor_day;
      if (r.anchor_day == 0)
        f.restart_cadence_today(r.profile);  // 0 means "start the cycle today"
      f.cfg_changed();
      this->set_profile_read_(r.profile, QUERY_HEADER);
      this->respond_write_(OP_PROFILE_WRITE, RES_OK, r.profile);
      return;
    }
    case PROFILE_TAG_EVENT: {
      ProfileEventRec r;
      if (!decode_profile_event(d.data(), d.size(), r) || !valid_profile_event(r)) {
        this->respond_write_(OP_PROFILE_WRITE, RES_INVALID_ARG, profile_hint);
        return;
      }
      Event &e = f.cfg.profiles[r.profile].ev[r.event];
      e.enabled = r.enabled;
      e.anchor = r.anchor;
      e.offset_min = r.offset_min;
      e.portions = r.portions;
      e.days = r.days;
      f.cfg_changed();
      this->set_profile_read_(r.profile, r.event);
      this->respond_write_(OP_PROFILE_WRITE, RES_OK, r.profile);
      return;
    }
    case PROFILE_TAG_QUERY: {
      if (d.size() != PROFILE_QUERY_SIZE || d[1] >= PROFILES || (d[2] != QUERY_HEADER && d[2] >= proto::EVENTS_PER_PROFILE)) {
        this->respond_write_(OP_PROFILE_WRITE, RES_INVALID_ARG, profile_hint);
        return;
      }
      // Copy first: set_value() replaces the characteristic's stored bytes, which `d` points into.
      const uint8_t profile = d[1];
      const uint8_t event = d[2];
      this->set_profile_read_(profile, event);
      this->respond_write_(OP_PROFILE_WRITE, RES_OK, profile);
      return;
    }
    default:
      this->respond_write_(OP_PROFILE_WRITE, RES_INVALID_ARG, profile_hint);
      return;
  }
}

// ---------------------------------------------------------------------------------------------
// Config characteristic
// ---------------------------------------------------------------------------------------------

bool PrecisionFeederBle::get_config_(uint8_t key, int32_t &value) const {
  using namespace proto;
  const Settings &c = this->feeder_->cfg;
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
      value = c.min_pulse_us;
      return true;
    case CFG_MAX_PULSE_US:
      value = c.max_pulse_us;
      return true;
    default:
      return false;
  }
}

void PrecisionFeederBle::set_config_read_(uint8_t key) {
  int32_t value = 0;
  if (!this->get_config_(key, value))
    return;
  uint8_t out[proto::CONFIG_SET_SIZE];
  out[0] = key;
  proto::put_i32(out + 1, value);
  this->config_->set_value(to_vector(out, sizeof(out)));
}

void PrecisionFeederBle::on_config_write_(std::span<const uint8_t> d) {
  using namespace proto;
  if (d.empty())
    return;
  PrecisionFeeder &f = *this->feeder_;

  if (d.size() == QUERY_SIZE && (d[0] & QUERY_FLAG)) {
    const uint8_t key = d[0] & ~QUERY_FLAG;
    if (!config_key_known(key)) {
      this->respond_write_(OP_CONFIG_WRITE, RES_INVALID_ARG, key);
      return;
    }
    this->set_config_read_(key);
    this->respond_write_(OP_CONFIG_WRITE, RES_OK, key);
    return;
  }

  const uint8_t key = d[0];
  if (d.size() != CONFIG_SET_SIZE || !config_key_known(key)) {
    this->respond_write_(OP_CONFIG_WRITE, RES_INVALID_ARG, key);
    return;
  }
  const int32_t v = get_i32(d.data() + 1);
  if (!config_value_ok(key, v)) {
    this->respond_write_(OP_CONFIG_WRITE, RES_INVALID_ARG, key);
    return;
  }
  Settings &c = f.cfg;
  switch (key) {
    case CFG_MIN_GAP_S:
      c.min_gap_s = static_cast<uint16_t>(v);
      break;
    case CFG_PORTION_GAP_S:
      c.portion_gap_s = static_cast<uint16_t>(v);
      break;
    case CFG_RELOAD_PAUSE_MIN:
      c.reload_pause_min = static_cast<uint16_t>(v);
      break;
    case CFG_REFILL_INTERVAL_DAYS:
      c.refill_interval_days = static_cast<uint16_t>(v);
      break;
    case CFG_DAILY_CAP:
      c.daily_cap = static_cast<uint8_t>(v);
      break;
    case CFG_MAX_PORTIONS_CMD:
      c.max_portions_cmd = static_cast<uint8_t>(v);
      break;
    case CFG_SCHEDULER_ENABLED:
      c.scheduler_enabled = static_cast<uint8_t>(v);
      break;
    case CFG_LATITUDE_E5:
      c.latitude = static_cast<float>(v) / 100000.0f;
      break;
    case CFG_LONGITUDE_E5:
      c.longitude = static_cast<float>(v) / 100000.0f;
      break;
    case CFG_MIN_PULSE_US:
    case CFG_MAX_PULSE_US: {
      // The pair must stay at least 500 us apart; the core checks it and moves the servo in calibration mode.
      const uint16_t lo = key == CFG_MIN_PULSE_US ? static_cast<uint16_t>(v) : c.min_pulse_us;
      const uint16_t hi = key == CFG_MAX_PULSE_US ? static_cast<uint16_t>(v) : c.max_pulse_us;
      if (f.set_pulse_range(lo, hi) != Result::OK) {
        this->respond_write_(OP_CONFIG_WRITE, RES_INVALID_ARG, key);
        return;
      }
      break;
    }
  }
  f.cfg_changed();
  this->set_config_read_(key);
  this->respond_write_(OP_CONFIG_WRITE, RES_OK, key);
}

// ---------------------------------------------------------------------------------------------
// Calibration characteristic
// ---------------------------------------------------------------------------------------------

void PrecisionFeederBle::set_calibration_read_(uint8_t index) {
  if (index >= proto::POSITIONS)
    return;
  uint8_t out[proto::CALIBRATION_SET_SIZE];
  out[0] = index;
  proto::put_i16(out + 1, this->feeder_->cfg.cal[index]);
  this->calibration_->set_value(to_vector(out, sizeof(out)));
}

void PrecisionFeederBle::on_calibration_write_(std::span<const uint8_t> d) {
  using namespace proto;
  if (d.empty())
    return;

  if (d.size() == QUERY_SIZE && (d[0] & QUERY_FLAG)) {
    const uint8_t index = d[0] & ~QUERY_FLAG;
    if (index >= POSITIONS) {
      this->respond_write_(OP_CALIBRATION_WRITE, RES_INVALID_ARG, index);
      return;
    }
    this->set_calibration_read_(index);
    this->respond_write_(OP_CALIBRATION_WRITE, RES_OK, index);
    return;
  }

  if (d.size() != CALIBRATION_SET_SIZE) {
    this->respond_write_(OP_CALIBRATION_WRITE, RES_INVALID_ARG, d[0]);
    return;
  }
  const uint8_t index = d[0];
  const int16_t tenths = get_i16(d.data() + 1);
  if (!calibration_ok(index, tenths)) {
    this->respond_write_(OP_CALIBRATION_WRITE, RES_INVALID_ARG, index);
    return;
  }
  this->feeder_->set_calibration_offset_at(index, tenths);
  this->set_calibration_read_(index);
  this->respond_write_(OP_CALIBRATION_WRITE, RES_OK, index);
}

// ---------------------------------------------------------------------------------------------
// Status and event log
// ---------------------------------------------------------------------------------------------

proto::StatusSnapshot PrecisionFeederBle::snapshot_() {
  using namespace proto;
  PrecisionFeeder &f = *this->feeder_;
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

void PrecisionFeederBle::publish_status_(bool force) {
  uint8_t buf[proto::STATUS_SIZE];
  proto::encode_status(this->snapshot_(), buf);
  if (!force && std::memcmp(buf, this->last_status_, sizeof(buf)) == 0)
    return;
  std::memcpy(this->last_status_, buf, sizeof(buf));
  this->status_->set_value(to_vector(buf, sizeof(buf)));
  this->status_->notify();
}

void PrecisionFeederBle::publish_event_(const FeedRecord &record) {
  if (!this->has_client_())
    return;
  uint8_t buf[proto::EVENT_LOG_SIZE];
  proto::encode_event_log(record.epoch, record.source, record.portions, record.result, buf);
  this->event_log_->set_value(to_vector(buf, sizeof(buf)));
  this->event_log_->notify();
  this->force_status_ = true;  // the tray moved on or a refusal changed "last result": send fresh status
}

}  // namespace precision_feeder
}  // namespace esphome
