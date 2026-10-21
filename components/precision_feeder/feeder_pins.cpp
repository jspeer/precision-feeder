// Precision Feeder core: the servo, button and LED pins set at run time (DESIGN.md section 2.1).
//
// The pins are fixed when the drivers start, so the adapter resolves the saved ones at boot (resolve_pins) before
// any driver is set up, and a change made here applies at the next restart. Only pins the ESP32-S3 can spare are
// accepted: see PINS_REFUSED and PINS_WARNED in feeder_core.h.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "feeder_internal.h"

#include <cctype>
#include <cstdlib>

namespace esphome {
namespace precision_feeder {

static const char *const ROLE_NAMES[3] = {"servo", "button", "LED"};

PinCheck check_pin(uint8_t gpio) {
  if (gpio > GPIO_MAX)
    return PinCheck::REFUSED;
  for (uint8_t p : PINS_REFUSED) {
    if (p == gpio)
      return PinCheck::REFUSED;
  }
  for (uint8_t p : PINS_WARNED) {
    if (p == gpio)
      return PinCheck::WARN;
  }
  return PinCheck::OK;
}

const char *pin_note(uint8_t gpio) {
  if (gpio > GPIO_MAX || (gpio >= 22 && gpio <= 25))
    return "not on the ESP32-S3";
  if (gpio >= 26 && gpio <= 32)
    return "used by the SPI flash and PSRAM";
  if (gpio == 19 || gpio == 20)
    return "the USB port";
  if (gpio == 0 || gpio == 45 || gpio == 46)
    return "a strapping pin (it decides how the chip boots)";
  if (gpio == 3)
    return "a strapping pin for the JTAG source, read only if the STRAP_JTAG_SEL eFuse is burned (not by default)";
  if (gpio >= 33 && gpio <= 37)
    return "used by the octal flash or PSRAM of some modules (for example N8R8); fine on boards without";
  if (gpio == 43 || gpio == 44)
    return "UART0: the chip prints boot messages on it at every reset";
  return "";
}

std::string pin_text(uint8_t gpio) {
  if (gpio == PIN_NONE)
    return "none";
  if (gpio == PIN_BUILD)
    return "build";
  return "GPIO" + std::to_string(gpio);
}

bool parse_pin(const std::string &text, uint8_t &gpio) {
  std::string t;
  for (char c : text)
    t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "none") {
    gpio = PIN_NONE;
    return true;
  }
  if (t.compare(0, 4, "gpio") == 0)
    t = t.substr(4);
  if (t.empty() || t.size() > 2)
    return false;
  for (char c : t) {
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return false;
  }
  gpio = static_cast<uint8_t>(std::atoi(t.c_str()));
  return true;
}

const char *board_name(Board board) {
  if (board == Board::BUILD)
    return "Build Default";
  for (const auto &p : BOARD_PRESETS) {
    if (p.board == board)
      return p.name;
  }
  return "Custom";
}

bool parse_board(const std::string &text, Board &board) {
  for (Board b : {Board::BUILD, Board::WAVESHARE_S3_ZERO, Board::DEVKITC_1, Board::CUSTOM}) {
    if (text == board_name(b)) {
      board = b;
      return true;
    }
  }
  return false;
}

static uint8_t saved_pin(const Device &d, PinRole role) {
  return role == PinRole::SERVO ? d.servo_pin : role == PinRole::BUTTON ? d.button_pin : d.led_pin;
}

static uint8_t &pin_of(Pins &p, PinRole role) {
  return role == PinRole::SERVO ? p.servo : role == PinRole::BUTTON ? p.button : p.led;
}

/// A saved pin a role can use: the servo needs one, none may be refused, and none may be one the mechanism drives.
static bool usable(PinRole role, uint8_t gpio, const ReservedPins *reserved = nullptr) {
  if (gpio == PIN_NONE)
    return role != PinRole::SERVO;
  if (reserved != nullptr && reserved->has(gpio))
    return false;
  return check_pin(gpio) != PinCheck::REFUSED;
}

/// Two roles on one GPIO. "None" never clashes: no button, no LED, and no servo pin (a build whose mechanism is not
/// the servo).
static bool clash(const Pins &p) {
  auto same = [](uint8_t a, uint8_t b) { return a != PIN_NONE && a == b; };
  return same(p.servo, p.button) || same(p.servo, p.led) || same(p.button, p.led);
}

Pins resolve_pins(const Device &device, const Pins &build, std::string *problem, const ReservedPins *reserved) {
  Pins out = build;
  for (PinRole role : {PinRole::SERVO, PinRole::BUTTON, PinRole::LED}) {
    const uint8_t saved = saved_pin(device, role);
    if (saved == PIN_BUILD)
      continue;
    if (usable(role, saved, reserved)) {
      pin_of(out, role) = saved;
    } else if (problem != nullptr) {
      *problem += std::string(problem->empty() ? "" : "; ") + "saved " + ROLE_NAMES[static_cast<uint8_t>(role)] +
                  " pin " + pin_text(saved) + " not usable, the build's kept";
    }
  }
  if (clash(out)) {
    if (problem != nullptr)
      *problem += std::string(problem->empty() ? "" : "; ") + "saved pins clash, the build's used";
    return build;
  }
  return out;
}

Pins FeederCore::pins_saved() const {
  Pins p = resolve_pins(this->dev_, this->build_pins_, nullptr, &this->reserved_pins_);
  if (!this->has_button_)
    p.button = PIN_NONE;
  if (!this->has_led_)
    p.led = PIN_NONE;
  return p;
}

Result FeederCore::set_pin(PinRole role, uint8_t gpio) {
  const char *name = ROLE_NAMES[static_cast<uint8_t>(role)];
  if (!this->pins_runtime_ || (role == PinRole::SERVO && !this->is_servo()) || (role == PinRole::BUTTON && !this->has_button_) ||
      (role == PinRole::LED && !this->has_led_))
    return Result::UNSUPPORTED;
  if (gpio != PIN_BUILD && !usable(role, gpio, &this->reserved_pins_)) {
    if (gpio == PIN_NONE)
      this->log_(LogLevel::kWarn, "The servo needs a pin");
    else if (this->reserved_pins_.has(gpio))
      this->log_(LogLevel::kWarn, "%s refused for the %s: the motor uses it", pin_text(gpio).c_str(), name);
    else
      this->log_(LogLevel::kWarn, "%s refused for the %s: %s", pin_text(gpio).c_str(), name, pin_note(gpio));
    return Result::INVALID_ARG;
  }
  Device next = this->dev_;
  (role == PinRole::SERVO ? next.servo_pin : role == PinRole::BUTTON ? next.button_pin : next.led_pin) = gpio;
  // Checked as the next restart will see them, so a pin taken by another role is refused here rather than
  // silently replaced by the build's pins at boot.
  Pins after = this->build_pins_;
  for (PinRole r : {PinRole::SERVO, PinRole::BUTTON, PinRole::LED}) {
    const uint8_t s = saved_pin(next, r);
    if (s != PIN_BUILD && usable(r, s, &this->reserved_pins_))
      pin_of(after, r) = s;
  }
  if (clash(after)) {
    this->log_(LogLevel::kWarn, "%s refused for the %s: another part already uses it", pin_text(gpio).c_str(), name);
    return Result::INVALID_ARG;
  }
  if (saved_pin(next, role) == saved_pin(this->dev_, role))
    return Result::OK;
  if (gpio != PIN_BUILD && gpio != PIN_NONE && check_pin(gpio) == PinCheck::WARN)
    this->log_(LogLevel::kWarn, "%s for the %s: %s", pin_text(gpio).c_str(), name, pin_note(gpio));
  this->dev_ = next;
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kInfo, "Pin for the %s set to %s; restart to apply", name,
             pin_text(gpio == PIN_BUILD ? pin_of(after, role) : gpio).c_str());
  return Result::OK;
}

Result FeederCore::set_board(Board board) {
  if (!this->pins_runtime_)
    return Result::UNSUPPORTED;
  Device next = this->dev_;
  if (board == Board::BUILD) {
    next.servo_pin = next.button_pin = next.led_pin = PIN_BUILD;
  } else if (board == Board::CUSTOM) {
    return Result::OK;
  } else {
    const BoardPreset *preset = nullptr;
    for (const auto &p : BOARD_PRESETS) {
      if (p.board == board)
        preset = &p;
    }
    if (preset == nullptr)
      return Result::INVALID_ARG;
    next.servo_pin = preset->pins.servo;
    next.button_pin = preset->pins.button;
    next.led_pin = preset->pins.led;
  }
  if (next.servo_pin == this->dev_.servo_pin && next.button_pin == this->dev_.button_pin &&
      next.led_pin == this->dev_.led_pin)
    return Result::OK;
  this->dev_ = next;
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kInfo, "Board set to %s; restart to apply", board_name(board));
  return Result::OK;
}

Board FeederCore::board() const {
  const Device &d = this->dev_;
  if (d.servo_pin == PIN_BUILD && d.button_pin == PIN_BUILD && d.led_pin == PIN_BUILD)
    return Board::BUILD;
  const Pins saved = this->pins_saved();
  for (const auto &p : BOARD_PRESETS) {
    if (p.pins == saved)
      return p.board;
  }
  return Board::CUSTOM;
}

std::string FeederCore::pins_text() const {
  const Pins &p = this->pins_in_use_;
  std::string out = "button " + pin_text(p.button) + ", LED " + pin_text(p.led);
  if (this->is_servo())
    return "servo " + pin_text(p.servo) + ", " + out;
  // Another mechanism: its own pins, set in its YAML.
  for (uint8_t i = 0; i < this->reserved_pins_.count; i++)
    out += std::string(i == 0 ? ", motor " : " ") + pin_text(this->reserved_pins_.pins[i]);
  return out;
}

}  // namespace precision_feeder
}  // namespace esphome
