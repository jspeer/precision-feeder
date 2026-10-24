// Host-side tests for the ACME client (components/precision_feeder_tls/acme.h, firmware 2.4.2).
//
// The server is a fake whose answers follow RFC 8555's examples (section 7) and step-ca's behaviour on the bench
// (docs/BENCH.md): an account found again by its key, one http-01 authorization, a certificate that takes a poll to
// be issued. Signatures are fake (the device's are mbedTLS's); SHA-256 is the project's own (crypto.h).
// Build and run:
//   c++ -std=c++17 -Wall -Wextra -I../components/precision_feeder_tls -I../components/precision_feeder test_acme.cpp \
//     -o /tmp/test_acme && /tmp/test_acme
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "acme.h"
#include "crypto.h"

using namespace esphome::precision_feeder::tls::acme;
namespace crypto = esphome::precision_feeder::crypto;

static int failures = 0;
#define CHECK(cond, msg)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
      failures++;                                         \
    }                                                     \
  } while (0)

static std::string b64url_decode(const std::string &s) {
  static const std::string ABC = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    const size_t v = ABC.find(c);
    if (v == std::string::npos)
      return "<bad>";
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

static const char *const DIR = "https://ca.example/acme/acme/directory";
static const char *const BASE = "https://ca.example/acme/acme/";
static const char *const NAME = "feeder-b00604.local";
static const char *const CHAIN =
    "-----BEGIN CERTIFICATE-----\nMIIBleaf\n-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\nMIIBintermediate\n-----END CERTIFICATE-----\n";

/// One request as the server saw it: the JWS decoded.
struct Seen {
  Method method;
  std::string url, protected_json, payload;
};

/// A fake ACME server and device in one.
class Fake : public Io {
 public:
  // What the server does (changed by the tests).
  bool authz_valid_already{false};  // a reused authorization
  bool challenge_fails{false};      // the server cannot fetch the token
  int bad_nonces{0};                // the next n signed requests get badNonce
  int processing_polls{1};          // polls of the order before the certificate is issued
  bool directory_down{false};
  std::string unreachable;  // non-empty: no answer at all, for this reason ("" reason: the device cannot tell)
  bool silent{false};
  bool offer_http01{true};
  bool csr_fails{false};

  // What the device did.
  std::vector<Seen> seen;
  std::vector<std::pair<std::string, std::string>> served;  // (token, key authorization), "" = stopped
  uint32_t slept_ms{0};

  std::string jwk() override {
    uint8_t x[32], y[32];
    for (int i = 0; i < 32; i++) {
      x[i] = static_cast<uint8_t>(i);
      y[i] = static_cast<uint8_t>(255 - i);
    }
    return jwk_p256(x, y);
  }
  std::string sha256(const std::string &data) override {
    uint8_t out[32];
    crypto::sha256(reinterpret_cast<const uint8_t *>(data.data()), data.size(), out);
    return std::string(reinterpret_cast<const char *>(out), 32);
  }
  std::string sign(const std::string &input) override {
    // Not a real signature: the server checks that it covers what was sent.
    std::string sig = this->sha256(input);
    return sig + sig;
  }
  std::string csr(const std::string &name) override { return csr_fails ? "" : b64url("CSR for " + name); }
  void serve(const std::string &token, const std::string &key_authorization) override {
    served.emplace_back(token, key_authorization);
  }
  void sleep_ms(uint32_t ms) override { slept_ms += ms; }

  Answer request(Method method, const std::string &url, const std::string &body) override {
    Answer a;
    if (silent || !unreachable.empty()) {
      a.why = unreachable;
      return a;
    }
    a.ok = true;
    a.nonce = "nonce" + std::to_string(++nonce_counter_);
    Seen s{method, url, "", ""};
    if (method == Method::POST) {
      const std::string prot = json::get(body, "protected"), payload = json::get(body, "payload"),
                        sig = json::get(body, "signature");
      s.protected_json = b64url_decode(prot);
      s.payload = b64url_decode(payload);
      const std::string want = this->sign(prot + "." + payload);
      // The signature covers the protected header and the payload as sent; the header names this URL and the nonce
      // the server last gave.
      if (b64url_decode(sig) != want || json::get(s.protected_json, "url") != url ||
          json::get(s.protected_json, "alg") != "ES256") {
        seen.push_back(s);
        return problem(a, 400, "urn:ietf:params:acme:error:malformed", "bad JWS");
      }
      if (json::get(s.protected_json, "nonce") != last_nonce_ || bad_nonces > 0) {
        if (bad_nonces > 0)
          bad_nonces--;
        seen.push_back(s);
        last_nonce_ = a.nonce;
        return problem(a, 400, "urn:ietf:params:acme:error:badNonce", "JWS has an invalid anti-replay nonce");
      }
    }
    seen.push_back(s);
    last_nonce_ = a.nonce;
    return this->route(a, s);
  }

  /// How many requests went to a URL ending in `tail`.
  int count(const std::string &tail) const {
    int n = 0;
    for (const Seen &s : seen)
      n += s.url.size() >= tail.size() && s.url.compare(s.url.size() - tail.size(), tail.size(), tail) == 0;
    return n;
  }

 protected:
  static Answer problem(Answer a, int status, const std::string &type, const std::string &detail) {
    a.status = status;
    a.body = "{\"type\":\"" + type + "\",\"detail\":\"" + detail + "\"}";
    return a;
  }

  Answer route(Answer a, const Seen &s) {
    const std::string u = s.url;
    const std::string base = BASE;
    if (u == DIR) {
      if (directory_down)
        return problem(a, 503, "about:blank", "Service Unavailable");
      a.status = 200;
      a.body = "{\n  \"newNonce\": \"" + base + "new-nonce\",\n  \"newAccount\": \"" + base +
               "new-account\",\n  \"newOrder\": \"" + base + "new-order\",\n  \"revokeCert\": \"" + base +
               "revoke-cert\",\n  \"keyChange\": \"" + base + "key-change\"\n}";
      return a;
    }
    if (u == base + "new-nonce") {
      a.status = s.method == Method::HEAD ? 200 : 204;
      return a;
    }
    if (u == base + "new-account") {
      // The first request carries the key itself, never an account URL.
      if (json::get(s.protected_json, "kid") != "" || !json::member(s.protected_json, "jwk", jwk_raw_))
        return problem(a, 400, "urn:ietf:params:acme:error:malformed", "need jwk");
      a.status = accounts_++ == 0 ? 201 : 200;
      a.location = base + "account/evOfKhNU60wg";
      a.body = "{\"status\":\"valid\",\"orders\":\"" + base + "account/evOfKhNU60wg/orders\"}";
      return a;
    }
    // Everything after the account is signed with the account's URL.
    if (s.method == Method::POST && json::get(s.protected_json, "kid") != base + "account/evOfKhNU60wg")
      return problem(a, 400, "urn:ietf:params:acme:error:malformed", "need kid");
    if (u == base + "new-order") {
      std::string ids;
      json::member(s.payload, "identifiers", ids);
      const auto list = json::elements(ids);
      if (list.size() != 1 || json::get(list[0], "type") != "dns")
        return problem(a, 400, "urn:ietf:params:acme:error:malformed", "identifiers");
      ordered_name_ = json::get(list[0], "value");
      a.status = 201;
      a.location = base + "order/TOlocE8rfgo";
      a.body = this->order_body(authz_valid_already ? "ready" : "pending");
      return a;
    }
    if (u == base + "authz/PAniVnsZcis") {
      a.status = 200;
      std::string chal = offer_http01 ? "{\"type\":\"http-01\",\"status\":\"" + challenge_state_ + "\",\"url\":\"" +
                                            base + "challenge/LoqXcYV8q5ONbJQx\",\"token\":\"" + TOKEN + "\"" +
                                            (challenge_state_ == "invalid"
                                                 ? ",\"error\":{\"type\":\"urn:ietf:params:acme:error:connection\","
                                                   "\"detail\":\"connection refused\"}"
                                                 : "") +
                                            "}"
                                      : "";
      std::string dns = "{\"type\":\"dns-01\",\"status\":\"pending\",\"url\":\"" + base +
                        "challenge/Rg5dV14Gh1Q\",\"token\":\"DGyRejmCefe7v4NfDGDKfA\"}";
      a.body = "{\"status\":\"" + authz_state() + "\",\"expires\":\"2026-10-30T14:09:07.99Z\",\"identifier\":{" +
               "\"type\":\"dns\",\"value\":\"" + ordered_name_ + "\"},\"challenges\":[" + dns +
               (chal.empty() ? "" : "," + chal) + "]}";
      return a;
    }
    if (u == base + "challenge/LoqXcYV8q5ONbJQx") {
      // The server fetches http://<name>/.well-known/acme-challenge/<token> and compares.
      const bool served_right = !served.empty() && served.back().first == TOKEN &&
                                served.back().second == std::string(TOKEN) + "." + b64url(sha256(jwk()));
      challenge_state_ = challenge_fails || !served_right ? "invalid" : "valid";
      a.status = 200;
      a.body = "{\"type\":\"http-01\",\"status\":\"processing\",\"token\":\"" + std::string(TOKEN) + "\"}";
      return a;
    }
    if (u == base + "order/TOlocE8rfgo/finalize") {
      csr_ = json::get(s.payload, "csr");
      finalized_ = true;
      a.status = 200;
      a.body = this->order_body(processing_polls > 0 ? "processing" : "valid");
      return a;
    }
    if (u == base + "order/TOlocE8rfgo") {
      a.status = 200;
      if (finalized_ && processing_polls > 0)
        processing_polls--;
      a.body = this->order_body(finalized_ ? (processing_polls > 0 ? "processing" : "valid")
                                           : (authz_state() == "valid" ? "ready" : "pending"));
      return a;
    }
    if (u == base + "certificate/mAt3xBGaobw") {
      if (!s.payload.empty())
        return problem(a, 400, "urn:ietf:params:acme:error:malformed", "POST-as-GET has an empty payload");
      a.status = 200;
      a.body = CHAIN;
      return a;
    }
    return problem(a, 404, "about:blank", "Not Found");
  }

  std::string authz_state() const {
    if (authz_valid_already)
      return "valid";
    return challenge_state_ == "valid" ? "valid" : challenge_state_ == "invalid" ? "invalid" : "pending";
  }

  std::string order_body(const std::string &state) {
    const std::string base = BASE;
    std::string b = "{\"status\":\"" + state + "\",\"identifiers\":[{\"type\":\"dns\",\"value\":\"" + ordered_name_ +
                    "\"}],\"authorizations\":[\"" + base + "authz/PAniVnsZcis\"],\"finalize\":\"" + base +
                    "order/TOlocE8rfgo/finalize\"";
    if (state == "valid")
      b += ",\"certificate\":\"" + base + "certificate/mAt3xBGaobw\"";
    return b + "}";
  }

 public:
  static constexpr const char *TOKEN = "LoqXcYV8q5ONbJQxbmR7SCTNo3tiAXDfowyjxAjEuX0";
  std::string ordered_name_, csr_;

 protected:
  int nonce_counter_{0};
  std::string last_nonce_, jwk_raw_, challenge_state_{"pending"};
  int accounts_{0};
  bool finalized_{false};
};

int main() {
  // ---- base64url (RFC 4648 section 10, without padding, URL alphabet) ----
  {
    CHECK(b64url("") == "", "empty");
    CHECK(b64url("f") == "Zg", "f");
    CHECK(b64url("fo") == "Zm8", "fo");
    CHECK(b64url("foo") == "Zm9v", "foo");
    CHECK(b64url("foob") == "Zm9vYg", "foob");
    CHECK(b64url("fooba") == "Zm9vYmE", "fooba");
    CHECK(b64url("foobar") == "Zm9vYmFy", "foobar");
    const uint8_t hi[3] = {0xFB, 0xFF, 0xBF};
    CHECK(b64url(hi, 3) == "-_-_", "the URL alphabet: - and _ for + and /");
  }

  // ---- JSON ----
  {
    const std::string obj = " { \"a\" : \"x\\\"y\\u0041\\/\", \"n\": 12, \"o\": {\"a\": \"inner\"}, \"arr\": [ \"p\", "
                            "{\"q\":[1,2]}, \"r\" ], \"t\": true, \"e\": [] } ";
    CHECK(json::get(obj, "a") == "x\"yA/", "a string member, with escapes");
    std::string raw;
    CHECK(json::member(obj, "n", raw) && raw == "12", "a number member, raw");
    CHECK(json::member(obj, "o", raw) && json::get(raw, "a") == "inner", "a nested object");
    CHECK(json::get(obj, "inner") == "", "members are found at the top level only");
    CHECK(json::member(obj, "arr", raw) && json::elements(raw).size() == 3, "an array's elements");
    CHECK(json::str(json::elements(raw)[2]) == "r", "an element after a nested one");
    CHECK(json::member(obj, "e", raw) && json::elements(raw).empty(), "an empty array");
    CHECK(json::member(obj, "t", raw) && raw == "true", "a literal");
    CHECK(json::get(obj, "missing") == "", "a missing member");
    CHECK(json::get("{\"a\": \"unterminated", "a") == "", "an unterminated string");
    CHECK(json::get("not json", "a") == "", "not an object");
    CHECK(json::get("{\"a\" \"x\"}", "a") == "", "a missing colon");
    CHECK(json::elements("[1, 2").empty(), "an unterminated array");
    std::string deep(100, '[');
    CHECK(json::skip_value(deep, 0) == std::string::npos, "nesting is bounded");
    CHECK(json::get("{\"a\":\"\\u00e9\"}", "a") == "?", "non-ASCII escapes become ?");
  }

  // ---- what may be asked for ----
  {
    CHECK(valid_name("feeder-b00604.local"), "an mDNS name");
    CHECK(valid_name("feeder.home.arpa"), "a LAN name");
    CHECK(!valid_name(""), "empty");
    CHECK(!valid_name("*.local"), "no wildcards (http-01 cannot prove one)");
    CHECK(!valid_name("a..b"), "an empty label");
    CHECK(!valid_name(".local") && !valid_name("feeder."), "a leading or trailing dot");
    CHECK(!valid_name(std::string(64, 'a') + ".local"), "a label over 63");
    CHECK(valid_name(std::string(63, 'a') + ".local"), "a label of 63");
    CHECK(!valid_name("feeder\",\"x"), "nothing that would break the JSON");
    CHECK(valid_directory(DIR), "an https directory");
    CHECK(!valid_directory("http://ca.example/acme/directory"), "never plain http");
    CHECK(!valid_directory("https://"), "a scheme alone");
    CHECK(!valid_directory("https://ca.example/a b"), "no spaces");
    CHECK(!valid_directory("https://ca.example/\"x"), "no quotes");
  }

  // ---- when to ask ----
  {
    CHECK(retry_delay_s(0) == 0 && retry_delay_s(1) == 300 && retry_delay_s(2) == 900 && retry_delay_s(3) == 3600 &&
              retry_delay_s(4) == 10800 && retry_delay_s(50) == 10800,
          "backing off: 5 min, 15 min, 1 h, then 3 h");
    CHECK(utc_seconds(1970, 1, 1, 0, 0, 0) == 0, "the epoch");
    CHECK(utc_seconds(2000, 1, 1, 0, 0, 0) == 946684800, "2000");
    CHECK(utc_seconds(2049, 12, 31, 23, 59, 59) == 2524607999LL, "the self-signed certificate's end");
    CHECK(utc_seconds(2024, 2, 29, 12, 0, 0) == 1709208000, "a leap day");

    const int64_t t0 = utc_seconds(2026, 10, 23, 12, 0, 0);
    Issued c;
    CHECK(!due(t0, false, c, 0), "never without a valid clock");
    CHECK(due(t0, true, c, 0), "no certificate: at once");
    CHECK(!due(t0, true, c, t0 + 1), "but not before the retry time");
    c.have = true;
    c.same_name = c.same_dir = true;
    c.not_before = t0;
    c.not_after = t0 + 24 * 3600;  // step-ca's default
    CHECK(renew_at(c) == t0 + 16 * 3600, "renewal two thirds in: 16 h of 24");
    CHECK(!due(t0 + 16 * 3600 - 1, true, c, 0), "not before");
    CHECK(due(t0 + 16 * 3600, true, c, 0), "from then");
    c.not_after = t0 + 90LL * 86400;
    CHECK(renew_at(c) == t0 + 60LL * 86400, "60 days of 90");
    c.same_dir = false;
    CHECK(due(t0, true, c, 0), "a certificate from another directory: asked for at once");
    CHECK(servable(t0 + 10, true, c), "but served until the new directory delivers");
    c.same_name = false;
    CHECK(due(t0, true, c, 0), "a certificate for another name: at once");
    c.same_name = c.same_dir = true;
    CHECK(servable(t0 + 10, true, c), "within its dates");
    CHECK(!servable(t0 + 90LL * 86400, true, c), "not once expired");
    CHECK(!servable(t0 - 10, true, c), "not before it starts");
    CHECK(servable(0, false, c), "without a clock it is served as it is");
    c.same_name = false;
    CHECK(!servable(t0 + 10, true, c), "never for another name");
  }


  // ---- ES256 signatures: DER to r || s ----
  {
    // r with a sign byte (33 bytes), s short (31 bytes): both come out as 32.
    std::vector<uint8_t> der = {0x30, 0x00, 0x02, 33, 0x00};
    for (int i = 0; i < 32; i++)
      der.push_back(static_cast<uint8_t>(0x80 + i));
    der.push_back(0x02);
    der.push_back(31);
    for (int i = 0; i < 31; i++)
      der.push_back(static_cast<uint8_t>(i + 1));
    der[1] = static_cast<uint8_t>(der.size() - 2);
    uint8_t raw[64];
    CHECK(der_to_raw(der.data(), der.size(), raw), "a valid signature");
    CHECK(raw[0] == 0x80 && raw[31] == 0x9F, "r without its sign byte");
    CHECK(raw[32] == 0 && raw[33] == 1 && raw[63] == 31, "s padded on the left");
    std::vector<uint8_t> bad = der;
    bad[0] = 0x31;
    CHECK(!der_to_raw(bad.data(), bad.size(), raw), "not a SEQUENCE");
    bad = der;
    bad.push_back(0);
    CHECK(!der_to_raw(bad.data(), bad.size(), raw), "trailing bytes");
    bad = der;
    bad.resize(der.size() - 3);
    CHECK(!der_to_raw(bad.data(), bad.size(), raw), "cut short");
    std::vector<uint8_t> big = {0x30, 0x46, 0x02, 0x21, 0x01};  // r of 33 significant bytes: too large for P-256
    for (int i = 0; i < 32; i++)
      big.push_back(1);
    big.insert(big.end(), {0x02, 0x21});
    for (int i = 0; i < 33; i++)
      big.push_back(1);
    CHECK(!der_to_raw(big.data(), big.size(), raw), "an integer over 32 bytes");
  }

  // ---- JWS pieces ----
  {
    uint8_t x[32] = {}, y[32] = {};
    const std::string jwk = jwk_p256(x, y);
    CHECK(jwk.rfind("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"", 0) == 0, "RFC 7638's member order: crv, kty, x, y");
    CHECK(json::get(jwk, "x") == b64url(x, 32) && json::get(jwk, "x").size() == 43, "x as 43 characters");
    const std::string first = protected_header("n1", "https://u", "", jwk);
    std::string raw;
    CHECK(json::member(first, "jwk", raw) && json::get(first, "kid") == "", "before the account: the key");
    const std::string later = protected_header("n2", "https://u", "https://acct", jwk);
    CHECK(json::get(later, "kid") == "https://acct" && !json::member(later, "jwk", raw), "after: the account's URL");
    CHECK(json::get(later, "nonce") == "n2" && json::get(later, "url") == "https://u", "the nonce and the URL");
  }

  // ---- the whole exchange ----
  {
    Fake fake;
    Client client(fake);
    const Result r = client.run(DIR, NAME);
    CHECK(r.ok, "a certificate");
    if (!r.ok)
      std::printf("  error: %s\n", r.error.c_str());
    CHECK(r.chain == CHAIN, "the chain as the server sent it");
    CHECK(fake.ordered_name_ == NAME, "ordered for the feeder's name");
    CHECK(fake.csr_ == b64url(std::string("CSR for ") + NAME), "the CSR as the device made it");
    CHECK(fake.served.size() >= 2 && fake.served.front().first == Fake::TOKEN, "the token was served");
    CHECK(fake.served.back().first.empty(), "and is no longer served at the end");
    CHECK(fake.count("new-account") == 1 && fake.count("new-order") == 1 && fake.count("/finalize") == 1,
          "one account, one order, one finalize");
    CHECK(fake.seen.front().url == DIR && fake.seen.front().method == Method::GET, "the directory first");
    CHECK(fake.seen[1].method == Method::HEAD, "then a nonce");
    CHECK(fake.slept_ms > 0 && fake.slept_ms <= 10 * Client::POLL_MS, "a few polls, not a busy loop");
    // The challenge is answered with "{}", every POST-as-GET with an empty payload.
    for (const Seen &s : fake.seen) {
      if (s.url.find("/challenge/") != std::string::npos)
        CHECK(s.payload == "{}", "the challenge is triggered with {}");
      if (s.url.find("/authz/") != std::string::npos || s.url.find("/certificate/") != std::string::npos)
        CHECK(s.payload.empty(), "POST-as-GET");
    }
  }
  {
    // The second run finds the same account (200), and a reused authorization skips the challenge.
    Fake fake;
    fake.authz_valid_already = true;
    fake.processing_polls = 0;
    Client client(fake);
    const Result r = client.run(DIR, NAME);
    CHECK(r.ok, "a certificate without a challenge");
    CHECK(fake.count("/challenge/LoqXcYV8q5ONbJQx") == 0, "no challenge for a valid authorization");
    CHECK(fake.served.size() == 1 && fake.served[0].first.empty(), "nothing served");
  }
  {
    // badNonce: retried once with the nonce the error brought.
    Fake fake;
    fake.bad_nonces = 1;
    Client client(fake);
    CHECK(client.run(DIR, NAME).ok, "one badNonce is retried");
    CHECK(fake.count("new-account") == 2, "the account request was sent twice");
    Fake worse;
    worse.bad_nonces = 2;
    Client c2(worse);
    const Result r = c2.run(DIR, NAME);
    CHECK(!r.ok && r.error.find("account") == 0 && r.error.find("nonce") != std::string::npos,
          "two in a row fail, with the server's words");
  }
  {
    Fake fake;
    fake.challenge_fails = true;
    Client client(fake);
    const Result r = client.run(DIR, NAME);
    CHECK(!r.ok && r.error == "challenge: connection refused", "the server's reason for a failed challenge");
    CHECK(fake.count("/finalize") == 0, "never finalized");
    CHECK(fake.served.back().first.empty(), "the token is no longer served after a failure");
  }
  {
    Fake fake;
    fake.directory_down = true;
    Client client(fake);
    const Result r = client.run(DIR, NAME);
    CHECK(!r.ok && r.error == "directory: Service Unavailable", "the directory down");
    CHECK(fake.seen.size() == 1, "nothing else asked");
  }
  {
    Fake fake;
    fake.silent = true;
    Client client(fake);
    CHECK(client.run(DIR, NAME).error == "directory: no answer", "no answer, the cause unknown");
    fake.silent = false;
    fake.unreachable = "its certificate does not check against the trust store";
    CHECK(client.run(DIR, NAME).error == "directory: its certificate does not check against the trust store",
          "no answer, and why when the device can tell");
  }
  {
    Fake fake;
    fake.offer_http01 = false;
    Client client(fake);
    const Result r = client.run(DIR, NAME);
    CHECK(!r.ok && r.error == "the server offers no http-01 challenge", "dns-01 alone is not answered");
    CHECK(fake.count("/challenge/") == 0, "no other challenge is tried");
  }
  {
    Fake fake;
    fake.csr_fails = true;
    Client client(fake);
    const Result r = client.run(DIR, NAME);
    CHECK(!r.ok && r.error == "could not make the certificate request", "a failed CSR");
    CHECK(fake.count("/finalize") == 0, "nothing finalized");
  }
  {
    Fake fake;
    fake.processing_polls = 1000;  // never issued
    Client client(fake);
    const Result r = client.run(DIR, NAME);
    CHECK(!r.ok && r.error.find("not issued") != std::string::npos, "a certificate that never comes");
    CHECK(fake.slept_ms <= (Client::POLLS + 5) * Client::POLL_MS, "the wait is bounded");
  }
  {
    Fake fake;
    Client client(fake);
    CHECK(!client.run("http://ca.example/directory", NAME).ok && fake.seen.empty(), "never over plain http");
    CHECK(!client.run(DIR, "*.local").ok && fake.seen.empty(), "never for a wildcard");
  }

  if (failures == 0)
    std::printf("test_acme: all passed\n");
  return failures == 0 ? 0 : 1;
}
