// Precision Feeder: a firmware image uploaded from the web app (firmware 2.0.0 N7, DESIGN.md section 9.5).
//
// The image arrives as the raw body of POST /feeder/firmware?sha256=<hex>, in chunks. This class checks the request
// was signed (by the caller, before the first chunk), streams the chunks into the update partition through an
// UpdateSink, hashes them as they pass, and only completes the update if the image is whole and its SHA-256 is the
// one the browser signed. Anything else aborts it, and the running firmware stays.
//
// Pure C++ with no ESPHome or ESP-IDF dependencies, so it is tested on a host (tests/test_core.cpp). The adapter
// (precision_feeder.cpp) gives it a sink over ESPHome's OTA backend.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "crypto.h"

namespace esphome {
namespace precision_feeder {

/// Where the image goes. The adapter implements it over ESPHome's OTA backend.
class UpdateSink {
 public:
  virtual ~UpdateSink() = default;
  /// Open the update partition for an image of `size` bytes. False if it cannot (too big, no partition).
  virtual bool begin(size_t size) = 0;
  virtual bool write(const uint8_t *data, size_t len) = 0;
  /// Check the image and mark it to boot next. False if it is not a valid image.
  virtual bool end() = 0;
  /// Drop a partly written image; the running firmware stays.
  virtual void abort() = 0;
};

class FirmwareUpload {
 public:
  enum class Status : uint8_t {
    IDLE = 0,        // no upload yet
    RECEIVING = 1,   // writing chunks
    OK = 2,          // the image is written and checked; restart to run it
    REFUSED = 3,     // not authorised: the caller has its answer (403 or 429), and the body is ignored
    BAD_REQUEST = 4, // no or malformed sha256, or an empty body
    FLASH = 5,       // the update partition refused it: too big, a write failed, or not a valid image
    INCOMPLETE = 6,  // fewer bytes arrived than announced
    MISMATCH = 7,    // the bytes are not the image the browser signed
  };

  /// The first chunk of a new upload. `expected` is the signed SHA-256 (nullptr if the parameter was malformed).
  /// An unfinished upload before it (a dropped connection) is aborted first.
  void start(const uint8_t *expected, size_t total, bool authorised, UpdateSink *sink) {
    this->abort_open_();
    this->sink_ = sink;
    this->total_ = total;
    this->received_ = 0;
    this->sha_.reset();
    if (!authorised) {
      this->status_ = Status::REFUSED;
      return;
    }
    if (expected == nullptr || total == 0 || sink == nullptr) {
      this->status_ = Status::BAD_REQUEST;
      return;
    }
    std::memcpy(this->expected_, expected, sizeof(this->expected_));
    if (!sink->begin(total)) {
      this->status_ = Status::FLASH;
      return;
    }
    this->status_ = Status::RECEIVING;
  }

  /// A chunk of the body. Ignored unless receiving.
  void write(const uint8_t *data, size_t len) {
    if (this->status_ != Status::RECEIVING)
      return;
    if (this->received_ + len > this->total_) {  // more than announced: the server never does this, but be safe
      this->fail_(Status::FLASH);
      return;
    }
    this->sha_.update(data, len);
    if (!this->sink_->write(data, len)) {
      this->fail_(Status::FLASH);
      return;
    }
    this->received_ += len;
  }

  /// The body has ended. Completes the update if everything checks out; returns the outcome.
  Status finish() {
    if (this->status_ != Status::RECEIVING)
      return this->status_;
    if (this->received_ != this->total_) {
      this->fail_(Status::INCOMPLETE);
      return this->status_;
    }
    uint8_t digest[crypto::Sha256::DIGEST_SIZE];
    this->sha_.finish(digest);
    if (std::memcmp(digest, this->expected_, sizeof(digest)) != 0) {
      this->fail_(Status::MISMATCH);
      return this->status_;
    }
    this->status_ = this->sink_->end() ? Status::OK : Status::FLASH;
    return this->status_;
  }

  /// A request that ended with no body at all (or not as a raw body): nothing was started for it.
  void no_body() {
    this->abort_open_();
    this->status_ = Status::BAD_REQUEST;
  }

  Status status() const { return this->status_; }
  size_t received() const { return this->received_; }

 protected:
  void fail_(Status status) {
    this->sink_->abort();
    this->status_ = status;
  }
  void abort_open_() {
    if (this->status_ == Status::RECEIVING && this->sink_ != nullptr)
      this->sink_->abort();
    this->status_ = Status::IDLE;
  }

  UpdateSink *sink_{nullptr};
  Status status_{Status::IDLE};
  size_t total_{0};
  size_t received_{0};
  uint8_t expected_[crypto::Sha256::DIGEST_SIZE]{};
  crypto::Sha256 sha_;
};

}  // namespace precision_feeder
}  // namespace esphome
