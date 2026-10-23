// Precision Feeder TLS: HTTPS for the web app, the trust store, MQTT over TLS (firmware 2.4.1) and ACME (2.4.2).
//
// The decisions (what port 80 does, how MQTT connects, what to pin) are plain C++ in tls_policy.h, and the ACME
// client (requests, JWS, renewal) in acme.h, both tested on a host.
// This class does the device's side: the certificates in NVS, the TLS relay on port 443, the web endpoints, and the
// MQTT client's TLS settings. Its tasks never call FeederCore; changes reach the main loop through defer().
// DESIGN.md section 9.7; docs/plans/https-tls.md.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/precision_feeder/precision_feeder.h"
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

#include "tls_policy.h"
#ifdef USE_PRECISION_FEEDER_ACME
#include "acme.h"
#endif

namespace esphome {
namespace precision_feeder {

/// The certificate and key HTTPS serves, with what the web app shows about it. Replaced whole, never changed, so a
/// session that started with one keeps it to the end.
struct TlsIdentity;

class PrecisionFeederTls : public Component, public PrecisionFeeder::RequestFilter {
 public:
  // ---- configuration (set from Python) ----
  void set_feeder(PrecisionFeeder *feeder) { this->feeder_ = feeder; }
  void set_max_sessions(uint8_t n) { this->max_sessions_ = n; }
  void set_idle_timeout_s(uint32_t s) { this->idle_timeout_s_ = s; }
  void set_port(uint16_t port) { this->port_ = port; }

  // ---- settings (from the package's entities; saved there) ----
  void set_redirect(bool on) { this->redirect_ = on; }
  void set_https_only(bool on) { this->https_only_ = on; }
#ifdef USE_PRECISION_FEEDER_ACME
  /// The ACME server's directory URL (empty: ACME off) and the name to ask for (empty: <name>.local). From the
  /// package's entities, at boot and when changed; a change asks again at once, with no restart.
  void set_acme_directory(const std::string &url);
  void set_acme_name(const std::string &name);
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  // Before the MQTT client's settings are applied (on_boot, priority 600) and before the web server starts.
  float get_setup_priority() const override { return setup_priority::DATA + 50.0f; }

  // ---- PrecisionFeeder::RequestFilter: port 80 once HTTPS runs ----
  bool wants(AsyncWebServerRequest *request) const override;
  void answer(AsyncWebServerRequest *request) override;

  // ---- status ----
  bool https_ready() const { return this->https_ready_.load(); }
  /// "self-signed feeder-b00604.local until 2049-12-31" and so on, for the HTTPS text sensor.
  std::string https_text();
  /// "off", "CA", "pinned ab:cd:..", "no CA certificate", "pinning..." and so on, for the MQTT TLS text sensor.
  std::string mqtt_text();
#ifdef USE_PRECISION_FEEDER_ACME
  /// "off", "certificate for <name> until <date>, renewed from <date>", "failed: <why>; next try in 15 min" and so
  /// on, for the ACME text sensor.
  std::string acme_text();
#endif
  /// GET /feeder/tls: everything above as JSON, for the web app. Safe on the web server's task.
  std::string status_json();

#ifdef USE_MQTT
  /// Give the MQTT client its TLS settings before it first connects (packages/mqtt_tls.yaml, at boot). With TLS on
  /// it never connects in plain text: without a CA (or a pin) it is not started. `pin`: check against the
  /// certificate the broker showed on first contact instead of the CA (D36). `enabled`: MQTT is on with a broker
  /// this boot, so a first contact may be made.
  void configure_mqtt(mqtt::MQTTClientComponent *client, bool tls, bool pin, bool enabled, const std::string &broker,
                      uint16_t port);
  /// Whether turning MQTT on after boot may start the client now: false while TLS waits for a pin or a CA (the client
  /// holds a placeholder certificate then), so the change waits for a restart instead.
  bool mqtt_can_start() const { return this->mqtt_startable_; }
#endif
  /// Forget the MQTT broker's pinned certificate: the next start pins again. Main loop.
  void forget_mqtt_pin();

  // ---- web endpoints (the web server's task) ----
  void handle_body(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
  void handle_request(AsyncWebServerRequest *request, const std::string &url);

  // ---- tasks (static entry points in the .cpp) ----
  void identity_task_();
  void listen_task_();
  void relay_task_(int client);
  void pin_task_();
#ifdef USE_PRECISION_FEEDER_ACME
  void acme_task_();
  void reselect_task_();
  /// The http-01 answer for a token, or empty (the web server's task).
  std::string acme_challenge(const std::string &token);
  void set_acme_challenge_(const std::string &token, const std::string &key_authorization);
#endif

 protected:
  std::shared_ptr<TlsIdentity> identity_();
  /// The identity to serve now: ACME's while it may be served, else an uploaded one that checks, else the
  /// self-signed one (made if missing or made for another name). Needs a big stack; nullptr if none could be made.
  std::shared_ptr<TlsIdentity> pick_identity_();
  void set_identity_(std::shared_ptr<TlsIdentity> identity);
  std::string redirect_host_();
  std::string local_name_();
  bool make_self_signed_(std::string &cert, std::string &key, const std::string &name);
  /// Work that needs a large stack (key generation, certificate checks), run in its own task while the caller waits.
  bool run_big_(std::function<bool()> &&work);

  PrecisionFeeder *feeder_{nullptr};
  uint8_t max_sessions_{4};
  uint32_t idle_timeout_s_{20};
  uint16_t port_{443};
  std::atomic<bool> redirect_{true};
  std::atomic<bool> https_only_{false};
  std::atomic<bool> https_ready_{false};
  std::atomic<uint8_t> sessions_{0};
  bool started_{false};

  Mutex lock_;  // identity_, ca_pem_, pin_pem_, the status strings
  std::shared_ptr<TlsIdentity> identity_ptr_;
  std::string ca_pem_;
  std::string ca_text_;
  std::string pin_pem_;
  std::string pin_text_;
  std::string mqtt_status_{"off"};
  std::string identity_error_;

  // An upload in progress (one at a time; the web server serves one request at a time).
  std::string body_;
  bool body_too_big_{false};

#ifdef USE_PRECISION_FEEDER_ACME
  void acme_check_();
  std::string acme_name_in_use_();         // under lock_
  tls::acme::Issued issued_now_();          // under lock_: issued_, with `same` for the settings now
  void serve_identity_(std::shared_ptr<TlsIdentity> identity);

  // Under lock_: the settings, what ACME last issued (from ac_cert, ac_name, ac_dir), the status, the challenge.
  std::string acme_dir_, acme_name_;
  std::string issued_name_, issued_dir_, issued_until_;
  tls::acme::Issued issued_;
  std::string acme_error_;  // why the last attempt failed, until one succeeds
  std::string challenge_token_, challenge_auth_;
  uint32_t acme_failures_{0};
  int64_t acme_next_try_{0};
  std::atomic<bool> acme_running_{false};  // a task (ACME or reselect) runs
  std::atomic<bool> acme_now_{false};      // renew now, or the settings changed
  std::atomic<bool> reselect_{false};      // pick the identity again (ACME turned off)
  uint32_t acme_checked_{0};  // millis() of the last check (main loop)
#endif

#ifdef USE_MQTT
  mqtt::MQTTClientComponent *mqtt_{nullptr};
  std::string mqtt_broker_;
  uint16_t mqtt_port_{8883};
  std::atomic<bool> pin_running_{false};
  std::atomic<bool> pin_done_{false};  // a pin task finished with a new pin, for loop()
  uint32_t pin_retry_at_{0};
  bool pin_wanted_{false};  // FETCH_PIN at boot: start MQTT once pinned
  bool mqtt_startable_{true};
#endif
};

}  // namespace precision_feeder
}  // namespace esphome
