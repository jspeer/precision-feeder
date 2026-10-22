// Precision Feeder core: constants and helpers shared by feeder_core.cpp, feeder_schedule.cpp and feeder_text.cpp.
// Internal to the component; not part of FeederCore's interface.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include "feeder_core.h"

namespace esphome {
namespace precision_feeder {

inline constexpr int HORIZON_DAYS = 30;
inline constexpr uint32_t CFG_SAVE_DELAY_MS = 2000;
inline constexpr int64_t STALE_CLOCK_MIN = 7 * 24 * 60;
// Late feeds are skipped by design. An event only fires during the minute it is due; if the feeder was
// offline, on hold, reloading, or the guard refused it for that whole minute, the event is lost. The window
// exists only so a BUSY or TOO_SOON answer can be retried on the next tick.
inline constexpr int64_t EVENT_WINDOW_S = 60;
inline constexpr uint8_t NO_PENDING = static_cast<uint8_t>(Result::NONE);

inline const char *const DAY_NAMES[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
inline const char *const DAY_ABBR[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
inline const char *const SOURCE_NAMES[6] = {"schedule", "ESPHome", "ble", "button", "one-shot", "adjustment"};

inline const char *source_name(Source s) { return SOURCE_NAMES[static_cast<uint8_t>(s) % 6]; }


}  // namespace precision_feeder
}  // namespace esphome
