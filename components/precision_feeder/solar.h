// Precision Feeder: sunrise/sunset calculation (pure function, no ESPHome dependencies).
//
// Uses the NOAA solar-position equations (Meeus-based), standard refraction
// correction (zenith 90.833 degrees). Accuracy is about a minute at mid latitudes,
// which is plenty for scheduling a feed.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cmath>
#include <cstdint>

namespace esphome {
namespace precision_feeder {

namespace solar_detail {
constexpr double kPi = 3.14159265358979323846;
inline double rad(double d) { return d * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }
}  // namespace solar_detail

/// Sunrise and sunset for one local calendar day.
///
/// @param day_no       days since 1970-01-01 of the LOCAL date
/// @param lat          latitude in degrees, north positive
/// @param lon          longitude in degrees, east positive
/// @param tz_minutes   UTC offset in minutes (east positive) in effect at local noon
/// @param rise         out: minutes after local midnight (may be outside 0..1439 at extreme longitudes)
/// @param set          out: minutes after local midnight
/// @return false when the sun does not rise or set that day (polar day or night)
inline bool sun_times(int32_t day_no, double lat, double lon, int tz_minutes, double &rise, double &set) {
  using namespace solar_detail;
  // Julian day of local noon.
  const double jd = day_no + 2440587.5 + 0.5 - tz_minutes / 1440.0;
  const double t = (jd - 2451545.0) / 36525.0;

  const double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
  const double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
  const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
  const double c = std::sin(rad(m)) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                   std::sin(rad(2 * m)) * (0.019993 - 0.000101 * t) + std::sin(rad(3 * m)) * 0.000289;
  const double true_long = l0 + c;
  const double omega = 125.04 - 1934.136 * t;
  const double lambda = true_long - 0.00569 - 0.00478 * std::sin(rad(omega));
  const double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
  const double eps = eps0 + 0.00256 * std::cos(rad(omega));
  const double decl = std::asin(std::sin(rad(eps)) * std::sin(rad(lambda)));

  double y = std::tan(rad(eps / 2.0));
  y *= y;
  const double eq_time =
      4.0 * deg(y * std::sin(2.0 * rad(l0)) - 2.0 * e * std::sin(rad(m)) +
                4.0 * e * y * std::sin(rad(m)) * std::cos(2.0 * rad(l0)) - 0.5 * y * y * std::sin(4.0 * rad(l0)) -
                1.25 * e * e * std::sin(2.0 * rad(m)));

  const double lat_r = rad(lat);
  const double cos_h = std::cos(rad(90.833)) / (std::cos(lat_r) * std::cos(decl)) - std::tan(lat_r) * std::tan(decl);
  if (!(cos_h >= -1.0 && cos_h <= 1.0))
    return false;  // also catches NaN
  const double h = deg(std::acos(cos_h));

  const double noon_utc = 720.0 - 4.0 * lon - eq_time;
  rise = noon_utc - 4.0 * h + tz_minutes;
  set = noon_utc + 4.0 * h + tz_minutes;
  return true;
}

}  // namespace precision_feeder
}  // namespace esphome
