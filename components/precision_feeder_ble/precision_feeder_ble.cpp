// Precision Feeder BLE control plane: the ESPHome adapter on NimBLE (see precision_feeder_ble.h; the protocol is
// engine.cpp, the hand-off between the two tasks is gatt_link.h).
//
// Written against ESP-IDF's NimBLE host API (Apache-2.0); nothing is copied from ESPHome's Bluetooth components or
// from ESP-IDF's examples.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder_ble.h"

#include <cstring>

#include <esp_random.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/dis/ble_svc_dis.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_WEBSERVER
#include "esphome/components/web_server_base/web_server_base.h"
#endif

extern "C" void ble_store_config_init(void);

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder_ble";

// NimBLE keeps pointers to the service definition, so it lives here for as long as the device runs. There is one
// PrecisionFeederBle per build (the component allows one).
static PrecisionFeederBle *instance = nullptr;
static ble_uuid128_t service_uuid;
static ble_uuid128_t chr_uuids[gatt::COUNT];
static ble_gatt_chr_def chr_defs[gatt::COUNT + 1];
static ble_gatt_svc_def svc_defs[2];

static int gap_trampoline(ble_gap_event *event, void *arg) {
  return static_cast<PrecisionFeederBle *>(arg)->on_gap_event(event);
}
static int access_trampoline(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt *ctxt, void *arg) {
  return instance->on_access(conn_handle, attr_handle, ctxt,
                             static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg)));
}
static void sync_trampoline() { instance->on_sync(); }
static void reset_trampoline(int reason) { ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason); }
static void host_task(void *) {
  nimble_port_run();  // returns only when the stack is stopped, which this firmware never does
  nimble_port_freertos_deinit();
}

#ifdef USE_WEBSERVER
/// GET /feeder/ble: the BLE activity log as JSON, for the web app. Added with add_handler(), so web auth applies
/// when it is configured. Runs on the web server's task, so it only reads the copy the main loop prepared.
class BleActivityHandler : public AsyncWebHandler {
 public:
  explicit BleActivityHandler(PrecisionFeederBle *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET)
      return false;
    char url[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url) == "/feeder/ble";
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    const std::string body = this->parent_->activity_json_copy();
    request->send(200, "application/json", body.c_str());
  }

 protected:
  PrecisionFeederBle *parent_;
};
#endif

void PrecisionFeederBle::setup() {
  instance = this;

  // The name, as ESPHome's esp32_ble gives it: "-" and the last 6 hex digits of the MAC with name_add_mac_suffix.
  const char *base = this->name_ != nullptr ? this->name_ : App.get_name().c_str();
  if (App.is_name_add_mac_suffix_enabled() && this->name_ != nullptr) {
    char mac[MAC_ADDRESS_BUFFER_SIZE];
    get_mac_address_into_buffer(mac);
    snprintf(this->device_name_, sizeof(this->device_name_), "%.13s-%s", base, mac + 6);
  } else {
    snprintf(this->device_name_, sizeof(this->device_name_), "%s", base);
  }

  // Every value starts as zeros of its record's length, until the engine sets it.
  const uint8_t zeros[ValueStore::MAX_VALUE]{};
  for (const auto &c : gatt::TABLE)
    this->values_.set(c.channel, zeros, c.initial_size);

  if (nimble_port_init() != ESP_OK) {
    ESP_LOGE(TAG, "NimBLE did not start (nimble_port_init); BLE is off");
    this->mark_failed();
    return;
  }
  ble_hs_cfg.sync_cb = sync_trampoline;
  ble_hs_cfg.reset_cb = reset_trampoline;
  // Pairing, if the client pairs: LE Secure Connections with bonding, "Just Works" (no screen or keypad), the same
  // as the Bluedroid build's io_capability: none and auth_req_mode: sc_bond. Bonds are kept in NVS; when the store
  // is full, the oldest bond makes room (ble_store_util_status_rr). Access control is the protocol 2 key.
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_dis_init();
  ble_svc_dis_manufacturer_name_set(this->manufacturer_);
  ble_svc_dis_model_number_set(this->model_);
  ble_svc_dis_firmware_revision_set(this->firmware_);

  gatt::uuid_le(gatt::SERVICE, service_uuid.value);
  service_uuid.u.type = BLE_UUID_TYPE_128;
  for (size_t i = 0; i < gatt::COUNT; i++) {
    const gatt::Characteristic &c = gatt::TABLE[i];
    chr_uuids[i].u.type = BLE_UUID_TYPE_128;
    gatt::uuid_le(c.channel, chr_uuids[i].value);
    ble_gatt_chr_flags flags = 0;
    if (c.props & gatt::READ)
      flags |= BLE_GATT_CHR_F_READ;
    if (c.props & gatt::WRITE)
      flags |= BLE_GATT_CHR_F_WRITE;
    if (c.props & gatt::NOTIFY)
      flags |= BLE_GATT_CHR_F_NOTIFY;
    chr_defs[i] = {};
    chr_defs[i].uuid = &chr_uuids[i].u;
    chr_defs[i].access_cb = access_trampoline;
    chr_defs[i].arg = reinterpret_cast<void *>(static_cast<uintptr_t>(c.channel));
    chr_defs[i].flags = flags;
    chr_defs[i].val_handle = &this->val_handles_[i];
  }
  chr_defs[gatt::COUNT] = {};
  svc_defs[0] = {};
  svc_defs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  svc_defs[0].uuid = &service_uuid.u;
  svc_defs[0].characteristics = chr_defs;
  svc_defs[1] = {};
  int rc = ble_gatts_count_cfg(svc_defs);
  if (rc == 0)
    rc = ble_gatts_add_svcs(svc_defs);
  if (rc != 0) {
    ESP_LOGE(TAG, "GATT table refused by NimBLE (%d); BLE is off", rc);
    this->mark_failed();
    return;
  }
  ble_svc_gap_device_name_set(this->device_name_);
  ble_store_config_init();
  nimble_port_freertos_init(host_task);
  this->stack_ok_ = true;
}

void PrecisionFeederBle::start_engine_() {
  this->engine_ = new BleEngine(*this->feeder_, *this);  // lives as long as the device runs
  this->engine_->set_time_policy(this->time_policy_);
  this->engine_->set_network_time_stale_ms(this->network_stale_ms_);
  this->engine_->start();
#ifdef USE_WEBSERVER
  if (web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new BleActivityHandler(this));  // lives as long as the device
#endif
  this->feeder_->set_feed_listener([this](const FeedRecord &record) { this->engine_->on_feed(record); });
}

void PrecisionFeederBle::loop() {
  if (this->engine_ == nullptr)
    this->start_engine_();  // the first pass, once every component (the feeder too) is set up
  LinkEvent e;
  while (this->queue_.pop(e)) {
    switch (e.kind) {
      case LinkEvent::CONNECT:
        this->engine_->on_connect();
        this->engine_->set_peer(e.data);
        break;
      case LinkEvent::DISCONNECT:
        this->engine_->on_disconnect();
        break;
      case LinkEvent::MTU:
        this->engine_->set_mtu(e.size);
        break;
      case LinkEvent::WRITE:
        this->engine_->on_write(e.channel, e.data, e.size);
        break;
      case LinkEvent::READ:
        this->engine_->on_read(e.channel, e.size);
        break;
    }
  }
  this->engine_->loop(millis());
  if (this->engine_->activity_last_id() != this->activity_json_id_) {
    std::string json = this->engine_->activity_json();
    LockGuard guard(this->activity_lock_);
    this->activity_json_ = std::move(json);
    this->activity_json_id_ = this->engine_->activity_last_id();
  }
}

std::string PrecisionFeederBle::activity_json_copy() {
  LockGuard guard(this->activity_lock_);
  return this->activity_json_;
}

// ---- NimBLE's task ------------------------------------------------------------------------------------------

void PrecisionFeederBle::on_sync() {
  ble_hs_util_ensure_addr(0);  // the chip's public address, as the Bluedroid build used
  ble_hs_id_infer_auto(0, &this->own_addr_type_);
  this->count_bonds_();
  this->advertise_();
}

void PrecisionFeederBle::count_bonds_() {
  ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
  int n = 0;
  if (ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) == 0)
    this->bonds_ = n;
}

void PrecisionFeederBle::forget_bonds() {
  if (!this->stack_ok_)
    return;
  ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
  int n = 0, forgotten = 0;
  if (ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) == 0) {
    for (int i = 0; i < n; i++) {
      // Deletes its keys and its subscriptions from the store, and ends its connection if it has one.
      if (ble_gap_unpair(&peers[i]) == 0)
        forgotten++;
    }
  }
  this->count_bonds_();
  ESP_LOGW(TAG, "Bluetooth bonds forgotten from the web app: %d of %d", forgotten, n);
}

void PrecisionFeederBle::advertise_() {
  if (ble_gap_adv_active() || this->conn_handle_.load() != BLE_HS_CONN_HANDLE_NONE)
    return;  // one client at a time: advertising starts again when it goes
  // The same packets as the Bluedroid build: the flags and the service UUID, then the name and the transmit power
  // in the scan response (scanners and tools/precision_feeder_client find the feeder by the service UUID).
  ble_hs_adv_fields adv{};
  adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv.uuids128 = &service_uuid;
  adv.num_uuids128 = 1;
  adv.uuids128_is_complete = 1;
  const char *step = "advertising data";
  int rc = ble_gap_adv_set_fields(&adv);
  ble_hs_adv_fields rsp{};
  rsp.name = reinterpret_cast<const uint8_t *>(this->device_name_);
  rsp.name_len = strlen(this->device_name_);
  rsp.name_is_complete = 1;
  rsp.tx_pwr_lvl_is_present = 1;
  rsp.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  if (rc == 0) {
    step = "scan response";
    rc = ble_gap_adv_rsp_set_fields(&rsp);
  }
  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = 0x20;  // 20 to 40 ms, as ESPHome's advertising
  params.itvl_max = 0x40;
  if (rc == 0) {
    step = "start";
    rc = ble_gap_adv_start(this->own_addr_type_, nullptr, BLE_HS_FOREVER, &params, gap_trampoline, this);
  }
  if (rc != 0)
    ESP_LOGW(TAG, "Advertising did not start: %s, error %d", step, rc);
}

int PrecisionFeederBle::on_gap_event(ble_gap_event *event) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      if (event->connect.status != 0) {
        this->advertise_();
        break;
      }
      this->conn_handle_ = event->connect.conn_handle;
      this->subscriptions_ = 0;
      uint8_t address[6]{};
      ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
        for (int i = 0; i < 6; i++)
          address[i] = desc.peer_ota_addr.val[5 - i];  // NimBLE keeps it least significant byte first
      }
      this->queue_.push_connect(address);
      break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      this->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
      this->subscriptions_ = 0;
      this->queue_.push_disconnect();
      this->count_bonds_();
      this->advertise_();
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      this->count_bonds_();  // pairing may have added a bond (or replaced the oldest)
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      this->advertise_();
      break;
    case BLE_GAP_EVENT_MTU:
      this->queue_.push_mtu(event->mtu.value);
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      for (size_t i = 0; i < gatt::COUNT; i++) {
        if (this->val_handles_[i] != event->subscribe.attr_handle)
          continue;
        const uint16_t bit = 1u << gatt::TABLE[i].channel;
        if (event->subscribe.cur_notify)
          this->subscriptions_ |= bit;
        else
          this->subscriptions_ &= static_cast<uint16_t>(~bit);
      }
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // A bonded peer pairs again (it lost its keys): forget the old bond and let it pair afresh.
      ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
      break;
  }
  return 0;
}

int PrecisionFeederBle::on_access(uint16_t /*conn_handle*/, uint16_t /*attr_handle*/, ble_gatt_access_ctxt *ctxt,
                                  uint8_t channel) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    uint8_t value[ValueStore::MAX_VALUE];
    const size_t n = this->values_.get(channel, value);
    if (os_mbuf_append(ctxt->om, value, n) != 0)
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    this->queue_.push_read(channel, n);
    return 0;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    const uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t data[LinkEvent::MAX_DATA];
    const uint16_t keep = n < sizeof(data) ? n : sizeof(data);
    if (os_mbuf_copydata(ctxt->om, 0, keep, data) != 0)
      return BLE_ATT_ERR_UNLIKELY;
    if (!this->queue_.push_write(channel, data, n))
      return BLE_ATT_ERR_INSUFFICIENT_RES;  // the client retries; the retry cache makes that safe
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

// ---- main loop ----------------------------------------------------------------------------------------------

bool PrecisionFeederBle::subscribed_(uint8_t channel) const { return (this->subscriptions_.load() >> channel) & 1u; }

void PrecisionFeederBle::set_value(uint8_t channel, const uint8_t *data, size_t n, bool notify) {
  if (!this->values_.set(channel, data, n) || !notify || !this->stack_ok_)
    return;
  const uint16_t conn = this->conn_handle_.load();
  if (conn == BLE_HS_CONN_HANDLE_NONE || !this->subscribed_(channel))
    return;  // as on Bluedroid, a notification goes only to a client that turned them on
  for (size_t i = 0; i < gatt::COUNT; i++) {
    if (gatt::TABLE[i].channel != channel)
      continue;
    os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
    if (om == nullptr || ble_gatts_notify_custom(conn, this->val_handles_[i], om) != 0) {
      ESP_LOGD(TAG, "Notification on channel %u not sent", channel);  // notify_custom frees om either way
    }
  }
}

void PrecisionFeederBle::random(uint8_t *out, size_t n) {
  esp_fill_random(out, n);  // hardware RNG; the radio is on, so it is a true random source
}

void PrecisionFeederBle::log(LogLevel level, const char *message) {
  switch (level) {
    case LogLevel::kError:
      ESP_LOGE(TAG, "%s", message);
      break;
    case LogLevel::kWarn:
      ESP_LOGW(TAG, "%s", message);
      break;
    case LogLevel::kInfo:
      ESP_LOGI(TAG, "%s", message);
      break;
    default:
      ESP_LOGD(TAG, "%s", message);
      break;
  }
}

void PrecisionFeederBle::dump_config() {
  const char *policy = "fallback_only";
  if (this->time_policy_ == proto::TimePolicy::ALWAYS)
    policy = "always";
  else if (this->time_policy_ == proto::TimePolicy::OFF)
    policy = "off";
  ESP_LOGCONFIG(TAG, "Precision Feeder BLE (NimBLE):");
  ESP_LOGCONFIG(TAG, "  Name: %s", this->device_name_);
  ESP_LOGCONFIG(TAG, "  Protocol version: %u", proto::PROTOCOL_VERSION);
  static const char *const KEYS[] = {"none set: every AUTH is refused until one is set in the web app",
                                     "from the build (YAML)", "saved on the device"};
  ESP_LOGCONFIG(TAG, "  Authentication: required; key: %s", KEYS[static_cast<uint8_t>(this->feeder_->ble_key_source())]);
  ESP_LOGCONFIG(TAG, "  BLE may change: guard limits %s, calibration %s", YESNO(this->feeder_->ble_may_change_guard()),
                YESNO(this->feeder_->ble_may_change_calibration()));
  ESP_LOGCONFIG(TAG, "  Time policy: %s (network time counts as fresh for %u h)", policy,
                (unsigned) (this->network_stale_ms_ / 3600000u));
}

}  // namespace precision_feeder
}  // namespace esphome
