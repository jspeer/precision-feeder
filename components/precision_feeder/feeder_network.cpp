// Precision Feeder core: the network settings saved on the device (DESIGN.md section 9.3): the name, the fallback
// access point's password and the NTP servers, and the checks for those and for a Wi-Fi network set from the web
// app. Applying them is
// the adapter's job (precision_feeder.cpp), at boot, before Wi-Fi starts.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "feeder_internal.h"

#include <cstring>

namespace esphome {
namespace precision_feeder {

bool check_hostname(const std::string &name, std::string *problem) {
  auto refuse = [problem](const char *why) {
    if (problem != nullptr)
      *problem = why;
    return false;
  };
  if (name.empty())
    return refuse("empty");
  if (name.size() > HOSTNAME_SIZE - 1)
    return refuse("longer than 31 characters");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return refuse("only lowercase letters, digits and hyphens");
  }
  if (name.front() == '-' || name.back() == '-')
    return refuse("starts or ends with a hyphen");
  return true;
}

bool parse_ntp_servers(const std::string &text, std::vector<std::string> &out, std::string *problem) {
  auto refuse = [problem](const char *why) {
    if (problem != nullptr)
      *problem = why;
    return false;
  };
  out.clear();
  std::string current;
  auto finish = [&]() {
    if (current.empty())
      return true;
    if (out.size() == NTP_SERVERS_MAX)
      return refuse("more than three servers");
    if (current.size() > NTP_SERVER_LEN_MAX)
      return refuse("a server longer than 63 characters");
    if (current.front() == '-' || current.front() == '.' || current.back() == '-')
      return refuse("a server starts or ends with '-' or '.'");
    out.push_back(current);
    current.clear();
    return true;
  };
  for (char c : text) {
    if (c == ',' || c == ' ') {
      if (!finish())
        return false;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
               c == ':') {
      current += c;
    } else {
      return refuse("only letters, digits, '.', '-' and ':' (host names and IP addresses)");
    }
  }
  return finish();
}

Result FeederCore::set_ntp_servers(const std::string &text) {
  std::vector<std::string> servers;
  std::string problem;
  if (!parse_ntp_servers(text, servers, &problem)) {
    this->log_(LogLevel::kWarn, "NTP servers refused: %s", problem.c_str());
    return Result::INVALID_ARG;
  }
  std::string joined;
  for (const auto &s : servers)
    joined += (joined.empty() ? "" : ",") + s;
  if (joined == this->dev_.ntp_servers)
    return Result::OK;
  std::memset(this->dev_.ntp_servers, 0, NTP_SERVERS_SIZE);
  std::memcpy(this->dev_.ntp_servers, joined.data(), joined.size());  // at most 3 * 63 + 2 characters: fits
  this->hal_save_device(this->dev_);
  if (servers.empty()) {
    this->hal_apply_ntp(nullptr);
    this->log_(LogLevel::kInfo, "NTP servers: back to the build's");
  } else {
    this->hal_apply_ntp(&servers);
    this->log_(LogLevel::kInfo, "NTP servers set to %s", this->dev_.ntp_servers);
  }
  return Result::OK;
}

static_assert(sizeof(Device) == sizeof(legacy::DeviceV1) + NTP_SERVERS_SIZE, "version 2 only adds the NTP servers");

namespace legacy {
Device migrate_device_v1(const DeviceV1 &v1) {
  Device d;
  std::memset(&d, 0, sizeof(d));
  d.version = DEVICE_VERSION;
  d.servo_pin = v1.servo_pin;
  d.button_pin = v1.button_pin;
  d.led_pin = v1.led_pin;
  d.flags = v1.flags;
  std::memcpy(d.hostname, v1.hostname, sizeof(d.hostname));
  std::memcpy(d.timezone, v1.timezone, sizeof(d.timezone));
  std::memcpy(d.ap_password, v1.ap_password, sizeof(d.ap_password));
  std::memcpy(d.ble_key, v1.ble_key, sizeof(d.ble_key));
  d.password_scheme = v1.password_scheme;
  std::memcpy(d.password_salt, v1.password_salt, sizeof(d.password_salt));
  std::memcpy(d.password_hash, v1.password_hash, sizeof(d.password_hash));
  return d;  // ntp_servers empty: the build's
}
}  // namespace legacy

bool check_wifi_password(const std::string &password, bool for_wifi) {
  if (password.empty())
    return for_wifi;  // an open network; the access point always has a password
  if (password.size() == WIFI_PASSWORD_MAX) {
    if (!for_wifi)
      return false;
    for (char c : password) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        return false;
    }
    return true;
  }
  if (password.size() < 8 || password.size() > WIFI_PASSWORD_MAX - 1)
    return false;
  for (char c : password) {
    if (c < 0x20 || c > 0x7e)
      return false;
  }
  return true;
}

bool check_ssid(const std::string &ssid) {
  if (ssid.empty() || ssid.size() > WIFI_SSID_MAX)
    return false;
  for (char c : ssid) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
      return false;
  }
  return true;
}

void FeederCore::set_network_in_use(const std::string &hostname, bool has_ap, const Device &applied) {
  this->network_runtime_ = true;
  this->has_ap_ = has_ap;
  this->hostname_in_use_ = hostname;
  // What core_start() keeps of the record too: a value it would drop counts as the build's.
  std::memset(this->boot_hostname_, 0, HOSTNAME_SIZE);
  std::memset(this->boot_ap_password_, 0, AP_PASSWORD_SIZE);
  const std::string name(applied.hostname, strnlen(applied.hostname, HOSTNAME_SIZE - 1));
  const std::string ap(applied.ap_password, strnlen(applied.ap_password, AP_PASSWORD_SIZE - 1));
  if (check_hostname(name))
    std::memcpy(this->boot_hostname_, name.data(), name.size());
  if (check_wifi_password(ap, false))
    std::memcpy(this->boot_ap_password_, ap.data(), ap.size());
}

void FeederCore::set_setup_done(bool done) {
  if (this->setup_needed() == !done)
    return;
  this->dev_.flags = static_cast<uint8_t>(done ? this->dev_.flags | DEVICE_SETUP_DONE
                                               : this->dev_.flags & ~DEVICE_SETUP_DONE);
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kInfo, done ? "Setup finished" : "Setup will be offered again");
}

Result FeederCore::set_hostname(const std::string &name) {
  if (!this->network_runtime_)
    return Result::UNSUPPORTED;
  if (!name.empty() && !check_hostname(name))
    return Result::INVALID_ARG;
  if (name == this->dev_.hostname)
    return Result::OK;
  std::memset(this->dev_.hostname, 0, HOSTNAME_SIZE);
  std::memcpy(this->dev_.hostname, name.data(), name.size());  // check_hostname() checked the length
  this->hal_save_device(this->dev_);
  if (name.empty())
    this->log_(LogLevel::kInfo, "Name: back to the build's at the next restart");
  else
    this->log_(LogLevel::kInfo, "Name set to %s (used after the next restart)", this->dev_.hostname);
  return Result::OK;
}

Result FeederCore::set_ap_password(const std::string &password) {
  if (!this->network_runtime_ || !this->has_ap_)
    return Result::UNSUPPORTED;
  if (!password.empty() && !check_wifi_password(password, false))
    return Result::INVALID_ARG;
  if (password == this->dev_.ap_password)
    return Result::OK;
  std::memset(this->dev_.ap_password, 0, AP_PASSWORD_SIZE);
  std::memcpy(this->dev_.ap_password, password.data(), password.size());
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kInfo, password.empty() ? "Access-point password: back to the build's at the next restart"
                                               : "Access-point password changed (used after the next restart)");
  return Result::OK;
}

}  // namespace precision_feeder
}  // namespace esphome
