// Precision Feeder BLE: the GATT table (UUIDs and properties), as plain C++.
//
// Since 2.4.0 the table is C++ rather than YAML (packages/ble_gatt.yaml): the adapter builds NimBLE's service
// definition from it. The UUIDs are fixed by the protocol (docs/PROTOCOL.md): base
// 02dd1dd9-ba75-4bbc-bdea-306d2101xxxx, this project's own, with the channel (proto::CHANNEL_*) in the low byte.
// tools/test_precision_feeder_ble.py checks this table against the Python client, and tests/test_ble_link.cpp
// checks the byte order against the UUID strings.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "protocol.h"

namespace esphome {
namespace precision_feeder {
namespace gatt {

constexpr uint8_t SERVICE = 0x01;  // the low byte of the service's UUID

/// Every UUID is this, then "00" and the low byte (0x01 for the service, or the channel) in hex.
constexpr const char *UUID_PREFIX = "02dd1dd9-ba75-4bbc-bdea-306d2101";

/// The 128-bit UUID in Bluetooth's byte order (least significant byte first), as NimBLE's ble_uuid128_t holds it.
inline void uuid_le(uint8_t low, uint8_t out[16]) {
  // 02dd1dd9-ba75-4bbc-bdea-306d2101_00ll, written backwards.
  static constexpr uint8_t BASE_BE[14] = {0x02, 0xdd, 0x1d, 0xd9, 0xba, 0x75, 0x4b,
                                          0xbc, 0xbd, 0xea, 0x30, 0x6d, 0x21, 0x01};
  out[0] = low;
  out[1] = 0x00;
  for (size_t i = 0; i < 14; i++)
    out[2 + i] = BASE_BE[13 - i];
}

/// What a client may do with a characteristic. Every value is plain read or write (no "encryption required";
/// DESIGN.md section 10): access control is the protocol 2 key.
enum Props : uint8_t { READ = 1, WRITE = 2, NOTIFY = 4 };

struct Characteristic {
  uint8_t channel;
  uint8_t props;
  uint8_t initial_size;  // the value before the engine first sets it: zeros of this length
};

/// The table, in the order the characteristics appear (the same order and properties as ble_gatt.yaml had).
constexpr Characteristic TABLE[] = {
    {proto::CHANNEL_INFO, READ, 6},
    {proto::CHANNEL_COMMAND, WRITE, 2},
    {proto::CHANNEL_RESPONSE, READ | NOTIFY, proto::RESPONSE_SIZE},
    {proto::CHANNEL_STATUS, READ | NOTIFY, proto::STATUS_SIZE},
    {proto::CHANNEL_PROFILE, READ | WRITE, proto::PROFILE_EVENT_SIZE},
    {proto::CHANNEL_CONFIG, READ | WRITE, proto::CONFIG_SET_SIZE},
    {proto::CHANNEL_CALIBRATION, READ | WRITE, proto::CALIBRATION_SET_SIZE},
    {proto::CHANNEL_EVENT_LOG, NOTIFY, proto::EVENT_LOG_SIZE},
    {proto::CHANNEL_AUTH, READ, crypto::NONCE_SIZE},
};
constexpr size_t COUNT = sizeof(TABLE) / sizeof(TABLE[0]);

}  // namespace gatt
}  // namespace precision_feeder
}  // namespace esphome
