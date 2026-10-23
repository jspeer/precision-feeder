// Precision Feeder TLS: the decisions, as plain C++ (firmware 2.4.1).
//
// What port 80 does with a request once HTTPS is up (pass it, redirect it, refuse it), how MQTT connects (plain,
// checked against the CA, pinned, or blocked), which certificate of a broker's chain to pin, the serial number of a
// certificate made on the device, and the upload format. The ESPHome and mbedTLS side is precision_feeder_tls.*;
// this file has no ESPHome or mbedTLS includes and is tested on a host (tests/test_tls.cpp).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace precision_feeder {
namespace tls {

// ---- Port 80 once HTTPS is up ---------------------------------------------------------------------------------

/// What port 80 does with a request.
enum class PortAction : uint8_t {
  PASS,      // handled as before
  REDIRECT,  // 302 to https://<the certificate's name>/
  REFUSE,    // 403: HTTPS only
};

struct PortRequest {
  bool loopback{false};     // from 127.0.0.1 or ::1: the HTTPS relay itself
  bool ap_active{false};    // the setup access point is up (the captive portal must stay plain HTTP)
  bool https_ready{false};  // the HTTPS listener runs with an identity
  bool redirect{true};      // "HTTPS Redirect": send a browser opening the app to HTTPS
  bool https_only{false};   // "HTTPS Only": nothing but the exceptions on port 80
  bool get{true};           // a GET request
  std::string path;         // without the query
};

/// The path the ACME http-01 challenge is served on (RFC 8555 section 8.3); never redirected or refused, ACME
/// servers fetch it over plain HTTP.
constexpr const char *ACME_CHALLENGE_PREFIX = "/.well-known/acme-challenge/";

inline PortAction port_action(const PortRequest &r) {
  // The relay, the access point's captive portal, a feeder without HTTPS and the ACME challenge are never touched.
  if (r.loopback || r.ap_active || !r.https_ready || r.path.rfind(ACME_CHALLENGE_PREFIX, 0) == 0)
    return PortAction::PASS;
  // Opening the app (Home Assistant's "Visit" link is http://<host>:80/): to HTTPS, by name.
  const bool page = r.get && (r.path == "/" || r.path == "/index.html");
  if (page && (r.redirect || r.https_only))
    return PortAction::REDIRECT;
  return r.https_only ? PortAction::REFUSE : PortAction::PASS;
}

/// The Location of a redirect: https://<host>/, or https://<host>:<port>/ for another port. IPv6 literals get
/// brackets. An empty host gives an empty string (nothing to redirect to).
inline std::string redirect_location(const std::string &host, uint16_t port = 443) {
  if (host.empty())
    return {};
  std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
  if (port != 443)
    h += ":" + std::to_string(port);
  return "https://" + h + "/";
}

// ---- MQTT over TLS --------------------------------------------------------------------------------------------

/// How the broker's certificate is checked ("MQTT TLS Check").
enum class MqttCheck : uint8_t {
  CA = 0,   // against the CA certificate in the trust store, with its name
  PIN = 1,  // against the certificate the broker showed on first contact (D36): "insecure", but not "anything"
};

/// What the MQTT client is given before it connects.
enum class MqttPlan : uint8_t {
  PLAIN,      // TLS off: plain TCP, as before
  USE_CA,     // TLS, checked against the CA, with the broker's name
  USE_PIN,    // TLS, checked against the pinned certificate, without the name
  FETCH_PIN,  // TLS, no pin yet: connect once, pin what the broker shows, then start
  NO_CA,      // TLS asked for, but no CA in the store: MQTT does not start (never plain instead)
};

inline MqttPlan mqtt_plan(bool tls_on, MqttCheck check, bool have_ca, bool have_pin) {
  if (!tls_on)
    return MqttPlan::PLAIN;
  if (check == MqttCheck::PIN)
    return have_pin ? MqttPlan::USE_PIN : MqttPlan::FETCH_PIN;
  return have_ca ? MqttPlan::USE_CA : MqttPlan::NO_CA;
}

/// One certificate of the chain a broker sends, leaf first.
struct ChainCert {
  bool self_signed{false};  // issuer and subject are the same and its own key verifies it
  bool ca{false};           // basicConstraints CA:TRUE
};

/// Which certificate to pin: the last one the broker sent if it can anchor a check. A self-signed certificate (a
/// broker's own, or a root it sends) or a CA certificate can; a leaf issued by a CA the broker does not send cannot
/// (its issuer would be missing), so nothing is pinned (-1) and the owner uploads that CA instead.
inline int pin_index(const ChainCert *chain, size_t n) {
  if (chain == nullptr || n == 0)
    return -1;
  const ChainCert &top = chain[n - 1];
  return top.self_signed || top.ca ? static_cast<int>(n - 1) : -1;
}

// ---- Certificates made on the device --------------------------------------------------------------------------

/// A random serial number made valid: positive (top bit clear) and without a leading zero byte, which DER forbids
/// (OpenSSL refused one in the experiment, docs/plans/https-tls.md).
inline void fix_serial(uint8_t *serial, size_t n) {
  if (n == 0)
    return;
  serial[0] = static_cast<uint8_t>((serial[0] & 0x7F) | 0x40);
}

// ---- Uploads --------------------------------------------------------------------------------------------------

constexpr size_t MAX_PEM = 6144;      // a chain of three P-256 or RSA-2048 certificates fits
constexpr size_t MAX_KEY_TEXT = 2048;  // a PEM private key (EC or RSA-2048 PKCS#8 is about 1.7 KB)

/// The body of POST /feeder/tls/identity: the private key masked with the settings password, in hex, on the first
/// line, then the certificate chain in PEM (leaf first). False if malformed.
inline bool split_identity_body(const std::string &body, std::string &key_hex, std::string &chain_pem) {
  const size_t eol = body.find('\n');
  if (eol == std::string::npos || eol == 0)
    return false;
  key_hex = body.substr(0, eol);
  if (!key_hex.empty() && key_hex.back() == '\r')
    key_hex.pop_back();
  if (key_hex.size() % 2 != 0 || key_hex.size() > 2 * MAX_KEY_TEXT)
    return false;
  for (char c : key_hex) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex)
      return false;
  }
  chain_pem = body.substr(eol + 1);
  return !chain_pem.empty() && chain_pem.size() <= MAX_PEM;
}

/// How many PEM blocks of this label ("CERTIFICATE") the text holds, complete with their END line.
inline size_t count_pem(const std::string &text, const char *label) {
  const std::string begin = std::string("-----BEGIN ") + label + "-----";
  const std::string end = std::string("-----END ") + label + "-----";
  size_t n = 0, at = 0;
  while ((at = text.find(begin, at)) != std::string::npos) {
    const size_t stop = text.find(end, at + begin.size());
    if (stop == std::string::npos)
      break;
    n++;
    at = stop + end.size();
  }
  return n;
}

}  // namespace tls
}  // namespace precision_feeder
}  // namespace esphome
