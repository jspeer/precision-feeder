// Host tests for the direct-drive disc mechanism (components/precision_feeder/mechanism_n20.*), on a simulated motor
// (tests/motor_sim.h). Two parts:
// - The motion against the owner's bench sketch (tests/reference/motor-test.cpp), compiled here with a stand-in
//   Arduino core (tests/reference/Arduino.h): the same commands on the same simulated motor must drive the bridge
//   identically, change for change and millisecond for millisecond.
// - The mechanism alone and inside FeederCore: feeds, an empty disc, reload, every fault, stop, the guided
//   calibration and the tuning values.
// Build and run (tools/check.sh does):
//   c++ -std=c++17 -I../components/precision_feeder -I. -Ireference test_n20.cpp ../components/precision_feeder/feeder_*.cpp ../components/precision_feeder/mechanism_*.cpp
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <math.h>
#include <memory>
#include <string>
#include <vector>

#include "motor_sim.h"

// ---- the bench sketch, as the reference ------------------------------------------------------------
MotorSim *g_ref_sim = nullptr;
std::string g_ref_serial_in;
std::function<void()> g_ref_bridge_written;
namespace ref {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "motor-test.cpp"
#pragma GCC diagnostic pop
}  // namespace ref

#include "feeder_core.h"
#include "mechanism_n20.h"

using namespace esphome::precision_feeder;

static int failures = 0;
static const char *current = "";
#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL [%s]: %s (line %d)\n", current, msg, __LINE__);   \
      failures++;                                                         \
    }                                                                     \
  } while (0)

#include "fake_feeder.h"

// ---- recording what the bridge does ------------------------------------------------------------------
struct BridgeState {
  int in1, in2;
  bool en;
  bool operator==(const BridgeState &o) const { return in1 == o.in1 && in2 == o.in2 && en == o.en; }
};
struct Change {
  uint32_t ms;
  BridgeState s;
};
/// The bridge's changes, one per millisecond at most (the state at the end of each instant), each different from the
/// one before: what the motor saw.
struct Recorder {
  std::vector<Change> raw;
  void note(uint32_t ms, const MotorSim &sim) { this->raw.push_back({ms, {sim.in1, sim.in2, sim.enable}}); }
  std::vector<Change> changes() const {
    std::vector<Change> out;
    for (size_t i = 0; i < this->raw.size(); i++) {
      if (i + 1 < this->raw.size() && this->raw[i + 1].ms == this->raw[i].ms)
        continue;
      if (!out.empty() && out.back().s == this->raw[i].s)
        continue;
      out.push_back(this->raw[i]);
    }
    return out;
  }
};

static void apply(MotorSim &sim, const Bridge &b) {
  switch (b.mode) {
    case BridgeMode::STANDBY:
      sim.in1 = sim.in2 = 0;
      sim.enable = false;
      break;
    case BridgeMode::COAST:
      sim.in1 = sim.in2 = 0;
      sim.enable = true;
      break;
    case BridgeMode::FORWARD:
      sim.in1 = b.duty;
      sim.in2 = 0;
      sim.enable = true;
      break;
    case BridgeMode::REVERSE:
      sim.in1 = 0;
      sim.in2 = b.duty;
      sim.enable = true;
      break;
    case BridgeMode::BRAKE:
      sim.in1 = sim.in2 = 255;
      sim.enable = true;
      break;
  }
}

// ---- part 1: the motion against the sketch ------------------------------------------------------------
// A session is a list of console commands for the sketch ("f 24", "w", "s", ...), each sent when the motor is idle,
// or at a given millisecond ("@1234 s" sends s then, whatever it is doing). Optional plant events: "@ms hold" and
// "@ms free" (something holds the disc, then lets go).
struct Step {
  int64_t at;  // -1: when idle
  std::string cmd;
};

static std::vector<Change> run_reference(const std::vector<Step> &session, uint32_t ms_limit, std::string *log) {
  MotorSim sim;
  Recorder rec;
  g_ref_sim = &sim;
  g_ref_serial_in.clear();
  g_ref_bridge_written = [&]() { rec.note(sim.millis(), sim); };
  // A fresh sketch: its globals as at power-up.
  ref::Serial.out.clear();
  ref::ticks = 0;
  ref::edgesA = ref::edgesB = ref::invalid = 0;
  ref::previous = 0;
  ref::phase = ref::IDLE;
  ref::plannedTicks = 0;
  ref::wiggle = ref::WiggleState{};
  ref::used = 0;
  ref::discard = false;
  ref::setup();  // waits 500 ms
  size_t next = 0;
  bool idle = false;
  for (uint32_t i = 0; i < ms_limit; i++) {
    // Plant events and timed commands, typed before this loop pass (the sketch reads them after its control).
    while (next < session.size() && session[next].at >= 0 && sim.millis() >= session[next].at) {
      const std::string &c = session[next++].cmd;
      if (c == "hold")
        sim.held = true;
      else if (c == "free")
        sim.held = false;
      else
        g_ref_serial_in += c == "s" ? std::string("s") : c + "\n";
    }
    if (idle && next < session.size() && session[next].at < 0)
      g_ref_serial_in += session[next++].cmd + "\n";
    ref::loop();
    idle = ref::phase == ref::IDLE && !ref::wiggle.active && g_ref_serial_in.empty();
    if (next >= session.size() && idle)
      break;
  }
  g_ref_bridge_written = nullptr;
  if (log)
    *log = ref::Serial.out;
  return rec.changes();
}

static std::vector<Change> run_motion(const std::vector<Step> &session, uint32_t ms_limit) {
  MotorSim sim;
  Recorder rec;
  N20Motion m;
  N20Timing t;
  t.drive_stall_ms = t.stall_ms;  // the sketch has one stall window
  t.timeout_base_ms = 120000;     // and a fixed 120 s limit
  t.watchdog_ms = 1000000;
  m.set_timing(t);
  m.set_approach_ticks(160);
  rec.note(0, sim);     // standby at power-up
  sim.advance_ms(500);  // as the sketch's setup() waits
  const double tpr = 8344.0;
  double plan = 0;  // the sketch's planned position, in fractional ticks
  size_t next = 0;
  bool idle = false, was_busy = false;
  auto command = [&](const std::string &c, uint32_t now) {
    const int32_t ticks = static_cast<int32_t>(sim.count());
    if (c == "s") {
      if (!m.busy())
        plan = ticks;
      m.stop(now);
    } else if (c[0] == 'w') {
      double amp = 2;
      unsigned cycles = 4;
      std::sscanf(c.c_str() + 1, "%lf %u", &amp, &cycles);
      m.set_power(100);
      m.start_wiggle(static_cast<int32_t>(std::lround(amp * tpr / 360)), static_cast<uint8_t>(cycles), ticks, now);
    } else {
      double amount = 0, power = 100;
      std::sscanf(c.c_str() + 1, "%lf %lf", &amount, &power);
      const double degrees = c[0] == 'v' ? amount * 360 : amount * (c[0] == 'r' ? -1 : 1);
      plan += degrees * tpr / 360;
      m.set_power(static_cast<uint8_t>(std::lround(power)));
      m.start(static_cast<int32_t>(std::lround(plan)), ticks, now);
    }
    apply(sim, m.bridge());
    rec.note(now, sim);
  };
  for (uint32_t i = 0; i < ms_limit; i++) {
    const uint32_t now = sim.millis();
    std::vector<std::string> typed;
    while (next < session.size() && session[next].at >= 0 && now >= session[next].at) {
      const std::string &c = session[next++].cmd;
      if (c == "hold")
        sim.held = true;
      else if (c == "free")
        sim.held = false;
      else
        typed.push_back(c);
    }
    if (idle && next < session.size() && session[next].at < 0)
      typed.push_back(session[next++].cmd);
    // The control first, then what was typed, as in the sketch's loop().
    apply(sim, m.step(now, static_cast<int32_t>(sim.count())));
    rec.note(now, sim);
    if (was_busy && !m.busy() && m.status() == N20Motion::Status::FAULT)
      plan = static_cast<double>(sim.count());  // as the sketch rebases its plan after a stop or a fault
    for (const auto &c : typed)
      command(c, now);
    was_busy = m.busy();
    sim.advance_ms(1);
    idle = !m.busy();
    if (next >= session.size() && idle)
      break;
  }
  return rec.changes();
}

static bool same_changes(const std::vector<Change> &a, const std::vector<Change> &b, const char *what) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    if (a[i].ms != b[i].ms || !(a[i].s == b[i].s)) {
      std::printf("  %s: change %zu differs: sketch %u ms (%d,%d,%d), firmware %u ms (%d,%d,%d)\n", what, i, a[i].ms,
                  a[i].s.in1, a[i].s.in2, a[i].s.en, b[i].ms, b[i].s.in1, b[i].s.in2, b[i].s.en);
      return false;
    }
  }
  if (a.size() != b.size()) {
    std::printf("  %s: %zu changes from the sketch, %zu from the firmware\n", what, a.size(), b.size());
    return false;
  }
  return true;
}

/// Runs a session through both; returns the sketch's console output, so a test can check what happened.
static std::string compare(const char *name, const std::vector<Step> &session, uint32_t limit = 600000) {
  current = name;
  std::string log;
  const auto a = run_reference(session, limit, &log);
  const auto b = run_motion(session, limit);
  CHECK(a.size() > 4, "the sketch drove the bridge");
  CHECK(same_changes(a, b, name), "the firmware drives the bridge exactly as the bench sketch does");
  return log;
}

static int occurrences(const std::string &text, const char *what) {
  int n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
    n++;
  return n;
}

static std::vector<Step> idle(std::initializer_list<const char *> cmds) {
  std::vector<Step> s;
  for (const char *c : cmds)
    s.push_back({-1, c});
  return s;
}

static void test_motion_matches_sketch() {
  std::vector<Step> turn;
  for (int i = 0; i < 15; i++)
    turn.push_back({-1, "f 24"});
  std::string log = compare("motion: 15 slots, one turn", turn);
  CHECK(log.find("target=8344") != std::string::npos, "the sketch's 15th move targets 8344 (one turn)");
  log = compare("motion: wiggle", idle({"f 24", "w", "f 24", "w 0.5 3", "w 5 1"}));
  CHECK(occurrences(log, "WIGGLE DONE") == 3, "three wiggles completed");
  compare("motion: long and short moves", idle({"v 1", "r 5", "f 1", "f 0.2", "r 24", "v -1"}));
  compare("motion: part power", idle({"f 24 60", "f 24 30", "r 10 80"}));
  // A stall: the disc is held mid-move (no encoder edges for 500 ms), then freed and moved on.
  log = compare("motion: stall", {{-1, "f 24"}, {600, "hold"}, {1500, "free"}, {-1, "f 24"}, {-1, "f 24"}});
  CHECK(occurrences(log, "FAULT: no encoder motion") == 1, "the sketch stalled once in it");
  // A stop mid-move, and one in the middle of a wiggle.
  // Times are from power-up; the sketch's setup takes 500 ms.
  log = compare("motion: stop", {{-1, "f 24"}, {620, "s"}, {-1, "f 24"}, {-1, "w"}, {2900, "s"}, {-1, "f 24"}});
  CHECK(occurrences(log, "USER STOP") == 2 && occurrences(log, "WIGGLE ABORTED") == 1,
        "the sketch stopped twice, once inside the wiggle");
}

// ---- part 2: the mechanism -----------------------------------------------------------------------------
/// A lock that checks it is always released (and never taken twice: the mechanism must not nest its guard).
struct CountingLock : N20Lock {
  int depth{0}, max_depth{0}, takes{0};
  void lock() override {
    depth++;
    takes++;
    max_depth = std::max(max_depth, depth);
  }
  void unlock() override { depth--; }
};

/// The feeder core with the disc mechanism on a simulated motor: every millisecond the adapter's control step, every
/// millisecond the core's loop.
struct Disc {
  Flash flash;
  MotorSim sim;
  CountingLock lock;
  std::unique_ptr<N20Mechanism> mech;
  std::unique_ptr<FakeFeeder> f;
  bool control_running{true};
  explicit Disc(uint8_t slots = 15) { this->boot(slots); }
  void boot(uint8_t slots = 15) {
    this->mech = std::make_unique<N20Mechanism>(slots);
    this->mech->set_lock(&this->lock);
    this->f = std::make_unique<FakeFeeder>(this->flash, T0 * 1000, true);
    this->f->set_mechanism(this->mech.get());
    this->f->start();
    this->run(1500);
  }
  void run(int64_t ms) {
    for (int64_t i = 0; i < ms; i++) {
      this->f->advance(1, 1);
      if (this->control_running)
        apply(this->sim, this->mech->control(static_cast<uint32_t>(this->f->uptime()), static_cast<int32_t>(this->sim.count())));
      this->sim.advance_ms(1);
    }
  }
  /// Run until the core is no longer moving (or `limit` ms).
  void settle(int64_t limit = 20000) {
    for (int64_t i = 0; i < limit && (this->f->tray_state() == TrayState::MOVING); i += 10)
      this->run(10);
    this->run(10);
  }
  void until_idle(int64_t limit = 60000) {
    for (int64_t i = 0; i < limit; i += 10) {
      this->run(10);
      if (std::strcmp(this->f->calibration_text(), "Moving...") != 0)
        return;
    }
  }
  Result cal(uint8_t action, int32_t arg = 0) {
    const Result r = this->f->calibration_action(action, arg);
    this->until_idle();
    // Home Assistant keeps at most 255 characters of a state.
    CHECK(std::strlen(this->f->calibration_text()) < 255, "every step's text fits Home Assistant");
    return r;
  }
  int32_t ticks() const { return static_cast<int32_t>(this->sim.count()); }
};

/// Set a value that may only change in calibration mode (where the slots are, which way is forward), as a user must.
static Result set_in_calibration(Disc &d, uint8_t index, int32_t value) {
  d.f->set_calibration_mode(true);
  const Result r = d.f->set_tuning(index, value);
  d.f->set_calibration_mode(false);
  d.run(10);
  return r;
}

static int32_t grid(int k, int32_t origin = 0, int32_t tpr = 8344, int slots = 15) {
  return origin + static_cast<int32_t>((2LL * k * tpr + slots) / (2LL * slots));
}

static void test_mechanism_feeds() {
  current = "disc: 14 feeds, empty, reload, one turn";
  Disc d;
  CHECK(d.f->capacity() == 14 && d.f->remaining() == 14, "15 open slots: 14 portions");
  CHECK(std::strcmp(d.f->mechanism().name(), "n20-direct") == 0 && !d.f->is_servo(), "the disc mechanism");
  d.f->set_min_gap_s(10);
  d.f->set_daily_cap(14);
  d.f->set_max_portions_cmd(14);
  int worst = 0;
  for (int k = 1; k <= 14; k++) {
    CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::OK, "feed accepted");
    d.settle();
    worst = std::max(worst, std::abs(d.ticks() - grid(k)));
    d.run(10000);
  }
  CHECK(d.f->position() == 14 && d.f->tray_state() == TrayState::EMPTY, "empty after 14");
  CHECK(worst <= 4, "every slot within 4 ticks of the grid (0.2 degrees)");
  CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::EMPTY, "empty refuses");
  const int32_t before = d.ticks();
  d.f->tray_reload();
  d.run(500);
  CHECK(d.ticks() == before && d.f->remaining() == 14, "reload: full again, and the disc did not move");
  CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::OK, "the 15th slot");
  d.settle();
  CHECK(std::abs(d.ticks() - 8344) <= 4, "15 slots: exactly one turn (8344 ticks), never rounding up");
  CHECK(d.lock.depth == 0 && d.lock.max_depth == 1 && d.lock.takes > 1000, "the lock is always released, never nested");
}

static void test_mechanism_wiggle_is_part_of_a_feed() {
  current = "disc: every feed wiggles";
  Disc d;
  // Count the reverse drives during one feed: the wiggle's legs back (the move itself only ever goes forward).
  int reverse_starts = 0;
  bool was_reverse = false;
  CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::OK, "feed");
  for (int i = 0; i < 20000 && d.f->tray_state() == TrayState::MOVING; i++) {
    d.run(1);
    const bool rev = d.sim.enable && d.sim.in2 > 0 && d.sim.in1 == 0;
    if (rev && !was_reverse)
      reverse_starts++;
    was_reverse = rev;
  }
  CHECK(reverse_starts >= 4, "four wiggle cycles: at least four drives back");
  CHECK(d.mech->last_feed_move_ms() > 300 && d.mech->last_feed_ms() > d.mech->last_feed_move_ms(),
        "the bench timings: the slot move, then the whole feed with the wiggle");
  std::printf("  simulated feed: move %u ms, whole feed %u ms\n", (unsigned) d.mech->last_feed_move_ms(),
              (unsigned) d.mech->last_feed_ms());
  CHECK(std::abs(d.ticks() - grid(1)) <= 4, "and it ends on the slot");
}

static void test_mechanism_multi_portion() {
  current = "disc: three portions in one feed";
  Disc d;
  d.f->set_max_portions_cmd(3);
  CHECK(d.f->request_feed(3, Source::ESPHOME) == Result::OK, "accepted");
  d.settle(60000);
  CHECK(d.f->position() == 3 && std::abs(d.ticks() - grid(3)) <= 4, "three slots, each with its own move and wiggle");
}

static void test_mechanism_faults() {
  {
    current = "disc fault: stall";
    Disc d;
    CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::OK, "feed");
    d.run(60);
    d.sim.held = true;
    d.settle();
    CHECK(d.f->position_unknown(), "a stall leaves the position unknown");
    CHECK(d.f->faults_text().find("motor fault: no encoder motion") != std::string::npos, "and says why");
    CHECK(d.f->portions_24h() == 1, "the portion counts as fed");
    CHECK(d.sim.enable == false, "the bridge is in standby afterwards");
  }
  {
    current = "disc fault: encoder unplugged at full speed";
    Disc d;
    CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::OK, "feed");
    d.run(40);
    d.sim.count_frozen = d.sim.count();
    const double at = d.sim.x;
    d.sim.unplugged = true;
    d.settle();
    CHECK(d.f->position_unknown(), "unknown");
    CHECK(d.sim.x - at < 8344 / 15 / 2, "it ran less than half a slot uncounted (the short stall window at full power)");
  }
  {
    current = "disc fault: the control loop stops";
    Disc d;
    CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::OK, "feed");
    d.run(30);
    d.control_running = false;
    for (int i = 0; i < 40; i++) {
      d.f->advance(1, 1);
      d.sim.advance_ms(1);
    }
    d.control_running = true;
    d.run(1);
    CHECK(d.sim.in1 == 255 && d.sim.in2 == 255, "the first step after a late one brakes");
    d.settle();
    CHECK(d.f->position_unknown() && d.f->faults_text().find("ran late") != std::string::npos, "and it is a fault");
  }
  {
    current = "disc fault: a brake that does not hold";
    Disc d;
    d.sim.tau_brake_ms = 400;  // nearly no braking
    d.sim.tau_coast_ms = 400;
    d.sim.friction = 0;
    d.f->request_feed(1, Source::ESPHOME);
    d.settle();
    CHECK(d.f->position_unknown() && d.f->faults_text().find("ran past its target") != std::string::npos,
          "more than half a slot past the target: a fault");
  }
  {
    current = "disc fault: encoder counting the wrong way";
    Disc d;
    CHECK(d.f->set_tuning(N20_REVERSE_ENCODER, 1) == Result::NOT_CALIBRATING, "not outside calibration mode");
    CHECK(set_in_calibration(d, N20_REVERSE_ENCODER, 1) == Result::OK, "in calibration mode");
    d.f->request_feed(1, Source::ESPHOME);
    d.settle();
    CHECK(d.f->position_unknown() && d.f->faults_text().find("wrong way") != std::string::npos, "a fault");
  }
  {
    current = "disc fault: too slow";
    Disc d;
    d.sim.v_max = 0.06;  // edges keep coming, but a slot would take 9 s
    d.f->request_feed(1, Source::ESPHOME);
    d.settle(30000);
    CHECK(d.f->position_unknown() && d.f->faults_text().find("took too long") != std::string::npos, "a fault");
  }
  {
    current = "disc: stop mid-feed";
    Disc d;
    d.f->request_feed(1, Source::ESPHOME);
    d.run(80);
    d.f->stop();
    d.run(400);
    CHECK(d.f->position_unknown() && !d.sim.enable, "stopped: unknown, bridge off");
  }
  {
    current = "disc: no control running";
    Disc d;
    // A mechanism the adapter never steps refuses to start a move rather than waiting for ever.
    N20Mechanism lone;
    Flash fl;
    FakeFeeder f(fl, T0 * 1000, true);
    f.set_mechanism(&lone);
    f.start();
    f.advance(1500);
    f.request_feed(1, Source::ESPHOME);
    f.advance(500);
    CHECK(f.position_unknown() && f.faults_text().find("not running") != std::string::npos, "a fault at once");
  }
}

static void test_mechanism_recovery_and_boot() {
  current = "disc: recovery";
  Disc d;
  d.f->request_feed(1, Source::ESPHOME);
  d.run(80);
  d.f->stop();
  d.run(400);
  CHECK(d.f->position_unknown(), "unknown after a stop");
  // Recovery: in calibration mode, nudge forward until a slot is over the hole, then declare.
  d.f->set_calibration_mode(true);
  const int32_t a = d.ticks();
  CHECK(d.cal(N20_CAL_NUDGE, 50) == Result::OK && d.ticks() > a + 100, "nudge 5 degrees forward");
  CHECK(d.f->calibration_action(N20_CAL_NUDGE, 0) == Result::INVALID_ARG, "a nudge needs 0.1 to 10 degrees");
  d.f->set_calibration_mode(false);
  CHECK(d.f->position_unknown(), "still unknown until declared");
  const int32_t here = d.ticks();
  d.f->declare_position(1);
  d.run(10);
  CHECK(!d.f->position_unknown() && d.f->remaining() == 13, "declared");
  d.f->set_min_gap_s(10);
  d.run(11000);
  CHECK(d.f->request_feed(1, Source::ESPHOME) == Result::OK, "feeds again");
  d.settle();
  CHECK(std::abs(d.ticks() - (here + grid(1))) <= 4, "the next slot counts from where it was declared");
}

static void test_calibration_flow() {
  current = "disc calibration: the guided flow";
  Disc d;
  CHECK(d.f->calibration_action(N20_CAL_START) == Result::NOT_CALIBRATING, "only in calibration mode");
  d.f->set_calibration_mode(true);
  CHECK(std::strstr(d.f->calibration_text(), "Step 1 of 5: check Disc slots (15)") != nullptr &&
            std::strstr(d.f->calibration_text(), "empty every slot") != nullptr,
        "first: the slot count, and empty the disc");
  CHECK(d.cal(N20_CAL_ZERO) == Result::INVALID_ARG, "no zero before the direction test");
  CHECK(d.cal(N20_CAL_START) == Result::OK, "direction test");
  CHECK(std::strstr(d.f->calibration_text(), "turn the way it should when it feeds") != nullptr, "asks the user");
  CHECK(d.cal(N20_CAL_YES) == Result::OK, "yes, and the encoder counted up");
  CHECK(std::strstr(d.f->calibration_text(), "then press Zero") != nullptr, "now zero");
  CHECK(d.cal(N20_CAL_OVERSHOT) == Result::OK, "overshot: back and forward again");
  CHECK(d.cal(N20_CAL_NUDGE, 3) == Result::OK, "nudge");
  CHECK(d.cal(N20_CAL_ZERO) == Result::OK, "zero");
  const int32_t zero = d.ticks();
  CHECK(std::strstr(d.f->calibration_text(), "Step 3 of 5: press Run turns") != nullptr, "now turns");
  CHECK(d.cal(N20_CAL_TURNS) == Result::OK, "one turn");
  CHECK(std::abs(d.ticks() - (zero + 8344)) <= 4, "turned 8344 ticks");
  // This disc really needs 8400 ticks a turn: the user nudges on to the mark (2.4 degrees), then Measured.
  CHECK(d.cal(N20_CAL_NUDGE, 24) == Result::OK, "nudge to the mark");
  const int32_t moved = d.ticks() - zero;
  CHECK(d.cal(N20_CAL_MEASURED) == Result::OK, "measured");
  d.run(10);
  CHECK(d.f->tuning_value(N20_TICKS_PER_TURN) == moved, "ticks per turn set to what it turned");
  d.run(6000);
  CHECK(d.flash.have_tuning && d.flash.tuning.values[N20_TICKS_PER_TURN] == moved, "and saved");
  const int32_t origin = d.ticks();
  for (int k = 1; k <= 15; k++) {
    CHECK(d.cal(k % 2 ? N20_CAL_NEXT : N20_CAL_YES) == Result::OK, "next slot");
    CHECK(std::abs(d.ticks() - grid(k, origin, moved)) <= 4, "each slot on the new grid, as a feed would put it");
  }
  CHECK(std::strstr(d.f->calibration_text(), "all 15 slots checked") != nullptr, "done");
  CHECK(d.cal(N20_CAL_NEXT) == Result::INVALID_ARG, "nothing after the last slot");
  d.f->set_calibration_mode(false);
  CHECK(d.f->position_unknown(), "the disc turned: reload before feeding");
  d.f->tray_reload();
  CHECK(!d.f->position_unknown() && d.f->remaining() == 14, "reloaded");
}

static void test_calibration_trim_here() {
  current = "disc calibration: Trim here";
  Disc d;
  d.f->set_calibration_mode(true);
  d.cal(N20_CAL_START);
  d.cal(N20_CAL_YES);
  d.cal(N20_CAL_ZERO);
  d.cal(N20_CAL_TURNS);
  d.cal(N20_CAL_MEASURED);
  d.run(10);
  const int32_t tpr = d.f->tuning_value(N20_TICKS_PER_TURN);
  const int32_t origin = d.ticks();
  CHECK(d.cal(N20_CAL_TRIM_HERE) == Result::INVALID_ARG, "not before the first slot is checked");
  CHECK(d.cal(N20_CAL_NEXT) == Result::OK, "slot 1");
  CHECK(std::strstr(d.f->calibration_text(), "then press Trim here") != nullptr, "says what to do if it is off");
  // Every slot stops 1 degree short of the hole: nudge this one on by 1 degree, then Trim here.
  CHECK(d.cal(N20_CAL_NUDGE, 10) == Result::OK, "nudge 1 degree");
  CHECK(d.cal(N20_CAL_TRIM_HERE) == Result::OK, "trim here");
  d.run(10);
  CHECK(std::abs(d.f->tuning_value(N20_TRIM) - 10) <= 1, "trim +1.0 degree, from the nudge");
  CHECK(std::strstr(d.f->calibration_text(), "trim set to +1.0 degrees") != nullptr ||
            std::strstr(d.f->calibration_text(), "trim set to +0.9") != nullptr ||
            std::strstr(d.f->calibration_text(), "trim set to +1.1") != nullptr,
        "and says so, with its sign");
  CHECK(d.cal(N20_CAL_NEXT) == Result::OK, "slot 2");
  const int32_t want = grid(2, origin, tpr) + static_cast<int32_t>(std::lround(d.f->tuning_value(N20_TRIM) * tpr / 3600.0));
  CHECK(std::abs(d.ticks() - want) <= 4, "the next slot stops a degree further on, like the one nudged");
  d.run(6000);
  CHECK(d.flash.have_tuning && d.flash.tuning.values[N20_TRIM] == d.f->tuning_value(N20_TRIM), "saved");
  // The other way: slot 3 goes past by about 2 degrees. Overshot backs up 5 and comes forward 4 (net -1 degree).
  CHECK(d.cal(N20_CAL_NEXT) == Result::OK, "slot 3");
  const int32_t before = d.f->tuning_value(N20_TRIM);
  CHECK(d.cal(N20_CAL_OVERSHOT) == Result::OK, "overshot");
  CHECK(d.cal(N20_CAL_TRIM_HERE) == Result::OK, "trim here");
  d.run(10);
  CHECK(std::abs(d.f->tuning_value(N20_TRIM) - (before - 10)) <= 2, "trim back by about a degree");
  CHECK(std::strstr(d.f->calibration_text(), "trim set to +0.") != nullptr ||
            std::strstr(d.f->calibration_text(), "trim set to -0.") != nullptr ||
            std::strstr(d.f->calibration_text(), "trim set to +0.0") != nullptr,
        "close to zero again");
}

static void test_calibration_direction() {
  {
    current = "disc calibration: motor wired the other way";
    Disc d;
    d.f->set_calibration_mode(true);
    d.f->set_tuning(N20_REVERSE_MOTOR, 1);  // as if M1 and M2 were swapped
    d.run(10);
    d.cal(N20_CAL_START);
    // It turned backwards, and the encoder (seeing it go backwards) counted down.
    CHECK(d.cal(N20_CAL_NO) == Result::OK, "no");
    d.run(10);
    CHECK(d.f->tuning_value(N20_REVERSE_MOTOR) == 0 && d.f->tuning_value(N20_REVERSE_ENCODER) == 0,
          "the motor reversed back; the encoder already agrees");
    CHECK(std::strstr(d.f->calibration_text(), "test once more") != nullptr, "test again");
    CHECK(d.cal(N20_CAL_YES) == Result::INVALID_ARG, "not without testing again");
    d.cal(N20_CAL_START);
    CHECK(d.cal(N20_CAL_YES) == Result::OK && std::strstr(d.f->calibration_text(), "Zero") != nullptr, "now right");
  }
  {
    current = "disc calibration: encoder wired the other way";
    Disc d;
    d.f->set_calibration_mode(true);
    d.f->set_tuning(N20_REVERSE_ENCODER, 1);
    d.run(10);
    d.cal(N20_CAL_START);
    CHECK(d.cal(N20_CAL_YES) == Result::OK, "it turned the feeding way, but counted down");
    d.run(10);
    CHECK(d.f->tuning_value(N20_REVERSE_ENCODER) == 0 && d.f->tuning_value(N20_REVERSE_MOTOR) == 0, "encoder fixed");
  }
  {
    current = "disc calibration: both the other way";
    Disc d;
    d.f->set_calibration_mode(true);
    d.cal(N20_CAL_START);
    // In the simulation it turns the feeding way and counts up; say "no" to see both reversed.
    CHECK(d.cal(N20_CAL_NO) == Result::OK, "no, but it counted up");
    d.run(10);
    CHECK(d.f->tuning_value(N20_REVERSE_MOTOR) == 1 && d.f->tuning_value(N20_REVERSE_ENCODER) == 1, "both reversed");
    d.cal(N20_CAL_START);
    const int32_t raw = d.ticks();
    (void) raw;
    CHECK(std::strstr(d.f->calibration_text(), "turn the way it should") != nullptr, "and it counted up again");
  }
  {
    current = "disc calibration: encoder not counting";
    Disc d;
    d.sim.count_frozen = 0;
    d.sim.unplugged = true;
    d.f->set_calibration_mode(true);
    d.cal(N20_CAL_START);
    CHECK(std::strstr(d.f->calibration_text(), "did not count") != nullptr, "says to check the wiring");
    CHECK(d.cal(N20_CAL_YES) == Result::INVALID_ARG, "and goes no further");
  }
}

static void test_calibration_measure_rejected() {
  current = "disc calibration: a bad measurement";
  Disc d;
  d.f->set_calibration_mode(true);
  d.cal(N20_CAL_START);
  d.cal(N20_CAL_YES);
  d.cal(N20_CAL_ZERO);
  d.cal(N20_CAL_TURNS);
  for (int i = 0; i < 25; i++)
    d.cal(N20_CAL_NUDGE, 100);  // 250 degrees more: a slipped disc, not a calibration
  CHECK(d.cal(N20_CAL_MEASURED) == Result::OK, "answered");
  d.run(10);
  CHECK(d.f->tuning_value(N20_TICKS_PER_TURN) == 8344, "not used");
  CHECK(std::strstr(d.f->calibration_text(), "too far") != nullptr, "and says so");
}

static void test_tuning_values() {
  current = "disc: tuning";
  Disc d;
  CHECK(d.f->tuning_count() == N20_TUNING_COUNT && d.f->tuning_index("ticks_per_turn") == N20_TICKS_PER_TURN, "table");
  CHECK(d.f->tuning_value(N20_POWER) == 100 && d.f->tuning_value(N20_BRAKING_DISTANCE) == 160 &&
            d.f->tuning_value(N20_WIGGLE_AMPLITUDE) == 20 && d.f->tuning_value(N20_WIGGLE_CYCLES) == 4,
        "the bench sketch's values");
  CHECK(d.f->set_tuning(N20_WIGGLE_CYCLES, 0) == Result::INVALID_ARG, "at least one wiggle (D13)");
  CHECK(d.f->set_tuning(N20_TRIM, 121) == Result::INVALID_ARG, "trim at most half a slot");
  CHECK(d.f->set_tuning(N20_TRIM, 15) == Result::NOT_CALIBRATING, "trim only in calibration mode");
  CHECK(d.f->set_tuning(N20_POWER, 90) == Result::OK, "drive power at any time");
  CHECK(set_in_calibration(d, N20_TRIM, 15) == Result::OK, "a 1.5 degree trim");
  d.run(6000);
  CHECK(d.flash.have_tuning && std::strcmp(d.flash.tuning.mechanism, "n20-direct") == 0, "saved for this mechanism");
  d.boot();
  CHECK(d.f->tuning_value(N20_TRIM) == 15, "kept across a reboot");
  // A Tuning record of another layout is not used.
  d.flash.tuning.layout = 99;
  d.boot();
  CHECK(d.f->tuning_value(N20_TRIM) == 0, "another layout: defaults");
}

static void test_capacity_other_disc() {
  current = "disc: another slot count";
  Disc d(8);
  CHECK(d.f->capacity() == 7, "8 slots: 7 portions");
  d.f->request_feed(1, Source::ESPHOME);
  d.settle();
  CHECK(std::abs(d.ticks() - grid(1, 0, 8344, 8)) <= 4, "45 degrees");
}

static void test_slot_count() {
  current = "disc: the slot count";
  Disc d;
  CHECK(d.f->tuning_value(N20_SLOTS) == 15 && d.f->capacity() == 14, "the build's 15 slots");
  CHECK(d.f->set_tuning(N20_SLOTS, 16) == Result::NOT_CALIBRATING, "never with food loaded");
  d.f->set_calibration_mode(true);
  CHECK(d.f->set_tuning(N20_SLOTS, 2) == Result::INVALID_ARG && d.f->set_tuning(N20_SLOTS, 61) == Result::INVALID_ARG,
        "3 to 60");
  CHECK(d.f->set_tuning(N20_SLOTS, 16) == Result::OK, "16 slots");
  CHECK(d.f->capacity() == 15, "15 portions a load");
  CHECK(std::strstr(d.f->calibration_text(), "Disc slots (16)") != nullptr, "the wizard shows it");
  d.f->set_calibration_mode(false);
  CHECK(d.f->position_unknown(), "another slot count: reload before feeding");
  d.f->tray_reload();
  CHECK(d.f->remaining() == 15, "a full load of 15");
  const int32_t origin = d.ticks();
  d.f->request_feed(1, Source::ESPHOME);
  d.settle();
  CHECK(std::abs(d.ticks() - grid(1, origin, 8344, 16)) <= 4, "22.5 degrees a slot");
  d.run(6000);
  d.boot(15);  // the build still says 15: the saved 16 wins
  CHECK(d.f->tuning_value(N20_SLOTS) == 16 && d.f->capacity() == 15, "kept across a reboot");
}

static void test_tuning_layout_1_kept() {
  current = "disc: tuning saved before Disc Slots existed";
  Disc d;
  set_in_calibration(d, N20_TRIM, 7);
  d.f->set_tuning(N20_POWER, 80);
  d.run(6000);
  // As layout 1 saved it: nine values, no slot count.
  d.flash.tuning.layout = 1;
  d.flash.tuning.count = 9;
  d.flash.tuning.values[N20_SLOTS] = 0;
  d.boot(15);
  CHECK(d.f->tuning_value(N20_TRIM) == 7 && d.f->tuning_value(N20_POWER) == 80, "layout 1's values kept");
  CHECK(d.f->tuning_value(N20_SLOTS) == 15, "the new value at its default");
}

static void test_smaller_load_keeps_settings() {
  current = "disc: a smaller load never rewrites the guard settings";
  Disc d;
  d.f->set_daily_cap(14);
  d.f->set_max_portions_cmd(10);
  enable_event(*d.f, 0, 0, Anchor::CLOCK, 8 * 60 + 10, 12);
  d.run(6000);
  set_in_calibration(d, N20_SLOTS, 8);  // 7 portions a load
  d.run(6000);
  d.boot(15);
  CHECK(d.f->capacity() == 7, "8 slots");
  CHECK(d.f->cfg.daily_cap == 14 && d.f->cfg.max_portions_cmd == 10 && d.f->cfg.profiles[0].ev[0].portions == 12,
        "the settings are as the user set them, after a reboot with the smaller load");
  CHECK(d.f->request_feed(10, Source::ESPHOME) == Result::INVALID_ARG, "a manual request for more than a load: refused");
  d.f->tray_reload();
  d.run(17 * 60 * 1000);  // past 08:10 (the reboot set the clock to 07:55): the event asks for 12
  CHECK(d.f->position() == 7 && d.f->tray_state() == TrayState::EMPTY, "the scheduled 12 fed the 7 a load holds");
  if (d.f->position() != 7)
    std::printf("  position %u, last %s, faults %s, today %s\n", d.f->position(), d.f->last_result_text().c_str(),
                d.f->faults_text().c_str(), d.f->todays_feeds_text().c_str());
  set_in_calibration(d, N20_SLOTS, 15);
  d.run(6000);
  d.boot(15);
  CHECK(d.f->cfg.daily_cap == 14 && d.f->cfg.max_portions_cmd == 10, "back to 15 slots: still 14 and 10");
}

int main() {
  test_motion_matches_sketch();
  test_mechanism_feeds();
  test_mechanism_wiggle_is_part_of_a_feed();
  test_mechanism_multi_portion();
  test_mechanism_faults();
  test_mechanism_recovery_and_boot();
  test_calibration_flow();
  test_calibration_trim_here();
  test_calibration_direction();
  test_calibration_measure_rejected();
  test_tuning_values();
  test_capacity_other_disc();
  test_slot_count();
  test_tuning_layout_1_kept();
  test_smaller_load_keeps_settings();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("All direct-drive tests passed\n");
  return 0;
}
