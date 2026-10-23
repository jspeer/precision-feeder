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
#include <mbedtls/ecp.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <nvs.h>

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
// (an uploaded identity), "ca" (the trust store), "mq_pin" (MQTT's pinned broker certificate).

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
  std::string source;     // "self-signed" or "uploaded"
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
              url == "/feeder/tls/mqtt-pin/forget");
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

void PrecisionFeederTls::identity_task_() {
  std::string name = App.get_name().c_str();
  name += ".local";
  auto identity = std::make_shared<TlsIdentity>();
  // An uploaded identity wins if it still checks; else the self-signed one, made again when the name changed.
  identity->cert = nvs_get("up_cert");
  identity->key = nvs_get("up_key");
  if (!identity->cert.empty()) {
    std::string ca, error;
    {
      LockGuard guard(this->lock_);
      ca = this->ca_pem_;
    }
    if (check_identity(identity->cert, identity->key, ca, error)) {
      identity->source = "uploaded";
    } else {
      ESP_LOGW(TAG, "The uploaded certificate no longer checks (%s); using the self-signed one", error.c_str());
      LockGuard guard(this->lock_);
      this->identity_error_ = error;
    }
  }
  if (identity->source.empty()) {
    identity->cert = nvs_get("ss_cert");
    identity->key = nvs_get("ss_key");
    if (identity->cert.empty() || identity->key.empty() || nvs_get("ss_name") != name) {
      const uint32_t start = millis();
      if (!this->make_self_signed_(identity->cert, identity->key, name)) {
        ESP_LOGE(TAG, "Could not make a self-signed certificate; HTTPS is off");
        return;
      }
      nvs_put("ss_cert", identity->cert);
      nvs_put("ss_key", identity->key);
      nvs_put("ss_name", name);
      ESP_LOGI(TAG, "Made a self-signed certificate for %s (%u ms)", name.c_str(), (unsigned) (millis() - start));
    }
    identity->source = "self-signed";
  }
  identity->prepare();
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
  j += ",\"mqtt\":{\"status\":\"" + json_escape(this->mqtt_status_) + "\",\"pin\":\"" + this->pin_text_ + "\"}}";
  return j;
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

  if (url == "/feeder/tls/mqtt-pin/forget") {
    this->feeder_->run_on_main_loop([this]() { this->forget_mqtt_pin(); });
    request->send(200, "application/json", "{}");
    return;
  }
  request->send(404, "application/json", "{}");
}

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
