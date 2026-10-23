// Host-side tests for the NimBLE adapter's pure parts (2.4.0): the hand-off between NimBLE's task and the main loop
// (components/precision_feeder_ble/gatt_link.h) and the GATT table (gatt_table.h).
// Build and run:
//   c++ -std=c++17 -Wall -Wextra -pthread -I../components/precision_feeder_ble test_ble_link.cpp -o /tmp/test_ble_link && /tmp/test_ble_link
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "gatt_link.h"
#include "gatt_table.h"

using namespace esphome::precision_feeder;

static int failures = 0;
#define CHECK(cond, msg)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
      failures++;                                         \
    }                                                     \
  } while (0)

/// The UUID string for a low byte, from the table's prefix.
static std::string uuid_string(uint8_t low) {
  char tail[5];
  std::snprintf(tail, sizeof(tail), "%04x", low);
  return std::string(gatt::UUID_PREFIX) + tail;
}

/// Bluetooth's byte order for a UUID string: the 16 bytes, least significant first.
static void parse_le(const std::string &s, uint8_t out[16]) {
  std::string hex;
  for (char c : s)
    if (c != '-')
      hex += c;
  for (int i = 0; i < 16; i++)
    out[15 - i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
}

int main() {
  // ---- The GATT table ----
  {
    CHECK(uuid_string(gatt::SERVICE) == "02dd1dd9-ba75-4bbc-bdea-306d21010001", "service UUID string");
    for (uint8_t low : {gatt::SERVICE, proto::CHANNEL_INFO, proto::CHANNEL_AUTH}) {
      uint8_t got[16], want[16];
      gatt::uuid_le(low, got);
      parse_le(uuid_string(low), want);
      CHECK(std::memcmp(got, want, 16) == 0, "UUID bytes are the string's, least significant first");
    }
    CHECK(gatt::COUNT == 9, "nine characteristics, Info to Auth");
    uint16_t seen = 0;
    for (const auto &c : gatt::TABLE) {
      CHECK(c.channel >= proto::CHANNEL_INFO && c.channel <= proto::CHANNEL_AUTH, "channel in range");
      CHECK(!(seen & (1u << c.channel)), "each channel once");
      seen |= static_cast<uint16_t>(1u << c.channel);
      CHECK(c.initial_size <= ValueStore::MAX_VALUE, "initial value fits the store");
      const bool writable = (c.props & gatt::WRITE) != 0;
      const bool should_write = c.channel == proto::CHANNEL_COMMAND || c.channel == proto::CHANNEL_PROFILE ||
                                c.channel == proto::CHANNEL_CONFIG || c.channel == proto::CHANNEL_CALIBRATION;
      CHECK(writable == should_write, "only Command, Profile, Config and Calibration are writable");
      const bool notifies = (c.props & gatt::NOTIFY) != 0;
      const bool should_notify = c.channel == proto::CHANNEL_RESPONSE || c.channel == proto::CHANNEL_STATUS ||
                                 c.channel == proto::CHANNEL_EVENT_LOG;
      CHECK(notifies == should_notify, "only Response, Status and Event log notify");
      const bool readable = (c.props & gatt::READ) != 0;
      CHECK(readable == (c.channel != proto::CHANNEL_COMMAND && c.channel != proto::CHANNEL_EVENT_LOG),
            "everything but Command and Event log is readable");
    }
  }

  // ---- LinkQueue: order, sizes, full ----
  {
    LinkQueue q;
    const uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
    CHECK(q.push_connect(peer), "connect queued");
    uint8_t w[30];
    for (int i = 0; i < 30; i++)
      w[i] = static_cast<uint8_t>(i);
    CHECK(q.push_write(proto::CHANNEL_COMMAND, w, 30), "long write queued");
    CHECK(q.push_mtu(185), "MTU queued");
    LinkEvent e;
    CHECK(q.pop(e) && e.kind == LinkEvent::CONNECT && std::memcmp(e.data, peer, 6) == 0, "connect first, with peer");
    CHECK(q.pop(e) && e.kind == LinkEvent::WRITE && e.channel == proto::CHANNEL_COMMAND && e.size == 30,
          "the write keeps the length the client sent");
    CHECK(std::memcmp(e.data, w, LinkEvent::MAX_DATA) == 0, "and its first bytes");
    CHECK(q.pop(e) && e.kind == LinkEvent::MTU && e.size == 185, "then the MTU");
    CHECK(!q.pop(e), "then empty");

    for (size_t i = 0; i < LinkQueue::SIZE; i++)
      CHECK(q.push_write(proto::CHANNEL_CONFIG, w, 5), "writes fill the queue");
    CHECK(!q.push_write(proto::CHANNEL_CONFIG, w, 5), "a write to a full queue is refused (the client gets an error)");
    CHECK(q.overflows() == 1, "and counted");
    CHECK(!q.push_read(proto::CHANNEL_STATUS, 20), "a read to a full queue is dropped");
    CHECK(q.overflows() == 1, "dropped reads are not overflows");
  }

  // ---- LinkQueue: reads never take the last slots; link events push out a read ----
  {
    LinkQueue q;
    size_t reads = 0;
    while (q.push_read(proto::CHANNEL_STATUS, 20))
      reads++;
    CHECK(reads == LinkQueue::SIZE - LinkQueue::READ_RESERVE, "reads stop short of the reserve");
    const uint8_t w[3] = {1, 2, 3};
    CHECK(q.push_write(proto::CHANNEL_COMMAND, w, 3) && q.push_write(proto::CHANNEL_COMMAND, w, 3),
          "writes still fit in the reserve");
    CHECK(q.count() == LinkQueue::SIZE, "full");
    CHECK(q.push_disconnect(), "a disconnect into a full queue takes a read's place");
    CHECK(q.count() == LinkQueue::SIZE && q.overflows() == 0, "still full, nothing lost but a read");
    LinkEvent e, last;
    size_t n = 0, read_count = 0;
    while (q.pop(e)) {
      n++;
      read_count += e.kind == LinkEvent::READ;
      last = e;
    }
    CHECK(n == LinkQueue::SIZE && read_count == reads - 1, "one read gave way");
    CHECK(last.kind == LinkEvent::DISCONNECT, "the disconnect comes last, after the writes before it");

    LinkQueue full;
    for (size_t i = 0; i < LinkQueue::SIZE; i++)
      full.push_write(proto::CHANNEL_COMMAND, w, 3);
    CHECK(!full.push_disconnect() && full.overflows() == 1, "with only writes queued, a link event is refused");
  }

  // ---- ValueStore ----
  {
    ValueStore v;
    uint8_t out[ValueStore::MAX_VALUE];
    CHECK(v.get(proto::CHANNEL_STATUS, out) == 0, "empty at first");
    const uint8_t a[4] = {9, 8, 7, 6};
    CHECK(v.set(proto::CHANNEL_RESPONSE, a, 4) && v.get(proto::CHANNEL_RESPONSE, out) == 4 && out[3] == 6,
          "a value reads back");
    uint8_t big[40] = {};
    big[19] = 0x55;
    CHECK(v.set(proto::CHANNEL_STATUS, big, 40) && v.get(proto::CHANNEL_STATUS, out) == ValueStore::MAX_VALUE &&
              out[19] == 0x55,
          "a longer value is cut to the largest record");
    CHECK(!v.set(ValueStore::CHANNELS, a, 4) && v.get(ValueStore::CHANNELS, out) == 0, "unknown channel refused");
  }

  // ---- Two threads: a reader never sees half a value, and every queued write arrives in order ----
  {
    ValueStore v;
    LinkQueue q;
    bool torn = false;
    std::thread host([&] {
      uint8_t out[ValueStore::MAX_VALUE];
      for (int i = 0; i < 200000; i++) {
        const size_t n = v.get(proto::CHANNEL_STATUS, out);
        for (size_t j = 1; j < n; j++)
          if (out[j] != out[0])
            torn = true;
        uint8_t w[2] = {static_cast<uint8_t>(i & 0xFF), 0};
        while (!q.push_write(proto::CHANNEL_COMMAND, w, 2)) {
        }
        if (i >= 1000)
          break;
      }
    });
    uint8_t value[ValueStore::MAX_VALUE];
    int next = 0;
    bool in_order = true;
    for (int round = 0; next <= 1000; round++) {
      std::memset(value, round & 0xFF, sizeof(value));
      v.set(proto::CHANNEL_STATUS, value, sizeof(value));
      LinkEvent e;
      while (q.pop(e)) {
        if (e.data[0] != (next & 0xFF))
          in_order = false;
        next++;
      }
    }
    host.join();
    CHECK(!torn, "no torn reads");
    CHECK(in_order && next == 1001, "every write arrives, in order");
  }

  if (failures == 0)
    std::printf("All BLE link tests passed\n");
  return failures == 0 ? 0 : 1;
}
