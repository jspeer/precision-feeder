// Precision Feeder BLE: the hand-off between NimBLE's host task and the main loop, as plain C++.
//
// NimBLE calls the adapter from its own FreeRTOS task, while BleEngine and FeederCore run on the main loop only. So
// every callback either copies an event into LinkQueue (connects, disconnects, the MTU, writes, reads) or answers a
// read from ValueStore, and a disconnect also closes ValueStore until the main loop has handled it; the main loop
// drains the queue into the engine and updates the values. Both are guarded by
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
  uint32_t generation{0};    // DISCONNECT: the number ValueStore::close() gave it
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
  /// A connection, with the peer's address (most significant byte first). Connects and disconnects are never
  /// refused, and the MTU takes a queued READ's place when the queue is full (push_link_()).
  bool push_connect(const uint8_t address[6]) {
    LinkEvent e;
    e.kind = LinkEvent::CONNECT;
    std::memcpy(e.data, address, 6);
    return this->push_link_(e);
  }
  bool push_disconnect(uint32_t generation) {
    LinkEvent e;
    e.kind = LinkEvent::DISCONNECT;
    e.generation = generation;
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
  /// Connects and disconnects bound a session, so they are never refused (2.4.5; a lost one let the next client
  /// inherit the last one's session). A full queue gives up its oldest READ (only counted), else its oldest WRITE (a
  /// write the client was told arrived is then never acted on: a missed command, never a doubled one), else its
  /// oldest MTU or link event (a session long gone: the newest disconnect is never the one given up). The MTU only
  /// takes a READ's place. Whatever gave way is counted, as a refused event is.
  bool push_link_(const LinkEvent &e) {
    std::lock_guard<std::mutex> guard(this->lock_);
    if (this->count_ >= SIZE && !this->drop_oldest_(LinkEvent::READ) && e.kind != LinkEvent::MTU) {
      if (!this->drop_oldest_(LinkEvent::WRITE) && !this->drop_oldest_(LinkEvent::MTU))
        this->drop_at_(0);
      this->overflows_++;
    }
    return this->push_locked_(e, 0);
  }
  bool drop_oldest_(LinkEvent::Kind kind) {
    for (size_t i = 0; i < this->count_; i++) {
      if (this->events_[(this->head_ + i) % SIZE].kind == kind) {
        this->drop_at_(i);
        return true;
      }
    }
    return false;
  }
  void drop_at_(size_t i) {
    for (size_t j = i; j + 1 < this->count_; j++)
      this->events_[(this->head_ + j) % SIZE] = this->events_[(this->head_ + j + 1) % SIZE];
    this->count_--;
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
  /// Copy a value into `out` (MAX_VALUE bytes of room); returns its length (0 for an unknown channel). While closed,
  /// every value but Info reads as zeros of its length.
  size_t get(uint8_t channel, uint8_t *out) {
    if (channel >= CHANNELS)
      return 0;
    std::lock_guard<std::mutex> guard(this->lock_);
    const size_t n = this->sizes_[channel];
    if (this->closed_ && channel != proto::CHANNEL_INFO)
      std::memset(out, 0, n);
    else
      std::memcpy(out, this->values_[channel], n);
    return n;
  }
  /// A client went (on NimBLE's task). Until the main loop has handled that disconnect (reopen()), the values read as
  /// zeros, so the next client never reads what the last one was shown, however late the main loop is (2.4.5).
  /// Returns the number to queue with the disconnect.
  uint32_t close() {
    std::lock_guard<std::mutex> guard(this->lock_);
    this->closed_ = true;
    return ++this->generation_;
  }
  /// The main loop handled the disconnect numbered `generation` (the engine has cleared the session's values). The
  /// values read again, unless another client has gone since.
  void reopen(uint32_t generation) {
    std::lock_guard<std::mutex> guard(this->lock_);
    if (generation == this->generation_)
      this->closed_ = false;
  }

 protected:
  std::mutex lock_;
  bool closed_{false};
  uint32_t generation_{0};
  uint8_t values_[CHANNELS][MAX_VALUE]{};
  uint8_t sizes_[CHANNELS]{};
};

}  // namespace precision_feeder
}  // namespace esphome
