// A simulated N20 gear motor with a quadrature encoder, behind a DRV8833 (one channel), for host tests of the
// direct-drive mechanism (components/precision_feeder/mechanism_n20.*) and of the bench sketch it was ported from
// (tests/reference/motor-test.cpp).
//
// It is a plausible plant, not a measured one (tuned so the bench sketch takes about 0.9 s per 24-degree move and stops
// within 1 to 3 ticks, against 0.735 s and 3 to 4 ticks on the bench): first-order spin-up to a speed set by the duty (35 rpm at the output
// is about 4.87 encoder ticks per millisecond at 8344 ticks per turn), a little static friction (a short pulse may
// not move it at all), fast decay when braked (both inputs high), slow decay when coasting. Both the sketch and the
// firmware's controller drive the same model, so comparing them is exact even where the model is not.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>

struct MotorSim {
  // Model constants.
  double v_max = 4.87;          // ticks/ms at 100% duty
  double tau_drive_ms = 4.0;    // spin-up time constant
  double tau_brake_ms = 2.5;    // decay with both inputs high
  double tau_coast_ms = 40.0;   // decay with the bridge off
  double friction = 0.002;      // ticks/ms^2, while not driven
  double stiction_ms = 0.6;     // drive needed from rest before it moves
  double dt_ms = 0.05;

  // Bridge inputs, as the firmware or the sketch sets them: IN1 and IN2 duty (0 to 255) and STBY (enable).
  int in1 = 0, in2 = 0;
  bool enable = false;
  // Physical state.
  double x = 0;  // position, ticks (the encoder counts floor(x))
  double v = 0;  // ticks/ms
  bool stuck = true;
  double stick_ms = 0;
  int64_t now_us = 0;
  bool held = false;     // something holds the disc (a stall test)
  bool unplugged = false;  // the encoder is disconnected: its count stops changing
  int64_t count_frozen = 0;
  // Called on every encoder edge: (channel 0 = A, 1 = B).
  std::function<void(int)> on_edge;

  int64_t count() const { return this->unplugged ? this->count_frozen : static_cast<int64_t>(std::floor(this->x)); }
  uint32_t millis() const { return static_cast<uint32_t>(this->now_us / 1000); }
  /// The quadrature level of channel A (0) or B (1) at the current count: forward runs 00, 10, 11, 01.
  int level(int ch) const {
    static const int states[4] = {0, 2, 3, 1};
    const int64_t c = this->count();
    const int s = states[((c % 4) + 4) % 4];
    return ch == 0 ? (s >> 1) & 1 : s & 1;
  }

  void advance_ms(double ms) {
    const int steps = static_cast<int>(std::lround(ms / this->dt_ms));
    for (int i = 0; i < steps; i++)
      this->step_();
  }

 private:
  void step_() {
    const double dt = this->dt_ms;
    int dir = 0;
    double duty = 0;
    bool brake = false;
    if (this->enable) {
      if (this->in1 >= 255 && this->in2 >= 255) {
        brake = true;
      } else if (this->in1 > 0 && this->in2 == 0) {
        dir = 1;
        duty = this->in1 / 255.0;
      } else if (this->in2 > 0 && this->in1 == 0) {
        dir = -1;
        duty = this->in2 / 255.0;
      }
    }
    if (this->held) {
      this->v = 0;
      this->stuck = true;
    } else if (dir != 0) {
      if (this->stuck) {
        this->stick_ms += dt;
        if (this->stick_ms >= this->stiction_ms)
          this->stuck = false;
      }
      if (!this->stuck)
        this->v += (dir * this->v_max * duty - this->v) * dt / this->tau_drive_ms;
    } else {
      this->stick_ms = 0;
      const double tau = brake ? this->tau_brake_ms : this->tau_coast_ms;
      this->v -= this->v * dt / tau;
      const double f = this->friction * dt;
      if (std::fabs(this->v) <= f) {
        this->v = 0;
        this->stuck = true;
      } else {
        this->v -= this->v > 0 ? f : -f;
      }
    }
    const int64_t before = this->count();
    this->x += this->v * dt;
    this->now_us += static_cast<int64_t>(std::lround(dt * 1000));
    const int64_t after = this->count();
    if (this->unplugged || !this->on_edge)
      return;
    // One edge per count step: forward 00 -> 10 toggles A, 10 -> 11 toggles B, and so on.
    for (int64_t c = before; c != after; c += (after > before ? 1 : -1)) {
      const int64_t from = after > before ? c : c - 1;  // the step between from and from + 1
      const int phase = static_cast<int>(((from % 4) + 4) % 4);
      this->on_edge(phase % 2 == 0 ? 0 : 1);
    }
  }
};
