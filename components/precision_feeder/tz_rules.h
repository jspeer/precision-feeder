// POSIX TZ strings ("EST5EDT,M3.2.0,M11.1.0"), parsed on the device so the timezone can change at run time.
//
// ESPHome parses its timezone when the firmware is built and Home Assistant sends a parsed one; neither can take a
// string at run time. This parser produces the same fields as ESPHome's (aioesphomeapi's posix_tz.py is the
// reference; tests/test_core.cpp holds golden vectors made with it), so the adapter can hand the result straight to
// ESPHome's time::set_global_tz(). It is stricter than that reference, because its input is typed by a person: it
// refuses trailing characters, a daylight-saving name with no rules, and offsets beyond 24 hours.
//
// Plain C++ with no ESPHome includes, tested on a host.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstdint>

namespace esphome {
namespace precision_feeder {

/// How a daylight-saving rule names its day (the values of ESPHome's time::DSTRuleType).
enum class TzRuleType : uint8_t {
  NONE = 0,
  MONTH_WEEK_DAY = 1,  // Mm.w.d: weekday d (0 = Sunday) of week w (5 = last) of month m
  JULIAN_NO_LEAP = 2,  // Jn: day 1..365, February 29 never counted
  DAY_OF_YEAR = 3,     // n: day 0..365, February 29 counted in leap years
};

/// One daylight-saving transition.
struct TzRule {
  int32_t time_s{0};  // local time of the change, seconds after midnight (may be negative or past 24 h)
  uint16_t day{0};    // JULIAN_NO_LEAP and DAY_OF_YEAR
  TzRuleType type{TzRuleType::NONE};
  uint8_t month{0};        // MONTH_WEEK_DAY: 1..12
  uint8_t week{0};         // MONTH_WEEK_DAY: 1..5
  uint8_t day_of_week{0};  // MONTH_WEEK_DAY: 0..6
};

/// A parsed timezone. Offsets follow POSIX: seconds *west* of UTC (New York is +18000).
struct TzRules {
  int32_t std_offset_s{0};
  int32_t dst_offset_s{0};
  TzRule dst_start;
  TzRule dst_end;
  bool has_dst() const { return this->dst_start.type != TzRuleType::NONE; }
};

/// Longest POSIX TZ string accepted (Device::timezone holds it with its terminator).
constexpr unsigned TZ_TEXT_MAX = 63;

namespace tz_detail {

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/// Digits at p as a number (at most 5 digits, so no overflow). False if there are none.
inline bool number(const char *&p, uint32_t &out) {
  if (!is_digit(*p))
    return false;
  out = 0;
  for (int n = 0; is_digit(*p); n++, p++) {
    if (n == 5)
      return false;
    out = out * 10 + static_cast<uint32_t>(*p - '0');
  }
  return true;
}

/// A zone name: three or more letters, or <...> holding three or more letters, digits, '+' or '-'.
inline bool name(const char *&p) {
  int n = 0;
  if (*p == '<') {
    p++;
    for (; *p != '>'; p++, n++) {
      if (!is_alpha(*p) && !is_digit(*p) && *p != '+' && *p != '-')
        return false;
    }
    p++;
  } else {
    for (; is_alpha(*p); p++)
      n++;
  }
  return n >= 3;
}

/// [+|-]hh[:mm[:ss]] as signed seconds, hours up to max_h, minutes and seconds up to 59.
inline bool offset(const char *&p, uint32_t max_h, int32_t &out) {
  int32_t sign = 1;
  if (*p == '+' || *p == '-') {
    sign = *p == '-' ? -1 : 1;
    p++;
  }
  uint32_t h = 0, m = 0, s = 0;
  if (!number(p, h) || h > max_h)
    return false;
  if (*p == ':') {
    p++;
    if (!number(p, m) || m > 59)
      return false;
    if (*p == ':') {
      p++;
      if (!number(p, s) || s > 59)
        return false;
    }
  }
  out = sign * static_cast<int32_t>(h * 3600 + m * 60 + s);
  return true;
}

/// Mm.w.d, Jn or n, then an optional /time (default 02:00).
inline bool rule(const char *&p, TzRule &r) {
  uint32_t v = 0;
  if (*p == 'M') {
    p++;
    r.type = TzRuleType::MONTH_WEEK_DAY;
    if (!number(p, v) || v < 1 || v > 12 || *p != '.')
      return false;
    r.month = static_cast<uint8_t>(v);
    p++;
    if (!number(p, v) || v < 1 || v > 5 || *p != '.')
      return false;
    r.week = static_cast<uint8_t>(v);
    p++;
    if (!number(p, v) || v > 6)
      return false;
    r.day_of_week = static_cast<uint8_t>(v);
  } else if (*p == 'J') {
    p++;
    r.type = TzRuleType::JULIAN_NO_LEAP;
    if (!number(p, v) || v < 1 || v > 365)
      return false;
    r.day = static_cast<uint16_t>(v);
  } else {
    r.type = TzRuleType::DAY_OF_YEAR;
    if (!number(p, v) || v > 365)
      return false;
    r.day = static_cast<uint16_t>(v);
  }
  r.time_s = 2 * 3600;
  if (*p == '/') {
    p++;
    // POSIX allows transition times from -167 to +167 hours.
    if (!offset(p, 167, r.time_s))
      return false;
  }
  return true;
}

}  // namespace tz_detail

/// Parse a POSIX TZ string. On failure returns false and leaves `out` unchanged.
inline bool parse_tz(const char *text, TzRules &out) {
  using namespace tz_detail;
  if (text == nullptr)
    return false;
  unsigned len = 0;
  while (text[len] != '\0') {
    if (++len > TZ_TEXT_MAX)
      return false;
  }
  TzRules tz{};
  const char *p = text;
  if (!name(p) || !offset(p, 24, tz.std_offset_s))
    return false;
  if (*p == '\0') {
    out = tz;
    return true;
  }
  if (!name(p))
    return false;
  tz.dst_offset_s = tz.std_offset_s - 3600;  // daylight saving is one hour ahead unless an offset follows
  if (*p != ',' && !offset(p, 24, tz.dst_offset_s))
    return false;
  // A daylight-saving name with no rules would leave the change dates to guesswork: refused.
  if (*p != ',')
    return false;
  p++;
  if (!rule(p, tz.dst_start) || *p != ',')
    return false;
  p++;
  if (!rule(p, tz.dst_end) || *p != '\0')
    return false;
  out = tz;
  return true;
}

}  // namespace precision_feeder
}  // namespace esphome
