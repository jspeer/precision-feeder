// Part of the Precision Feeder ESPHome firmware (MIT licence).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder";
static constexpr float SERVO_PERIOD_US = 20000.0f;  // 50 Hz

void PrecisionFeeder::setup() {
  this->pref_settings_ = global_preferences->make_preference<Settings>(fnv1_hash("precision_feeder.settings"));
  this->pref_state_ = global_preferences->make_preference<State>(fnv1_hash("precision_feeder.state"));
  Settings settings;
  State state;
  const bool have_settings = this->pref_settings_.load(&settings);
  const bool have_state = this->pref_state_.load(&state);
  this->core_start(have_settings ? &settings : nullptr, have_state ? &state : nullptr);
}

void PrecisionFeeder::dump_config() {
  ESP_LOGCONFIG(TAG, "Precision Feeder:");
  ESP_LOGCONFIG(TAG, "  Sweep: %.1f deg, pitch %.1f deg, invert: %s", this->sweep_, this->sweep_ / NUM_POSITIONS,
                YESNO(this->invert_));
  ESP_LOGCONFIG(TAG, "  Pulse: %u-%u us (saved), step time %u ms, wiggle: %s, detach delay %u ms",
                (unsigned) this->cfg.min_pulse_us, (unsigned) this->cfg.max_pulse_us, (unsigned) this->step_ms_,
                YESNO(this->wiggle_), (unsigned) this->detach_ms_);
  ESP_LOGCONFIG(TAG, "  Tray position: %u (%s), remaining: %u", this->position(), this->tray_state_text().c_str(),
                this->remaining());
}

void PrecisionFeeder::note_sntp_sync() {
  // ESP-IDF marks its sync status before calling the notification hook, and nothing else reads it (reading a
  // completed status resets it). RESET here means ESPHome's own boot-time check, not a reply from a server.
  if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
    ESP_LOGD(TAG, "Clock valid at boot (kept across a restart); waiting for a real NTP sync");
    return;
  }
  this->note_time_sync(TimeSource::NTP);
}

uint32_t PrecisionFeeder::hal_millis() { return millis(); }

int64_t PrecisionFeeder::hal_uptime_ms() { return esp_timer_get_time() / 1000; }

bool PrecisionFeeder::hal_time_valid() { return this->time_ != nullptr && this->time_->now().is_valid(); }

int64_t PrecisionFeeder::hal_epoch() { return static_cast<int64_t>(this->time_->timestamp_now()); }

void PrecisionFeeder::hal_set_epoch(int64_t epoch) {
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(epoch);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

LocalTime PrecisionFeeder::hal_local(int64_t epoch) {
  const ESPTime t = ESPTime::from_epoch_local(static_cast<time_t>(epoch));
  return LocalTime{t.year, t.month, t.day_of_month, t.hour, t.minute, t.second};
}

int64_t PrecisionFeeder::hal_local_to_epoch(int year, int month, int day, int hour, int minute) {
  ESPTime t{};
  t.year = static_cast<uint16_t>(year);
  t.month = static_cast<uint8_t>(month);
  t.day_of_month = static_cast<uint8_t>(day);
  t.hour = static_cast<uint8_t>(hour);
  t.minute = static_cast<uint8_t>(minute);
  t.second = 0;
  t.recalc_timestamp_local();  // resolves daylight saving; -1 for an invalid date
  return static_cast<int64_t>(t.timestamp);
}

void PrecisionFeeder::hal_servo_write(float angle) {
  const float span = static_cast<float>(this->cfg.max_pulse_us) - static_cast<float>(this->cfg.min_pulse_us);
  const float pulse = static_cast<float>(this->cfg.min_pulse_us) + (angle / 180.0f) * span;
  this->output_->set_level(pulse / SERVO_PERIOD_US);
}

void PrecisionFeeder::hal_servo_release() { this->output_->set_level(0.0f); }

void PrecisionFeeder::hal_save_settings(const Settings &settings, bool flush) {
  this->pref_settings_.save(&settings);
  if (flush)
    global_preferences->sync();
}

void PrecisionFeeder::hal_save_state(const State &state, bool flush) {
  this->pref_state_.save(&state);
  if (flush)
    global_preferences->sync();
}

void PrecisionFeeder::hal_log(LogLevel level, const char *message) {
  switch (level) {
    case LogLevel::kError:
      ESP_LOGE(TAG, "%s", message);
      break;
    case LogLevel::kWarn:
      ESP_LOGW(TAG, "%s", message);
      break;
    case LogLevel::kInfo:
      ESP_LOGI(TAG, "%s", message);
      break;
    default:
      ESP_LOGD(TAG, "%s", message);
      break;
  }
}

}  // namespace precision_feeder
}  // namespace esphome
