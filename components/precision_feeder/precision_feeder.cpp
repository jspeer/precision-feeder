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

#include <array>
#include <cstdio>
#include <cstring>

#include <esp_random.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder";
static constexpr float SERVO_PERIOD_US = 20000.0f;  // 50 Hz

void PrecisionFeeder::setup() {
  // Each record layout has its own key (see SETTINGS_VERSION in feeder_core.h), so an older record can still be
  // loaded and migrated after an update instead of failing to load and being replaced by defaults.
  this->pref_settings_ = global_preferences->make_preference<Settings>(fnv1_hash("precision_feeder.settings.v4"));
  this->pref_calibration_ =
      global_preferences->make_preference<Calibration>(fnv1_hash("precision_feeder.calibration.v1"));
  this->pref_state_ = global_preferences->make_preference<State>(fnv1_hash("precision_feeder.state"));
  this->pref_device_ = global_preferences->make_preference<Device>(fnv1_hash("precision_feeder.device.v1"));
  Settings settings;
  Calibration calibration;
  State state;
  Device device;
  const bool have_settings = this->pref_settings_.load(&settings);
  const bool have_calibration = this->pref_calibration_.load(&calibration);
  const bool have_state = this->pref_state_.load(&state);
  const bool have_device = this->pref_device_.load(&device);
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
  this->core_start(have_settings ? &settings : nullptr, have_calibration ? &calibration : nullptr,
                   have_state ? &state : nullptr, have_v3 ? &v3 : nullptr, have_device ? &device : nullptr);
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

void PrecisionFeeder::apply_pins() {
  if (this->servo_pin_ == nullptr)
    return;  // a build without runtime pins: every driver keeps its YAML pin
  const Pins build{this->servo_pin_->get_pin(), this->button_pin_ != nullptr ? this->button_pin_->get_pin() : PIN_NONE,
                   this->led_output_ != nullptr ? this->led_build_pin_ : PIN_NONE};
  Device saved{};
  saved.servo_pin = saved.button_pin = saved.led_pin = PIN_BUILD;
  Device loaded;
  auto pref = global_preferences->make_preference<Device>(fnv1_hash("precision_feeder.device.v1"));
  if (pref.load(&loaded) && loaded.version == DEVICE_VERSION)
    saved = loaded;
  std::string problem;
  Pins use = resolve_pins(saved, build, &problem);
  const bool has_button = this->button_pin_ != nullptr, has_led = this->led_output_ != nullptr;
  if (!has_button)
    use.button = PIN_NONE;  // not in this build, whatever is saved
  if (!has_led)
    use.led = PIN_NONE;
  if (!problem.empty())
    ESP_LOGW(TAG, "Pins: %s", problem.c_str());

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
  ESP_LOGCONFIG(TAG, "  Sweep: %.1f deg, pitch %.1f deg, invert: %s", this->sweep_, this->sweep_ / NUM_POSITIONS,
                YESNO(this->invert_));
  ESP_LOGCONFIG(TAG, "  Pulse: %u-%u us (saved), step time %u ms, wiggle: %s, detach delay %u ms",
                (unsigned) this->calib.min_pulse_us, (unsigned) this->calib.max_pulse_us, (unsigned) this->step_ms_,
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
  const float span = static_cast<float>(this->calib.max_pulse_us) - static_cast<float>(this->calib.min_pulse_us);
  const float pulse = static_cast<float>(this->calib.min_pulse_us) + (angle / 180.0f) * span;
  this->output_->set_level(pulse / SERVO_PERIOD_US);
}

void PrecisionFeeder::hal_servo_release() { this->output_->set_level(0.0f); }

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

void PrecisionFeeder::hal_save_device(const Device &device) {
  this->pref_device_.save(&device);
  global_preferences->sync();
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

class SettingsGuard : public AsyncWebHandler {
 public:
  explicit SettingsGuard(PrecisionFeeder *feeder) : feeder_(feeder) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const std::string url = request->url_to(buf).str();
    if (request->method() == HTTP_GET)
      return url == "/feeder/auth";
    if (request->method() != HTTP_POST)
      return false;
    if (url == "/feeder/auth/password" || url == "/feeder/auth/check" || url == "/feeder/keys/api" ||
        url == "/feeder/keys/ble")
      return true;
    // POST /<domain>/<name>/<action>: protected when "<domain>/<name>" is on the list.
    const size_t action = url.rfind('/');
    return action != std::string::npos && action > 0 && this->feeder_->is_protected(url.substr(1, action - 1));
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const std::string url = request->url_to(buf).str();
    if (request->method() == HTTP_GET) {
      this->send_challenge_(request);
      return;
    }
    if (url == "/feeder/auth/password") {
      this->change_password_(request);
      return;
    }
    if (url == "/feeder/keys/api" || url == "/feeder/keys/ble") {
      this->set_key_(request, url);
      return;
    }
    if (url == "/feeder/auth/check") {  // "is this the password?", for the app's Unlock
      if (this->authorised_(request, url + "\n"))
        request->send(200, "application/json", "{}");
      return;
    }
    // What the tag covers: the path and the value being written, so an answer cannot be moved to another request.
    std::string param;
    if (request->hasParam("value"))
      param = request->arg("value");
    else if (request->hasParam("option"))
      param = request->arg("option");
    if (!this->authorised_(request, url + "\n" + param))
      return;
    this->feeder_->web_server()->handleRequest(request);
  }

 protected:
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

  /// Check the auth header against `message`; on failure answer the request and return false.
  bool authorised_(AsyncWebServerRequest *request, const std::string &message) {
    uint8_t nonce[AUTH_NONCE_SIZE]{}, tag[AUTH_TAG_SIZE]{};
    const auto header = request->get_header(AUTH_HEADER);
    const bool parsed = header.has_value() && header->size() == 2 * (AUTH_NONCE_SIZE + AUTH_TAG_SIZE) &&
                        from_hex(*header, 0, nonce, AUTH_NONCE_SIZE) &&
                        from_hex(*header, 2 * AUTH_NONCE_SIZE, tag, AUTH_TAG_SIZE);
    uint32_t wait = 0;
    AuthResult result;
    {
      LockGuard guard(this->feeder_->password_lock());
      if (!this->feeder_->password_set())
        return true;
      result = parsed ? this->feeder_->password_check(nonce, tag, message, wait) : AuthResult::NO_CHALLENGE;
    }
    if (result == AuthResult::OK)
      return true;
    static const char *const ERRORS[] = {"", "refused", "wait", "challenge"};
    char body[64];
    std::snprintf(body, sizeof(body), "{\"error\":\"%s\",\"wait\":%u}", ERRORS[static_cast<uint8_t>(result)],
                  static_cast<unsigned>(wait));
    if (result == AuthResult::REFUSED)
      ESP_LOGW(TAG, "Settings password refused for %s", message.substr(0, message.find('\n')).c_str());
    request->send(result == AuthResult::WAIT ? 429 : 403, "application/json", body);
    return false;
  }

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
