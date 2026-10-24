// Precision Feeder TLS: the ACME client (RFC 8555), as plain C++ (firmware 2.4.2).
//
// Everything about ACME that is not I/O or cryptography: the JSON it reads, the JWS it sends (ES256, RFC 7515 and
// 7518), the order of requests (account, order, http-01 challenge, finalize, certificate), the retries, when to renew
// and what to check before asking. The device gives it an HTTPS client, the account key's signatures and a CSR through
// small interfaces (precision_feeder_tls.cpp); the host tests give it a recorded ACME server (tests/test_acme.cpp).
// No ESPHome or mbedTLS includes.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace esphome {
namespace precision_feeder {
namespace tls {
namespace acme {

// ---- base64url (RFC 4648 section 5, no padding) ---------------------------------------------------------------

inline std::string b64url(const uint8_t *data, size_t n) {
  static const char *const ABC = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out += ABC[v >> 18];
    out += ABC[(v >> 12) & 63];
    out += ABC[(v >> 6) & 63];
    out += ABC[v & 63];
  }
  if (n - i == 1) {
    const uint32_t v = uint32_t(data[i]) << 16;
    out += ABC[v >> 18];
    out += ABC[(v >> 12) & 63];
  } else if (n - i == 2) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out += ABC[v >> 18];
    out += ABC[(v >> 12) & 63];
    out += ABC[(v >> 6) & 63];
  }
  return out;
}

inline std::string b64url(const std::string &s) { return b64url(reinterpret_cast<const uint8_t *>(s.data()), s.size()); }

// ---- JSON: just enough to read an ACME server's answers -------------------------------------------------------
// Values are handled as raw text (a slice of the answer); member() finds one by name at the top level of an object,
// elements() splits an array, str() decodes a string. Malformed input gives empty results, never a crash.

namespace json {

inline size_t skip_ws(const std::string &s, size_t i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
    i++;
  return i;
}

/// The index just past the value starting at i, or npos if it is malformed. Objects and arrays nest at most 32 deep.
inline size_t skip_value(const std::string &s, size_t i, int depth = 0) {
  i = skip_ws(s, i);
  if (i >= s.size() || depth > 32)
    return std::string::npos;
  const char c = s[i];
  if (c == '"') {
    for (i++; i < s.size(); i++) {
      if (s[i] == '\\')
        i++;
      else if (s[i] == '"')
        return i + 1;
    }
    return std::string::npos;
  }
  if (c == '{' || c == '[') {
    const char close = c == '{' ? '}' : ']';
    i = skip_ws(s, i + 1);
    if (i < s.size() && s[i] == close)
      return i + 1;
    for (;;) {
      if (c == '{') {
        i = skip_value(s, i, depth + 1);  // the name
        if (i == std::string::npos)
          return i;
        i = skip_ws(s, i);
        if (i >= s.size() || s[i] != ':')
          return std::string::npos;
        i++;
      }
      i = skip_value(s, i, depth + 1);
      if (i == std::string::npos)
        return i;
      i = skip_ws(s, i);
      if (i >= s.size())
        return std::string::npos;
      if (s[i] == close)
        return i + 1;
      if (s[i] != ',')
        return std::string::npos;
      i++;
    }
  }
  // A number, true, false or null.
  const size_t start = i;
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' &&
         s[i] != '\t')
    i++;
  return i > start ? i : std::string::npos;
}

/// A string value's text, decoded. \uXXXX outside ASCII becomes '?' (ACME's URLs, tokens and states are ASCII).
inline std::string str(const std::string &raw) {
  std::string out;
  const size_t i0 = skip_ws(raw, 0);
  if (i0 >= raw.size() || raw[i0] != '"')
    return out;
  for (size_t i = i0 + 1; i < raw.size() && raw[i] != '"'; i++) {
    if (raw[i] != '\\') {
      out += raw[i];
      continue;
    }
    if (++i >= raw.size())
      break;
    switch (raw[i]) {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        out += '\r';
        break;
      case 'b':
      case 'f':
        break;
      case 'u': {
        unsigned v = 0;
        for (int k = 0; k < 4 && i + 1 < raw.size(); k++) {
          const char h = raw[++i];
          v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
        }
        out += v >= 0x20 && v < 0x80 ? static_cast<char>(v) : '?';
        break;
      }
      default:
        out += raw[i];  // \" \\ \/
    }
  }
  return out;
}

/// The raw value of the member `name` of the object `obj` (top level only); false if absent or malformed.
inline bool member(const std::string &obj, const char *name, std::string &raw) {
  size_t i = skip_ws(obj, 0);
  if (i >= obj.size() || obj[i] != '{')
    return false;
  i = skip_ws(obj, i + 1);
  while (i < obj.size() && obj[i] != '}') {
    const size_t name_end = skip_value(obj, i);
    if (name_end == std::string::npos || obj[i] != '"')
      return false;
    const std::string key = str(obj.substr(i, name_end - i));
    i = skip_ws(obj, name_end);
    if (i >= obj.size() || obj[i] != ':')
      return false;
    const size_t v = skip_ws(obj, i + 1);
    const size_t v_end = skip_value(obj, v);
    if (v_end == std::string::npos)
      return false;
    if (key == name) {
      raw = obj.substr(v, v_end - v);
      return true;
    }
    i = skip_ws(obj, v_end);
    if (i < obj.size() && obj[i] == ',')
      i = skip_ws(obj, i + 1);
  }
  return false;
}

/// The member `name` as a string ("" if absent or not a string).
inline std::string get(const std::string &obj, const char *name) {
  std::string raw;
  return member(obj, name, raw) ? str(raw) : std::string();
}

/// The raw elements of an array (empty if it is not one).
inline std::vector<std::string> elements(const std::string &arr) {
  std::vector<std::string> out;
  size_t i = skip_ws(arr, 0);
  if (i >= arr.size() || arr[i] != '[')
    return out;
  i = skip_ws(arr, i + 1);
  while (i < arr.size() && arr[i] != ']') {
    const size_t end = skip_value(arr, i);
    if (end == std::string::npos)
      return {};
    out.push_back(arr.substr(i, end - i));
    i = skip_ws(arr, end);
    if (i < arr.size() && arr[i] == ',')
      i = skip_ws(arr, i + 1);
  }
  if (i >= arr.size())
    return {};  // no closing bracket
  return out;
}

}  // namespace json

// ---- What the feeder asks for ---------------------------------------------------------------------------------

/// A DNS name the feeder may ask a certificate for: letters, digits, '-' and '.', labels of 1 to 63, at most 253 in
/// all, no wildcard (http-01 cannot prove one). Also keeps the name safe to put into JSON and a CSR as it is.
inline bool valid_name(const std::string &name) {
  if (name.empty() || name.size() > 253 || name.front() == '.' || name.back() == '.')
    return false;
  size_t label = 0;
  for (char c : name) {
    if (c == '.') {
      if (label == 0)
        return false;
      label = 0;
      continue;
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok || ++label > 63)
      return false;
  }
  return true;
}

/// The directory's URL: https only (the server is always checked against the trust store), printable, no quotes.
inline bool valid_directory(const std::string &url) {
  if (url.rfind("https://", 0) != 0 || url.size() <= 8 || url.size() > 255)
    return false;
  for (char c : url) {
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F || c == '"' || c == '\\')
      return false;
  }
  return true;
}

// ---- When to ask ----------------------------------------------------------------------------------------------

/// Waits after a failed attempt: 5 min, 15 min, 1 h, then every 3 h. A certificate that keeps failing to renew stays
/// in use until it expires (then the self-signed one), so retrying faster gains nothing.
inline uint32_t retry_delay_s(uint32_t failures) {
  static const uint32_t STEPS[] = {300, 900, 3600, 10800};
  if (failures == 0)
    return 0;
  return STEPS[failures - 1 < 3 ? failures - 1 : 3];
}

/// The certificate ACME last issued, as far as renewal cares.
struct Issued {
  bool have{false};
  bool same_name{false};  // issued for the name in use now
  bool same_dir{false};   // and by the directory set now
  int64_t not_before{0}, not_after{0};
};

/// When to renew: two thirds into the certificate's lifetime (16 h into step-ca's default 24 h).
inline int64_t renew_at(const Issued &c) { return c.not_before + (c.not_after - c.not_before) * 2 / 3; }

/// Whether to run ACME now. Only with a valid clock (the server's certificate and the dates cannot be checked
/// without one), and not before `next_try` after a failure. A missing certificate, or one for another name or
/// directory, is asked for at once; else at renew_at().
inline bool due(int64_t now, bool clock_valid, const Issued &c, int64_t next_try) {
  if (!clock_valid || now < next_try)
    return false;
  if (!c.have || !c.same_name || !c.same_dir)
    return true;
  return now >= renew_at(c);
}

/// Whether an issued certificate may be served: for the name in use, within its dates. One from a directory set
/// before stays until the new one delivers or it expires, as after any failed renewal (a typo in the directory must
/// not cost a working certificate). Without a valid clock it is used as it is (the feeder cannot tell, and a restart
/// before the clock is set must not drop a good certificate).
inline bool servable(int64_t now, bool clock_valid, const Issued &c) {
  if (!c.have || !c.same_name)
    return false;
  return !clock_valid || (now >= c.not_before && now < c.not_after);
}

/// Seconds since 1970 of a UTC date and time (for certificate dates; no time zone, no leap seconds).
inline int64_t utc_seconds(int year, int mon, int day, int hour, int min, int sec) {
  // Days from civil (Howard Hinnant's algorithm), valid for the Gregorian calendar.
  year -= mon <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + hour * 3600 + min * 60 + sec;
}

// ---- JWS (RFC 7515) with ES256 --------------------------------------------------------------------------------

/// An ES256 signature as DER (what mbedTLS gives: SEQUENCE { INTEGER r, INTEGER s }) into JWS's form: r || s, 32
/// bytes each, big-endian. False if malformed.
inline bool der_to_raw(const uint8_t *der, size_t n, uint8_t raw[64]) {
  size_t i = 0;
  auto length = [&](size_t &len) {
    if (i >= n)
      return false;
    len = der[i++];
    if (len & 0x80) {
      const size_t bytes = len & 0x7F;
      if (bytes == 0 || bytes > 2 || i + bytes > n)
        return false;
      len = 0;
      for (size_t k = 0; k < bytes; k++)
        len = (len << 8) | der[i++];
    }
    return i + len <= n;
  };
  size_t len = 0;
  if (n < 8 || der[i++] != 0x30 || !length(len))
    return false;
  for (int part = 0; part < 2; part++) {
    if (i >= n || der[i++] != 0x02 || !length(len) || len == 0)
      return false;
    const uint8_t *p = der + i;
    i += len;
    while (len > 0 && *p == 0) {  // the sign byte, and any leading zeros
      p++;
      len--;
    }
    if (len > 32)
      return false;
    uint8_t *out = raw + 32 * part;
    for (size_t k = 0; k < 32 - len; k++)
      out[k] = 0;
    for (size_t k = 0; k < len; k++)
      out[32 - len + k] = p[k];
  }
  return i == n;
}

/// The JWK of a P-256 public key, with its members in the order RFC 7638 hashes them, so it is also the input of
/// the key's thumbprint.
inline std::string jwk_p256(const uint8_t x[32], const uint8_t y[32]) {
  return "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"" + b64url(x, 32) + "\",\"y\":\"" + b64url(y, 32) + "\"}";
}

/// The protected header: the account's URL (`kid`) once there is one, else the key itself (`jwk`).
inline std::string protected_header(const std::string &nonce, const std::string &url, const std::string &kid,
                                    const std::string &jwk) {
  std::string h = "{\"alg\":\"ES256\",\"nonce\":\"" + nonce + "\",\"url\":\"" + url + "\",";
  h += kid.empty() ? "\"jwk\":" + jwk + "}" : "\"kid\":\"" + kid + "\"}";
  return h;
}

// ---- The client -----------------------------------------------------------------------------------------------

enum class Method : uint8_t { GET, HEAD, POST };

/// One answer of the server. `ok` false: no answer at all (name not found, TLS refused, timeout), and `why` says
/// which when the device can tell.
struct Answer {
  bool ok{false};
  int status{0};
  std::string body, nonce, location, why;
};

/// The device's side: an HTTPS client checked against the trust store, and the account key.
class Io {
 public:
  virtual ~Io() = default;
  /// One request; a POST's body is JSON (`application/jose+json`).
  virtual Answer request(Method method, const std::string &url, const std::string &body) = 0;
  /// The account key's public half as a JWK (jwk_p256).
  virtual std::string jwk() = 0;
  /// The SHA-256 of `data`, raw.
  virtual std::string sha256(const std::string &data) = 0;
  /// An ES256 signature of `input` with the account key, as r || s (64 bytes); empty on failure.
  virtual std::string sign(const std::string &input) = 0;
  /// The CSR for `name`, DER in base64url, with a new certificate key the device keeps; empty on failure.
  virtual std::string csr(const std::string &name) = 0;
  /// Serve the http-01 key authorization for this token on port 80 (empty token: stop serving).
  virtual void serve(const std::string &token, const std::string &key_authorization) = 0;
  /// Wait before polling again.
  virtual void sleep_ms(uint32_t ms) = 0;
};

struct Result {
  bool ok{false};
  std::string chain;  // PEM, leaf first
  std::string error;  // why not, for the ACME status
};

class Client {
 public:
  static constexpr uint32_t POLL_MS = 2000;
  static constexpr int POLLS = 30;  // a minute for each wait (step-ca answered in 23.7 s in all, docs/BENCH.md)

  explicit Client(Io &io) : io_(io) {}

  /// The whole exchange for one name: account (made or found), order, the http-01 challenge, finalize, certificate.
  /// Blocks for as long as it takes (up to a few minutes); run it in its own task.
  Result run(const std::string &directory, const std::string &name) {
    Result r = this->run_(directory, name);
    this->io_.serve("", "");
    return r;
  }

  /// The number of requests made, for the tests.
  int requests() const { return this->requests_; }

 protected:
  Result fail(const std::string &why) {
    Result r;
    r.error = why;
    return r;
  }

  /// The server's own words about a failed request ("detail" of its problem document), else its status.
  static std::string problem(const Answer &a) {
    if (!a.ok)
      return a.why.empty() ? "no answer" : a.why;
    const std::string detail = json::get(a.body, "detail");
    return detail.empty() ? "HTTP " + std::to_string(a.status) : detail.substr(0, 120);
  }

  Answer request(Method m, const std::string &url, const std::string &body = "") {
    this->requests_++;
    Answer a = this->io_.request(m, url, body);
    if (!a.nonce.empty())
      this->nonce_ = a.nonce;
    return a;
  }

  /// A signed POST (an empty payload is POST-as-GET). A badNonce is retried once with the nonce it brought.
  Answer post(const std::string &url, const std::string &payload) {
    Answer a;
    for (int attempt = 0; attempt < 2; attempt++) {
      const std::string prot = b64url(protected_header(this->nonce_, url, this->kid_, this->jwk_));
      const std::string body = b64url(payload);  // "" for POST-as-GET
      const std::string sig = this->io_.sign(prot + "." + body);
      if (sig.size() != 64) {
        a = Answer{};
        a.ok = true;  // not the server's fault: say so instead of "no answer"
        a.body = "{\"detail\":\"signing failed\"}";
        return a;
      }
      const std::string jws = "{\"protected\":\"" + prot + "\",\"payload\":\"" + body + "\",\"signature\":\"" +
                              b64url(sig) + "\"}";
      a = this->request(Method::POST, url, jws);
      const bool bad_nonce = a.ok && a.status == 400 && json::get(a.body, "type") == "urn:ietf:params:acme:error:badNonce";
      if (!bad_nonce)
        break;
    }
    return a;
  }

  static bool good(const Answer &a) { return a.ok && a.status >= 200 && a.status < 300; }

  /// POST-as-GET `url` until its "status" is one of `done` (or "invalid"); its last answer in `last`.
  bool poll(const std::string &url, const char *done, Answer &last) {
    for (int i = 0; i < POLLS; i++) {
      this->io_.sleep_ms(POLL_MS);
      last = this->post(url, "");
      if (!good(last))
        return false;
      const std::string state = json::get(last.body, "status");
      if (state == done || state == "invalid")
        return state == done;
    }
    return false;
  }

  Result run_(const std::string &directory, const std::string &name) {
    if (!valid_directory(directory))
      return this->fail("the directory must be an https:// URL");
    if (!valid_name(name))
      return this->fail("not a name a certificate can be asked for: " + name);
    this->jwk_ = this->io_.jwk();
    if (this->jwk_.empty())
      return this->fail("no account key");
    const std::string thumb = this->io_.sha256(this->jwk_);
    if (thumb.size() != 32)
      return this->fail("no account key");

    Answer dir = this->request(Method::GET, directory);
    if (!good(dir))
      return this->fail("directory: " + problem(dir));
    const std::string new_nonce = json::get(dir.body, "newNonce"), new_account = json::get(dir.body, "newAccount"),
                      new_order = json::get(dir.body, "newOrder");
    if (new_nonce.empty() || new_account.empty() || new_order.empty())
      return this->fail("directory: not an ACME directory");
    const Answer n = this->request(Method::HEAD, new_nonce);
    if (!n.ok || this->nonce_.empty())
      return this->fail("nonce: " + problem(n));

    // The account: made at the first run, found again by its key after that (200 instead of 201).
    const Answer acct = this->post(new_account, "{\"termsOfServiceAgreed\":true}");
    if (!good(acct) || acct.location.empty())
      return this->fail("account: " + problem(acct));
    this->kid_ = acct.location;

    const Answer order = this->post(new_order, "{\"identifiers\":[{\"type\":\"dns\",\"value\":\"" + name + "\"}]}");
    if (!good(order) || order.location.empty())
      return this->fail("order: " + problem(order));
    const std::string order_url = order.location, finalize = json::get(order.body, "finalize");
    std::string raw;
    if (finalize.empty() || !json::member(order.body, "authorizations", raw))
      return this->fail("order: no authorizations");
    const std::string order_state = json::get(order.body, "status");

    // Every authorization (one per name; the feeder asks for one) through http-01, unless already valid.
    for (const std::string &authz_raw : json::elements(raw)) {
      const std::string authz_url = json::str(authz_raw);
      Answer authz = this->post(authz_url, "");
      if (!good(authz))
        return this->fail("authorization: " + problem(authz));
      const std::string state = json::get(authz.body, "status");
      if (state == "valid")
        continue;
      if (state != "pending")
        return this->fail("authorization is " + state);
      std::string challenges, chal_url, token;
      json::member(authz.body, "challenges", challenges);
      for (const std::string &ch : json::elements(challenges)) {
        if (json::get(ch, "type") == "http-01") {
          chal_url = json::get(ch, "url");
          token = json::get(ch, "token");
        }
      }
      if (chal_url.empty() || token.empty() || token.find_first_of("/\"\\ ") != std::string::npos)
        return this->fail("the server offers no http-01 challenge");
      this->io_.serve(token, token + "." + b64url(thumb));
      const Answer go = this->post(chal_url, "{}");
      if (!good(go))
        return this->fail("challenge: " + problem(go));
      if (!this->poll(authz_url, "valid", authz)) {
        // The server's reason is in the challenge's "error" (for example: it could not reach the feeder's port 80).
        std::string why = "the server could not check the feeder (port 80 reachable at " + name + "?)";
        json::member(authz.body, "challenges", challenges);
        for (const std::string &ch : json::elements(challenges)) {
          std::string err;
          if (json::get(ch, "type") == "http-01" && json::member(ch, "error", err) && !json::get(err, "detail").empty())
            why = json::get(err, "detail").substr(0, 120);
        }
        return this->fail("challenge: " + why);
      }
      this->io_.serve("", "");
    }

    // Finalize once the order is ready (it may already be, when every authorization was valid).
    Answer state;
    if (order_state != "ready" && order_state != "valid" && !this->poll(order_url, "ready", state))
      return this->fail("order not ready: " + (state.ok ? json::get(state.body, "status") : problem(state)));
    const std::string csr = this->io_.csr(name);
    if (csr.empty())
      return this->fail("could not make the certificate request");
    Answer fin = this->post(finalize, "{\"csr\":\"" + csr + "\"}");
    if (!good(fin))
      return this->fail("finalize: " + problem(fin));
    std::string cert_url = json::get(fin.body, "certificate");
    if (json::get(fin.body, "status") != "valid" || cert_url.empty()) {
      if (!this->poll(order_url, "valid", fin))
        return this->fail("the certificate was not issued: " + (fin.ok ? json::get(fin.body, "status") : problem(fin)));
      cert_url = json::get(fin.body, "certificate");
    }
    if (cert_url.empty())
      return this->fail("the certificate was not issued");
    const Answer cert = this->post(cert_url, "");
    if (!good(cert) || cert.body.find("-----BEGIN CERTIFICATE-----") == std::string::npos)
      return this->fail("download: " + problem(cert));
    Result r;
    r.ok = true;
    r.chain = cert.body;
    return r;
  }

  Io &io_;
  std::string nonce_, kid_, jwk_;
  int requests_{0};
};

}  // namespace acme
}  // namespace tls
}  // namespace precision_feeder
}  // namespace esphome
