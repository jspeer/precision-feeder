// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_WEBSERVER
#include "esphome/components/web_server_base/web_server_base.h"
#endif
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
#if defined(USE_WEBSERVER) && defined(USE_OTA)
#include "esphome/components/ota/ota_backend_factory.h"
#include "firmware_upload.h"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include <esp_random.h>
#ifdef USE_WIFI
#include <esp_wifi.h>
#endif
#include <esp_sntp.h>
#include <sdkconfig.h>
#include <esp_timer.h>
#include <sys/time.h>

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder";
static constexpr float SERVO_PERIOD_US = 20000.0f;  // 50 Hz

void PrecisionFeeder::setup() {
  if (this->is_servo() && this->output_ == nullptr) {
    // Neither the servo's output nor another mechanism: nothing could move the tray, so nothing may feed.
    ESP_LOGE(TAG, "No mechanism: give precision_feeder an output (the servo) or include a mechanism package");
    this->mark_failed();
    return;
  }
  // Each record layout has its own key (see SETTINGS_VERSION in feeder_core.h), so an older record can still be
  // loaded and migrated after an update instead of failing to load and being replaced by defaults.
  this->pref_settings_ = global_preferences->make_preference<Settings>(fnv1_hash("precision_feeder.settings.v4"));
  this->pref_calibration_ =
      global_preferences->make_preference<Calibration>(fnv1_hash("precision_feeder.calibration.v1"));
  this->pref_state_ = global_preferences->make_preference<State>(fnv1_hash("precision_feeder.state"));
  this->pref_device_ = global_preferences->make_preference<Device>(fnv1_hash("precision_feeder.device.v2"));
  Settings settings;
  Calibration calibration;
  State state;
  Device device;
  const bool have_settings = this->pref_settings_.load(&settings);
  const bool have_calibration = this->pref_calibration_.load(&calibration);
  const bool have_state = this->pref_state_.load(&state);
  const bool have_device = this->pref_device_.load(&device);
  legacy::DeviceV1 device_v1;
  bool have_device_v1 = false;
  if (!have_device) {  // firmware 2.0.0 and 2.0.1: migrated by core_start() and saved under the new key
    auto pref_v1 = global_preferences->make_preference<legacy::DeviceV1>(fnv1_hash("precision_feeder.device.v1"));
    have_device_v1 = pref_v1.load(&device_v1);
  }
#ifdef SNTP_SERVER_COUNT
  // The sntp component (set up earlier, BEFORE_CONNECTION) has given ESP-IDF the build's servers: remember them, to
  // go back to when the servers set here are cleared. They point at string literals, which last.
  for (uint8_t i = 0; i < NTP_SERVERS_MAX && i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++)
    this->ntp_build_[i] = esp_sntp_getservername(i);
#endif
#ifdef USE_TIME_TIMEZONE
  // The time platforms set the build's timezone before any component starts.
  this->build_tz_ = time::get_global_tz();
#endif
  legacy::SettingsV3 v3;
  bool have_v3 = false;
  if (!have_settings) {
    auto pref_v3 = global_preferences->make_preference<legacy::SettingsV3>(fnv1_hash("precision_feeder.settings"));
    have_v3 = pref_v3.load(&v3);
  }
  // The mechanism's tuning values (firmware 2.1.0), only for a mechanism that has any: the servo has none, so a servo
  // build never touches this record.
  Tuning tuning;
  bool have_tuning = false;
  if (this->tuning_count() > 0) {
    this->pref_tuning_ = global_preferences->make_preference<Tuning>(fnv1_hash("precision_feeder.tuning.v1"));
    have_tuning = this->pref_tuning_.load(&tuning);
  }
  this->core_start(have_settings ? &settings : nullptr, have_calibration ? &calibration : nullptr,
                   have_state ? &state : nullptr, have_v3 ? &v3 : nullptr, have_device ? &device : nullptr,
                   have_device_v1 ? &device_v1 : nullptr, have_tuning ? &tuning : nullptr);
#if defined(USE_OTA_PASSWORD) && defined(USE_OTA_ENCRYPTION_PROVISIONED)
  if (this->ota_ != nullptr) {
    uint8_t random[16];
    char password[2 * sizeof(random) + 1];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++)
      std::snprintf(password + 2 * i, 3, "%02x", random[i]);
    this->ota_->set_auth_password(password);
    std::memset(password, 0, sizeof(password));
  }
#endif
  this->update_api_key_state_();
}

void PrecisionFeeder::restart_if_due_() {
  const uint32_t at = this->restart_at_.load();
  if (at == 0 || static_cast<int32_t>(millis() - at) < 0)
    return;
  // Not in the middle of a feed: an interrupted move would leave the tray position unknown. Feeding goes on until
  // the tray stops; the image waits in the update partition.
  if (this->tray_state() == TrayState::MOVING)
    return;
  this->restart_at_.store(0);
  ESP_LOGI(TAG, "Restarting into the uploaded firmware");
  App.safe_reboot();
}

bool PrecisionFeeder::api_key_settable() {
#if defined(USE_API_NOISE) && !defined(USE_API_NOISE_PSK_FROM_YAML)
  return true;
#else
  return false;
#endif
}

void PrecisionFeeder::update_api_key_state_() {
  ApiKey state = ApiKey::OFF;
#if defined(USE_API_NOISE) && defined(USE_API_NOISE_PSK_FROM_YAML)
  state = ApiKey::BUILD;
#elif defined(USE_API_NOISE)
  if (api::global_api_server != nullptr)
    state = api::global_api_server->get_noise_ctx().has_psk() ? ApiKey::DEVICE : ApiKey::NONE;
#endif
  this->api_key_state_.store(state);
}

bool PrecisionFeeder::set_api_key(const uint8_t *key) {
#if defined(USE_API_NOISE) && !defined(USE_API_NOISE_PSK_FROM_YAML)
  if (api::global_api_server == nullptr)
    return false;
  if (key == nullptr) {
    ESP_LOGW(TAG, "API key cleared from the web app: Home Assistant must provision a new one");
    return api::global_api_server->clear_noise_psk(true);
  }
  noise::psk_t psk;
  std::memcpy(psk.data(), key, psk.size());
  ESP_LOGW(TAG, "API key set from the web app: Home Assistant needs the new key");
  const bool ok = api::global_api_server->save_noise_psk(psk, true);
  psk.fill(0);
  return ok;
#else
  (void) key;
  return false;
#endif
}

Device PrecisionFeeder::load_device_early_() {
  Device saved{};
  saved.servo_pin = saved.button_pin = saved.led_pin = PIN_BUILD;
  Device loaded;
  auto pref = global_preferences->make_preference<Device>(fnv1_hash("precision_feeder.device.v2"));
  legacy::DeviceV1 v1;
  auto pref_v1 = global_preferences->make_preference<legacy::DeviceV1>(fnv1_hash("precision_feeder.device.v1"));
  bool ok = pref.load(&loaded) && loaded.version == DEVICE_VERSION;
  if (!ok && pref_v1.load(&v1) && v1.version == 1) {  // not migrated yet: core_start() does that, later
    loaded = legacy::migrate_device_v1(v1);
    ok = true;
  }
  if (ok) {
    saved = loaded;
    saved.hostname[HOSTNAME_SIZE - 1] = '\0';
    saved.ap_password[AP_PASSWORD_SIZE - 1] = '\0';
  }
  return saved;
}

// ESPHome sets the name once, from the YAML (App.pre_setup(), before any component starts) and has no setter. A name
// saved on the device replaces it here, before Wi-Fi (DHCP host name), mDNS, the API, BLE and the access point read it.
// The derived class only gives access to the protected member (checked against ESPHome 2026.9.1: `StringRef name_`);
// if ESPHome renames it, this stops compiling rather than misbehaving.
namespace {
struct AppName : Application {
  static void set(const char *name, size_t len) { App.*(&AppName::name_) = StringRef(name, len); }
};
}  // namespace

void PrecisionFeeder::apply_network() {
  const Device saved = load_device_early_();
  this->hostname_build_ = App.get_name().str();
  if (saved.hostname[0] != '\0') {
    if (check_hostname(saved.hostname)) {
      static char name[HOSTNAME_SIZE];  // App keeps a reference: it must live as long as the device
      std::memcpy(name, saved.hostname, HOSTNAME_SIZE);
      AppName::set(name, std::strlen(name));
    } else {
      ESP_LOGW(TAG, "Saved name not usable, using the build's");
    }
  }
  bool has_ap = false;
#if defined(USE_WIFI) && defined(USE_WIFI_AP)
  auto *wifi = wifi::global_wifi_component;
  if (wifi != nullptr && wifi->has_ap()) {
    has_ap = true;
    wifi::WiFiAP ap = wifi->get_ap();
    if (ap.get_ssid().empty()) {
      // No SSID in the YAML: "<name>-setup", so the name in use (MAC suffix included) tells feeders apart.
      static const char SUFFIX[] = "-setup";
      const std::string &name = App.get_name().str();
      ap.set_ssid(name.substr(0, WIFI_SSID_MAX - (sizeof(SUFFIX) - 1)) + SUFFIX);
    }
    this->ap_build_open_ = ap.get_password().empty();  // the prebuilt binaries: open on a clean flash
    if (saved.ap_password[0] != '\0' && check_wifi_password(saved.ap_password, false))
      ap.set_password(saved.ap_password);
    wifi->set_ap(ap);
  }
#endif
  {
    LockGuard guard(this->password_lock_);
    this->set_network_in_use(App.get_name().str(), has_ap, saved);
  }
  ESP_LOGI(TAG, "Name: %s (%s)", App.get_name().c_str(),
           App.get_name().str() == this->hostname_build_ ? "the build's" : "saved on the device");
}

bool PrecisionFeeder::wifi_settable() {
#ifdef USE_WIFI
  return true;
#else
  return false;
#endif
}

bool PrecisionFeeder::set_wifi(const std::string &ssid, const std::string &password) {
#ifdef USE_WIFI
  if (wifi::global_wifi_component == nullptr)
    return false;
  ESP_LOGW(TAG, "Wi-Fi network set from the web app: joining it now");
  wifi::global_wifi_component->save_wifi_sta(ssid, password);
  // save_wifi_sta() only connects when Wi-Fi is idle (found on the bench): a connected feeder would stay on the old
  // network until it next lost it. Dropping the link hands the switch to ESPHome's own reconnect, which now tries the
  // saved network and opens the fallback access point if it cannot join. A second later, so the web app's request
  // is answered over the old link first.
  this->set_timeout("wifi_switch", 1000, []() {
    if (wifi::global_wifi_component->is_connected())
      esp_wifi_disconnect();
  });
  return true;
#else
  (void) ssid;
  (void) password;
  return false;
#endif
}

void PrecisionFeeder::apply_motor_pins_early() {
  if (this->motor_pin_count() == 0 || this->motor_user_ == nullptr)
    return;
  this->pref_motor_pins_ =
      global_preferences->make_preference<MotorPins>(fnv1_hash("precision_feeder.motor_pins.v1"));
  MotorPins saved;
  const bool have = this->pref_motor_pins_.load(&saved);
  std::string problem;
  this->apply_motor_pins(have ? &saved : nullptr, &problem);
  if (!problem.empty())
    ESP_LOGW(TAG, "Motor pins: %s; the build's used", problem.c_str());
  uint8_t pins[RESERVED_PINS_MAX];
  for (uint8_t i = 0; i < this->motor_pin_count(); i++)
    pins[i] = this->motor_pin_in_use(i);
  this->motor_user_->use_pins(pins, this->motor_pin_count());
}

void PrecisionFeeder::hal_save_motor_pins(const MotorPins &pins) {
  this->pref_motor_pins_.save(&pins);
  global_preferences->sync();
}

void PrecisionFeeder::apply_pins() {
  if (this->servo_pin_ == nullptr && this->button_pin_ == nullptr && this->led_output_ == nullptr)
    return;  // a build without runtime pins: every driver keeps its YAML pin
  // No servo pin: a mechanism that drives its own pins (they are reserved, so the button and LED keep off them).
  const Pins build{this->servo_pin_ != nullptr ? static_cast<uint8_t>(this->servo_pin_->get_pin()) : PIN_NONE,
                   this->button_pin_ != nullptr ? this->button_pin_->get_pin() : PIN_NONE,
                   this->led_output_ != nullptr ? this->led_build_pin_ : PIN_NONE};
  const Device saved = load_device_early_();
  std::string problem;
  Pins use = resolve_pins(saved, build, &problem, &this->reserved_pins());
  const bool has_button = this->button_pin_ != nullptr, has_led = this->led_output_ != nullptr;
  if (!has_button)
    use.button = PIN_NONE;  // not in this build, whatever is saved
  if (!has_led)
    use.led = PIN_NONE;
  if (this->servo_pin_ == nullptr)
    use.servo = PIN_NONE;  // a servo pin saved by a servo build means nothing here
  if (!problem.empty())
    ESP_LOGW(TAG, "Pins: %s", problem.c_str());

  if (this->servo_pin_ != nullptr)
    this->servo_pin_->set_pin(static_cast<gpio_num_t>(use.servo));
  if (this->button_pin_ != nullptr) {
    if (use.button == PIN_NONE)
      this->button_->disable_loop();  // before its setup(): ESPHome then never sets it up, so the pin stays free
    else
      this->button_pin_->set_pin(static_cast<gpio_num_t>(use.button));
  }
  if (this->led_output_ != nullptr) {
    if (use.led == PIN_NONE) {
      this->led_light_->disable_loop();
      this->led_output_->disable_loop();
    } else {
      this->led_set_pin_(this->led_output_, use.led);
    }
  }
  this->set_build_pins(build);
  this->set_pins_in_use(use, has_button, has_led);
  ESP_LOGI(TAG, "Pins: %s", this->pins_text().c_str());
}

void PrecisionFeeder::dump_config() {
  ESP_LOGCONFIG(TAG, "Precision Feeder:");
  ESP_LOGCONFIG(TAG, "  Mechanism: %s, %u portions per load", this->mechanism().name(), this->capacity());
  if (this->is_servo()) {
    ServoMechanism &servo = this->servo();
    ESP_LOGCONFIG(TAG, "  Sweep: %.1f deg, pitch %.1f deg, invert: %s", servo.sweep(), servo.sweep() / NUM_POSITIONS,
                  YESNO(servo.invert()));
    ESP_LOGCONFIG(TAG, "  Pulse: %u-%u us (saved), step time %u ms, wiggle: %s, detach delay %u ms",
                  (unsigned) this->calib.min_pulse_us, (unsigned) this->calib.max_pulse_us,
                  (unsigned) servo.step_time(), YESNO(servo.wiggle()), (unsigned) servo.settle_ms());
  }
  for (uint8_t i = 0; i < this->tuning_count(); i++) {
    const TuningParam *p = this->tuning_param(i);
    if (p != nullptr) {
      ESP_LOGCONFIG(TAG, "  %s: %ld (x%u, %s)", p->name, (long) this->tuning_value(i), (unsigned) p->scale, p->unit);
    }
  }
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
  if (this->output_ == nullptr)
    return;
  const float span = static_cast<float>(this->calib.max_pulse_us) - static_cast<float>(this->calib.min_pulse_us);
  const float pulse = static_cast<float>(this->calib.min_pulse_us) + (angle / 180.0f) * span;
  this->output_->set_level(pulse / SERVO_PERIOD_US);
}

void PrecisionFeeder::hal_servo_release() {
  if (this->output_ != nullptr)
    this->output_->set_level(0.0f);
}

void PrecisionFeeder::hal_save_settings(const Settings &settings, bool flush) {
  this->pref_settings_.save(&settings);
  if (flush)
    global_preferences->sync();
}

void PrecisionFeeder::hal_save_calibration(const Calibration &calibration, bool flush) {
  this->pref_calibration_.save(&calibration);
  if (flush)
    global_preferences->sync();
}

void PrecisionFeeder::hal_save_state(const State &state, bool flush) {
  this->pref_state_.save(&state);
  if (flush)
    global_preferences->sync();
}

void PrecisionFeeder::hal_save_tuning(const Tuning &tuning, bool flush) {
  if (this->tuning_count() == 0)
    return;
  this->pref_tuning_.save(&tuning);
  if (flush)
    global_preferences->sync();
}

void PrecisionFeeder::hal_save_device(const Device &device) {
  this->pref_device_.save(&device);
  global_preferences->sync();
}

void PrecisionFeeder::hal_apply_ntp(const std::vector<std::string> *servers) {
#ifdef SNTP_SERVER_COUNT
  // ESPHome sets its servers once (sntp component, setup()) and has no setter; underneath it is ESP-IDF's SNTP client,
  // which takes new names after a stop and polls them from the next start. It keeps the pointers, so the names live
  // here. CONFIG_LWIP_SNTP_MAX_SERVERS (core.yaml) decides how many are asked.
  static char names[NTP_SERVERS_MAX][NTP_SERVER_LEN_MAX + 1];
  esp_sntp_stop();
  for (uint8_t i = 0; i < NTP_SERVERS_MAX && i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++) {
    const char *name = nullptr;
    if (servers == nullptr) {
      name = this->ntp_build_[i];
    } else if (i < servers->size()) {
      std::snprintf(names[i], sizeof(names[i]), "%s", (*servers)[i].c_str());  // parse_ntp_servers() checked lengths
      name = names[i];
    }
    esp_sntp_setservername(i, name);
  }
  esp_sntp_init();
  ESP_LOGI(TAG, "NTP: asking %s", servers == nullptr ? "the build's servers" : "the servers set on the feeder");
#else
  if (servers != nullptr)
    ESP_LOGW(TAG, "This build has no NTP (sntp time platform): servers saved, not used");
#endif
}

void PrecisionFeeder::hal_apply_timezone(const TzRules *rules) {
#ifdef USE_TIME_TIMEZONE
  if (rules == nullptr) {
    // Home Assistant, if it sends a timezone, sends it again at its next time sync.
    time::set_global_tz(this->build_tz_);
    return;
  }
  // The same fields and meanings as ESPHome's parser (tz_rules.h).
  auto rule = [](const TzRule &r) {
    time::DSTRule out{};
    out.time_seconds = r.time_s;
    out.day = r.day;
    out.type = static_cast<time::DSTRuleType>(r.type);
    out.month = r.month;
    out.week = r.week;
    out.day_of_week = r.day_of_week;
    return out;
  };
  time::ParsedTimezone tz{};
  tz.std_offset_seconds = rules->std_offset_s;
  tz.dst_offset_seconds = rules->dst_offset_s;
  tz.dst_start = rule(rules->dst_start);
  tz.dst_end = rule(rules->dst_end);
  time::set_global_tz(tz);
#else
  (void) rules;
  ESP_LOGW(TAG, "This build has no timezone support (no time platform with a timezone): timezone not applied");
#endif
}

void PrecisionFeeder::hal_random(uint8_t *out, size_t n) { esp_fill_random(out, n); }

#ifdef USE_WEBSERVER
// ---- Settings guard (DESIGN.md section 10.1) -------------------------------------------------------------------
// Runs on the web server's task. FeederCore's password state is only touched under password_lock(); anything that
// writes to flash is deferred to the main loop.

static const char *const AUTH_HEADER = "X-Feeder-Auth";  // hex: nonce (16 bytes) then tag (32 bytes)

static void to_hex(const uint8_t *data, size_t n, char *out) {
  static const char DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = DIGITS[data[i] >> 4];
    out[2 * i + 1] = DIGITS[data[i] & 15];
  }
  out[2 * n] = '\0';
}

static bool from_hex(const std::string &text, size_t offset, uint8_t *out, size_t n) {
  if (text.size() < offset + 2 * n)
    return false;
  for (size_t i = 0; i < 2 * n; i++) {
    const char c = text[offset + i];
    int v;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      return false;
    out[i / 2] = static_cast<uint8_t>(i % 2 ? (out[i / 2] | v) : v << 4);
  }
  return true;
}

#ifdef USE_OTA
/// The firmware upload's sink (firmware_upload.h) over ESPHome's own OTA backend, the one its OTA and web upload use.
class OtaSink : public UpdateSink {
 public:
  bool begin(size_t size) override {
    this->backend_ = ota::make_ota_backend();
    if (this->backend_ && this->backend_->begin(size) == ota::OTA_RESPONSE_OK)
      return true;
    this->backend_.reset();
    return false;
  }
  bool write(const uint8_t *data, size_t len) override {
    return this->backend_->write(const_cast<uint8_t *>(data), len) == ota::OTA_RESPONSE_OK;
  }
  bool end() override {
    const bool ok = this->backend_->end() == ota::OTA_RESPONSE_OK;
    this->backend_.reset();
    return ok;
  }
  void abort() override {
    if (this->backend_)
      this->backend_->abort();
    this->backend_.reset();
  }

 protected:
  ota::OTABackendPtr backend_{nullptr};
};
#endif

class SettingsGuard : public AsyncWebHandler {
 public:
  explicit SettingsGuard(PrecisionFeeder *feeder) : feeder_(feeder) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const std::string url = request->url_to(buf).str();
    if (request->method() == HTTP_GET)
      return url == "/feeder/auth" || url == "/feeder/network";
    if (request->method() != HTTP_POST)
      return false;
    if (url == "/feeder/auth/password" || url == "/feeder/auth/check" || url == "/feeder/keys/api" ||
        url == "/feeder/keys/ble" || url == "/feeder/network/name" || url == "/feeder/network/ap" ||
        url == "/feeder/network/wifi" || url == "/feeder/setup")
      return true;
#ifdef USE_OTA
    if (url == "/feeder/firmware")
      return true;
#endif
    // POST /<domain>/<name>/<action>: protected when "<domain>/<name>" is on the list.
    const size_t action = url.rfind('/');
    return action != std::string::npos && action > 0 && this->feeder_->is_protected(url.substr(1, action - 1));
  }

  // The web server's task has a small stack (ESP-IDF's 4 KB plus 256 bytes in ESPHome 2026.9.1), and a protected write
  // passed on below runs the web server's own handler and the entity's action on top of ours. So everything of ours
  // (the URL buffer, the routes) lives in handle_own_(), whose frame is gone before the web server's handler starts.
  // Inlined into one frame, they overflowed that stack on every protected write (found on the bench, 2.0.0 N8).
  void handleRequest(AsyncWebServerRequest *request) override {
    if (!this->handle_own_(request))
      this->feeder_->web_server()->handleRequest(request);
  }

 protected:
  /// Answer one of our routes, or check a protected write. True if the request is answered (ours, or refused); false
  /// if it is an authorised write for the web server's own handler.
  __attribute__((noinline)) bool handle_own_(AsyncWebServerRequest *request) {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const std::string url = request->url_to(buf).str();
    if (request->method() == HTTP_GET) {
      if (url == "/feeder/network")
        this->send_network_(request);
      else
        this->send_challenge_(request);
      return true;
    }
    if (url == "/feeder/setup") {
      this->set_setup_(request);
      return true;
    }
#ifdef USE_OTA
    if (url == "/feeder/firmware") {
      this->finish_firmware_(request);
      return true;
    }
#endif
    if (url.rfind("/feeder/network/", 0) == 0) {
      this->set_network_(request, url);
      return true;
    }
    if (url == "/feeder/auth/password") {
      this->change_password_(request);
      return true;
    }
    if (url == "/feeder/keys/api" || url == "/feeder/keys/ble") {
      this->set_key_(request, url);
      return true;
    }
    if (url == "/feeder/auth/check") {  // "is this the password?", for the app's Unlock
      if (this->authorised_(request, url + "\n"))
        request->send(200, "application/json", "{}");
      return true;
    }
    // What the tag covers: the path and the value being written, so an answer cannot be moved to another request.
    std::string param;
    if (request->hasParam("value"))
      param = request->arg("value");
    else if (request->hasParam("option"))
      param = request->arg("option");
    return !this->authorised_(request, url + "\n" + param);  // refused: answered; authorised: the web server's
  }

  void send_challenge_(AsyncWebServerRequest *request) {
    uint8_t nonce[AUTH_NONCE_SIZE];
    Device d;
    uint32_t wait;
    KeySource ble = KeySource::NONE;
    bool ble_built;
    {
      LockGuard guard(this->feeder_->password_lock());
      this->feeder_->password_challenge(nonce);
      d = this->feeder_->device();
      wait = this->feeder_->password_wait_ms();
      ble_built = this->feeder_->ble_built();
      if (ble_built)
        ble = this->feeder_->ble_key_source();
    }
    std::memset(d.ble_key, 0, sizeof(d.ble_key));  // never sent; only where the key comes from
    char nonce_hex[2 * AUTH_NONCE_SIZE + 1], salt_hex[2 * PASSWORD_SALT_SIZE + 1];
    to_hex(nonce, AUTH_NONCE_SIZE, nonce_hex);
    to_hex(d.password_salt, PASSWORD_SALT_SIZE, salt_hex);
    char head[200];
    std::snprintf(head, sizeof(head), "{\"scheme\":%u,\"iterations\":%u,\"salt\":\"%s\",\"nonce\":\"%s\",\"wait\":%u,",
                  static_cast<unsigned>(d.password_scheme), static_cast<unsigned>(PASSWORD_ITERATIONS), salt_hex,
                  nonce_hex, static_cast<unsigned>(wait));
    static const char *const SOURCES[] = {"none", "build", "device", "off"};
    std::string body = head;
    // Where each key comes from: none (settable, not set yet), build (the YAML's), device (saved here) or off (this
    // build cannot have one). Never the keys themselves.
    body += "\"keys\":{\"api\":\"";
    body += SOURCES[static_cast<uint8_t>(this->feeder_->api_key_state())];
    body += "\",\"ble\":\"";
    body += ble_built ? SOURCES[static_cast<uint8_t>(ble)] : "off";
    body += "\"},";
    body += "\"protected\":[";
    bool first = true;
    for (const auto &name : this->feeder_->protected_entities()) {
      body += first ? "\"" : ",\"";
      body += name;  // entity names never contain quotes or backslashes (tests/test_consistency.py)
      body += '"';
      first = false;
    }
    body += "]}";
    request->send(200, "application/json", body.c_str());
  }

  /// Check the auth header against `message`. Answers nothing.
  AuthResult check_auth_(AsyncWebServerRequest *request, const std::string &message, uint32_t &wait) {
    uint8_t nonce[AUTH_NONCE_SIZE]{}, tag[AUTH_TAG_SIZE]{};
    const auto header = request->get_header(AUTH_HEADER);
    const bool parsed = header.has_value() && header->size() == 2 * (AUTH_NONCE_SIZE + AUTH_TAG_SIZE) &&
                        from_hex(*header, 0, nonce, AUTH_NONCE_SIZE) &&
                        from_hex(*header, 2 * AUTH_NONCE_SIZE, tag, AUTH_TAG_SIZE);
    wait = 0;
    AuthResult result;
    {
      LockGuard guard(this->feeder_->password_lock());
      if (!this->feeder_->password_set())
        return AuthResult::OK;
      result = parsed ? this->feeder_->password_check(nonce, tag, message, wait) : AuthResult::NO_CHALLENGE;
    }
    if (result == AuthResult::REFUSED)
      ESP_LOGW(TAG, "Settings password refused for %s", message.substr(0, message.find('\n')).c_str());
    return result;
  }

  /// Answer a request the settings password refused: 403, or 429 inside the wait after a failure.
  static void send_refusal_(AsyncWebServerRequest *request, AuthResult result, uint32_t wait) {
    static const char *const ERRORS[] = {"", "refused", "wait", "challenge"};
    char body[64];
    std::snprintf(body, sizeof(body), "{\"error\":\"%s\",\"wait\":%u}", ERRORS[static_cast<uint8_t>(result)],
                  static_cast<unsigned>(wait));
    // ESPHome's ESP-IDF request (2026.9.1) knows only 200, 204, 400, 401, 404, 409 and 422 and sends anything else as
    // 500 (found on the bench), so these two go straight to ESP-IDF, which takes any status line.
    httpd_req_t *raw = *request;
    httpd_resp_set_status(raw, result == AuthResult::WAIT ? "429 Too Many Requests" : "403 Forbidden");
    httpd_resp_set_type(raw, "application/json");
    httpd_resp_send(raw, body, HTTPD_RESP_USE_STRLEN);
  }

  /// Check the auth header against `message`; on failure answer the request and return false.
  bool authorised_(AsyncWebServerRequest *request, const std::string &message) {
    uint32_t wait;
    const AuthResult result = this->check_auth_(request, message, wait);
    if (result == AuthResult::OK)
      return true;
    send_refusal_(request, result, wait);
    return false;
  }

#ifdef USE_OTA
  // ---- Firmware upload (DESIGN.md section 9.5) ----
  // POST /feeder/firmware?sha256=<hex>, the image as the raw body (Content-Type application/octet-stream), signed
  // over the path and the hash as sent. The web server hands the body over in chunks before handleRequest().

 public:
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    // The flash writes run on the web server's small stack too, so the URL buffer and the check live in
    // start_firmware_(), gone before the first write (see handleRequest).
    if (index == 0)
      this->firmware_body_ = this->start_firmware_(request, total);
    if (this->firmware_body_)
      this->upload_.write(data, len);
  }

 protected:
  /// The first chunk of a body: if it is a firmware upload, check its signature and start it. False for any other
  /// request's body (ignored).
  __attribute__((noinline)) bool start_firmware_(AsyncWebServerRequest *request, size_t total) {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    if (request->url_to(buf).str() != "/feeder/firmware")
      return false;
    this->body_seen_ = true;
    const std::string sha = request->hasParam("sha256") ? request->arg("sha256") : std::string();
    uint8_t expected[crypto::Sha256::DIGEST_SIZE];
    const bool parsed = sha.size() == 2 * sizeof(expected) && from_hex(sha, 0, expected, sizeof(expected));
    this->auth_ = this->check_auth_(request, "/feeder/firmware\n" + sha, this->auth_wait_);
    this->upload_.start(parsed ? expected : nullptr, total, this->auth_ == AuthResult::OK, &this->sink_);
    if (this->upload_.status() == FirmwareUpload::Status::RECEIVING)
      ESP_LOGI(TAG, "Firmware upload started (%u bytes)", static_cast<unsigned>(total));
    return true;
  }

 protected:
  void finish_firmware_(AsyncWebServerRequest *request) {
    using S = FirmwareUpload::Status;
    if (!this->body_seen_)
      this->upload_.no_body();
    this->body_seen_ = false;
    const S status = this->upload_.finish();
    switch (status) {
      case S::OK:
        ESP_LOGI(TAG, "Firmware upload checked (SHA-256 matches); restarting into it");
        request->send(200, "application/json", "{\"restart\":true}");
        this->feeder_->restart_after_upload();
        return;
      case S::REFUSED:
        send_refusal_(request, this->auth_, this->auth_wait_);
        return;
      case S::BAD_REQUEST:
        request->send(400, "application/json", "{\"error\":\"request\"}");
        return;
      default: {
        static const char *const ERRORS[] = {"", "", "", "", "", "flash", "incomplete", "sha256", "image"};
        ESP_LOGW(TAG, "Firmware upload failed (%s); the running firmware stays", ERRORS[static_cast<uint8_t>(status)]);
        char body[48];
        std::snprintf(body, sizeof(body), "{\"error\":\"%s\"}", ERRORS[static_cast<uint8_t>(status)]);
        request->send(422, "application/json", body);
        return;
      }
    }
  }

  OtaSink sink_;
  FirmwareUpload upload_;
  AuthResult auth_{AuthResult::OK};
  uint32_t auth_wait_{0};
  bool body_seen_{false};
  bool firmware_body_{false};  // the body being received is a firmware upload
#endif

  /// POST /feeder/auth/password?salt=<hex>&key=<hex>: set or change it (key masked, see password_unmask), or clear
  /// it (no key). Needs the current password when one is set.
  void change_password_(AsyncWebServerRequest *request) {
    const std::string salt_hex = request->arg("salt"), key_hex = request->arg("key");
    if (!this->authorised_(request, "/feeder/auth/password\n" + salt_hex + key_hex))
      return;
    if (key_hex.empty()) {
      bool had;
      {
        LockGuard guard(this->feeder_->password_lock());
        had = this->feeder_->password_set();
      }
      if (had) {
        PrecisionFeeder *feeder = this->feeder_;
        feeder->run_on_main_loop([feeder]() {
          LockGuard guard(feeder->password_lock());
          feeder->clear_password("from the web app");
        });
      }
      request->send(200, "application/json", "{}");
      return;
    }
    uint8_t salt[PASSWORD_SALT_SIZE], masked[PASSWORD_HASH_SIZE], nonce[AUTH_NONCE_SIZE]{};
    const auto header = request->get_header(AUTH_HEADER);
    if (header.has_value())
      from_hex(*header, 0, nonce, AUTH_NONCE_SIZE);  // checked above when a password is set; unused otherwise
    if (!from_hex(salt_hex, 0, salt, PASSWORD_SALT_SIZE) || salt_hex.size() != 2 * PASSWORD_SALT_SIZE ||
        !from_hex(key_hex, 0, masked, PASSWORD_HASH_SIZE) || key_hex.size() != 2 * PASSWORD_HASH_SIZE) {
      request->send(400, "application/json", "{\"error\":\"format\"}");
      return;
    }
    // Unmasked now, with the key that checked this request; stored on the main loop.
    struct NewPassword {
      uint8_t salt[PASSWORD_SALT_SIZE];
      uint8_t key[PASSWORD_HASH_SIZE];
    };
    auto *p = new NewPassword;
    std::memcpy(p->salt, salt, PASSWORD_SALT_SIZE);
    {
      LockGuard guard(this->feeder_->password_lock());
      this->feeder_->password_unmask(nonce, masked, p->key);
    }
    PrecisionFeeder *feeder = this->feeder_;
    feeder->run_on_main_loop([feeder, p]() {
      {
        LockGuard guard(feeder->password_lock());
        feeder->set_password(p->salt, p->key);
      }
      std::memset(p, 0, sizeof(*p));
      delete p;
    });
    request->send(200, "application/json", "{}");
  }

  /// POST /feeder/keys/api?key=<hex> or /feeder/keys/ble?key=<hex>: save a 32-byte key, masked like a new
  /// password key (password_unmask: plain when no settings password is set); /feeder/keys/api with no key clears
  /// the API key. Signed over the path and the hex. The key is never sent back.
  void set_key_(AsyncWebServerRequest *request, const std::string &url) {
    const bool api = url == "/feeder/keys/api";
    const std::string key_hex = request->arg("key");
    if (!this->authorised_(request, url + "\n" + key_hex))
      return;
    bool possible;
    {
      LockGuard guard(this->feeder_->password_lock());
      possible = api ? PrecisionFeeder::api_key_settable() : this->feeder_->ble_built();
    }
    if (!possible) {  // a YAML key (change it there), or no API encryption / no BLE in this build
      request->send(409, "application/json", "{\"error\":\"build\"}");
      return;
    }
    PrecisionFeeder *feeder = this->feeder_;
    if (key_hex.empty()) {
      if (!api) {
        request->send(400, "application/json", "{\"error\":\"format\"}");
        return;
      }
      feeder->run_on_main_loop([feeder]() { feeder->set_api_key(nullptr); });
      request->send(200, "application/json", "{}");
      return;
    }
    uint8_t masked[DEVICE_KEY_SIZE], nonce[AUTH_NONCE_SIZE]{};
    const auto header = request->get_header(AUTH_HEADER);
    if (header.has_value())
      from_hex(*header, 0, nonce, AUTH_NONCE_SIZE);  // checked above when a password is set; unused otherwise
    static_assert(DEVICE_KEY_SIZE == PASSWORD_HASH_SIZE, "keys are masked like a password key");
    auto *key = new std::array<uint8_t, DEVICE_KEY_SIZE>{};
    bool ok = key_hex.size() == 2 * DEVICE_KEY_SIZE && from_hex(key_hex, 0, masked, DEVICE_KEY_SIZE);
    if (ok) {
      LockGuard guard(this->feeder_->password_lock());
      this->feeder_->password_unmask(nonce, masked, key->data());
    }
    uint8_t any = 0;
    for (uint8_t b : *key)
      any |= b;
    if (!ok || any == 0) {  // malformed, or the all-zero key that anyone could compute with
      delete key;
      request->send(400, "application/json", "{\"error\":\"format\"}");
      return;
    }
    feeder->run_on_main_loop([feeder, key, api]() {
      if (api) {
        feeder->set_api_key(key->data());
      } else {
        LockGuard guard(feeder->password_lock());
        feeder->set_ble_key(key->data());
      }
      key->fill(0);
      delete key;
    });
    request->send(200, "application/json", "{}");
  }

  /// GET /feeder/network: the name in use, the one saved here and the build's, where the access point's password
  /// comes from, and whether a Wi-Fi network can be saved. Never a password.
  void send_network_(AsyncWebServerRequest *request) {
    std::string in_use, saved;
    bool has_ap, restart, setup;
    KeySource ap = KeySource::BUILD;  // "ap" below: device (saved here), build (the YAML's), open (the build has none:
                                      // the prebuilt binaries) or off (no access point)
    {
      LockGuard guard(this->feeder_->password_lock());
      in_use = this->feeder_->hostname_in_use();
      saved = this->feeder_->hostname_saved();
      has_ap = this->feeder_->has_ap();
      ap = this->feeder_->ap_password_source();
      restart = this->feeder_->network_restart_needed();
      setup = this->feeder_->setup_needed();
    }
    // Names are checked hostnames (letters, digits, hyphens), so they need no escaping.
    std::string body = "{\"name\":\"" + in_use + "\",\"name_saved\":\"" + saved + "\",\"name_build\":\"" +
                       this->feeder_->hostname_build() + "\",\"ap\":\"" +
                       (has_ap ? (ap == KeySource::DEVICE ? "device" : this->feeder_->ap_build_open() ? "open" : "build")
                               : "off") +
                       "\",\"ap_build_open\":" + (this->feeder_->ap_build_open() ? "true" : "false") + ",\"wifi\":" +
                       (PrecisionFeeder::wifi_settable() ? "true" : "false") + ",\"restart\":" +
                       (restart ? "true" : "false") + ",\"setup\":" + (setup ? "true" : "false") + "}";
    request->send(200, "application/json", body.c_str());
  }

  /// POST /feeder/setup?done=1 (the setup wizard finished or skipped) or done=0 (offer it again). Signed.
  void set_setup_(AsyncWebServerRequest *request) {
    const std::string done = request->arg("done");
    if (!this->authorised_(request, "/feeder/setup\n" + done))
      return;
    if (done != "0" && done != "1") {
      request->send(400, "application/json", "{\"error\":\"format\"}");
      return;
    }
    PrecisionFeeder *feeder = this->feeder_;
    feeder->run_on_main_loop([feeder, done]() {
      LockGuard guard(feeder->password_lock());
      feeder->set_setup_done(done == "1");
    });
    request->send(200, "application/json", "{}");
  }

  /// A secret text sent masked, in hex (password_unmask_text), unmasked into `out`; false if malformed or too long.
  bool secret_arg_(AsyncWebServerRequest *request, const std::string &hex, std::string &out) {
    if (hex.size() % 2 != 0 || hex.size() > 2 * SECRET_TEXT_MAX)
      return false;
    uint8_t masked[SECRET_TEXT_MAX]{}, nonce[AUTH_NONCE_SIZE]{};
    const size_t n = hex.size() / 2;
    if (n > 0 && !from_hex(hex, 0, masked, n))
      return false;
    const auto header = request->get_header(AUTH_HEADER);
    if (header.has_value())
      from_hex(*header, 0, nonce, AUTH_NONCE_SIZE);  // checked already when a password is set; unused otherwise
    LockGuard guard(this->feeder_->password_lock());
    out = this->feeder_->password_unmask_text(nonce, masked, n);
    std::memset(masked, 0, sizeof(masked));
    return true;
  }

  /// POST /feeder/network/name?name=<text> (empty: the build's), /feeder/network/ap?password=<hex> (masked; empty:
  /// the build's) and /feeder/network/wifi?ssid=<hex>&password=<hex> (the password masked). Each is signed over the
  /// path and its parameters, as they are sent. The name and the access point's password apply at the next restart;
  /// a Wi-Fi network is joined at once.
  void set_network_(AsyncWebServerRequest *request, const std::string &url) {
    const std::string name = request->arg("name"), ssid_hex = request->arg("ssid"),
                      password_hex = request->arg("password");
    const std::string signed_part = url == "/feeder/network/name"  ? name
                                    : url == "/feeder/network/ap" ? password_hex
                                                                  : ssid_hex + password_hex;
    if (!this->authorised_(request, url + "\n" + signed_part))
      return;
    PrecisionFeeder *feeder = this->feeder_;
    auto bad = [request]() { request->send(400, "application/json", "{\"error\":\"format\"}"); };
    if (url == "/feeder/network/name") {
      if (!name.empty() && !check_hostname(name))
        return bad();
      feeder->run_on_main_loop([feeder, name]() {
        LockGuard guard(feeder->password_lock());
        feeder->set_hostname(name);
      });
    } else if (url == "/feeder/network/ap") {
      bool has_ap;
      {
        LockGuard guard(feeder->password_lock());
        has_ap = feeder->has_ap();
      }
      if (!has_ap) {
        request->send(409, "application/json", "{\"error\":\"build\"}");
        return;
      }
      auto *password = new std::string;
      if (!this->secret_arg_(request, password_hex, *password) ||
          (!password->empty() && !check_wifi_password(*password, false))) {
        delete password;
        return bad();
      }
      feeder->run_on_main_loop([feeder, password]() {
        {
          LockGuard guard(feeder->password_lock());
          feeder->set_ap_password(*password);
        }
        std::fill(password->begin(), password->end(), '\0');
        delete password;
      });
    } else {
      if (!PrecisionFeeder::wifi_settable()) {
        request->send(409, "application/json", "{\"error\":\"build\"}");
        return;
      }
      uint8_t ssid_bytes[WIFI_SSID_MAX];
      const size_t n = ssid_hex.size() / 2;
      if (ssid_hex.size() % 2 != 0 || n == 0 || n > WIFI_SSID_MAX || !from_hex(ssid_hex, 0, ssid_bytes, n))
        return bad();
      const std::string ssid(reinterpret_cast<const char *>(ssid_bytes), n);
      auto *password = new std::string;
      if (!check_ssid(ssid) || !this->secret_arg_(request, password_hex, *password) ||
          !check_wifi_password(*password, true)) {
        delete password;
        return bad();
      }
      feeder->run_on_main_loop([feeder, ssid, password]() {
        feeder->set_wifi(ssid, *password);
        std::fill(password->begin(), password->end(), '\0');
        delete password;
      });
    }
    request->send(200, "application/json", "{}");
  }

  PrecisionFeeder *feeder_;
};

void PrecisionFeeder::register_settings_guard() {
  if (this->web_server_ != nullptr && web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new SettingsGuard(this));  // lives as long as the device
}

bool PrecisionFeeder::is_protected(const std::string &entity) const {
  for (const auto &name : this->protected_) {
    if (name == entity)
      return true;
  }
  return false;
}
#endif

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
