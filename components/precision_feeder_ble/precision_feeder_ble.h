// Precision Feeder BLE control plane: the ESPHome adapter, on ESP-IDF's NimBLE host (since 2.4.0).
//
// Everything the protocol does lives in BleEngine (engine.h), plain C++ that is tested on a host; this class only
// connects it to the radio: the GATT table (gatt_table.h), advertising, pairing, the hardware random number
// generator and the log. NimBLE runs its own FreeRTOS task, so its callbacks only copy into a LinkQueue or read a
// ValueStore (gatt_link.h); the engine runs on the main loop. The byte layouts are in protocol.h and
// docs/PROTOCOL.md.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <atomic>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/precision_feeder/precision_feeder.h"

#include "engine.h"
#include "gatt_link.h"
#include "gatt_table.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace esphome {
namespace precision_feeder {

class PrecisionFeederBle : public Component, public BleTransport {
 public:
  // ---- configuration (set from Python) ----
  void set_feeder(PrecisionFeeder *feeder) { this->feeder_ = feeder; }
  /// The advertised name (at most 20 characters); with name_add_mac_suffix, "-" and the end of the MAC is added.
  void set_name(const char *name) { this->name_ = name; }
  /// The Device Information Service.
  void set_device_info(const char *manufacturer, const char *model, const char *firmware) {
    this->manufacturer_ = manufacturer;
    this->model_ = model;
    this->firmware_ = firmware;
  }
  void set_time_policy(proto::TimePolicy policy) { this->time_policy_ = policy; }
  void set_network_time_stale_ms(uint32_t ms) { this->network_stale_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // The stack starts early, as ESPHome's own Bluetooth does, while the heap is still in large blocks; the engine
  // starts on the first loop(), once the feeder is set up.
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // ---- BleTransport ----
  void set_value(uint8_t channel, const uint8_t *data, size_t n, bool notify) override;
  void random(uint8_t *out, size_t n) override;
  void log(LogLevel level, const char *message) override;

  // ---- diagnostics ----
  /// For the "BLE Info" and "BLE Session" text sensors (packages/ble_gatt.yaml).
  /// BLE Info ends with ";bonds=N", the clients paired with the feeder (2.4.3), once the stack has counted them.
  std::string info_text() const {
    if (this->engine_ == nullptr)
      return "";
    const int bonds = this->bonds_.load();
    return this->engine_->info_text() + (bonds >= 0 ? ";bonds=" + std::to_string(bonds) : std::string());
  }
  std::string session_text() const { return this->engine_ != nullptr ? this->engine_->session_text() : ""; }
  /// The activity log as JSON (GET /feeder/ble on the web server). Safe to call from the web server's task: it
  /// returns a copy made on the main loop.
  std::string activity_json_copy();

  // ---- bonds ----
  /// Forget every bond (the web app's "Forget BLE Bonds", 2.4.3): each bonded client is unpaired, and disconnected
  /// if it is the one connected. Clients that never paired, such as a PLC bridge, are untouched. Main loop only.
  void forget_bonds();

  // ---- NimBLE callbacks (its host task; static trampolines in the .cpp) ----
  int on_gap_event(ble_gap_event *event);
  int on_access(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt *ctxt, uint8_t channel);
  void on_sync();

 protected:
  void start_engine_();
  void advertise_();
  void count_bonds_();
  bool subscribed_(uint8_t channel) const;

  PrecisionFeeder *feeder_{nullptr};
  const char *name_{nullptr};
  const char *manufacturer_{""};
  const char *model_{""};
  const char *firmware_{""};
  proto::TimePolicy time_policy_{proto::TimePolicy::FALLBACK_ONLY};
  uint32_t network_stale_ms_{24u * 3600u * 1000u};
  BleEngine *engine_{nullptr};  // created on the first loop(), once the feeder is set up
  bool stack_ok_{false};

  LinkQueue queue_;
  ValueStore values_;
  char device_name_[21]{};
  uint8_t own_addr_type_{0};
  // Set on NimBLE's task, read on the main loop.
  std::atomic<uint16_t> conn_handle_{0xFFFF};  // BLE_HS_CONN_HANDLE_NONE: no client
  std::atomic<uint16_t> subscriptions_{0};     // bit per channel: the client turned notifications on
  std::atomic<int> bonds_{-1};                 // bonds in the store; -1 until the stack has synced
  uint16_t val_handles_[gatt::COUNT]{};

  // The activity JSON, rebuilt on the main loop when the log changes and read by the web server's task.
  Mutex activity_lock_;
  std::string activity_json_{"{\"entries\":[]}"};
  uint32_t activity_json_id_{0};
};

}  // namespace precision_feeder
}  // namespace esphome
