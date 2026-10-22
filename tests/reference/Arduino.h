// Just enough of the Arduino core (ESP32) for the bench sketch (motor-test.cpp) to run on a computer against the
// simulated motor (tests/motor_sim.h). Included inside `namespace ref` by tests/test_n20.cpp, which defines
// ::g_ref_sim (the motor), ::g_ref_serial_in (what the console "types") and ::g_ref_bridge_written (called after each
// bridge write) before including the sketch.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

using std::max;
using std::min;

#define IRAM_ATTR
#define DRAM_ATTR
#define portENTER_CRITICAL(m) (void) (m)
#define portEXIT_CRITICAL(m) (void) (m)
#define portENTER_CRITICAL_ISR(m) (void) (m)
#define portEXIT_CRITICAL_ISR(m) (void) (m)
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;

constexpr int LOW = 0, HIGH = 1, OUTPUT = 1, INPUT_PULLUP = 2, CHANGE = 3;
// The sketch's pins (motor-test.cpp): IN1 1, IN2 2, ENABLE (STBY) 6, encoder A 4, B 5.
inline void (*isr_a)() = nullptr;
inline void (*isr_b)() = nullptr;
inline int ledc_pin[2] = {-1, -1};

// Called after every write to the bridge (IN1, IN2, STBY), so the test records each change when it happens.
inline void bridge_written() {
  if (::g_ref_bridge_written)
    ::g_ref_bridge_written();
}

inline void pinMode(int, int) {}
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int pin, void (*fn)(), int) {
  if (pin == 4)
    isr_a = fn;
  else if (pin == 5)
    isr_b = fn;
  ::g_ref_sim->on_edge = [](int ch) {
    if (ch == 0 && isr_a)
      isr_a();
    if (ch == 1 && isr_b)
      isr_b();
  };
}
inline int digitalRead(int pin) { return pin == 4 ? ::g_ref_sim->level(0) : pin == 5 ? ::g_ref_sim->level(1) : 0; }
inline void digitalWrite(int pin, int level) {
  if (pin == 6) {
    ::g_ref_sim->enable = level == HIGH;
    bridge_written();
  }
}
inline void ledcSetup(int, int, int) {}
inline void ledcAttachPin(int pin, int channel) { ledc_pin[channel] = pin; }
inline void ledcWrite(int channel, int duty) {
  if (ledc_pin[channel] == 1)
    ::g_ref_sim->in1 = duty;
  else if (ledc_pin[channel] == 2)
    ::g_ref_sim->in2 = duty;
  bridge_written();
}
inline uint32_t millis() { return ::g_ref_sim->millis(); }
inline void delay(uint32_t ms) { ::g_ref_sim->advance_ms(ms); }

struct FakeSerial {
  std::string out;
  void begin(int) {}
  explicit operator bool() const { return true; }  // always connected: the disconnect abort is console-only
  void println(const char *s) {
    this->out += s;
    this->out += '\n';
  }
  int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[512];
    va_list a;
    va_start(a, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    this->out += buf;
    return n;
  }
  int available() const { return static_cast<int>(::g_ref_serial_in.size()); }
  int read() {
    if (::g_ref_serial_in.empty())
      return -1;
    const char c = ::g_ref_serial_in.front();
    ::g_ref_serial_in.erase(0, 1);
    return c;
  }
};
inline FakeSerial Serial;
