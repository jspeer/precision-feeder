// Host-side tests for the TLS decisions (components/precision_feeder_tls/tls_policy.h, firmware 2.4.1).
// Build and run:
//   c++ -std=c++17 -Wall -Wextra -I../components/precision_feeder_tls test_tls.cpp -o /tmp/test_tls && /tmp/test_tls
#include <cstdio>
#include <string>

#include "tls_policy.h"

using namespace esphome::precision_feeder::tls;

static int failures = 0;
#define CHECK(cond, msg)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
      failures++;                                         \
    }                                                     \
  } while (0)

static PortRequest page(const char *path = "/") {
  PortRequest r;
  r.https_ready = true;
  r.path = path;
  return r;
}

int main() {
  // ---- Port 80 ----
  {
    CHECK(port_action(page()) == PortAction::REDIRECT, "opening the app on port 80 goes to HTTPS");
    CHECK(port_action(page("/index.html")) == PortAction::REDIRECT, "index.html too");
    CHECK(port_action(page("/events")) == PortAction::PASS, "an open page's requests keep working on port 80");
    PortRequest r = page();
    r.redirect = false;
    CHECK(port_action(r) == PortAction::PASS, "redirect off: the app opens on port 80 as before");
    r.https_only = true;
    CHECK(port_action(r) == PortAction::REDIRECT, "HTTPS only redirects the page even with the redirect off");
    r.path = "/feeder/auth";
    CHECK(port_action(r) == PortAction::REFUSE, "HTTPS only refuses everything else");
    r.get = false;
    r.path = "/";
    CHECK(port_action(r) == PortAction::REFUSE, "a POST is never redirected");
    r.path = "/.well-known/acme-challenge/abc";
    r.get = true;
    CHECK(port_action(r) == PortAction::PASS, "the ACME challenge stays on plain HTTP");
    r.path = "/feeder/auth";
    r.loopback = true;
    CHECK(port_action(r) == PortAction::PASS, "the HTTPS relay (loopback) is never refused");
    r.loopback = false;
    r.ap_active = true;
    CHECK(port_action(r) == PortAction::PASS, "the captive portal stays on plain HTTP");
    r.ap_active = false;
    r.https_ready = false;
    CHECK(port_action(r) == PortAction::PASS, "no HTTPS running: nothing is refused (no lock-out)");
    CHECK(port_action(PortRequest{}) == PortAction::PASS, "defaults: HTTPS not ready, pass");
  }
  {
    CHECK(redirect_location("feeder-b00604.local") == "https://feeder-b00604.local/", "by name");
    CHECK(redirect_location("feeder.local", 8443) == "https://feeder.local:8443/", "another port");
    CHECK(redirect_location("fd00::1") == "https://[fd00::1]/", "IPv6 in brackets");
    CHECK(redirect_location("").empty(), "no name, no redirect");
  }

  // ---- MQTT ----
  {
    CHECK(mqtt_plan(false, MqttCheck::CA, true, true) == MqttPlan::PLAIN, "TLS off: plain");
    CHECK(mqtt_plan(true, MqttCheck::CA, true, false) == MqttPlan::USE_CA, "CA");
    CHECK(mqtt_plan(true, MqttCheck::CA, false, true) == MqttPlan::NO_CA, "no CA: never plain instead");
    CHECK(mqtt_plan(true, MqttCheck::PIN, false, true) == MqttPlan::USE_PIN, "pinned");
    CHECK(mqtt_plan(true, MqttCheck::PIN, true, false) == MqttPlan::FETCH_PIN, "no pin yet: first contact");
  }
  {
    CHECK(pin_index(nullptr, 0) == -1, "nothing sent");
    const ChainCert self[] = {{true, false}};
    CHECK(pin_index(self, 1) == 0, "a self-signed broker certificate is pinned");
    const ChainCert with_ca[] = {{false, false}, {false, true}};
    CHECK(pin_index(with_ca, 2) == 1, "a chain with its CA: the CA is pinned");
    const ChainCert with_root[] = {{false, false}, {false, true}, {true, true}};
    CHECK(pin_index(with_root, 3) == 2, "a chain up to its root: the root");
    const ChainCert leaf_only[] = {{false, false}};
    CHECK(pin_index(leaf_only, 1) == -1, "a leaf without its issuer cannot anchor a check");
  }

  // ---- Serial numbers ----
  {
    for (int first = 0; first < 256; first++) {
      uint8_t s[16] = {static_cast<uint8_t>(first)};
      fix_serial(s, sizeof(s));
      CHECK(s[0] != 0 && (s[0] & 0x80) == 0, "positive, no leading zero byte");
    }
    fix_serial(nullptr, 0);  // no crash
  }

  // ---- Uploads ----
  {
    std::string key, chain;
    CHECK(split_identity_body("0a1B\n-----BEGIN CERTIFICATE-----\nx\n-----END CERTIFICATE-----\n", key, chain) &&
              key == "0a1B" && count_pem(chain, "CERTIFICATE") == 1,
          "key line, then the chain");
    CHECK(split_identity_body("abcd\r\nPEM", key, chain) && key == "abcd", "a CRLF line end");
    CHECK(!split_identity_body("abc\nPEM", key, chain), "odd hex");
    CHECK(!split_identity_body("zz\nPEM", key, chain), "not hex");
    CHECK(!split_identity_body("\nPEM", key, chain), "no key");
    CHECK(!split_identity_body("abcd\n", key, chain), "no chain");
    CHECK(!split_identity_body("abcd", key, chain), "one line");
    CHECK(!split_identity_body(std::string(2 * MAX_KEY_TEXT + 2, 'a') + "\nPEM", key, chain), "key too long");
    CHECK(!split_identity_body("ab\n" + std::string(MAX_PEM + 1, 'x'), key, chain), "chain too long");
    CHECK(identity_body_too_large("ab\n" + std::string(MAX_PEM + 1, 'x')), "too large: the chain");
    CHECK(identity_body_too_large(std::string(2 * MAX_KEY_TEXT + 2, 'a') + "\nPEM"), "too large: the key line");
    CHECK(!identity_body_too_large(std::string(2 * MAX_KEY_TEXT, 'a') + "\r\n" + std::string(MAX_PEM, 'x')),
          "at the limits, with a CRLF");
    CHECK(!identity_body_too_large("zz\nPEM"), "malformed is not too large");
  }
  {
    const std::string two = "-----BEGIN CERTIFICATE-----\na\n-----END CERTIFICATE-----\n"
                            "-----BEGIN CERTIFICATE-----\nb\n-----END CERTIFICATE-----\n";
    CHECK(count_pem(two, "CERTIFICATE") == 2, "two certificates");
    CHECK(count_pem("-----BEGIN CERTIFICATE-----\na\n", "CERTIFICATE") == 0, "an unfinished block does not count");
    CHECK(count_pem(two, "PRIVATE KEY") == 0, "another label");
  }

  if (failures == 0)
    std::printf("All TLS tests passed\n");
  return failures == 0 ? 0 : 1;
}
