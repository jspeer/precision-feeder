// Precision Feeder BLE: the hand-off between NimBLE's host task and the main loop, as plain C++.
//
// NimBLE calls the adapter from its own FreeRTOS task, while BleEngine and FeederCore run on the main loop only. So
// every callback either copies an event into LinkQueue (connects, disconnects, the MTU, writes, reads) or answers a
// read from ValueStore; the main loop drains the queue into the engine and updates the values. Both are guarded by
// one std::mutex and have fixed sizes, so nothing allocates on the host task. Tested on a host
// (tests/test_ble_link.cpp).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "protocol.h"

namespace esphome {
namespace precision_feeder {

/// One thing that happened on the radio, for the main loop.
struct LinkEvent {
  enum Kind : uint8_t { CONNECT, DISCONNECT, MTU, WRITE, READ };
  static constexpr size_t MAX_DATA = proto::ReplayCache::MAX_COMMAND;  // the engine reads no more of a write

  Kind kind{CONNECT};
  uint8_t channel{0};  // WRITE, READ
  uint16_t size{0};    // WRITE: the length the client wrote (only MAX_DATA bytes are kept); READ: bytes read; MTU
  uint8_t data[MAX_DATA]{};  // WRITE: the first bytes; CONNECT: the peer address, most significant byte first
};

/// A fixed queue of LinkEvents, filled by NimBLE's task and drained by the main loop.
class LinkQueue {
 public:
  static constexpr size_t SIZE = 8;
  /// Reads are only counted, so they never take the last slots: a client that polls cannot crowd out a write.
  static constexpr size_t READ_RESERVE = 2;

  /// Queue a write. False if the queue is full: the adapter then answers the write with an ATT error, so the client
  /// knows and retries (an exact retry is answered from the engine's retry cache and never acts twice).
  bool push_write(uint8_t channel, const uint8_t *data, size_t n) {
    LinkEvent e;
    e.kind = LinkEvent::WRITE;
    e.channel = channel;
    e.size = static_cast<uint16_t>(n > 0xFFFF ? 0xFFFF : n);
    std::memcpy(e.data, data, n < LinkEvent::MAX_DATA ? n : LinkEvent::MAX_DATA);
    return this->push_(e, 0);
  }
  /// Count a read. Dropped (false) when the queue is nearly full: it is only a statistic.
  bool push_read(uint8_t channel, size_t n) {
    LinkEvent e;
    e.kind = LinkEvent::READ;
    e.channel = channel;
    e.size = static_cast<uint16_t>(n);
    return this->push_(e, READ_RESERVE);
  }
  /// A connection, with the peer's address (most significant byte first). Connects, disconnects and the MTU take
  /// the place of a queued READ when the queue is full (push_link_()).
  bool push_connect(const uint8_t address[6]) {
    LinkEvent e;
    e.kind = LinkEvent::CONNECT;
    std::memcpy(e.data, address, 6);
    return this->push_link_(e);
  }
  bool push_disconnect() {
    LinkEvent e;
    e.kind = LinkEvent::DISCONNECT;
    return this->push_link_(e);
  }
  bool push_mtu(uint16_t mtu) {
    LinkEvent e;
    e.kind = LinkEvent::MTU;
    e.size = mtu;
    return this->push_link_(e);
  }

  /// Take the oldest event. False when empty.
  bool pop(LinkEvent &out) {
    std::lock_guard<std::mutex> guard(this->lock_);
    if (this->count_ == 0)
      return false;
    out = this->events_[this->head_];
    this->head_ = (this->head_ + 1) % SIZE;
    this->count_--;
    return true;
  }
  size_t count() {
    std::lock_guard<std::mutex> guard(this->lock_);
    return this->count_;
  }
  /// Events refused because the queue was full, since boot (writes and link events; dropped reads are not counted).
  uint32_t overflows() {
    std::lock_guard<std::mutex> guard(this->lock_);
    return this->overflows_;
  }

 protected:
  bool push_(const LinkEvent &e, size_t reserve) {
    std::lock_guard<std::mutex> guard(this->lock_);
    return this->push_locked_(e, reserve);
  }
  bool push_locked_(const LinkEvent &e, size_t reserve) {
    if (this->count_ + reserve >= SIZE) {
      if (e.kind != LinkEvent::READ)
        this->overflows_++;
      return false;
    }
    this->events_[(this->head_ + this->count_) % SIZE] = e;
    this->count_++;
    return true;
  }
  /// Connects, disconnects and the MTU matter to the session, so a full queue gives up its oldest READ for them.
  /// With no READ to give up the event is refused and counted (the queue is then full of writes, which a client
  /// sends one at a time, so this does not happen in practice).
  bool push_link_(const LinkEvent &e) {
    std::lock_guard<std::mutex> guard(this->lock_);
    for (size_t i = 0; this->count_ >= SIZE && i < this->count_; i++) {
      if (this->events_[(this->head_ + i) % SIZE].kind != LinkEvent::READ)
        continue;
      for (size_t j = i; j + 1 < this->count_; j++)
        this->events_[(this->head_ + j) % SIZE] = this->events_[(this->head_ + j + 1) % SIZE];
      this->count_--;
    }
    return this->push_locked_(e, 0);
  }

  std::mutex lock_;
  LinkEvent events_[SIZE];
  size_t head_{0};
  size_t count_{0};
  uint32_t overflows_{0};
};

/// The readable value of every characteristic, set on the main loop and read on NimBLE's task. A read copies the
/// whole value under the lock, so it never sees half of one.
class ValueStore {
 public:
  static constexpr size_t MAX_VALUE = proto::STATUS_SIZE;  // the largest record (Status, 20 bytes)
  static constexpr size_t CHANNELS = proto::CHANNEL_AUTH + 1;

  /// Store a value (longer ones are cut to MAX_VALUE; no record is longer). False for an unknown channel.
  bool set(uint8_t channel, const uint8_t *data, size_t n) {
    if (channel >= CHANNELS)
      return false;
    if (n > MAX_VALUE)
      n = MAX_VALUE;
    std::lock_guard<std::mutex> guard(this->lock_);
    std::memcpy(this->values_[channel], data, n);
    this->sizes_[channel] = static_cast<uint8_t>(n);
    return true;
  }
  /// Copy a value into `out` (MAX_VALUE bytes of room); returns its length (0 for an unknown channel).
  size_t get(uint8_t channel, uint8_t *out) {
    if (channel >= CHANNELS)
      return 0;
    std::lock_guard<std::mutex> guard(this->lock_);
    const size_t n = this->sizes_[channel];
    std::memcpy(out, this->values_[channel], n);
    return n;
  }

 protected:
  std::mutex lock_;
  uint8_t values_[CHANNELS][MAX_VALUE]{};
  uint8_t sizes_[CHANNELS]{};
};

}  // namespace precision_feeder
}  // namespace esphome
