// Precision Feeder TLS: the device's side (see precision_feeder_tls.h; the decisions are in tls_policy.h).
//
// Written against ESP-IDF's esp-tls and mbedTLS APIs (Apache-2.0); nothing is copied from ESPHome or ESP-IDF's
// examples.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder_tls.h"

#include <cstring>
#include <ctime>
#include <strings.h>
#include <functional>
#include <vector>

#include <esp_http_server.h>
#include <esp_random.h>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <nvs.h>
#ifdef USE_PRECISION_FEEDER_ACME
#include <esp_http_client.h>
#include <mbedtls/x509_csr.h>
#endif

#include "acme.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/web_server_base/web_server_base.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder_tls";
static const char *const NVS_NAMESPACE = "pf_tls";  // new in 2.4.1; no existing record changes
// Keys: "ss_cert", "ss_key", "ss_name" (the self-signed identity and the name it was made for), "up_cert", "up_key"
// (an uploaded identity), "ca" (the trust store), "mq_pin" (MQTT's pinned broker certificate); since 2.4.2 "ac_acct"
// (the ACME account key), "ac_cert", "ac_key" (the identity ACME issued), "ac_name", "ac_dir" (the name it was
// issued for and the directory it came from).

static constexpr uint32_t BIG_STACK = 10240;   // key generation, certificate checks, a TLS client handshake
static constexpr uint32_t RELAY_STACK = 6144;  // one HTTPS session (measured on the board, docs/BENCH.md)
static constexpr uint32_t PIN_RETRY_MS = 60000;
static const char *const PLACEHOLDER_CA = "none";  // not a certificate: a TLS client given it can never connect

// ---- small helpers -------------------------------------------------------------------------------------------

static int rng(void *, unsigned char *buf, size_t len) {
  esp_fill_random(buf, len);
  return 0;
}

static std::string nvs_get(const char *key) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    return {};
  size_t len = 0;
  std::string out;
  if (nvs_get_blob(h, key, nullptr, &len) == ESP_OK && len > 0) {
    out.resize(len);
    if (nvs_get_blob(h, key, out.data(), &len) != ESP_OK)
      out.clear();
  }
  nvs_close(h);
  return out;
}

/// Save a value (empty: erase it). False if flash refused it.
static bool nvs_put(const char *key, const std::string &value) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = value.empty() ? nvs_erase_key(h, key) : nvs_set_blob(h, key, value.data(), value.size());
  if (err == ESP_ERR_NVS_NOT_FOUND)
    err = ESP_OK;
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

static std::string json_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      out += c;
    }
  }
  return out;
}

static std::string hex_fingerprint(const unsigned char *der, size_t len, size_t bytes = 32) {
  unsigned char h[32];
  mbedtls_sha256(der, len, h, 0);
  static const char *const HEX = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < bytes && i < 32; i++) {
    if (i)
      out += ':';
    out += HEX[h[i] >> 4];
    out += HEX[h[i] & 15];
  }
  return out;
}

/// What the web app shows about a certificate: its first DNS name, its subject, when it expires, its fingerprint.
struct CertFacts {
  std::string dns, subject, not_after, fingerprint;
  int64_t from{0}, until{0};  // seconds since 1970, UTC
  bool ok{false};
};

static CertFacts facts_of(const mbedtls_x509_crt &crt) {
  CertFacts f;
  char buf[160];
  if (mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.subject) > 0)
    f.subject = buf;
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day);
  f.not_after = buf;
  f.fingerprint = hex_fingerprint(crt.raw.p, crt.raw.len);
  f.from = tls::acme::utc_seconds(crt.valid_from.year, crt.valid_from.mon, crt.valid_from.day, crt.valid_from.hour,
                                  crt.valid_from.min, crt.valid_from.sec);
  f.until = tls::acme::utc_seconds(crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day, crt.valid_to.hour,
                                   crt.valid_to.min, crt.valid_to.sec);
  for (const mbedtls_x509_sequence *san = &crt.subject_alt_names; san != nullptr && san->buf.p != nullptr;
       san = san->next) {
    mbedtls_x509_subject_alternative_name name;
    if (mbedtls_x509_parse_subject_alt_name(&san->buf, &name) != 0)
      continue;
    if (name.type == MBEDTLS_X509_SAN_DNS_NAME && f.dns.empty())
      f.dns.assign(reinterpret_cast<const char *>(name.san.unstructured_name.p), name.san.unstructured_name.len);
    mbedtls_x509_free_subject_alt_name(&name);
  }
  f.ok = true;
  return f;
}

static CertFacts facts_of_pem(const std::string &pem) {
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  CertFacts f;
  if (mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1) >= 0 &&
      crt.raw.p != nullptr)
    f = facts_of(crt);
  mbedtls_x509_crt_free(&crt);
  return f;
}

/// The clock is set (NTP or Home Assistant): certificate dates can be checked.
static bool clock_valid() { return std::time(nullptr) > 1700000000; }

// ---- the identity --------------------------------------------------------------------------------------------

struct TlsIdentity {
  std::string cert, key;  // PEM; std::string keeps the terminating NUL that mbedTLS's parser wants
  std::string source;     // "self-signed", "uploaded" or "ACME"
  CertFacts facts;
  esp_tls_cfg_server_t cfg{};
  bool tickets{false};

  void prepare() {
    this->cfg.servercert_buf = reinterpret_cast<const unsigned char *>(this->cert.c_str());
    this->cfg.servercert_bytes = this->cert.size() + 1;
    this->cfg.serverkey_buf = reinterpret_cast<const unsigned char *>(this->key.c_str());
    this->cfg.serverkey_bytes = this->key.size() + 1;
    this->tickets = esp_tls_cfg_server_session_tickets_init(&this->cfg) == ESP_OK;
    this->facts = facts_of_pem(this->cert);
  }
  ~TlsIdentity() {
    if (this->tickets)
      esp_tls_cfg_server_session_tickets_free(&this->cfg);
  }
};

std::shared_ptr<TlsIdentity> PrecisionFeederTls::identity_() {
  LockGuard guard(this->lock_);
  return this->identity_ptr_;
}

void PrecisionFeederTls::set_identity_(std::shared_ptr<TlsIdentity> identity) {
  LockGuard guard(this->lock_);
  this->identity_ptr_ = std::move(identity);
}

bool PrecisionFeederTls::make_self_signed_(std::string &cert, std::string &key, const std::string &name) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  uint8_t serial[16];
  esp_fill_random(serial, sizeof(serial));
  tls::fix_serial(serial, sizeof(serial));
  mbedtls_x509_san_list san{};
  san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
  san.node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(const_cast<char *>(name.c_str()));
  san.node.san.unstructured_name.len = name.size();
  const std::string dn = "CN=" + name + ",O=Precision Feeder";
  std::vector<unsigned char> buf(1200);
  bool ok = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, nullptr) == 0;
  if (ok) {
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    // A fixed, long validity: the feeder has no clock at its first boot (docs/plans/https-tls.md).
    ok = mbedtls_x509write_crt_set_subject_name(&crt, dn.c_str()) == 0 &&
         mbedtls_x509write_crt_set_issuer_name(&crt, dn.c_str()) == 0 &&
         mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) == 0 &&
         mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959") == 0 &&
         mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) == 0 &&
         mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san) == 0 &&
         mbedtls_x509write_crt_pem(&crt, buf.data(), buf.size(), rng, nullptr) == 0;
  }
  if (ok) {
    cert = reinterpret_cast<const char *>(buf.data());
    ok = mbedtls_pk_write_key_pem(&pk, buf.data(), buf.size()) == 0;
    if (ok)
      key = reinterpret_cast<const char *>(buf.data());
  }
  std::fill(buf.begin(), buf.end(), 0);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&pk);
  return ok;
}

/// Check an uploaded identity: every certificate parses, the key parses and matches the first certificate, and when
/// a CA is in the trust store the chain verifies against it (dates only once the clock is set). `error` says why not.
static bool check_identity(const std::string &cert, const std::string &key, const std::string &ca, std::string &error) {
  mbedtls_x509_crt chain, anchor;
  mbedtls_pk_context pk;
  mbedtls_x509_crt_init(&chain);
  mbedtls_x509_crt_init(&anchor);
  mbedtls_pk_init(&pk);
  bool ok = false;
  do {
    if (tls::count_pem(cert, "CERTIFICATE") == 0 ||
        mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char *>(cert.c_str()), cert.size() + 1) != 0) {
      error = "certificate";
      break;
    }
    if (mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char *>(key.c_str()), key.size() + 1, nullptr, 0,
                             rng, nullptr) != 0) {
      error = "key";
      break;
    }
    if (mbedtls_pk_check_pair(&chain.pk, &pk, rng, nullptr) != 0) {
      error = "key does not match the certificate";
      break;
    }
    if (!ca.empty()) {
      uint32_t flags = 0;
      if (mbedtls_x509_crt_parse(&anchor, reinterpret_cast<const unsigned char *>(ca.c_str()), ca.size() + 1) != 0) {
        error = "trust store";
        break;
      }
      mbedtls_x509_crt_verify(&chain, &anchor, nullptr, nullptr, &flags, nullptr, nullptr);
      if (!clock_valid())
        flags &= ~static_cast<uint32_t>(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
      if (flags != 0) {
        error = "chain does not verify against the CA certificate";
        break;
      }
    }
    ok = true;
  } while (false);
  mbedtls_x509_crt_free(&chain);
  mbedtls_x509_crt_free(&anchor);
  mbedtls_pk_free(&pk);
  return ok;
}

/// Check a CA upload: one or more certificates, every one of them parses. `facts` describes the first.
static bool check_ca(const std::string &pem, CertFacts &facts) {
  if (tls::count_pem(pem, "CERTIFICATE") == 0)
    return false;
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  const bool ok =
      mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1) == 0;
  if (ok)
    facts = facts_of(crt);
  mbedtls_x509_crt_free(&crt);
  return ok;
}

struct BigJob {
  std::function<bool()> work;
  bool result{false};
  SemaphoreHandle_t done{nullptr};
};

static void big_task(void *arg) {
  auto *job = static_cast<BigJob *>(arg);
  job->result = job->work();
  xSemaphoreGive(job->done);
  vTaskDelete(nullptr);
}

bool PrecisionFeederTls::run_big_(std::function<bool()> &&work) {
  auto *job = new BigJob();
  job->work = std::move(work);
  job->done = xSemaphoreCreateBinary();
  if (job->done == nullptr || xTaskCreate(big_task, "tls_work", BIG_STACK, job, 2, nullptr) != pdPASS) {
    if (job->done != nullptr)
      vSemaphoreDelete(job->done);
    delete job;
    return false;
  }
  // Waits for as long as it takes: the work refers to the caller's stack.
  xSemaphoreTake(job->done, portMAX_DELAY);
  const bool result = job->result;
  vSemaphoreDelete(job->done);
  delete job;
  return result;
}

// ---- start-up ------------------------------------------------------------------------------------------------

void PrecisionFeederTls::setup() {
  this->ca_pem_ = nvs_get("ca");
  if (!this->ca_pem_.empty()) {
    const CertFacts f = facts_of_pem(this->ca_pem_);
    this->ca_text_ = f.ok ? f.subject : "unreadable";
  }
  this->pin_pem_ = nvs_get("mq_pin");
  if (!this->pin_pem_.empty())
    this->pin_text_ = facts_of_pem(this->pin_pem_).fingerprint.substr(0, 23);
  this->feeder_->set_request_filter(this);
#ifdef USE_PRECISION_FEEDER_ACME
  {
    const CertFacts f = facts_of_pem(nvs_get("ac_cert"));
    if (f.ok) {
      this->issued_.have = true;
      this->issued_.not_before = f.from;
      this->issued_.not_after = f.until;
      this->issued_until_ = f.not_after;
      this->issued_name_ = nvs_get("ac_name");
      this->issued_dir_ = nvs_get("ac_dir");
    }
  }
#endif

  class Endpoints : public AsyncWebHandler {
   public:
    explicit Endpoints(PrecisionFeederTls *parent) : parent_(parent) {}
    bool canHandle(AsyncWebServerRequest *request) const override {
      char buf[AsyncWebServerRequest::URL_BUF_SIZE];
      const std::string url = request->url_to(buf).str();
      if (request->method() == HTTP_GET)
        return url == "/feeder/tls";
      return request->method() == HTTP_POST &&
             (url == "/feeder/tls/ca" || url == "/feeder/tls/identity" || url == "/feeder/tls/self-signed" ||
              url == "/feeder/tls/mqtt-pin/forget" || url == "/feeder/tls/acme/renew");
    }
    void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
      this->parent_->handle_body(request, data, len, index, total);
    }
    void handleRequest(AsyncWebServerRequest *request) override {
      char buf[AsyncWebServerRequest::URL_BUF_SIZE];
      this->parent_->handle_request(request, request->url_to(buf).str());
    }

   protected:
    PrecisionFeederTls *parent_;
  };
  if (web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new Endpoints(this));  // lives as long as the device

#ifdef USE_PRECISION_FEEDER_ACME
  // ACME's http-01 challenge on port 80: the key authorization for the one token being asked about, else nothing (the
  // request goes on to the other handlers and ends in a 404).
  class Challenge : public AsyncWebHandler {
   public:
    explicit Challenge(PrecisionFeederTls *parent) : parent_(parent) {}
    bool canHandle(AsyncWebServerRequest *request) const override {
      if (request->method() != HTTP_GET)
        return false;
      char buf[AsyncWebServerRequest::URL_BUF_SIZE];
      const std::string url = request->url_to(buf).str();
      const size_t n = std::strlen(tls::ACME_CHALLENGE_PREFIX);
      return url.size() > n && url.compare(0, n, tls::ACME_CHALLENGE_PREFIX) == 0 &&
             !this->parent_->acme_challenge(url.substr(n)).empty();
    }
    void handleRequest(AsyncWebServerRequest *request) override {
      char buf[AsyncWebServerRequest::URL_BUF_SIZE];
      const std::string url = request->url_to(buf).str();
      const std::string answer = this->parent_->acme_challenge(url.substr(std::strlen(tls::ACME_CHALLENGE_PREFIX)));
      ESP_LOGI(TAG, "ACME challenge fetched");
      request->send(200, "application/octet-stream", answer.c_str());
    }

   protected:
    PrecisionFeederTls *parent_;
  };
  if (web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new Challenge(this));
#endif
}

static void identity_entry(void *arg) {
  static_cast<PrecisionFeederTls *>(arg)->identity_task_();
  vTaskDelete(nullptr);
}

#ifdef USE_MQTT
static void pin_entry(void *arg) {
  static_cast<PrecisionFeederTls *>(arg)->pin_task_();
  vTaskDelete(nullptr);
}
#endif

void PrecisionFeederTls::loop() {
  if (!this->started_) {
    // The first pass: every component is set up, so the device's name is the one it will keep.
    this->started_ = true;
    if (xTaskCreate(identity_entry, "tls_start", BIG_STACK, this, 2, nullptr) != pdPASS)
      ESP_LOGE(TAG, "HTTPS did not start (no memory for its task)");
  }
#ifdef USE_PRECISION_FEEDER_ACME
  this->acme_check_();
#endif
#ifdef USE_MQTT
  if (this->pin_done_.exchange(false)) {
    std::string pin;
    {
      LockGuard guard(this->lock_);
      pin = this->pin_pem_;
    }
    if (this->pin_wanted_ && this->mqtt_ != nullptr && !pin.empty()) {
      this->pin_wanted_ = false;
      this->mqtt_->set_ca_certificate(pin.c_str());
      this->mqtt_->set_skip_cert_cn_check(true);
      this->mqtt_->enable();
    } else if (!pin.empty()) {
      this->feeder_->note_restart_needed("MQTT broker certificate pinned");
    }
  }
  if (this->pin_wanted_ && !this->pin_running_ && millis() >= this->pin_retry_at_) {
    this->pin_retry_at_ = millis() + PIN_RETRY_MS;
    this->pin_running_ = true;
    if (xTaskCreate(pin_entry, "tls_pin", BIG_STACK, this, 2, nullptr) != pdPASS)
      this->pin_running_ = false;
  }
#endif
}

std::string PrecisionFeederTls::local_name_() { return std::string(App.get_name().c_str()) + ".local"; }

std::shared_ptr<TlsIdentity> PrecisionFeederTls::pick_identity_() {
  const std::string name = this->local_name_();
  std::string ca;
  {
    LockGuard guard(this->lock_);
    ca = this->ca_pem_;
  }
  auto identity = std::make_shared<TlsIdentity>();
#ifdef USE_PRECISION_FEEDER_ACME
  // ACME's certificate wins while ACME is on, the certificate is for the name and directory set now, and it is within
  // its dates (as far as the clock can tell).
  bool acme_ok;
  {
    LockGuard guard(this->lock_);
    acme_ok = tls::acme::servable(std::time(nullptr), clock_valid(), this->issued_now_());
  }
  if (acme_ok) {
    identity->cert = nvs_get("ac_cert");
    identity->key = nvs_get("ac_key");
    std::string error;
    if (check_identity(identity->cert, identity->key, ca, error))
      identity->source = "ACME";
    else
      ESP_LOGW(TAG, "The ACME certificate does not check (%s)", error.c_str());
  }
#endif
  // Then an uploaded identity if it still checks; else the self-signed one, made again when the name changed.
  if (identity->source.empty()) {
    identity->cert = nvs_get("up_cert");
    identity->key = nvs_get("up_key");
    if (!identity->cert.empty()) {
      std::string error;
      if (check_identity(identity->cert, identity->key, ca, error)) {
        identity->source = "uploaded";
      } else {
        ESP_LOGW(TAG, "The uploaded certificate no longer checks (%s); using the self-signed one", error.c_str());
        LockGuard guard(this->lock_);
        this->identity_error_ = error;
      }
    }
  }
  if (identity->source.empty()) {
    identity->cert = nvs_get("ss_cert");
    identity->key = nvs_get("ss_key");
    if (identity->cert.empty() || identity->key.empty() || nvs_get("ss_name") != name) {
      const uint32_t start = millis();
      if (!this->make_self_signed_(identity->cert, identity->key, name)) {
        ESP_LOGE(TAG, "Could not make a self-signed certificate");
        return nullptr;
      }
      nvs_put("ss_cert", identity->cert);
      nvs_put("ss_key", identity->key);
      nvs_put("ss_name", name);
      ESP_LOGI(TAG, "Made a self-signed certificate for %s (%u ms)", name.c_str(), (unsigned) (millis() - start));
    }
    identity->source = "self-signed";
  }
  identity->prepare();
  return identity;
}

void PrecisionFeederTls::identity_task_() {
  auto identity = this->pick_identity_();
  if (identity == nullptr) {
    ESP_LOGE(TAG, "HTTPS is off: no certificate");
    return;
  }
  this->set_identity_(identity);
  this->listen_task_();  // runs for as long as the device does
}

// ---- the HTTPS relay -----------------------------------------------------------------------------------------

struct RelayArgs {
  PrecisionFeederTls *parent;
  int client;
};

static void relay_entry(void *arg) {
  auto *a = static_cast<RelayArgs *>(arg);
  PrecisionFeederTls *parent = a->parent;
  const int client = a->client;
  delete a;
  parent->relay_task_(client);
  vTaskDelete(nullptr);
}

void PrecisionFeederTls::listen_task_() {
  // Dual stack where the build has IPv6 (an IPv6 socket bound to any address accepts IPv4 too in lwIP), else IPv4.
  int s = ::socket(AF_INET6, SOCK_STREAM, 0);
  bool v6 = s >= 0;
  if (!v6)
    s = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int rc;
  if (v6) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(this->port_);
    rc = bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(this->port_);
    rc = bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  }
  if (s < 0 || rc != 0 || listen(s, 3) != 0) {
    ESP_LOGE(TAG, "HTTPS could not listen on port %u", this->port_);
    if (s >= 0)
      close(s);
    return;
  }
  this->https_ready_ = true;
  ESP_LOGI(TAG, "HTTPS on port %u, at most %u sessions", this->port_, this->max_sessions_);
  for (;;) {
    // A connection beyond the cap waits in the backlog until a session ends (relay_task_ lowers sessions_).
    while (this->sessions_.load() >= this->max_sessions_)
      vTaskDelay(pdMS_TO_TICKS(50));
    const int c = accept(s, nullptr, nullptr);
    if (c < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    auto *args = new RelayArgs{this, c};
    this->sessions_++;
    if (xTaskCreate(relay_entry, "tls_relay", RELAY_STACK, args, 5, nullptr) != pdPASS) {
      delete args;
      close(c);
      this->sessions_--;
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void PrecisionFeederTls::relay_task_(int client) {
  std::shared_ptr<TlsIdentity> identity = this->identity_();  // kept for the whole session
  esp_tls_t *tls = esp_tls_init();
  int backend = -1;
  timeval handshake_timeout{10, 0};
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &handshake_timeout, sizeof(handshake_timeout));
  if (tls != nullptr && identity != nullptr && esp_tls_server_session_create(&identity->cfg, client, tls) == 0) {
    timeval none{0, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    backend = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (backend >= 0 && connect(backend, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      char buf[1024];
      bool open = true;
      while (open) {
        if (esp_tls_get_bytes_avail(tls) == 0) {
          fd_set rd;
          FD_ZERO(&rd);
          FD_SET(client, &rd);
          FD_SET(backend, &rd);
          timeval idle{static_cast<long>(this->idle_timeout_s_), 0};
          const int maxfd = client > backend ? client : backend;
          if (::select(maxfd + 1, &rd, nullptr, nullptr, &idle) <= 0)
            break;  // idle (or an error): give the session back
          if (FD_ISSET(backend, &rd)) {
            const int n = recv(backend, buf, sizeof(buf), 0);
            if (n <= 0)
              break;
            for (int off = 0; off < n;) {
              const int w = esp_tls_conn_write(tls, buf + off, n - off);
              if (w <= 0) {
                open = false;
                break;
              }
              off += w;
            }
          }
          if (!open || !FD_ISSET(client, &rd))
            continue;
        }
        const int n = esp_tls_conn_read(tls, buf, sizeof(buf));
        if (n <= 0 || send(backend, buf, n, 0) != n)
          break;
      }
    }
  }
  if (backend >= 0)
    close(backend);
  if (tls != nullptr)
    esp_tls_server_session_delete(tls);  // closes the client's socket
  else
    close(client);
  this->sessions_--;
}

// ---- port 80 -------------------------------------------------------------------------------------------------

static bool from_loopback(AsyncWebServerRequest *request) {
  httpd_req_t *r = *request;
  const int fd = httpd_req_to_sockfd(r);
  sockaddr_storage a{};
  socklen_t len = sizeof(a);
  if (fd < 0 || getpeername(fd, reinterpret_cast<sockaddr *>(&a), &len) != 0)
    return false;
  if (a.ss_family == AF_INET)
    return reinterpret_cast<sockaddr_in *>(&a)->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
#if LWIP_IPV6
  if (a.ss_family == AF_INET6) {
    const uint8_t *b = reinterpret_cast<sockaddr_in6 *>(&a)->sin6_addr.s6_addr;
    static const uint8_t V6_LOOPBACK[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t V4_MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(b, V6_LOOPBACK, 16) == 0 || (std::memcmp(b, V4_MAPPED, 12) == 0 && b[12] == 127);
  }
#endif
  return false;
}

static tls::PortRequest port_request(AsyncWebServerRequest *request, bool ready, bool redirect, bool https_only) {
  tls::PortRequest r;
  r.https_ready = ready;
  r.redirect = redirect;
  r.https_only = https_only;
  if (!ready || (!redirect && !https_only))
    return r;  // PASS whatever the rest says: skip the socket calls
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  r.path = request->url_to(buf).str();
  r.get = request->method() == HTTP_GET;
  r.loopback = from_loopback(request);
#ifdef USE_WIFI
  r.ap_active = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_ap_active();
#endif
  return r;
}

bool PrecisionFeederTls::wants(AsyncWebServerRequest *request) const {
  return tls::port_action(port_request(request, this->https_ready_, this->redirect_, this->https_only_)) !=
         tls::PortAction::PASS;
}

std::string PrecisionFeederTls::redirect_host_() {
  auto identity = this->identity_();
  if (identity != nullptr && !identity->facts.dns.empty() && identity->facts.dns[0] != '*')
    return identity->facts.dns;
  return std::string(App.get_name().c_str()) + ".local";
}

void PrecisionFeederTls::answer(AsyncWebServerRequest *request) {
  const tls::PortAction action =
      tls::port_action(port_request(request, this->https_ready_, this->redirect_, this->https_only_));
  if (action == tls::PortAction::REDIRECT) {
    const std::string location = tls::redirect_location(this->redirect_host_(), this->port_);
    httpd_req_t *raw = *request;
    httpd_resp_set_status(raw, "302 Found");
    httpd_resp_set_hdr(raw, "Location", location.c_str());
    httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
    httpd_resp_send(raw, nullptr, 0);
    return;
  }
  httpd_req_t *raw = *request;
  httpd_resp_set_status(raw, "403 Forbidden");
  httpd_resp_set_type(raw, "application/json");
  httpd_resp_send(raw, "{\"error\":\"https\"}", HTTPD_RESP_USE_STRLEN);
}

// ---- status --------------------------------------------------------------------------------------------------

std::string PrecisionFeederTls::https_text() {
  auto identity = this->identity_();
  if (!this->https_ready_ || identity == nullptr)
    return "starting";
  std::string text = identity->source + " " + identity->facts.dns + " until " + identity->facts.not_after;
  LockGuard guard(this->lock_);
  if (!this->identity_error_.empty())
    text += " (upload refused: " + this->identity_error_ + ")";
  return text;
}

std::string PrecisionFeederTls::mqtt_text() {
  LockGuard guard(this->lock_);
  return this->mqtt_status_;
}

std::string PrecisionFeederTls::status_json() {
  auto identity = this->identity_();
#ifdef USE_PRECISION_FEEDER_ACME
  const std::string acme_status = this->acme_text();
#endif
  std::string j = "{\"https\":{\"ready\":";
  j += this->https_ready_ ? "true" : "false";
  j += ",\"port\":" + std::to_string(this->port_);
  j += ",\"sessions\":" + std::to_string(this->sessions_.load());
  j += ",\"max_sessions\":" + std::to_string(this->max_sessions_);
  j += ",\"redirect\":";
  j += this->redirect_ ? "true" : "false";
  j += ",\"https_only\":";
  j += this->https_only_ ? "true" : "false";
  if (identity != nullptr) {
    j += ",\"source\":\"" + identity->source + "\",\"name\":\"" + json_escape(identity->facts.dns) +
         "\",\"subject\":\"" + json_escape(identity->facts.subject) + "\",\"not_after\":\"" +
         identity->facts.not_after + "\",\"fingerprint\":\"" + identity->facts.fingerprint + "\"";
  }
  LockGuard guard(this->lock_);
  j += ",\"upload_error\":\"" + json_escape(this->identity_error_) + "\"}";
  j += ",\"ca\":{\"set\":";
  j += this->ca_pem_.empty() ? "false" : "true";
  j += ",\"subject\":\"" + json_escape(this->ca_text_) + "\"}";
  j += ",\"mqtt\":{\"status\":\"" + json_escape(this->mqtt_status_) + "\",\"pin\":\"" + this->pin_text_ + "\"}";
#ifdef USE_PRECISION_FEEDER_ACME
  j += ",\"acme\":{\"directory\":\"" + json_escape(this->acme_dir_) + "\",\"name\":\"" +
       json_escape(this->acme_name_in_use_()) + "\",\"status\":\"" + json_escape(acme_status) + "\",\"until\":\"" +
       (this->issued_now_().same ? this->issued_until_ : std::string()) + "\",\"error\":\"" +
       json_escape(this->acme_error_) + "\"}";
#endif
  return j + "}";
}

void PrecisionFeederTls::dump_config() {
  ESP_LOGCONFIG(TAG, "Precision Feeder TLS:");
  ESP_LOGCONFIG(TAG, "  HTTPS port %u, at most %u sessions, idle timeout %u s", this->port_, this->max_sessions_,
                (unsigned) this->idle_timeout_s_);
  ESP_LOGCONFIG(TAG, "  Trust store: %s", this->ca_pem_.empty() ? "empty" : this->ca_text_.c_str());
}

// ---- web endpoints -------------------------------------------------------------------------------------------
// Every change is signed with the settings password like the keys: the tag covers the path and the SHA-256 of the
// body as sent (?sha256=<hex>), as the firmware upload's does. Bodies are raw (Content-Type
// application/octet-stream): ESPHome's form parser takes at most 1 KB, too little for a certificate chain.

void PrecisionFeederTls::handle_body(AsyncWebServerRequest *, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    this->body_.clear();
    this->body_too_big_ = total > tls::MAX_PEM + 2 * tls::MAX_KEY_TEXT + 2;
    if (!this->body_too_big_)
      this->body_.reserve(total);
  }
  if (!this->body_too_big_)
    this->body_.append(reinterpret_cast<const char *>(data), len);
}

static bool from_hex_text(const std::string &hex, std::vector<uint8_t> &out) {
  if (hex.size() % 2 != 0)
    return false;
  out.resize(hex.size() / 2);
  for (size_t i = 0; i < out.size(); i++) {
    char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char *end = nullptr;
    out[i] = static_cast<uint8_t>(std::strtoul(pair, &end, 16));
    if (end != pair + 2)
      return false;
  }
  return true;
}

void PrecisionFeederTls::handle_request(AsyncWebServerRequest *request, const std::string &url) {
  if (request->method() == HTTP_GET) {
    const std::string body = this->status_json();
    request->send(200, "application/json", body.c_str());
    return;
  }
  // A POST with an empty body never reaches handle_body: what is left there belongs to an earlier request.
  std::string body;
  body.swap(this->body_);
  const bool too_big = this->body_too_big_;
  this->body_too_big_ = false;
  if (!too_big && body.size() != request->contentLength())
    body.clear();
  if (too_big) {
    request->send(400, "application/json", "{\"error\":\"too large\"}");
    return;
  }
  // The signature covers the body through its hash.
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char *>(body.data()), body.size(), digest, 0);
  static const char *const HEX = "0123456789abcdef";
  std::string sha;
  for (unsigned char b : digest) {
    sha += HEX[b >> 4];
    sha += HEX[b & 15];
  }
  if (!request->hasParam("sha256") || request->arg("sha256") != sha) {
    request->send(400, "application/json", "{\"error\":\"sha256\"}");
    return;
  }
  // The nonce of the signature also masks an uploaded key; read it before the check uses it up.
  uint8_t nonce[AUTH_NONCE_SIZE]{};
  const auto header = request->get_header("X-Feeder-Auth");
  std::vector<uint8_t> nonce_bytes;
  if (header.has_value() && header->size() >= 2 * AUTH_NONCE_SIZE &&
      from_hex_text(header->substr(0, 2 * AUTH_NONCE_SIZE), nonce_bytes))
    std::memcpy(nonce, nonce_bytes.data(), AUTH_NONCE_SIZE);
  if (!this->feeder_->web_authorised(request, url + "\n" + sha))
    return;

  if (url == "/feeder/tls/ca") {
    CertFacts facts;
    if (!body.empty() && !this->run_big_([&]() { return check_ca(body, facts); })) {
      request->send(422, "application/json", "{\"error\":\"certificate\"}");
      return;
    }
    if (!nvs_put("ca", body)) {
      request->send(422, "application/json", "{\"error\":\"flash\"}");
      return;
    }
    {
      LockGuard guard(this->lock_);
      this->ca_pem_ = body;
      this->ca_text_ = body.empty() ? "" : facts.subject;
    }
    ESP_LOGI(TAG, "Trust store %s", body.empty() ? "cleared" : ("set: " + facts.subject).c_str());
    this->feeder_->run_on_main_loop([this]() { this->feeder_->note_restart_needed("trust store"); });
    request->send(200, "application/json", "{\"restart\":true}");
    return;
  }

  if (url == "/feeder/tls/identity") {
    std::string cert, key;
    if (!body.empty()) {
      std::string key_hex;
      std::vector<uint8_t> masked;
      if (!tls::split_identity_body(body, key_hex, cert) || !from_hex_text(key_hex, masked) ||
          masked.size() > SECRET_BLOB_MAX) {
        request->send(400, "application/json", "{\"error\":\"request\"}");
        return;
      }
      {
        LockGuard guard(this->feeder_->password_lock());
        key = this->feeder_->password_unmask_blob(nonce, masked.data(), masked.size());
      }
      std::string ca, error;
      {
        LockGuard guard(this->lock_);
        ca = this->ca_pem_;
      }
      if (!this->run_big_([&]() { return check_identity(cert, key, ca, error); })) {
        std::fill(key.begin(), key.end(), '\0');
        const std::string answer = "{\"error\":\"" + json_escape(error.empty() ? "check" : error) + "\"}";
        request->send(422, "application/json", answer.c_str());
        return;
      }
    }
    const bool saved = nvs_put("up_cert", cert) && nvs_put("up_key", key);
    std::fill(key.begin(), key.end(), '\0');
    if (!saved) {
      request->send(422, "application/json", "{\"error\":\"flash\"}");
      return;
    }
    {
      LockGuard guard(this->lock_);
      this->identity_error_.clear();
    }
    ESP_LOGI(TAG, "HTTPS certificate %s; used after a restart", cert.empty() ? "cleared (self-signed)" : "uploaded");
    this->feeder_->run_on_main_loop([this]() { this->feeder_->note_restart_needed("HTTPS certificate"); });
    request->send(200, "application/json", "{\"restart\":true}");
    return;
  }

  if (url == "/feeder/tls/self-signed") {
    nvs_put("ss_name", "");  // made again at the next start
    this->feeder_->run_on_main_loop([this]() { this->feeder_->note_restart_needed("HTTPS certificate"); });
    request->send(200, "application/json", "{\"restart\":true}");
    return;
  }

  if (url == "/feeder/tls/acme/renew") {
#ifdef USE_PRECISION_FEEDER_ACME
    {
      LockGuard guard(this->lock_);
      if (this->acme_dir_.empty()) {
        request->send(409, "application/json", "{\"error\":\"acme off\"}");
        return;
      }
      this->acme_failures_ = 0;
      this->acme_next_try_ = 0;
    }
    this->acme_now_ = true;
    request->send(200, "application/json", "{}");
#else
    request->send(404, "application/json", "{}");
#endif
    return;
  }

  if (url == "/feeder/tls/mqtt-pin/forget") {
    this->feeder_->run_on_main_loop([this]() { this->forget_mqtt_pin(); });
    request->send(200, "application/json", "{}");
    return;
  }
  request->send(404, "application/json", "{}");
}

#ifdef USE_PRECISION_FEEDER_ACME
// ---- ACME ----------------------------------------------------------------------------------------------------
// The exchange itself is acme.h's Client; this is its I/O: an HTTPS client checked against the trust store, the
// account key, the CSR. It runs in its own task, at low priority, never on the main loop.

static constexpr uint32_t ACME_CHECK_MS = 30000;
static constexpr size_t ACME_MAX_BODY = 16384;  // a chain of a few certificates; ACME's JSON is far smaller

static bool make_p256(mbedtls_pk_context *pk) {
  return mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
         mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), rng, nullptr) == 0;
}

static std::string key_pem(mbedtls_pk_context *pk) {
  std::vector<unsigned char> buf(800);
  std::string out;
  if (mbedtls_pk_write_key_pem(pk, buf.data(), buf.size()) == 0)
    out = reinterpret_cast<const char *>(buf.data());
  std::fill(buf.begin(), buf.end(), 0);
  return out;
}

class AcmeIo : public tls::acme::Io {
 public:
  AcmeIo(PrecisionFeederTls *parent, mbedtls_pk_context *account, const std::string &ca)
      : parent_(parent), account_(account), ca_(ca) {
    mbedtls_pk_init(&this->cert_key);
  }
  ~AcmeIo() override { mbedtls_pk_free(&this->cert_key); }

  mbedtls_pk_context cert_key;  // the certificate's new key, made by csr()

  tls::acme::Answer request(tls::acme::Method method, const std::string &url, const std::string &body) override {
    tls::acme::Answer a;
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.method = method == tls::acme::Method::GET    ? HTTP_METHOD_GET
                 : method == tls::acme::Method::HEAD ? HTTP_METHOD_HEAD
                                                     : HTTP_METHOD_POST;
    cfg.cert_pem = this->ca_.c_str();  // the server is always checked against the trust store, with its name
    cfg.timeout_ms = 15000;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size = 1024;  // a response header line (Link, Location) must fit
    cfg.event_handler = on_event;
    cfg.user_data = &a;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == nullptr)
      return a;
    if (method == tls::acme::Method::POST) {
      esp_http_client_set_header(c, "Content-Type", "application/jose+json");
      esp_http_client_set_post_field(c, body.c_str(), static_cast<int>(body.size()));
    }
    const esp_err_t err = esp_http_client_perform(c);
    a.status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    a.ok = err == ESP_OK;
    if (!a.ok)
      ESP_LOGW(TAG, "ACME: %s %s", url.c_str(), esp_err_to_name(err));
    return a;
  }

  std::string jwk() override {
    // The public key's DER ends with the uncompressed point: 0x04, X, Y.
    unsigned char der[160];
    const int len = mbedtls_pk_write_pubkey_der(this->account_, der, sizeof(der));
    if (len < 65 || der[sizeof(der) - 65] != 0x04)
      return {};
    const unsigned char *xy = der + sizeof(der) - 64;
    return tls::acme::jwk_p256(xy, xy + 32);
  }

  std::string sha256(const std::string &data) override {
    unsigned char h[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), h, 0);
    return std::string(reinterpret_cast<const char *>(h), sizeof(h));
  }

  std::string sign(const std::string &input) override {
    unsigned char h[32], der[MBEDTLS_ECDSA_MAX_LEN];
    uint8_t raw[64];
    size_t len = 0;
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), h, 0);
    if (mbedtls_pk_sign(this->account_, MBEDTLS_MD_SHA256, h, sizeof(h), der, sizeof(der), &len, rng, nullptr) != 0 ||
        !tls::acme::der_to_raw(der, len, raw))
      return {};
    return std::string(reinterpret_cast<const char *>(raw), sizeof(raw));
  }

  std::string csr(const std::string &name) override {
    mbedtls_pk_free(&this->cert_key);
    mbedtls_pk_init(&this->cert_key);
    if (!make_p256(&this->cert_key))
      return {};
    mbedtls_x509write_csr req;
    mbedtls_x509write_csr_init(&req);
    mbedtls_x509_san_list san{};
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(const_cast<char *>(name.c_str()));
    san.node.san.unstructured_name.len = name.size();
    const std::string dn = "CN=" + name;
    std::vector<unsigned char> der(700);
    mbedtls_x509write_csr_set_key(&req, &this->cert_key);
    mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
    int len = -1;
    if (mbedtls_x509write_csr_set_subject_name(&req, dn.c_str()) == 0 &&
        mbedtls_x509write_csr_set_subject_alternative_name(&req, &san) == 0)
      len = mbedtls_x509write_csr_der(&req, der.data(), der.size(), rng, nullptr);  // written at the buffer's end
    mbedtls_x509write_csr_free(&req);
    if (len <= 0)
      return {};
    return tls::acme::b64url(der.data() + der.size() - len, static_cast<size_t>(len));
  }

  void serve(const std::string &token, const std::string &key_authorization) override {
    this->parent_->set_acme_challenge_(token, key_authorization);
  }

  void sleep_ms(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }

 protected:
  static esp_err_t on_event(esp_http_client_event_t *evt) {
    auto *a = static_cast<tls::acme::Answer *>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
      if (strcasecmp(evt->header_key, "Replay-Nonce") == 0)
        a->nonce = evt->header_value;
      else if (strcasecmp(evt->header_key, "Location") == 0)
        a->location = evt->header_value;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && a->body.size() + evt->data_len <= ACME_MAX_BODY) {
      a->body.append(static_cast<const char *>(evt->data), evt->data_len);
    }
    return ESP_OK;
  }

  PrecisionFeederTls *parent_;
  mbedtls_pk_context *account_;
  const std::string &ca_;
};

/// What ACME issued, checked before it is kept: the chain parses, its key is the one the CSR was made with, and it
/// verifies against the trust store for the name asked for.
static bool check_issued(const std::string &chain, mbedtls_pk_context *key, const std::string &ca,
                         const std::string &name, std::string &error) {
  mbedtls_x509_crt crt, anchor;
  mbedtls_x509_crt_init(&crt);
  mbedtls_x509_crt_init(&anchor);
  uint32_t flags = 0;
  bool ok = false;
  if (mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(chain.c_str()), chain.size() + 1) != 0)
    error = "the certificate does not parse";
  else if (mbedtls_pk_check_pair(&crt.pk, key, rng, nullptr) != 0)
    error = "the certificate is not for the key asked with";
  else if (mbedtls_x509_crt_parse(&anchor, reinterpret_cast<const unsigned char *>(ca.c_str()), ca.size() + 1) != 0 ||
           mbedtls_x509_crt_verify(&crt, &anchor, nullptr, name.c_str(), &flags, nullptr, nullptr) != 0)
    error = "the certificate does not verify against the trust store";
  else
    ok = true;
  mbedtls_x509_crt_free(&crt);
  mbedtls_x509_crt_free(&anchor);
  return ok;
}

static void acme_entry(void *arg) {
  static_cast<PrecisionFeederTls *>(arg)->acme_task_();
  vTaskDelete(nullptr);
}

static void reselect_entry(void *arg) {
  static_cast<PrecisionFeederTls *>(arg)->reselect_task_();
  vTaskDelete(nullptr);
}

std::string PrecisionFeederTls::acme_name_in_use_() {
  return this->acme_name_.empty() ? this->local_name_() : this->acme_name_;
}

tls::acme::Issued PrecisionFeederTls::issued_now_() {
  tls::acme::Issued c = this->issued_;
  c.same = c.have && !this->acme_dir_.empty() && this->issued_dir_ == this->acme_dir_ &&
           this->issued_name_ == this->acme_name_in_use_();
  return c;
}

void PrecisionFeederTls::set_acme_directory(const std::string &url) {
  LockGuard guard(this->lock_);
  if (url == this->acme_dir_)
    return;
  this->acme_dir_ = url;
  this->acme_failures_ = 0;
  this->acme_next_try_ = 0;
  this->acme_error_.clear();
  if (this->started_)
    this->reselect_ = true;  // ACME's certificate is served, or no longer, at once
}

void PrecisionFeederTls::set_acme_name(const std::string &name) {
  LockGuard guard(this->lock_);
  if (name == this->acme_name_)
    return;
  this->acme_name_ = name;
  this->acme_failures_ = 0;
  this->acme_next_try_ = 0;
  this->acme_error_.clear();
  if (this->started_)
    this->reselect_ = true;
}

std::string PrecisionFeederTls::acme_challenge(const std::string &token) {
  LockGuard guard(this->lock_);
  return !token.empty() && token == this->challenge_token_ ? this->challenge_auth_ : std::string();
}

void PrecisionFeederTls::set_acme_challenge_(const std::string &token, const std::string &key_authorization) {
  LockGuard guard(this->lock_);
  this->challenge_token_ = token;
  this->challenge_auth_ = key_authorization;
}

static std::string utc_text(int64_t t) {
  const time_t tt = static_cast<time_t>(t);
  struct tm tm {};
  gmtime_r(&tt, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tm);
  return buf;
}

std::string PrecisionFeederTls::acme_text() {
  const int64_t now = std::time(nullptr);
  const bool clock = clock_valid();
  LockGuard guard(this->lock_);
  if (this->acme_dir_.empty())
    return "off";
  if (this->acme_running_ && !this->challenge_token_.empty())
    return "asking for a certificate for " + this->acme_name_in_use_() + " (challenge served)";
  if (this->acme_running_)
    return "asking for a certificate for " + this->acme_name_in_use_();
  const tls::acme::Issued c = this->issued_now_();
  std::string t = c.same ? "certificate for " + this->issued_name_ + " until " + utc_text(c.not_after) +
                               ", renewed from " + utc_text(tls::acme::renew_at(c))
                         : std::string("no certificate yet");
  if (!this->acme_error_.empty()) {
    t += "; failed: " + this->acme_error_;
    if (this->acme_next_try_ > now)
      t += "; next try in " + std::to_string((this->acme_next_try_ - now + 59) / 60) + " min";
  }
  if (!clock)
    t += " (waiting for a valid clock)";
  return t;
}

void PrecisionFeederTls::serve_identity_(std::shared_ptr<TlsIdentity> identity) {
  if (identity == nullptr)
    return;
  auto current = this->identity_();
  if (current != nullptr && current->facts.fingerprint == identity->facts.fingerprint)
    return;
  ESP_LOGI(TAG, "HTTPS now serves the %s certificate for %s until %s", identity->source.c_str(),
           identity->facts.dns.c_str(), identity->facts.not_after.c_str());
  this->set_identity_(std::move(identity));  // new sessions get it; open ones keep theirs to the end
}

void PrecisionFeederTls::acme_check_() {
  // Every 30 s once HTTPS runs, or at once when asked (renew now, settings changed). Main loop: cheap, no I/O.
  if (!this->https_ready_ || this->acme_running_)
    return;
  const bool asked = this->acme_now_ || this->reselect_;
  if (!asked && millis() - this->acme_checked_ < ACME_CHECK_MS)
    return;
  this->acme_checked_ = millis();
  const int64_t now = std::time(nullptr);
  const bool clock = clock_valid();
  tls::acme::Issued issued;
  bool on;
  int64_t next_try;
  {
    LockGuard guard(this->lock_);
    issued = this->issued_now_();
    on = !this->acme_dir_.empty();
    next_try = this->acme_next_try_;
  }
  if (!on)
    this->acme_now_ = false;
  auto current = this->identity_();
  const bool serving_acme = current != nullptr && current->source == "ACME";
  void (*entry)(void *) = nullptr;
  if (on && clock && (this->acme_now_ || tls::acme::due(now, clock, issued, next_try))) {
    this->acme_now_ = false;
    this->reselect_ = false;  // the run picks the identity at its end
    entry = acme_entry;
  } else if (this->reselect_.exchange(false) || (serving_acme && !tls::acme::servable(now, clock, issued))) {
    // Settings changed, or ACME's certificate expired (a failed renewal kept it until now): the next best one.
    entry = reselect_entry;
  }
  if (entry == nullptr)
    return;
  this->acme_running_ = true;
  if (xTaskCreate(entry, "tls_acme", BIG_STACK, this, 2, nullptr) != pdPASS)
    this->acme_running_ = false;
}

void PrecisionFeederTls::reselect_task_() {
  this->serve_identity_(this->pick_identity_());
  this->acme_running_ = false;
}

void PrecisionFeederTls::acme_task_() {
  std::string dir, name, ca;
  {
    LockGuard guard(this->lock_);
    dir = this->acme_dir_;
    name = this->acme_name_in_use_();
    ca = this->ca_pem_;
  }
  const uint32_t start = millis();
  std::string error, chain, key;
  if (ca.empty()) {
    error = "no CA certificate in the trust store (the ACME server is checked against it)";
  } else {
    // The account key: made at the first run and kept, so the server finds the same account every time.
    mbedtls_pk_context account;
    mbedtls_pk_init(&account);
    std::string pem = nvs_get("ac_acct");
    bool have = !pem.empty() && mbedtls_pk_parse_key(&account, reinterpret_cast<const unsigned char *>(pem.c_str()),
                                                     pem.size() + 1, nullptr, 0, rng, nullptr) == 0;
    std::fill(pem.begin(), pem.end(), '\0');
    if (!have) {
      mbedtls_pk_free(&account);
      mbedtls_pk_init(&account);
      std::string made;
      have = make_p256(&account) && !(made = key_pem(&account)).empty() && nvs_put("ac_acct", made);
      std::fill(made.begin(), made.end(), '\0');
    }
    if (!have) {
      error = "could not make the account key";
    } else {
      AcmeIo io(this, &account, ca);
      tls::acme::Client client(io);
      const tls::acme::Result r = client.run(dir, name);
      if (!r.ok) {
        error = r.error;
      } else if (check_issued(r.chain, &io.cert_key, ca, name, error)) {
        chain = r.chain;
        key = key_pem(&io.cert_key);
        if (key.empty())
          error = "could not keep the certificate's key";
      }
    }
    mbedtls_pk_free(&account);
  }
  if (error.empty() &&
      !(nvs_put("ac_cert", chain) && nvs_put("ac_key", key) && nvs_put("ac_name", name) && nvs_put("ac_dir", dir)))
    error = "flash";
  std::fill(key.begin(), key.end(), '\0');
  const CertFacts f = error.empty() ? facts_of_pem(chain) : CertFacts{};
  {
    LockGuard guard(this->lock_);
    if (error.empty()) {
      this->issued_.have = true;
      this->issued_.not_before = f.from;
      this->issued_.not_after = f.until;
      this->issued_until_ = f.not_after;
      this->issued_name_ = name;
      this->issued_dir_ = dir;
      this->acme_failures_ = 0;
      this->acme_next_try_ = 0;
      this->acme_error_.clear();
    } else {
      this->acme_failures_++;
      this->acme_next_try_ = std::time(nullptr) + tls::acme::retry_delay_s(this->acme_failures_);
      this->acme_error_ = error;
    }
  }
  if (error.empty())
    ESP_LOGI(TAG, "ACME: certificate for %s until %s (%u ms)", name.c_str(), f.not_after.c_str(),
             (unsigned) (millis() - start));
  else
    ESP_LOGW(TAG, "ACME: no certificate for %s: %s", name.c_str(), error.c_str());
  // A new certificate is served at once; a failed renewal keeps the current one while it is valid.
  this->serve_identity_(this->pick_identity_());
  this->acme_running_ = false;
}
#endif

// ---- MQTT ----------------------------------------------------------------------------------------------------

void PrecisionFeederTls::forget_mqtt_pin() {
  nvs_put("mq_pin", "");
  {
    LockGuard guard(this->lock_);
    this->pin_pem_.clear();
    this->pin_text_.clear();
  }
  ESP_LOGI(TAG, "MQTT broker certificate forgotten; pinned again at the next start");
  this->feeder_->note_restart_needed("MQTT broker certificate forgotten");
}

#ifdef USE_MQTT
void PrecisionFeederTls::configure_mqtt(mqtt::MQTTClientComponent *client, bool tls_on, bool pin, bool enabled,
                                        const std::string &broker, uint16_t port) {
  this->mqtt_ = client;
  this->mqtt_broker_ = broker;
  this->mqtt_port_ = port;
  std::string ca, pinned;
  {
    LockGuard guard(this->lock_);
    ca = this->ca_pem_;
    pinned = this->pin_pem_;
  }
  const tls::MqttPlan plan =
      tls::mqtt_plan(tls_on, pin ? tls::MqttCheck::PIN : tls::MqttCheck::CA, !ca.empty(), !pinned.empty());
  std::string status;
  switch (plan) {
    case tls::MqttPlan::PLAIN:
      status = "off";
      break;
    case tls::MqttPlan::USE_CA:
      client->set_ca_certificate(ca.c_str());
      client->set_skip_cert_cn_check(false);
      status = "checked against the CA certificate";
      break;
    case tls::MqttPlan::USE_PIN:
      client->set_ca_certificate(pinned.c_str());
      client->set_skip_cert_cn_check(true);
      status = "checked against the pinned certificate";
      break;
    case tls::MqttPlan::FETCH_PIN:
    case tls::MqttPlan::NO_CA:
      // Never plain instead: a client that starts anyway uses TLS and cannot connect.
      client->set_ca_certificate(PLACEHOLDER_CA);
      client->set_enable_on_boot(false);
      this->mqtt_startable_ = false;
      if (plan == tls::MqttPlan::NO_CA) {
        status = "not started: no CA certificate in the trust store";
      } else if (enabled && !broker.empty()) {
        status = "pinning on first contact";
        this->pin_wanted_ = true;
      } else {
        status = "pinned on first contact once MQTT is on (after a restart)";
      }
      break;
  }
  ESP_LOGI(TAG, "MQTT TLS: %s", status.c_str());
  LockGuard guard(this->lock_);
  this->mqtt_status_ = status;
}

/// Read and write for mbedTLS over a plain socket.
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  const int n = send(*static_cast<int *>(ctx), buf, len, 0);
  return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : n;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
  const int n = recv(*static_cast<int *>(ctx), buf, len, 0);
  if (n == 0)
    return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
  return n < 0 ? MBEDTLS_ERR_SSL_TIMEOUT : n;
}

void PrecisionFeederTls::pin_task_() {
  // Connect once, take the chain the broker shows without checking it, and pin its top certificate if it can anchor
  // a check (tls_policy.h, pin_index). Nothing is sent but the handshake.
  std::string pin_pem, why;
  int fd = -1;
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  const std::string port = std::to_string(this->mqtt_port_);
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);
  do {
    if (getaddrinfo(this->mqtt_broker_.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
      why = "broker name not found";
      break;
    }
    fd = ::socket(res->ai_family, SOCK_STREAM, 0);
    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
      why = "broker unreachable";
      break;
    }
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
      why = "TLS setup";
      break;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);  // first contact: take what is shown
    mbedtls_ssl_conf_rng(&conf, rng, nullptr);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
      why = "TLS setup";
      break;
    }
    mbedtls_ssl_set_hostname(&ssl, this->mqtt_broker_.c_str());
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, nullptr);
    int rc;
    while ((rc = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
    }
    const mbedtls_x509_crt *chain = mbedtls_ssl_get_peer_cert(&ssl);
    if (rc != 0 || chain == nullptr) {
      why = "no TLS on that port";
      break;
    }
    std::vector<tls::ChainCert> facts;
    std::vector<const mbedtls_x509_crt *> certs;
    for (const mbedtls_x509_crt *c = chain; c != nullptr && c->raw.p != nullptr; c = c->next) {
      tls::ChainCert f;
      f.ca = c->MBEDTLS_PRIVATE(ca_istrue) != 0;
      f.self_signed = c->issuer_raw.len == c->subject_raw.len &&
                      std::memcmp(c->issuer_raw.p, c->subject_raw.p, c->subject_raw.len) == 0;
      facts.push_back(f);
      certs.push_back(c);
    }
    const int index = tls::pin_index(facts.data(), facts.size());
    if (index < 0) {
      why = "the broker sends no CA certificate to pin; upload its CA instead";
      break;
    }
    std::vector<unsigned char> buf(4096);
    size_t olen = 0;
    if (mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n", certs[index]->raw.p,
                                 certs[index]->raw.len, buf.data(), buf.size(), &olen) != 0) {
      why = "certificate too large";
      break;
    }
    pin_pem.assign(reinterpret_cast<const char *>(buf.data()));
    mbedtls_ssl_close_notify(&ssl);
  } while (false);
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  if (fd >= 0)
    close(fd);
  if (res != nullptr)
    freeaddrinfo(res);

  if (!pin_pem.empty() && nvs_put("mq_pin", pin_pem)) {
    const std::string fp = facts_of_pem(pin_pem).fingerprint.substr(0, 23);
    ESP_LOGI(TAG, "MQTT broker certificate pinned (%s)", fp.c_str());
    LockGuard guard(this->lock_);
    this->pin_pem_ = pin_pem;
    this->pin_text_ = fp;
    this->mqtt_status_ = "checked against the pinned certificate";
    this->pin_done_ = true;
  } else {
    if (why.empty())
      why = "flash";
    ESP_LOGW(TAG, "MQTT broker certificate not pinned: %s; trying again in a minute", why.c_str());
    LockGuard guard(this->lock_);
    this->mqtt_status_ = "not pinned yet: " + why;
  }
  this->pin_running_ = false;
}
#endif

}  // namespace precision_feeder
}  // namespace esphome
