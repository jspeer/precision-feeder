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
    CHECK(q.push_disconnect(1), "a disconnect into a full queue takes a read's place");
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

    // 2.4.5 (code review of 2.4.3, R1): with only writes queued, a disconnect and the next connect still go in, in
    // order, each in place of the oldest write; the MTU is refused.
    LinkQueue full;
    for (size_t i = 0; i < LinkQueue::SIZE; i++) {
      uint8_t numbered[3] = {static_cast<uint8_t>(i), 0, 0};
      full.push_write(proto::CHANNEL_COMMAND, numbered, 3);
    }
    const uint8_t peer[6] = {6, 5, 4, 3, 2, 1};
    CHECK(full.push_disconnect(7) && full.push_connect(peer), "a disconnect and a connect into a queue of writes");
    CHECK(!full.push_mtu(185), "the MTU is refused there");
    CHECK(full.overflows() == 3, "the two writes that gave way and the MTU are counted");
    n = 0;
    bool writes_in_order = true;
    LinkEvent got[LinkQueue::SIZE];
    while (full.pop(e))
      got[n++] = e;
    CHECK(n == LinkQueue::SIZE, "still full");
    for (size_t i = 0; i + 2 < n; i++)
      writes_in_order = writes_in_order && got[i].kind == LinkEvent::WRITE && got[i].data[0] == i + 2;
    CHECK(writes_in_order, "the oldest two writes gave way, the rest kept their order");
    CHECK(got[n - 2].kind == LinkEvent::DISCONNECT && got[n - 2].generation == 7, "then the disconnect, numbered");
    CHECK(got[n - 1].kind == LinkEvent::CONNECT && std::memcmp(got[n - 1].data, peer, 6) == 0, "then the connect");

    // A queue of nothing but link events (a main loop stalled through several connections): the oldest gives way.
    LinkQueue links;
    for (uint32_t i = 0; i < LinkQueue::SIZE; i++)
      links.push_disconnect(i + 1);
    CHECK(links.push_disconnect(100), "a disconnect into a queue of disconnects");
    LinkEvent newest;
    while (links.pop(e))
      newest = e;
    CHECK(newest.generation == 100, "the newest disconnect is kept");
  }

  // ---- ValueStore: closed from the radio's disconnect until the main loop has handled it (2.4.5, R1) ----
  {
    ValueStore v;
    uint8_t out[ValueStore::MAX_VALUE];
    const uint8_t status[proto::STATUS_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    const uint8_t info[6] = {2, 0, 0, 0, 0, 1};
    v.set(proto::CHANNEL_STATUS, status, sizeof(status));
    v.set(proto::CHANNEL_INFO, info, sizeof(info));
    v.set(proto::CHANNEL_AUTH, status, 16);
    const uint32_t first = v.close();
    bool zeros = v.get(proto::CHANNEL_STATUS, out) == sizeof(status);
    for (size_t i = 0; i < sizeof(status); i++)
      zeros = zeros && out[i] == 0;
    CHECK(zeros, "closed: the last session's Status reads as zeros of its length");
    CHECK(v.get(proto::CHANNEL_AUTH, out) == 16 && out[0] == 0, "and the old challenge too");
    CHECK(v.get(proto::CHANNEL_INFO, out) == 6 && out[5] == 1, "Info is public and still reads");
    const uint32_t second = v.close();  // another client came and went before the main loop caught up
    v.reopen(first);
    CHECK(v.get(proto::CHANNEL_STATUS, out) == sizeof(status) && out[0] == 0, "the first disconnect alone does not reopen");
    v.reopen(second);
    CHECK(v.get(proto::CHANNEL_STATUS, out) == sizeof(status) && out[0] == 1, "the latest one does");
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
