// Precision Feeder: civil-calendar helpers (pure functions, no ESPHome dependencies).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstdint>

namespace esphome {
namespace precision_feeder {

/// Days since 1970-01-01 for a civil (proleptic Gregorian) date.
/// Algorithm: Howard Hinnant's days_from_civil.
inline int32_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

/// Inverse of days_from_civil.
inline void civil_from_days(int32_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

/// Weekday for a day number: 0 = Sunday ... 6 = Saturday.
inline int weekday_from_days(int32_t z) {
  int w = (z + 4) % 7;  // 1970-01-01 was a Thursday (4)
  return w < 0 ? w + 7 : w;
}

}  // namespace precision_feeder
}  // namespace esphome
