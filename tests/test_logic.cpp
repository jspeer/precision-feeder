// Host-side tests for the pure logic headers.
// Build and run:  g++ -std=c++17 -I../components/precision_feeder test_logic.cpp -o /tmp/test_logic && /tmp/test_logic
#include <cstdio>
#include <cstdlib>

#include "calendar.h"
#include "solar.h"

using namespace esphome::precision_feeder;

static int failures = 0;
#define CHECK(cond, msg)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      std::printf("FAIL: %s (line %d)\n", msg, __LINE__);   \
      failures++;                                           \
    }                                                       \
  } while (0)

static void hhmm(double minutes, char *out) {
  int m = static_cast<int>(minutes + 0.5);
  std::snprintf(out, 8, "%02d:%02d", (m / 60) % 24, m % 60);
}

int main() {
  // Calendar round trip and weekdays.
  CHECK(days_from_civil(1970, 1, 1) == 0, "epoch day");
  CHECK(weekday_from_days(0) == 4, "1970-01-01 is Thursday");
  CHECK(weekday_from_days(days_from_civil(2026, 10, 13)) == 2, "2026-10-13 is Tuesday");
  for (int32_t d = -1000; d < 40000; d += 37) {
    int y;
    unsigned m, dd;
    civil_from_days(d, y, m, dd);
    CHECK(days_from_civil(y, m, dd) == d, "civil round trip");
  }

  // Sunrise/sunset. Reference values are published almanac figures, tolerance 3 minutes.
  struct Case {
    const char *name;
    int y, m, d;
    double lat, lon;
    int tz;
    double rise, set;  // minutes after local midnight
  };
  const Case cases[] = {
      // Indianapolis, 2026-10-13, EDT (UTC-4): sunrise ~07:53, sunset ~19:09
      {"Indianapolis Oct", 2026, 10, 13, 39.7684, -86.1581, -240, 7 * 60 + 53, 19 * 60 + 9},
      // London, 2026-06-21, BST (UTC+1): sunrise ~04:43, sunset ~21:21
      {"London solstice", 2026, 6, 21, 51.5074, -0.1278, 60, 4 * 60 + 43, 21 * 60 + 21},
      // Sydney, 2026-12-21, AEDT (UTC+11): sunrise ~05:41, sunset ~20:05
      {"Sydney summer", 2026, 12, 21, -33.8688, 151.2093, 660, 5 * 60 + 41, 20 * 60 + 5},
  };
  for (const Case &c : cases) {
    double rise = 0, set = 0;
    bool ok = sun_times(days_from_civil(c.y, c.m, c.d), c.lat, c.lon, c.tz, rise, set);
    char a[8], b[8];
    hhmm(rise, a);
    hhmm(set, b);
    std::printf("%-18s sunrise %s sunset %s\n", c.name, a, b);
    CHECK(ok, "sun_times returned a value");
    CHECK(std::fabs(rise - c.rise) <= 3.0, "sunrise within 3 minutes");
    CHECK(std::fabs(set - c.set) <= 3.0, "sunset within 3 minutes");
  }

  // Polar night and midnight sun.
  double rise, set;
  CHECK(!sun_times(days_from_civil(2026, 12, 21), 78.22, 15.65, 60, rise, set), "Svalbard polar night");
  CHECK(!sun_times(days_from_civil(2026, 6, 21), 78.22, 15.65, 120, rise, set), "Svalbard midnight sun");

  if (failures == 0)
    std::printf("All tests passed\n");
  return failures == 0 ? 0 : 1;
}
