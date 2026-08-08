#include "platform/ble_hid.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "esp_bt.h"
#include "easy_input_esp_hid_owner.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "keyboard/ble_status_wire.h"
#include "keyboard/config_status.h"
#include "keyboard/hid_keycode.h"
#include "platform/nvs_store.h"
#include "sdkconfig.h"

extern "C" {
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);
}

namespace easy_input {
namespace {

constexpr const char* kTag = "ble_hid";
constexpr const char* kBleDeviceName = "EasyInput AI";
constexpr const char* kBleShortName = "EasyInput AI";
constexpr std::uint16_t kAppearanceHidGeneric = 0x03C0;
constexpr std::uint16_t kBleHidVersion = 0x010A;
// Revision 4 removes the retired auxiliary bulk characteristics while
// retaining HID, configuration/status and Agent characteristics.
constexpr std::uint8_t kGattSchemaRevision = 4;
constexpr std::uint8_t kReportIdKeyboard = 0x01;
constexpr std::uint8_t kReportIdMouse = 0x02;
constexpr std::uint8_t kReportIdConfig = 0x10;
constexpr std::uint8_t kReportIdAppCommand = 0x11;
constexpr std::uint8_t kAppCommandKindHotkey = 0x02;
constexpr std::uint8_t kAppCommandKindConfigAck = 0x03;
constexpr std::uint8_t kAppCommandHotkeyPressed = 0x01;
constexpr std::uint8_t kAppCommandHotkeyReleased = 0x02;
constexpr std::size_t kAppCommandReportPayloadLen = 63;
constexpr std::size_t kAppCommandHeaderLen = 4;
constexpr std::size_t kAppCommandChunkDataLen =
    kAppCommandReportPayloadLen - kAppCommandHeaderLen;
static_assert(kReportIdAppCommand ==
              ai_keyboard::kFixedTextAppCommandReportId);
static_assert(kAppCommandReportPayloadLen ==
              ai_keyboard::kFixedTextAppCommandPayloadLen);
static_assert(kAppCommandHeaderLen ==
              ai_keyboard::kFixedTextAppCommandHeaderLen);
static_assert(kAppCommandChunkDataLen ==
              ai_keyboard::kFixedTextAppCommandChunkDataLen);
constexpr char kConfigStatusFallbackJson[] =
    "{\"schema\":\"ai_keyboard.config_status.v1\",\"firmware\":\"unknown\","
    "\"phase\":\"status\",\"status\":\"status_too_large\",\"bytes\":0,\"crc16\":0,"
    "\"capabilities\":{\"config_max_bytes\":2048},"
    "\"ptt_hotkey\":\"\",\"edit_ptt_hotkey\":\"\",\"saved\":false}";
static_assert(ai_keyboard::kConfigMaxJsonLen == 2048);
static_assert(sizeof(kConfigStatusFallbackJson) - 1 <=
              ai_keyboard::kConfigStatusGattSafeLen);
static_assert(CONFIG_BT_NIMBLE_EATT_CHAN_NUM == 0,
              "per-connection GATT long-read snapshots require EATT disabled");
constexpr int32_t kDirectedReconnectDurationMs = 10000;
constexpr int32_t kFastAdvertisingDurationMs = 60000;
constexpr std::int64_t kConnectedConfigAdvertisingWindowUs = 5LL * 60 * 1000 * 1000;
constexpr std::uint16_t kConfigAdvertisingIntervalMin = 0x00A0;  // 100 ms
constexpr std::uint16_t kConfigAdvertisingIntervalMax = 0x0140;  // 200 ms
constexpr std::uint16_t kSlowAdvertisingIntervalMin = 0x0800;  // 1.28 s
constexpr std::uint16_t kSlowAdvertisingIntervalMax = 0x1000;  // 2.56 s
constexpr std::uint16_t kInvalidConnHandle = 0xFFFF;
constexpr std::uint16_t kActiveConnIntervalMin = 12;  // 15 ms
constexpr std::uint16_t kActiveConnIntervalMax = 36;  // 45 ms
constexpr std::uint16_t kActiveConnLatency = 0;
constexpr std::uint16_t kActiveConnSupervisionTimeout = 400;  // 4 s
constexpr std::uint16_t kIdleConnIntervalMin = 48;            // 60 ms
constexpr std::uint16_t kIdleConnIntervalMax = 96;            // 120 ms
constexpr std::uint16_t kIdleConnLatency = 4;
constexpr std::uint16_t kIdleConnSupervisionTimeout = 600;  // 6 s
constexpr std::uint16_t kDeepIdleConnIntervalMin = 96;       // 120 ms
constexpr std::uint16_t kDeepIdleConnIntervalMax = 160;      // 200 ms
constexpr std::uint16_t kDeepIdleConnLatency = 8;
constexpr std::uint16_t kDeepIdleConnSupervisionTimeout = 800;  // 8 s
constexpr std::uint16_t kSafeConnSupervisionTimeout =
    kActiveConnSupervisionTimeout;
constexpr std::int64_t kAdvertisingRetryInitialUs = 20LL * 1000;
constexpr std::int64_t kAdvertisingRetryMaxUs = 500LL * 1000;
constexpr int kMaxReconnectPeers = 4;

TickType_t delay_ticks(std::uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks == 0 ? 1 : ticks;
}

std::uint32_t monotonic_ms() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

std::string normalized_hotkey_token(const std::string& hotkey) {
  auto begin = hotkey.begin();
  while (begin != hotkey.end() && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
    ++begin;
  }
  auto end = hotkey.end();
  while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
    --end;
  }

  std::string normalized(begin, end);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return normalized;
}

bool should_bridge_hotkey_to_app_command(const std::string& hotkey) {
  const auto normalized = normalized_hotkey_token(hotkey);
  return normalized == "fn" || normalized == "function";
}

const char* advertising_speed_name(bool slow) {
  return slow ? "slow" : "fast";
}

const char* advertising_mode_name(ai_keyboard::BleAdvertisingMode mode) {
  switch (mode) {
    case ai_keyboard::BleAdvertisingMode::Stopped:
      return "stopped";
    case ai_keyboard::BleAdvertisingMode::Directed:
      return "directed";
    case ai_keyboard::BleAdvertisingMode::HidFast:
      return "hid_fast";
    case ai_keyboard::BleAdvertisingMode::HidSlow:
      return "hid_slow";
    case ai_keyboard::BleAdvertisingMode::HidConfig:
      return "hid_config";
    case ai_keyboard::BleAdvertisingMode::ControlSlow:
      return "control_slow";
    case ai_keyboard::BleAdvertisingMode::ControlConfig:
      return "control_config";
  }
  return "unknown";
}

const char* input_report_name(std::uint8_t report_id) {
  switch (report_id) {
    case kReportIdKeyboard:
      return "keyboard";
    case kReportIdMouse:
      return "mouse";
    case kReportIdAppCommand:
      return "app_command";
    default:
      return "hid";
  }
}

constexpr std::array<std::uint8_t, 4> kPreferredConnectionIntervalLe = {
    static_cast<std::uint8_t>(kActiveConnIntervalMin & 0xFF),
    static_cast<std::uint8_t>(kActiveConnIntervalMin >> 8),
    static_cast<std::uint8_t>(kActiveConnIntervalMax & 0xFF),
    static_cast<std::uint8_t>(kActiveConnIntervalMax >> 8)};

constexpr std::uint8_t kReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Keyboard LeftControl)
    0x29, 0xE7,        //   Usage Maximum (Keyboard Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x05, 0xFF,        //   Usage Page (AppleVendor Top Case)
    0x09, 0x03,        //   Usage (Keyboard Fn)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (Reserved)
    0x29, 0x65,        //   Usage Maximum (Keyboard Application)
    0x81, 0x00,        //   Input (Data,Array,Abs)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Const,Var,Abs)
    0xC0,              // End Collection
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0xC0,              //   End Collection
    0xC0,              // End Collection
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x02,        //   Usage (Vendor Usage 2)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x10,        //   Report ID (16)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x09, 0x02,        //   Usage (Vendor Usage 2)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x11,        //   Report ID (17)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x09, 0x02,        //   Usage (Vendor Usage 2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0xC0,              // End Collection
};

const ble_uuid16_t kHidServiceUuid = BLE_UUID16_INIT(0x1812);
const ble_uuid128_t kConfigServiceUuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x32, 0x53, 0x46, 0x6D, 0x01, 0x8B,
                     0x2D, 0x4A, 0x6B, 0x6F, 0x10, 0x4D, 0x2F, 0x7D);
const ble_uuid128_t kConfigWriteUuid =
    BLE_UUID128_INIT(0x02, 0x00, 0x32, 0x53, 0x46, 0x6D, 0x01, 0x8B,
                     0x2D, 0x4A, 0x6B, 0x6F, 0x10, 0x4D, 0x2F, 0x7D);
const ble_uuid128_t kConfigStatusUuid =
    BLE_UUID128_INIT(0x03, 0x00, 0x32, 0x53, 0x46, 0x6D, 0x01, 0x8B,
                     0x2D, 0x4A, 0x6B, 0x6F, 0x10, 0x4D, 0x2F, 0x7D);
const ble_uuid128_t kAgentStatusWriteUuid =
    BLE_UUID128_INIT(0x04, 0x00, 0x32, 0x53, 0x46, 0x6D, 0x01, 0x8B,
                     0x2D, 0x4A, 0x6B, 0x6F, 0x10, 0x4D, 0x2F, 0x7D);

BleHidTransport* s_transport = nullptr;
std::uint16_t s_config_status_handle = 0;
std::uint16_t s_agent_status_write_handle = 0;

void host_task(void* param) {
  (void)param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

int gap_event_callback(ble_gap_event* event, void* arg) {
  auto* transport = static_cast<BleHidTransport*>(arg);
  return transport == nullptr ? 0 : transport->handle_gap_event(event);
}

void hidd_event_callback(void* handler_arg,
                         esp_event_base_t event_base,
                         std::int32_t event_id,
                         void* event_data) {
  (void)handler_arg;
  (void)event_base;
  if (s_transport != nullptr) {
    s_transport->handle_hidd_event(event_id, event_data);
  }
}

int config_access_callback(std::uint16_t conn_handle,
                           std::uint16_t attr_handle,
                           ble_gatt_access_ctxt* ctxt,
                           void* arg) {
  (void)arg;
  if (s_transport == nullptr || ctxt == nullptr) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  return s_transport->handle_config_access(conn_handle, attr_handle, ctxt);
}

const ble_gatt_chr_def kConfigCharacteristics[] = {
    {
        reinterpret_cast<const ble_uuid_t*>(&kConfigWriteUuid),
        config_access_callback,
        nullptr,
        nullptr,
        BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        0,
        nullptr,
        nullptr,
    },
    {
        reinterpret_cast<const ble_uuid_t*>(&kConfigStatusUuid),
        config_access_callback,
        nullptr,
        nullptr,
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        0,
        &s_config_status_handle,
        nullptr,
    },
    {
        reinterpret_cast<const ble_uuid_t*>(&kAgentStatusWriteUuid),
        config_access_callback,
        nullptr,
        nullptr,
        BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        0,
        &s_agent_status_write_handle,
        nullptr,
    },
    {},
};

const ble_gatt_svc_def kConfigServices[] = {
    {
        BLE_GATT_SVC_TYPE_PRIMARY,
        reinterpret_cast<const ble_uuid_t*>(&kConfigServiceUuid),
        nullptr,
        kConfigCharacteristics,
    },
    {},
};

const char* receive_status_name(ai_keyboard::ConfigReceiveStatus status) {
  switch (status) {
    case ai_keyboard::ConfigReceiveStatus::Pending:
      return "pending";
    case ai_keyboard::ConfigReceiveStatus::Complete:
      return "complete";
    case ai_keyboard::ConfigReceiveStatus::InvalidReport:
      return "invalid_report";
    case ai_keyboard::ConfigReceiveStatus::MalformedChunk:
      return "malformed_chunk";
    case ai_keyboard::ConfigReceiveStatus::OutOfOrder:
      return "out_of_order";
    case ai_keyboard::ConfigReceiveStatus::CrcMismatch:
      return "crc_mismatch";
  }
  return "unknown";
}

const char* gap_event_name(std::uint8_t type) {
  switch (type) {
    case BLE_GAP_EVENT_CONNECT:
      return "CONNECT";
    case BLE_GAP_EVENT_DISCONNECT:
      return "DISCONNECT";
    case BLE_GAP_EVENT_CONN_UPDATE:
      return "CONN_UPDATE";
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      return "CONN_UPDATE_REQ";
    case BLE_GAP_EVENT_ADV_COMPLETE:
      return "ADV_COMPLETE";
    case BLE_GAP_EVENT_TERM_FAILURE:
      return "TERM_FAILURE";
    case BLE_GAP_EVENT_ENC_CHANGE:
      return "ENC_CHANGE";
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      return "PASSKEY_ACTION";
    case BLE_GAP_EVENT_SUBSCRIBE:
      return "SUBSCRIBE";
    case BLE_GAP_EVENT_MTU:
      return "MTU";
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
      return "IDENTITY_RESOLVED";
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      return "REPEAT_PAIRING";
    case BLE_GAP_EVENT_NOTIFY_TX:
      return "NOTIFY_TX";
    default:
      return "OTHER";
  }
}

void format_addr(const ble_addr_t& addr, char* out, std::size_t len) {
  std::snprintf(out,
                len,
                "%02X:%02X:%02X:%02X:%02X:%02X type=%u",
                addr.val[5],
                addr.val[4],
                addr.val[3],
                addr.val[2],
                addr.val[1],
                addr.val[0],
                static_cast<unsigned>(addr.type));
}

void format_addr_le(const std::array<std::uint8_t, 6>& addr_le, char* out, std::size_t len) {
  std::snprintf(out,
                len,
                "%02X:%02X:%02X:%02X:%02X:%02X",
                addr_le[5],
                addr_le[4],
                addr_le[3],
                addr_le[2],
                addr_le[1],
                addr_le[0]);
}

void derive_static_random_addr_le(const std::array<std::uint8_t, 6>& mac,
                                  std::array<std::uint8_t, 6>* out) {
  for (std::size_t i = 0; i < out->size(); ++i) {
    (*out)[i] = mac[mac.size() - 1 - i];
  }

  // Static random BLE addresses use the two most significant address bits set to 1.
  (*out)[5] = ((*out)[5] & 0x3F) | 0xC0;
}

bool bonded_peer_for_reconnect(ble_addr_t* out, int* out_count) {
  if (out == nullptr || out_count == nullptr) {
    return false;
  }

  std::array<ble_addr_t, kMaxReconnectPeers> peers = {};
  int peer_count = 0;
  const int rc =
      ble_store_util_bonded_peers(peers.data(), &peer_count, static_cast<int>(peers.size()));
  if (rc != 0) {
    ESP_LOGW(kTag, "BLE HID bonded peer lookup failed: %d", rc);
    *out_count = 0;
    return false;
  }

  *out_count = peer_count;
  if (peer_count <= 0) {
    return false;
  }

  *out = peers[0];
  return true;
}

void log_connection_desc(const char* phase, std::uint16_t conn_handle) {
  ble_gap_conn_desc desc = {};
  const int rc = ble_gap_conn_find(conn_handle, &desc);
  if (rc != 0) {
    ESP_LOGW(kTag, "GAP %s conn_handle=%u desc_unavailable rc=%d",
             phase,
             static_cast<unsigned>(conn_handle),
             rc);
    return;
  }

  char peer[32] = {};
  char local[32] = {};
  format_addr(desc.peer_ota_addr, peer, sizeof(peer));
  format_addr(desc.our_ota_addr, local, sizeof(local));
  ESP_LOGI(kTag,
           "GAP %s conn_handle=%u peer=%s local=%s interval=%u latency=%u timeout=%u encrypted=%u authenticated=%u bonded=%u key_size=%u",
           phase,
           static_cast<unsigned>(conn_handle),
           peer,
           local,
           static_cast<unsigned>(desc.conn_itvl),
           static_cast<unsigned>(desc.conn_latency),
           static_cast<unsigned>(desc.supervision_timeout),
           static_cast<unsigned>(desc.sec_state.encrypted),
           static_cast<unsigned>(desc.sec_state.authenticated),
           static_cast<unsigned>(desc.sec_state.bonded),
           static_cast<unsigned>(desc.sec_state.key_size));
}

}  // namespace

esp_err_t BleHidTransport::begin() {
  if (initialized_) {
    return ESP_OK;
  }
  if (s_transport != nullptr && s_transport != this) {
    return ESP_ERR_INVALID_STATE;
  }
  s_transport = this;

  std::uint8_t stored_gatt_schema_revision = 0;
  esp_err_t gatt_schema_err = ESP_OK;
  NvsConfigStore nvs_store;
  if (!nvs_store.load_gatt_schema_revision(&stored_gatt_schema_revision,
                                           &gatt_schema_err) ||
      stored_gatt_schema_revision != kGattSchemaRevision) {
    gatt_schema_change_pending_ = true;
    ESP_LOGI(kTag,
             "GATT schema migration pending stored=%u current=%u load=%s",
             static_cast<unsigned>(stored_gatt_schema_revision),
             static_cast<unsigned>(kGattSchemaRevision),
             esp_err_to_name(gatt_schema_err));
  }

  esp_err_t err = init_low_level();
  if (err != ESP_OK) {
    return err;
  }

  esp_hid_raw_report_map_t report_map = {};
  report_map.data = kReportMap;
  report_map.len = sizeof(kReportMap);

  esp_hid_device_config_t hid_config = {};
  hid_config.vendor_id = 0x303A;
  hid_config.product_id = 0x1006;
  hid_config.version = kBleHidVersion;
  hid_config.device_name = kBleDeviceName;
  hid_config.manufacturer_name = "AIOTWAN";
  hid_config.serial_number = "";
  hid_config.report_maps = &report_map;
  hid_config.report_maps_len = 1;

  err = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &hid_dev_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_hidd_dev_init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = register_config_service();
  if (err != ESP_OK) {
    return err;
  }

  int rc = ble_svc_gap_device_name_set(kBleDeviceName);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_svc_gap_device_name_set failed: %d", rc);
    return ESP_FAIL;
  }
  rc = ble_svc_gap_device_appearance_set(kAppearanceHidGeneric);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_svc_gap_device_appearance_set failed: %d", rc);
    return ESP_FAIL;
  }

  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_store_config_init();

  err = prepare_identity_address();
  if (err != ESP_OK) {
    return err;
  }

  nimble_port_freertos_init(host_task);

  initialized_ = true;
  ESP_LOGI(kTag,
           "BLE HID init done name='%s' short='%s' addr=%s mtu=128 "
           "config_service=7d2f4d10-6f6b-4a2d-8b01-6d4653320001",
           kBleDeviceName,
           kBleShortName,
           identity_address_text());
  return ESP_OK;
}

bool BleHidTransport::connected() const {
  return !owner_recovery_pending_.load(std::memory_order_acquire) &&
         hid_dev_ != nullptr && esp_hidd_dev_connected(hid_dev_);
}

void BleHidTransport::refresh_connection_identity() {
  if (owner_recovery_pending_.load(std::memory_order_acquire)) {
    return;
  }
  if (connected()) {
    sync_hid_owner("main_poll");
    return;
  }

  bool cached_owner_active = false;
  portENTER_CRITICAL(&connection_power_mux_);
  cached_owner_active =
      active_conn_handle_ != kInvalidConnHandle ||
      active_owner_generation_ != 0;
  portEXIT_CRITICAL(&connection_power_mux_);
  if (cached_owner_active) {
    ESP_LOGW(kTag,
             "BLE HID owner vanished without matching GAP disconnect; "
             "invalidating cached connection lifetime");
    reset_connection_power_state(false);
    directed_reconnect_active_.store(false, std::memory_order_release);
    directed_reconnect_attempted_.store(false, std::memory_order_release);
    slow_advertising_.store(false, std::memory_order_release);
    request_advertising_reconcile();
  }
}

std::uint32_t BleHidTransport::connection_epoch() const {
  if (owner_recovery_pending_.load(std::memory_order_acquire)) {
    return 0;
  }
  std::uint32_t generation = 0;
  portENTER_CRITICAL(&connection_power_mux_);
  if (active_conn_handle_ != kInvalidConnHandle) {
    generation = active_owner_generation_;
  }
  portEXIT_CRITICAL(&connection_power_mux_);
  return generation;
}

ai_keyboard::BleOwnerToken BleHidTransport::connection_identity() const {
  if (owner_recovery_pending_.load(std::memory_order_acquire) ||
      hid_dev_ == nullptr) {
    return {};
  }
  easy_input_hidd_owner_snapshot_t owner_snapshot{};
  owner_snapshot.conn_handle = EASY_INPUT_HIDD_OWNER_NONE;
  if (easy_input_hidd_owner_snapshot_get(hid_dev_, &owner_snapshot) != ESP_OK ||
      owner_snapshot.conn_handle == EASY_INPUT_HIDD_OWNER_NONE ||
      owner_snapshot.generation == 0) {
    return {};
  }
  return {
      owner_snapshot.conn_handle,
      owner_snapshot.generation,
  };
}

bool BleHidTransport::sync_hid_owner(const char* reason) {
  if (owner_recovery_pending_.load(std::memory_order_acquire) ||
      hid_dev_ == nullptr) {
    return false;
  }

  easy_input_hidd_owner_snapshot_t owner_snapshot{};
  owner_snapshot.conn_handle = EASY_INPUT_HIDD_OWNER_NONE;
  const esp_err_t owner_err =
      easy_input_hidd_owner_snapshot_get(hid_dev_, &owner_snapshot);
  if (owner_err != ESP_OK ||
      owner_snapshot.conn_handle == EASY_INPUT_HIDD_OWNER_NONE ||
      owner_snapshot.generation == 0) {
    return false;
  }
  const std::uint16_t owner = owner_snapshot.conn_handle;
  const std::uint32_t owner_generation = owner_snapshot.generation;

  bool changed = false;
  std::uint16_t previous_owner = kInvalidConnHandle;
  std::uint32_t previous_generation = 0;
  portENTER_CRITICAL(&connection_power_mux_);
  previous_owner = active_conn_handle_;
  previous_generation = active_owner_generation_;
  if (active_conn_handle_ != owner ||
      active_owner_generation_ != owner_generation) {
    active_conn_handle_ = owner;
    active_owner_generation_ = owner_generation;
    if (control_conn_handle_ == owner) {
      control_conn_handle_ = kInvalidConnHandle;
    }
    requested_connection_profile_ = ConnectionPowerProfile::Unknown;
    connection_update_in_flight_ = false;
    connection_update_retry_after_us_ = 0;
    connection_update_retry_attempt_ = 0;
    preferred_connection_profile_ = ConnectionPowerProfile::Active;
    actual_connection_params_valid_ = false;
    changed = true;
  }
  portEXIT_CRITICAL(&connection_power_mux_);

  if (!changed) {
    return true;
  }

  // The adapter generation is the authoritative endpoint lifetime. Clear
  // reports from the previous host before accepting any report for this
  // owner, including reconnects that reuse the same numeric connection handle.
  input_report_reset_requested_.store(true, std::memory_order_release);
  ESP_LOGI(kTag,
           "BLE HID owner selected conn_handle=%u generation=%lu "
           "previous=%u/%lu reason=%s",
           static_cast<unsigned>(owner),
           static_cast<unsigned long>(owner_generation),
           previous_owner == kInvalidConnHandle
               ? 0U
               : static_cast<unsigned>(previous_owner),
           static_cast<unsigned long>(previous_generation),
           reason == nullptr ? "" : reason);
  // CONNECT is delivered through a bounded best-effort event loop. Make owner
  // polling authoritative for advertising policy as well, so a dropped event
  // cannot leave HID+control advertising active after the owner is selected.
  directed_reconnect_active_.store(false, std::memory_order_release);
  directed_reconnect_attempted_.store(false, std::memory_order_release);
  slow_advertising_.store(true, std::memory_order_release);
  request_advertising_reconcile();
  cache_connection_status(owner, 0);
  request_connection_parameters(ConnectionPowerProfile::Active, "hid_owner");
  return true;
}

void BleHidTransport::open_config_window(const char* reason) {
  const bool was_active = connected_config_window_active();
  portENTER_CRITICAL(&connection_power_mux_);
  connected_config_advertising_deadline_us_ =
      esp_timer_get_time() + kConnectedConfigAdvertisingWindowUs;
  connected_config_advertising_enabled_ = true;
  portEXIT_CRITICAL(&connection_power_mux_);

  const bool advertising_active = ble_gap_adv_active();
  if (!was_active || !advertising_active) {
    ESP_LOGI(kTag,
             "CONFIG advertising window open reason=%s connected=%u duration_ms=%ld",
             reason == nullptr ? "" : reason,
             connected() ? 1U : 0U,
             static_cast<long>(kConnectedConfigAdvertisingWindowUs / 1000));
  }

  // Extending an already active config window changes its absolute deadline,
  // so the existing finite advertising procedure must be restarted. Keep all
  // GAP stop/start calls on the application task through the reconciler.
  request_advertising_reconcile(true);
  service_advertising_reconcile();
}

bool BleHidTransport::config_window_active() {
  return connected_config_window_active();
}

void BleHidTransport::set_connection_power_profile(ConnectionPowerProfile profile) {
  if (profile == ConnectionPowerProfile::Unknown) {
    profile = ConnectionPowerProfile::Active;
  }

  portENTER_CRITICAL(&connection_power_mux_);
  if (preferred_connection_profile_ != profile) {
    connection_update_retry_after_us_ = 0;
    connection_update_retry_attempt_ = 0;
  }
  preferred_connection_profile_ = profile;
  const bool has_connection = active_conn_handle_ != kInvalidConnHandle;
  portEXIT_CRITICAL(&connection_power_mux_);

  if (!connected() || !has_connection) {
    return;
  }
  reconcile_connection_power_profile("app_power_mode");
}

void BleHidTransport::prepare_for_input_delivery() {
  set_connection_power_profile(ConnectionPowerProfile::Active);
}

bool BleHidTransport::connection_profile_matches_actual_locked(
    ConnectionPowerProfile profile) const {
  if (!actual_connection_params_valid_) {
    return false;
  }
  const ai_keyboard::BleConnectionParameters actual{
      actual_conn_interval_,
      actual_conn_latency_,
      actual_conn_supervision_timeout_,
  };
  switch (profile) {
    case ConnectionPowerProfile::Active:
      return ai_keyboard::ble_connection_parameters_match(
          actual,
          {
              0,
              kActiveConnIntervalMax,
              kActiveConnLatency,
              kSafeConnSupervisionTimeout,
          });
    case ConnectionPowerProfile::Idle:
      return ai_keyboard::ble_connection_parameters_match(
          actual,
          {
              kIdleConnIntervalMin,
              kIdleConnIntervalMax,
              kIdleConnLatency,
              kSafeConnSupervisionTimeout,
          });
    case ConnectionPowerProfile::DeepIdle:
      return ai_keyboard::ble_connection_parameters_match(
          actual,
          {
              kDeepIdleConnIntervalMin,
              std::numeric_limits<std::uint16_t>::max(),
              kDeepIdleConnLatency,
              kSafeConnSupervisionTimeout,
          });
    case ConnectionPowerProfile::Unknown:
      return false;
  }
  return false;
}

void BleHidTransport::schedule_connection_update_retry_locked(
    std::int64_t now_us) {
  const auto retry_delay_us =
      ai_keyboard::ble_connection_update_retry_delay_us(
          connection_update_retry_attempt_);
  connection_update_retry_after_us_ = now_us + retry_delay_us;
  if (connection_update_retry_attempt_ < 4) {
    ++connection_update_retry_attempt_;
  }
}

bool BleHidTransport::input_delivery_pending() const {
  return pending_input_report_count_.load(std::memory_order_relaxed) != 0 ||
         pending_wheel_report_count_.load(std::memory_order_relaxed) != 0 ||
         fixed_text_stream_.pending();
}

void BleHidTransport::clear_pending_input_reports(const char* reason) {
  const auto input_count = pending_input_reports_.size();
  const auto wheel_count = pending_wheel_reports_.size();
  const auto fixed_text_remaining = fixed_text_stream_.remaining_bytes();
  pending_input_reports_.clear();
  pending_wheel_reports_.clear();
  fixed_text_stream_.reset();
  pending_input_report_count_.store(0, std::memory_order_relaxed);
  pending_wheel_report_count_.store(0, std::memory_order_relaxed);
  dropped_input_report_count_.fetch_add(
      static_cast<std::uint32_t>(input_count), std::memory_order_relaxed);
  dropped_wheel_report_count_.fetch_add(
      static_cast<std::uint32_t>(wheel_count), std::memory_order_relaxed);
  input_scheduler_.mark_disconnected();
  if (input_count > 0 || wheel_count > 0 || fixed_text_remaining > 0) {
    ESP_LOGW(kTag,
             "HID delivery cleared reason=%s reports=%u wheel_runs=%u "
             "fixed_text_remaining=%u",
             reason == nullptr ? "" : reason,
             static_cast<unsigned>(input_count),
             static_cast<unsigned>(wheel_count),
             static_cast<unsigned>(fixed_text_remaining));
  }
}

void BleHidTransport::pump_fixed_text_stream(std::uint32_t now_ms) {
  if (!fixed_text_stream_.pending()) {
    return;
  }

  const auto owner = connection_identity();
  const auto result =
      fixed_text_stream_.pump(owner, &pending_input_reports_, now_ms);
  if (result.queued_chunks > 0) {
    queued_input_report_count_.fetch_add(
        static_cast<std::uint32_t>(result.queued_chunks),
        std::memory_order_relaxed);
  }
  pending_input_report_count_.store(
      static_cast<std::uint32_t>(pending_input_reports_.size()),
      std::memory_order_relaxed);

  if (result.owner_changed) {
    ESP_LOGW(kTag,
             "APP_COMMAND fixed_text canceled after owner lifetime changed");
  } else if (result.blocked) {
    ESP_LOGD(kTag,
             "APP_COMMAND fixed_text waiting for queue capacity next=%u/%u "
             "pending=%u",
             static_cast<unsigned>(fixed_text_stream_.next_chunk()),
             static_cast<unsigned>(fixed_text_stream_.total_chunks()),
             static_cast<unsigned>(pending_input_reports_.size()));
  } else if (result.completed) {
    ESP_LOGI(kTag, "APP_COMMAND fixed_text queued complete");
  }
}

void BleHidTransport::apply_deferred_input_reset(const char* reason) {
  if (input_report_reset_requested_.exchange(false, std::memory_order_acq_rel)) {
    clear_pending_input_reports(reason);
  }
}

void BleHidTransport::reset_connection_power_state(bool keep_preferred_idle) {
  portENTER_CRITICAL(&connection_power_mux_);
  active_conn_handle_ = kInvalidConnHandle;
  active_owner_generation_ = 0;
  requested_connection_profile_ = ConnectionPowerProfile::Unknown;
  connection_update_in_flight_ = false;
  connection_update_retry_after_us_ = 0;
  connection_update_retry_attempt_ = 0;
  actual_connection_params_valid_ = false;
  actual_conn_interval_ = 0;
  actual_conn_latency_ = 0;
  actual_conn_supervision_timeout_ = 0;
  last_conn_update_status_ = 0;
  connected_config_advertising_enabled_ = false;
  connected_config_advertising_deadline_us_ = 0;
  if (!keep_preferred_idle) {
    preferred_connection_profile_ = ConnectionPowerProfile::Active;
  }
  portEXIT_CRITICAL(&connection_power_mux_);
  // This method runs from NimBLE callbacks. Queue mutation is deferred to the
  // main task so the host task can never deadlock against an HID submission.
  input_report_reset_requested_.store(true, std::memory_order_release);
}

const char* BleHidTransport::connection_profile_name(ConnectionPowerProfile profile) const {
  switch (profile) {
    case ConnectionPowerProfile::Active:
      return "active";
    case ConnectionPowerProfile::Idle:
      return "idle";
    case ConnectionPowerProfile::DeepIdle:
      return "deep_idle";
    case ConnectionPowerProfile::Unknown:
      return "unknown";
  }
  return "unknown";
}

bool BleHidTransport::connected_config_window_active() {
  bool active = false;
  const auto now_us = esp_timer_get_time();
  portENTER_CRITICAL(&connection_power_mux_);
  if (connected_config_advertising_enabled_ &&
      now_us < connected_config_advertising_deadline_us_) {
    active = true;
  } else if (connected_config_advertising_enabled_) {
    connected_config_advertising_enabled_ = false;
    connected_config_advertising_deadline_us_ = 0;
  }
  portEXIT_CRITICAL(&connection_power_mux_);
  return active;
}

std::int32_t BleHidTransport::connected_config_window_remaining_ms() const {
  std::int64_t deadline_us = 0;
  bool enabled = false;
  portENTER_CRITICAL(&connection_power_mux_);
  enabled = connected_config_advertising_enabled_;
  deadline_us = connected_config_advertising_deadline_us_;
  portEXIT_CRITICAL(&connection_power_mux_);
  if (!enabled) {
    return 0;
  }
  const std::int64_t remaining_us = deadline_us - esp_timer_get_time();
  if (remaining_us <= 0) {
    return 0;
  }
  return static_cast<std::int32_t>((remaining_us + 999) / 1000);
}

void BleHidTransport::cache_connection_status(std::uint16_t conn_handle,
                                              std::int32_t update_status) {
  if (conn_handle == kInvalidConnHandle) {
    portENTER_CRITICAL(&connection_power_mux_);
    last_conn_update_status_ = update_status;
    actual_connection_params_valid_ = false;
    portEXIT_CRITICAL(&connection_power_mux_);
    return;
  }

  ble_gap_conn_desc desc = {};
  const int rc = ble_gap_conn_find(conn_handle, &desc);
  if (rc != 0) {
    portENTER_CRITICAL(&connection_power_mux_);
    if (conn_handle == active_conn_handle_) {
      last_conn_update_status_ = update_status;
      actual_connection_params_valid_ = false;
    }
    portEXIT_CRITICAL(&connection_power_mux_);
    ESP_LOGW(kTag,
             "GAP conn_params cache failed conn_handle=%u rc=%d",
             static_cast<unsigned>(conn_handle),
             rc);
    return;
  }

  portENTER_CRITICAL(&connection_power_mux_);
  if (conn_handle == active_conn_handle_) {
    last_conn_update_status_ = update_status;
    actual_connection_params_valid_ = true;
    actual_conn_interval_ = desc.conn_itvl;
    actual_conn_latency_ = desc.conn_latency;
    actual_conn_supervision_timeout_ = desc.supervision_timeout;
  }
  portEXIT_CRITICAL(&connection_power_mux_);
}

void BleHidTransport::request_connection_parameters(ConnectionPowerProfile profile,
                                                    const char* reason) {
  ble_gap_upd_params params = {};
  const char* profile_name = connection_profile_name(profile);
  switch (profile) {
    case ConnectionPowerProfile::Active:
      params.itvl_min = kActiveConnIntervalMin;
      params.itvl_max = kActiveConnIntervalMax;
      params.latency = kActiveConnLatency;
      params.supervision_timeout = kActiveConnSupervisionTimeout;
      break;
    case ConnectionPowerProfile::Idle:
      params.itvl_min = kIdleConnIntervalMin;
      params.itvl_max = kIdleConnIntervalMax;
      params.latency = kIdleConnLatency;
      params.supervision_timeout = kIdleConnSupervisionTimeout;
      break;
    case ConnectionPowerProfile::DeepIdle:
      params.itvl_min = kDeepIdleConnIntervalMin;
      params.itvl_max = kDeepIdleConnIntervalMax;
      params.latency = kDeepIdleConnLatency;
      params.supervision_timeout = kDeepIdleConnSupervisionTimeout;
      break;
    case ConnectionPowerProfile::Unknown:
      return;
  }

  params.min_ce_len = 0;
  params.max_ce_len = 0;

  const auto now_us = esp_timer_get_time();
  std::uint16_t conn_handle = kInvalidConnHandle;
  portENTER_CRITICAL(&connection_power_mux_);
  if (active_conn_handle_ == kInvalidConnHandle || connection_update_in_flight_ ||
      now_us < connection_update_retry_after_us_ ||
      (requested_connection_profile_ == profile &&
       connection_profile_matches_actual_locked(profile))) {
    portEXIT_CRITICAL(&connection_power_mux_);
    return;
  }
  conn_handle = active_conn_handle_;
  requested_connection_profile_ = profile;
  connection_update_in_flight_ = true;
  connection_update_retry_after_us_ = 0;
  portEXIT_CRITICAL(&connection_power_mux_);

  const int rc = ble_gap_update_params(conn_handle, &params);
  bool desired_changed_while_requesting = false;
  portENTER_CRITICAL(&connection_power_mux_);
  if (conn_handle == active_conn_handle_) {
    last_conn_update_status_ = rc;
  }
  if (rc != 0) {
    if (conn_handle == active_conn_handle_ &&
        requested_connection_profile_ == profile) {
      desired_changed_while_requesting =
          preferred_connection_profile_ != profile;
      requested_connection_profile_ = ConnectionPowerProfile::Unknown;
      connection_update_in_flight_ = false;
      if (desired_changed_while_requesting) {
        connection_update_retry_after_us_ = 0;
        connection_update_retry_attempt_ = 0;
      } else {
        schedule_connection_update_retry_locked(now_us);
      }
    }
    portEXIT_CRITICAL(&connection_power_mux_);
    ESP_LOGW(kTag,
             "GAP conn_params request profile=%s reason=%s conn_handle=%u rc=%d interval=%u-%u latency=%u timeout=%u",
             profile_name,
             reason == nullptr ? "" : reason,
             static_cast<unsigned>(conn_handle),
             rc,
             static_cast<unsigned>(params.itvl_min),
             static_cast<unsigned>(params.itvl_max),
             static_cast<unsigned>(params.latency),
             static_cast<unsigned>(params.supervision_timeout));
    if (desired_changed_while_requesting) {
      reconcile_connection_power_profile("profile_changed_during_request");
    }
    return;
  }
  portEXIT_CRITICAL(&connection_power_mux_);

  ESP_LOGI(kTag,
           "GAP conn_params request profile=%s reason=%s conn_handle=%u interval=%u-%u latency=%u timeout=%u",
           profile_name,
           reason == nullptr ? "" : reason,
           static_cast<unsigned>(conn_handle),
           static_cast<unsigned>(params.itvl_min),
           static_cast<unsigned>(params.itvl_max),
           static_cast<unsigned>(params.latency),
           static_cast<unsigned>(params.supervision_timeout));
}

void BleHidTransport::reconcile_connection_power_profile(const char* reason) {
  const auto now_us = esp_timer_get_time();
  ConnectionPowerProfile desired = ConnectionPowerProfile::Unknown;
  bool should_request = false;
  portENTER_CRITICAL(&connection_power_mux_);
  desired = preferred_connection_profile_;
  should_request = active_conn_handle_ != kInvalidConnHandle &&
                   !connection_update_in_flight_ &&
                   now_us >= connection_update_retry_after_us_ &&
                   (requested_connection_profile_ != desired ||
                    !connection_profile_matches_actual_locked(desired));
  portEXIT_CRITICAL(&connection_power_mux_);
  if (should_request) {
    request_connection_parameters(desired, reason);
  }
}

void BleHidTransport::update_battery_level(std::uint8_t percent) {
  if (hid_dev_ == nullptr) {
    return;
  }
  percent = std::min<std::uint8_t>(percent, 100);
  if (battery_level_ == percent) {
    return;
  }

  const esp_err_t err = esp_hidd_dev_battery_set(hid_dev_, percent);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "battery level update failed: %s", esp_err_to_name(err));
    return;
  }
  battery_level_ = percent;
  ESP_LOGI(kTag, "battery level updated %u%%", static_cast<unsigned>(percent));
}

void BleHidTransport::set_status_read_callback(StatusReadCallback callback, void* context) {
  status_read_callback_ = callback;
  status_read_context_ = context;
}

bool BleHidTransport::take_pending_config(
    std::string* out,
    ai_keyboard::BleOwnerToken* origin_owner) {
  if (out == nullptr || origin_owner == nullptr) {
    return false;
  }

  std::string completed_json;
  ai_keyboard::BleOwnerToken completed_owner{};
  bool ready = false;

  portENTER_CRITICAL(&pending_config_mux_);
  if (pending_config_ready_) {
    completed_json.swap(pending_config_json_);
    completed_owner = pending_config_owner_;
    pending_config_ready_ = false;
    pending_config_owner_ = {};
    ready = true;
  }
  portEXIT_CRITICAL(&pending_config_mux_);

  if (!ready || completed_json.empty()) {
    return false;
  }
  out->swap(completed_json);
  *origin_owner = completed_owner;
  return true;
}

bool BleHidTransport::take_pending_agent_status(ai_keyboard::AgentStatusCommand* out) {
  if (out == nullptr) {
    return false;
  }

  bool ready = false;
  portENTER_CRITICAL(&pending_agent_status_mux_);
  if (pending_agent_status_ready_) {
    *out = pending_agent_status_;
    pending_agent_status_ready_ = false;
    ready = true;
  }
  portEXIT_CRITICAL(&pending_agent_status_mux_);
  return ready;
}

void BleHidTransport::publish_status_json(const std::string& status_json) {
  const bool fits_safe_gatt_read =
      status_json.size() <= ai_keyboard::kConfigStatusGattSafeLen;
  const std::string bounded_status =
      fits_safe_gatt_read ? status_json : kConfigStatusFallbackJson;
  auto wire_status = status_json_for_publish(bounded_status);
  if (wire_status.size() > ai_keyboard::kConfigStatusGattSafeLen) {
    wire_status = kConfigStatusFallbackJson;
  }

  bool published = false;
  portENTER_CRITICAL(&status_mux_);
  published = status_read_cache_.publish(wire_status.data(), wire_status.size());
  portEXIT_CRITICAL(&status_mux_);
  if (!published) {
    ESP_LOGE(kTag,
             "CONFIG status cache rejected bounded value bytes=%u",
             static_cast<unsigned>(wire_status.size()));
    return;
  }

  if (s_config_status_handle != 0) {
    ble_gatts_chr_updated(s_config_status_handle);
  }
  if (!fits_safe_gatt_read) {
    ESP_LOGW(kTag,
             "CONFIG status too large for safe GATT read: %u bytes, published fallback",
             static_cast<unsigned>(status_json.size()));
  }
  ESP_LOGD(kTag,
           "CONFIG status published source_bytes=%u wire_bytes=%u",
           static_cast<unsigned>(status_json.size()),
           static_cast<unsigned>(wire_status.size()));
}

bool BleHidTransport::send_firmware_event(const char* source,
                                          const ai_keyboard::FirmwareEvent& event) {
  return send_firmware_event_for_owner(source, event, {});
}

bool BleHidTransport::send_firmware_event_for_owner(
    const char* source,
    const ai_keyboard::FirmwareEvent& event,
    ai_keyboard::BleOwnerToken expected_owner) {
  switch (event.kind) {
    case ai_keyboard::FirmwareEventKind::None:
      return true;
    case ai_keyboard::FirmwareEventKind::HidKeyDown:
      ESP_LOGI(kTag, "ACTION %s hid_down %s", source, event.value.c_str());
      if (event.bridge_app_hotkey && should_bridge_hotkey_to_app_command(event.value)) {
        return send_hotkey_app_command(event.value, true, expected_owner);
      }
      return send_hotkey_report(event.value, true);
    case ai_keyboard::FirmwareEventKind::HidKeyUp:
      ESP_LOGI(kTag, "ACTION %s hid_up %s", source, event.value.c_str());
      if (event.bridge_app_hotkey && should_bridge_hotkey_to_app_command(event.value)) {
        return send_hotkey_app_command(event.value, false, expected_owner);
      }
      return send_hotkey_report(event.value, false);
    case ai_keyboard::FirmwareEventKind::HidTap:
      ESP_LOGI(kTag, "ACTION %s hid_tap %s", source, event.value.c_str());
      if (event.bridge_app_hotkey && should_bridge_hotkey_to_app_command(event.value)) {
        if (!send_hotkey_app_command(event.value, true, expected_owner)) {
          return false;
        }
        vTaskDelay(delay_ticks(15));
        return send_hotkey_app_command(event.value, false, expected_owner);
      }
      return tap_hotkey(event.value);
    case ai_keyboard::FirmwareEventKind::FixedText: {
      const auto chunks =
          ai_keyboard::fixed_text_chunk_count(event.value.size());
      ESP_LOGI(kTag,
               "ACTION %s fixed_text bytes=%u chunks=%u",
               source,
               static_cast<unsigned>(event.value.size()),
               static_cast<unsigned>(chunks));
      return send_fixed_text_command(event.value, expected_owner);
    }
    case ai_keyboard::FirmwareEventKind::AppCommand:
      ESP_LOGI(kTag, "ACTION %s app_command %s", source, event.value.c_str());
      return false;
  }
  return false;
}

bool BleHidTransport::send_keyboard_report(std::uint8_t modifier, std::uint8_t keycode) {
  std::array<std::uint8_t, 6> keycodes{};
  keycodes[0] = keycode;
  return send_keyboard_report(modifier, keycodes);
}

bool BleHidTransport::send_keyboard_report(std::uint8_t modifier,
                                           const std::array<std::uint8_t, 6>& keycodes,
                                           bool apple_fn) {
  const bool all_released =
      modifier == 0 && !apple_fn &&
      std::all_of(keycodes.begin(), keycodes.end(), [](std::uint8_t keycode) {
        return keycode == 0;
      });
  return send_keyboard_report(
      modifier,
      keycodes,
      apple_fn,
      all_released ? ai_keyboard::HidReportClass::KeyboardAllReleased
                   : ai_keyboard::HidReportClass::KeyboardPress);
}

bool BleHidTransport::send_keyboard_report(
    std::uint8_t modifier,
    const std::array<std::uint8_t, 6>& keycodes,
    bool apple_fn,
    ai_keyboard::HidReportClass report_class) {
  return send_keyboard_report_for_owner(
      modifier, keycodes, apple_fn, report_class, {});
}

bool BleHidTransport::send_keyboard_report_for_owner(
    std::uint8_t modifier,
    const std::array<std::uint8_t, 6>& keycodes,
    bool apple_fn,
    ai_keyboard::HidReportClass report_class,
    ai_keyboard::BleOwnerToken expected_owner) {
  std::array<std::uint8_t, 8> report{};
  report[0] = modifier;
  report[1] = apple_fn ? 0x01 : 0x00;
  std::copy(keycodes.begin(), keycodes.end(), report.begin() + 2);

  return send_input_report(kReportIdKeyboard,
                           report.data(),
                           report.size(),
                           "keyboard",
                           report_class,
                           expected_owner);
}

bool BleHidTransport::send_input_report(std::uint8_t report_id,
                                        const std::uint8_t* data,
                                        std::size_t len,
                                        const char* context,
                                        ai_keyboard::HidReportClass report_class,
                                        ai_keyboard::BleOwnerToken expected_owner) {
  // A disconnect callback may have requested a queue reset while no input was
  // pending. Consume it before accepting the first report on a new HID owner;
  // otherwise poll_input_delivery() would immediately clear that fresh report.
  apply_deferred_input_reset("connection_reset_before_enqueue");
  refresh_connection_identity();
  apply_deferred_input_reset("identity_refresh_before_enqueue");
  const auto current_owner = connection_identity();
  if (!current_owner.valid() ||
      (expected_owner.valid() && current_owner != expected_owner)) {
    return false;
  }
  if (!expected_owner.valid()) {
    expected_owner = current_owner;
  }

  prepare_for_input_delivery();
  const auto now_ms = monotonic_ms();
  std::size_t pending_count = 0;
  const auto result = pending_input_reports_.push_classified(
      report_id, data, len, now_ms, report_class, expected_owner);
  if (result.status == ai_keyboard::HidQueuePushStatus::Queued) {
    queued_input_report_count_.fetch_add(1, std::memory_order_relaxed);
  }
  pending_count = pending_input_reports_.size();
  pending_input_report_count_.store(
      static_cast<std::uint32_t>(pending_count), std::memory_order_relaxed);
  if (!result.accepted()) {
    dropped_input_report_count_.fetch_add(1, std::memory_order_relaxed);
    if (result.status == ai_keyboard::HidQueuePushStatus::Invalid) {
      ESP_LOGE(kTag,
               "%s report invalid report_id=%u len=%u",
               context == nullptr ? "HID" : context,
               static_cast<unsigned>(report_id),
               static_cast<unsigned>(len));
    } else {
      ESP_LOGD(kTag,
               "%s report queue full report_id=%u len=%u pending=%u",
               context == nullptr ? "HID" : context,
               static_cast<unsigned>(report_id),
               static_cast<unsigned>(len),
               static_cast<unsigned>(pending_count));
    }
    return false;
  }

  ESP_LOGD(kTag,
           "HID delivery queued seq=%lu report=%s class=%u pending=%u coalesced=%u",
           static_cast<unsigned long>(result.sequence),
           input_report_name(report_id),
           static_cast<unsigned>(report_class),
           static_cast<unsigned>(pending_count),
           result.status == ai_keyboard::HidQueuePushStatus::Coalesced ? 1U : 0U);
  poll_input_delivery(now_ms);
  return true;
}

ai_keyboard::BleInputTxResult BleHidTransport::transmit_scheduled_report_callback(
    void* context,
    const ai_keyboard::BleScheduledReport& report) {
  if (context == nullptr) {
    return ai_keyboard::BleInputTxResult::Fatal;
  }
  return static_cast<BleHidTransport*>(context)->transmit_scheduled_report(report);
}

ai_keyboard::BleInputTxResult BleHidTransport::transmit_scheduled_report(
    const ai_keyboard::BleScheduledReport& report) {
  portENTER_CRITICAL(&connection_power_mux_);
  const bool has_connection = active_conn_handle_ != kInvalidConnHandle;
  portEXIT_CRITICAL(&connection_power_mux_);
  if (!connected() || !has_connection || hid_dev_ == nullptr) {
    return ai_keyboard::BleInputTxResult::Disconnected;
  }

  std::array<std::uint8_t, 5> wheel_data{};
  std::uint8_t report_id = report.report_id;
  const std::uint8_t* data = report.data.data();
  std::size_t len = report.len;
  if (report.kind == ai_keyboard::BleScheduledReportKind::MouseWheel) {
    report_id = kReportIdMouse;
    wheel_data[3] = static_cast<std::uint8_t>(report.wheel_vertical);
    wheel_data[4] = static_cast<std::uint8_t>(report.wheel_horizontal);
    data = wheel_data.data();
    len = wheel_data.size();
  }

  if (!report.ble_owner.valid()) {
    return ai_keyboard::BleInputTxResult::Fatal;
  }
  const easy_input_hidd_owner_snapshot_t expected_owner{
      report.ble_owner.conn_handle,
      report.ble_owner.generation,
  };
  const esp_err_t err = easy_input_hidd_dev_input_set_for_owner(
      hid_dev_,
      &expected_owner,
      0,
      report_id,
      const_cast<std::uint8_t*>(data),
      len);
  if (err == ESP_OK) {
    return ai_keyboard::BleInputTxResult::Accepted;
  }
  if (err == ESP_ERR_NO_MEM) {
    return ai_keyboard::BleInputTxResult::RetryableNoBuffer;
  }
  if (err == ESP_ERR_NOT_FINISHED) {
    return ai_keyboard::BleInputTxResult::RetryableBusy;
  }
  if (!connected() || err == ESP_ERR_INVALID_STATE) {
    return ai_keyboard::BleInputTxResult::Disconnected;
  }
  const bool optional_report =
      report.report_class == ai_keyboard::HidReportClass::AppCommand ||
      report.kind == ai_keyboard::BleScheduledReportKind::MouseWheel;
  if (optional_report &&
      (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND)) {
    ESP_LOGW(kTag,
             "HID delivery unsupported report=%s class=%u len=%u err=%s",
             input_report_name(report_id),
             static_cast<unsigned>(report.report_class),
             static_cast<unsigned>(len),
             esp_err_to_name(err));
    return ai_keyboard::BleInputTxResult::DroppedUnsupported;
  }
  ESP_LOGE(kTag,
           "HID delivery permanent failure report=%s class=%u len=%u err=%s",
           input_report_name(report_id),
           static_cast<unsigned>(report.report_class),
           static_cast<unsigned>(len),
           esp_err_to_name(err));
  return ai_keyboard::BleInputTxResult::Fatal;
}

bool BleHidTransport::read_hidd_lifecycle(
    bool* connected_out,
    ai_keyboard::BleOwnerToken* owner_out,
    std::uint32_t* host_generation_out,
    bool* host_synced_out) const {
  if (connected_out == nullptr || owner_out == nullptr || hid_dev_ == nullptr) {
    return false;
  }
  *connected_out = false;
  *owner_out = {};
  if (host_generation_out != nullptr) {
    *host_generation_out = 0;
  }
  if (host_synced_out != nullptr) {
    *host_synced_out = false;
  }

  easy_input_hidd_lifecycle_snapshot_t snapshot{};
  snapshot.conn_handle = EASY_INPUT_HIDD_OWNER_NONE;
  const esp_err_t err =
      easy_input_hidd_lifecycle_snapshot_get(hid_dev_, &snapshot);
  if (err != ESP_OK) {
    return false;
  }
  *connected_out = snapshot.connected;
  if (host_generation_out != nullptr) {
    *host_generation_out = snapshot.host_generation;
  }
  if (host_synced_out != nullptr) {
    *host_synced_out = snapshot.host_synced;
  }
  if (snapshot.connected &&
      snapshot.conn_handle != EASY_INPUT_HIDD_OWNER_NONE &&
      snapshot.generation != 0) {
    *owner_out = {snapshot.conn_handle, snapshot.generation};
  }
  return true;
}

void BleHidTransport::complete_owner_recovery(const char* reason) {
  portENTER_CRITICAL(&connection_power_mux_);
  owner_recovery_.reset();
  portEXIT_CRITICAL(&connection_power_mux_);
  owner_recovery_pending_.store(false, std::memory_order_release);
  reset_connection_power_state(false);
  directed_reconnect_active_.store(false, std::memory_order_release);
  directed_reconnect_attempted_.store(false, std::memory_order_release);
  slow_advertising_.store(false, std::memory_order_release);
  request_advertising_reconcile();
  ESP_LOGW(kTag,
           "HID owner recovery completed reason=%s",
           reason == nullptr ? "" : reason);
}

void BleHidTransport::request_advertising_reconcile(bool force_restart) {
  if (force_restart) {
    advertising_force_restart_requested_.store(true,
                                                std::memory_order_release);
  }
  advertising_reconcile_requested_.store(true, std::memory_order_release);
}

ai_keyboard::BleAdvertisingMode
BleHidTransport::desired_advertising_mode() {
  const bool config_window_active = connected_config_window_active();
  const bool owner_connected = connected();

  portENTER_CRITICAL(&connection_power_mux_);
  const auto active_conn_handle = active_conn_handle_;
  const auto control_conn_handle = control_conn_handle_;
  portEXIT_CRITICAL(&connection_power_mux_);
  const bool separate_control_connected =
      control_conn_handle != kInvalidConnHandle &&
      control_conn_handle != active_conn_handle;

  if (config_window_active) {
    return owner_connected ? ai_keyboard::BleAdvertisingMode::ControlConfig
                           : ai_keyboard::BleAdvertisingMode::HidConfig;
  }
  if (owner_connected) {
    return separate_control_connected
               ? ai_keyboard::BleAdvertisingMode::Stopped
               : ai_keyboard::BleAdvertisingMode::ControlSlow;
  }
  if (directed_reconnect_active_.load(std::memory_order_acquire) ||
      !directed_reconnect_attempted_.load(std::memory_order_acquire)) {
    return ai_keyboard::BleAdvertisingMode::Directed;
  }
  return slow_advertising_.load(std::memory_order_acquire)
             ? ai_keyboard::BleAdvertisingMode::HidSlow
             : ai_keyboard::BleAdvertisingMode::HidFast;
}

void BleHidTransport::service_advertising_reconcile() {
  bool adapter_connected = false;
  ai_keyboard::BleOwnerToken adapter_owner{};
  std::uint32_t host_generation = 0;
  bool host_synced = false;
  if (!read_hidd_lifecycle(&adapter_connected,
                           &adapter_owner,
                           &host_generation,
                           &host_synced)) {
    return;
  }
  (void)adapter_owner;

  const auto host_observation =
      advertising_state_.observe_host(host_generation, host_synced);
  if (host_observation.generation_changed) {
    // A host reset invalidates all GAP procedure state even when reset+sync
    // happen between two application polls. Re-open a fresh reconnect cycle.
    directed_reconnect_attempted_.store(false, std::memory_order_release);
    directed_reconnect_active_.store(false, std::memory_order_release);
    slow_advertising_.store(false, std::memory_order_release);
    advertising_retry_after_us_ = 0;
    advertising_retry_attempt_ = 0;
    clear_status_read_snapshots();
    ESP_LOGW(kTag,
             "NimBLE host lifetime changed generation=%lu synced=%u",
             static_cast<unsigned long>(host_generation),
             host_synced ? 1U : 0U);
  }

  // Consume event hints, but never require one. The adapter generation and
  // GAP active level below make a dropped START/CONNECT event self-healing.
  (void)advertising_reconcile_requested_.exchange(
      false, std::memory_order_acq_rel);
  bool force_restart = advertising_force_restart_requested_.exchange(
      false, std::memory_order_acq_rel);

  if (!host_synced || !ble_hs_synced()) {
    // The sync callback advances the authoritative level. Do not exchange ->
    // re-store every 5 ms while the host is intentionally unavailable.
    if (force_restart) {
      advertising_force_restart_requested_.store(true,
                                                  std::memory_order_release);
    }
    return;
  }
  if (owner_recovery_pending_.load(std::memory_order_acquire)) {
    if (force_restart) {
      advertising_force_restart_requested_.store(true,
                                                  std::memory_order_release);
    }
    return;
  }

  const bool advertising_active = ble_gap_adv_active();
  const auto ended_mode = advertising_state_.current_mode();
  if (!advertising_active) {
    if (ended_mode == ai_keyboard::BleAdvertisingMode::Directed) {
      directed_reconnect_active_.store(false, std::memory_order_release);
    } else if (ended_mode == ai_keyboard::BleAdvertisingMode::HidFast &&
               !adapter_connected) {
      // A finite fast procedure can complete even if its best-effort
      // ADV_COMPLETE event is delayed or dropped.
      slow_advertising_.store(true, std::memory_order_release);
    }
  }

  auto desired = desired_advertising_mode();
  auto action = advertising_state_.next_action(
      advertising_active, desired, force_restart);
  if (action == ai_keyboard::BleAdvertisingState::Action::None) {
    advertising_retry_after_us_ = 0;
    advertising_retry_attempt_ = 0;
    return;
  }

  const auto now_us = esp_timer_get_time();
  if (now_us < advertising_retry_after_us_) {
    if (force_restart) {
      advertising_force_restart_requested_.store(true,
                                                  std::memory_order_release);
    }
    return;
  }

  const auto schedule_retry = [&](const char* operation, int error) {
    const auto shift = std::min<std::uint8_t>(advertising_retry_attempt_, 5);
    const auto delay_us = std::min<std::int64_t>(
        kAdvertisingRetryInitialUs << shift, kAdvertisingRetryMaxUs);
    advertising_retry_after_us_ = now_us + delay_us;
    if (advertising_retry_attempt_ < 6) {
      ++advertising_retry_attempt_;
    }
    request_advertising_reconcile(force_restart);
    ESP_LOGW(kTag,
             "BLE advertising reconcile retry operation=%s mode=%s err=%d delay_ms=%ld",
             operation,
             advertising_mode_name(desired),
             error,
             static_cast<long>(delay_us / 1000));
  };

  if (action == ai_keyboard::BleAdvertisingState::Action::Stop) {
    const auto stopped_mode = advertising_state_.current_mode();
    const int stop_rc = ble_gap_adv_stop();
    if (stop_rc != 0 && stop_rc != BLE_HS_EALREADY) {
      schedule_retry("stop", stop_rc);
      return;
    }
    if (stopped_mode == ai_keyboard::BleAdvertisingMode::Directed) {
      directed_reconnect_active_.store(false, std::memory_order_release);
    }
    advertising_state_.note_stopped();
    advertising_retry_after_us_ = 0;
    advertising_retry_attempt_ = 0;
    desired = desired_advertising_mode();
    action = advertising_state_.next_action(false, desired, false);
  }

  if (action != ai_keyboard::BleAdvertisingState::Action::Start) {
    return;
  }

  const esp_err_t identity_err = ensure_identity_set();
  if (identity_err != ESP_OK) {
    schedule_retry("identity", static_cast<int>(identity_err));
    return;
  }

  if (desired == ai_keyboard::BleAdvertisingMode::Directed) {
    if (start_directed_reconnect_advertising()) {
      advertising_state_.note_started(desired);
      advertising_retry_after_us_ = 0;
      advertising_retry_attempt_ = 0;
      return;
    }
    advertising_state_.note_stopped();
    if (ble_gap_adv_active()) {
      // EALREADY means an unknown procedure won the race. Never claim it has
      // our directed payload; the next level pass will stop and rebuild it.
      request_advertising_reconcile();
      return;
    }
    desired = desired_advertising_mode();
  }

  if (desired == ai_keyboard::BleAdvertisingMode::Stopped) {
    advertising_state_.note_stopped();
    return;
  }

  const esp_err_t start_err = start_advertising(desired);
  if (start_err != ESP_OK) {
    schedule_retry("start", static_cast<int>(start_err));
    return;
  }
  advertising_state_.note_started(desired);
  advertising_retry_after_us_ = 0;
  advertising_retry_attempt_ = 0;
}

void BleHidTransport::service_owner_recovery(std::uint64_t now_us) {
  if (!owner_recovery_pending_.load(std::memory_order_acquire)) {
    return;
  }

  bool adapter_connected = false;
  ai_keyboard::BleOwnerToken adapter_owner{};
  const bool snapshot_valid =
      read_hidd_lifecycle(&adapter_connected, &adapter_owner);

  ai_keyboard::BleOwnerRecoveryState::Action action;
  ai_keyboard::BleOwnerToken target{};
  portENTER_CRITICAL(&connection_power_mux_);
  action = owner_recovery_.observe(
      snapshot_valid, adapter_connected, adapter_owner, now_us);
  target = owner_recovery_.target();
  portEXIT_CRITICAL(&connection_power_mux_);

  if (action == ai_keyboard::BleOwnerRecoveryState::Action::Completed) {
    complete_owner_recovery("adapter_lifetime_changed");
    return;
  }
  if (action == ai_keyboard::BleOwnerRecoveryState::Action::None) {
    return;
  }

  if (action ==
      ai_keyboard::BleOwnerRecoveryState::Action::RequestHostReset) {
    request_advertising_reconcile();
    ESP_LOGE(kTag,
             "HID owner recovery scheduling NimBLE host reset owner=%u/%lu",
             static_cast<unsigned>(target.conn_handle),
             static_cast<unsigned long>(target.generation));
    ble_hs_sched_reset(BLE_HS_ECONTROLLER);
    portENTER_CRITICAL(&connection_power_mux_);
    owner_recovery_.note_host_reset_scheduled(now_us);
    portEXIT_CRITICAL(&connection_power_mux_);
    return;
  }

  const easy_input_hidd_owner_snapshot_t expected_owner{
      target.conn_handle,
      target.generation,
  };
  const esp_err_t terminate_err = easy_input_hidd_owner_terminate(
      hid_dev_, &expected_owner, BLE_ERR_REM_USER_CONN_TERM);
  auto result = ai_keyboard::BleOwnerRecoveryState::TerminateResult::Failed;
  if (terminate_err == ESP_OK) {
    result = ai_keyboard::BleOwnerRecoveryState::TerminateResult::Accepted;
  } else if (terminate_err == ESP_ERR_NOT_FINISHED ||
             terminate_err == ESP_ERR_NO_MEM) {
    result = ai_keyboard::BleOwnerRecoveryState::TerminateResult::Retryable;
  } else if (terminate_err == ESP_ERR_INVALID_STATE) {
    result = ai_keyboard::BleOwnerRecoveryState::TerminateResult::NotConnected;
  }

  bool completed = false;
  portENTER_CRITICAL(&connection_power_mux_);
  owner_recovery_.note_terminate_result(result, now_us);
  completed = !owner_recovery_.pending();
  portEXIT_CRITICAL(&connection_power_mux_);
  owner_recovery_pending_.store(!completed, std::memory_order_release);

  if (completed) {
    complete_owner_recovery("terminate_owner_changed");
  } else if (result ==
             ai_keyboard::BleOwnerRecoveryState::TerminateResult::Failed) {
    ESP_LOGE(kTag,
             "HID owner recovery terminate failed owner=%u/%lu err=%s",
             static_cast<unsigned>(target.conn_handle),
             static_cast<unsigned long>(target.generation),
             esp_err_to_name(terminate_err));
  }
}

void BleHidTransport::recover_fatal_input_delivery(const char* reason) {
  const auto target = connection_identity();
  if (!target.valid()) {
    reset_connection_power_state(false);
    return;
  }

  const auto now_us = static_cast<std::uint64_t>(esp_timer_get_time());
  portENTER_CRITICAL(&connection_power_mux_);
  const bool began = owner_recovery_.begin(target, now_us);
  portEXIT_CRITICAL(&connection_power_mux_);
  owner_recovery_pending_.store(began, std::memory_order_release);
  if (!began) {
    return;
  }
  ESP_LOGE(kTag,
           "HID owner recovery started owner=%u/%lu reason=%s",
           static_cast<unsigned>(target.conn_handle),
           static_cast<unsigned long>(target.generation),
           reason == nullptr ? "" : reason);
  // Keep the fatal head and every stateful release quarantined until the
  // authoritative adapter generation proves that the old host is gone.
  service_owner_recovery(now_us);
}

void BleHidTransport::poll_input_delivery(std::uint32_t now_ms) {
  const auto now_us = static_cast<std::uint64_t>(esp_timer_get_time());
  service_owner_recovery(now_us);
  if (owner_recovery_pending_.load(std::memory_order_acquire)) {
    return;
  }
  // HIDD host reset can omit the normal GAP DISCONNECT callback. Refresh the
  // exact owner lifetime before touching queued reports so an old generation
  // can never drain into a reconnected host that reused the same handle.
  refresh_connection_identity();
  apply_deferred_input_reset("connection_reset");
  service_advertising_reconcile();
  if (!connected()) {
    if (input_delivery_pending()) {
      clear_pending_input_reports("disconnected");
    }
    return;
  }

  if (!ai_keyboard::ble_input_delivery_requires_active_profile(
          true, input_delivery_pending())) {
    return;
  }
  prepare_for_input_delivery();
  pump_fixed_text_stream(now_ms);

  std::uint16_t interval = 0;
  bool interval_valid = false;
  portENTER_CRITICAL(&connection_power_mux_);
  interval = actual_conn_interval_;
  interval_valid = actual_connection_params_valid_;
  portEXIT_CRITICAL(&connection_power_mux_);
  input_scheduler_.set_connection_interval(interval, interval_valid);
  input_scheduler_.take_state_resync_required();

  const auto before = input_scheduler_.diagnostics();
  const auto poll_result = input_scheduler_.poll(
      static_cast<std::uint64_t>(esp_timer_get_time()),
      &pending_input_reports_,
      &pending_wheel_reports_,
      &BleHidTransport::transmit_scheduled_report_callback,
      this);
  const auto after = input_scheduler_.diagnostics();
  retryable_input_report_count_.store(
      after.retryable_no_buffer + after.retryable_busy,
      std::memory_order_relaxed);
  hid_queue_high_watermark_.store(
      static_cast<std::uint32_t>(after.hid_queue_high_watermark),
      std::memory_order_relaxed);
  if (after.hid_accepted != before.hid_accepted) {
    transmitted_input_report_count_.fetch_add(
        after.hid_accepted - before.hid_accepted, std::memory_order_relaxed);
  }
  if (after.wheel_accepted != before.wheel_accepted) {
    transmitted_wheel_report_count_.fetch_add(
        after.wheel_accepted - before.wheel_accepted, std::memory_order_relaxed);
  }
  pending_input_report_count_.store(
      static_cast<std::uint32_t>(pending_input_reports_.size()),
      std::memory_order_relaxed);
  pending_wheel_report_count_.store(
      static_cast<std::uint32_t>(pending_wheel_reports_.size()),
      std::memory_order_relaxed);

  if (poll_result == ai_keyboard::BleInputPollResult::Disconnected) {
    clear_pending_input_reports("send_disconnected");
    return;
  } else if (poll_result ==
             ai_keyboard::BleInputPollResult::DroppedUnsupported) {
    ESP_LOGD(kTag, "HID delivery dropped an unsupported optional report");
  } else if (poll_result == ai_keyboard::BleInputPollResult::Fatal) {
    ESP_LOGE(kTag,
             "HID delivery scheduler reported fatal error; rebuilding owner");
    recover_fatal_input_delivery("fatal_send");
    return;
  }
  // Accepted/dropped reports release one queue slot. Refill it from the
  // owner-bound stream without waiting for another physical input edge.
  pump_fixed_text_stream(now_ms);
}

bool BleHidTransport::send_mouse_wheel(std::int8_t vertical, std::int8_t horizontal) {
  return send_mouse_wheel_for_owner(vertical, horizontal, {});
}

bool BleHidTransport::send_mouse_wheel_for_owner(
    std::int8_t vertical,
    std::int8_t horizontal,
    ai_keyboard::BleOwnerToken expected_owner) {
  if (vertical == 0 && horizontal == 0) {
    return true;
  }
  apply_deferred_input_reset("connection_reset_before_wheel_enqueue");
  refresh_connection_identity();
  apply_deferred_input_reset("identity_refresh_before_wheel_enqueue");
  const auto current_owner = connection_identity();
  if (!current_owner.valid() ||
      (expected_owner.valid() && current_owner != expected_owner)) {
    return false;
  }
  if (!expected_owner.valid()) {
    expected_owner = current_owner;
  }

  prepare_for_input_delivery();
  const auto now_ms = monotonic_ms();
  std::uint32_t sequence = 0;
  bool coalesced = false;
  bool saturated = false;
  const bool queued = pending_wheel_reports_.push(
      vertical,
      horizontal,
      now_ms,
      &sequence,
      &coalesced,
      &saturated,
      expected_owner);
  const auto pending_count = pending_wheel_reports_.size();
  pending_wheel_report_count_.store(
      static_cast<std::uint32_t>(pending_count), std::memory_order_relaxed);
  if (queued) {
    queued_wheel_report_count_.fetch_add(1, std::memory_order_relaxed);
    if (coalesced) {
      coalesced_wheel_report_count_.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    dropped_wheel_report_count_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!queued) {
    ESP_LOGD(kTag,
             "mouse wheel queue full vertical=%d horizontal=%d pending=%u",
             static_cast<int>(vertical),
             static_cast<int>(horizontal),
             static_cast<unsigned>(pending_count));
    return false;
  }

  ESP_LOGD(kTag,
           "mouse wheel queued seq=%lu vertical=%d horizontal=%d pending=%u coalesced=%u saturated=%u",
           static_cast<unsigned long>(sequence),
           static_cast<int>(vertical),
           static_cast<int>(horizontal),
           static_cast<unsigned>(pending_count),
           coalesced ? 1U : 0U,
           saturated ? 1U : 0U);
  poll_input_delivery(now_ms);
  return true;
}

void BleHidTransport::handle_hidd_event(std::int32_t event_id, void* event_data) {
  auto* data = static_cast<esp_hidd_event_data_t*>(event_data);
  switch (event_id) {
    case ESP_HIDD_START_EVENT:
      ESP_LOGI(kTag, "HID started");
      if (gatt_schema_change_pending_) {
        ble_svc_gatt_changed(0x0001, 0xFFFF);
        esp_err_t save_err = ESP_OK;
        NvsConfigStore nvs_store;
        if (nvs_store.save_gatt_schema_revision(kGattSchemaRevision, &save_err)) {
          gatt_schema_change_pending_ = false;
          ESP_LOGI(kTag,
                   "GATT schema revision %u announced and persisted",
                   static_cast<unsigned>(kGattSchemaRevision));
        } else {
          ESP_LOGW(kTag,
                   "GATT schema revision %u announced but persistence failed: %s",
                   static_cast<unsigned>(kGattSchemaRevision),
                   esp_err_to_name(save_err));
        }
      }
      slow_advertising_.store(false, std::memory_order_release);
      request_advertising_reconcile();
      return;
    case ESP_HIDD_CONNECT_EVENT:
      directed_reconnect_active_.store(false, std::memory_order_release);
      directed_reconnect_attempted_.store(false, std::memory_order_release);
      slow_advertising_.store(true, std::memory_order_release);
      sync_hid_owner("hidd_connect");
      ESP_LOGI(kTag, "BLE HID owner connected config_window=closed");
      request_advertising_reconcile();
      return;
    case ESP_HIDD_DISCONNECT_EVENT:
      ESP_LOGI(kTag, "BLE HID disconnected");
      // GAP and the authoritative adapter lifecycle carry the exact owner
      // identity. This best-effort event is informational only.
      return;
    case ESP_HIDD_FEATURE_EVENT:
      if (data != nullptr && data->feature.report_id == kReportIdConfig) {
        receive_config_report(
            data->feature.data, data->feature.length, connection_identity());
      }
      return;
    case ESP_HIDD_OUTPUT_EVENT:
      return;
    default:
      return;
  }
}

int BleHidTransport::handle_gap_event(ble_gap_event* event) {
  if (event == nullptr) {
    return 0;
  }

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      ESP_LOGI(kTag,
               "GAP %s status=%d conn_handle=%u",
               gap_event_name(event->type),
               event->connect.status,
               static_cast<unsigned>(event->connect.conn_handle));
      if (event->connect.status == 0) {
        forget_status_read_snapshot(event->connect.conn_handle);
        directed_reconnect_active_.store(false, std::memory_order_release);
        // A GAP connection already exists (either the future HID owner or an
        // auxiliary config client). Do not start a new directed procedure
        // while its HID subscription role is still being resolved.
        directed_reconnect_attempted_.store(true,
                                            std::memory_order_release);
        slow_advertising_.store(true, std::memory_order_release);
        portENTER_CRITICAL(&connection_power_mux_);
        if (event->connect.conn_handle != active_conn_handle_) {
          control_conn_handle_ = event->connect.conn_handle;
        }
        portEXIT_CRITICAL(&connection_power_mux_);
        log_connection_desc("connected", event->connect.conn_handle);
        ESP_LOGI(kTag,
                 "GAP connection awaiting HID INPUT subscription conn_handle=%u",
                 static_cast<unsigned>(event->connect.conn_handle));
        const int security_rc = ble_gap_security_initiate(event->connect.conn_handle);
        ESP_LOGI(kTag,
                 "GAP security_initiate conn_handle=%u rc=%d",
                 static_cast<unsigned>(event->connect.conn_handle),
                 security_rc);
        request_advertising_reconcile();
      }
      if (event->connect.status != 0) {
        directed_reconnect_active_.store(false, std::memory_order_release);
        slow_advertising_.store(false, std::memory_order_release);
        request_advertising_reconcile();
      }
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
      ESP_LOGI(kTag,
               "GAP %s reason=%d conn_handle=%u",
               gap_event_name(event->type),
               event->disconnect.reason,
               static_cast<unsigned>(event->disconnect.conn.conn_handle));
      portENTER_CRITICAL(&connection_power_mux_);
      const bool disconnected_active_handle =
          event->disconnect.conn.conn_handle == active_conn_handle_;
      const bool disconnected_control_handle =
          event->disconnect.conn.conn_handle == control_conn_handle_;
      if (disconnected_control_handle) {
        control_conn_handle_ = kInvalidConnHandle;
      }
      portEXIT_CRITICAL(&connection_power_mux_);
      forget_status_read_snapshot(event->disconnect.conn.conn_handle);
      // GAP events are useful wake-up hints, but queue cleanup during fatal
      // recovery is deliberately left to service_owner_recovery(), which
      // compares the adapter's authoritative owner generation. A numeric
      // handle alone is not a lifetime identity and may be reused.
      if (disconnected_active_handle) {
        reset_connection_power_state(false);
      } else if (disconnected_control_handle) {
        ESP_LOGI(kTag,
                 "GAP auxiliary control disconnected conn_handle=%u",
                 static_cast<unsigned>(event->disconnect.conn.conn_handle));
      }
      if (!ble_gap_adv_active()) {
        directed_reconnect_active_.store(false, std::memory_order_release);
        directed_reconnect_attempted_.store(false,
                                             std::memory_order_release);
      }
      slow_advertising_.store(false, std::memory_order_release);
      request_advertising_reconcile();
      return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
      ESP_LOGI(kTag, "GAP %s reason=%d", gap_event_name(event->type), event->adv_complete.reason);
      // Do not infer the completed procedure from connection flags in the
      // callback. The main-task reconciler owns the exact started mode and
      // converts Directed/HidFast completion to the appropriate next mode
      // after confirming ble_gap_adv_active()==false.
      request_advertising_reconcile();
      return 0;
    case BLE_GAP_EVENT_CONN_UPDATE: {
      ESP_LOGI(kTag,
               "GAP %s status=%d conn_handle=%u",
               gap_event_name(event->type),
               event->conn_update.status,
               static_cast<unsigned>(event->conn_update.conn_handle));
      log_connection_desc("conn_update", event->conn_update.conn_handle);
      bool updated_active_handle = false;
      bool desired_changed_while_in_flight = false;
      portENTER_CRITICAL(&connection_power_mux_);
      if (event->conn_update.conn_handle == active_conn_handle_) {
        updated_active_handle = true;
        desired_changed_while_in_flight =
            requested_connection_profile_ != preferred_connection_profile_;
        connection_update_in_flight_ = false;
        if (event->conn_update.status != 0) {
          requested_connection_profile_ = ConnectionPowerProfile::Unknown;
        }
      }
      portEXIT_CRITICAL(&connection_power_mux_);
      if (updated_active_handle) {
        cache_connection_status(event->conn_update.conn_handle,
                                event->conn_update.status);
      }
      if (updated_active_handle) {
        const auto now_us = esp_timer_get_time();
        portENTER_CRITICAL(&connection_power_mux_);
        const bool actual_matches =
            event->conn_update.status == 0 &&
            !desired_changed_while_in_flight &&
            connection_profile_matches_actual_locked(
                preferred_connection_profile_);
        const auto disposition = ai_keyboard::classify_ble_connection_update(
            event->conn_update.status == 0,
            desired_changed_while_in_flight,
            actual_matches);
        switch (disposition) {
          case ai_keyboard::BleConnectionUpdateDisposition::Settled:
          case ai_keyboard::BleConnectionUpdateDisposition::RetryImmediately:
            connection_update_retry_after_us_ = 0;
            connection_update_retry_attempt_ = 0;
            break;
          case ai_keyboard::BleConnectionUpdateDisposition::RetryWithBackoff:
            schedule_connection_update_retry_locked(now_us);
            break;
        }
        portEXIT_CRITICAL(&connection_power_mux_);
        reconcile_connection_power_profile("conn_update_complete");
      }
      return 0;
    }
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      if (event->conn_update_req.peer_params != nullptr) {
        ESP_LOGI(kTag,
                 "GAP %s conn_handle=%u peer_itvl=%u-%u latency=%u timeout=%u",
                 gap_event_name(event->type),
                 static_cast<unsigned>(event->conn_update_req.conn_handle),
                 static_cast<unsigned>(event->conn_update_req.peer_params->itvl_min),
                 static_cast<unsigned>(event->conn_update_req.peer_params->itvl_max),
                 static_cast<unsigned>(event->conn_update_req.peer_params->latency),
                 static_cast<unsigned>(event->conn_update_req.peer_params->supervision_timeout));
      } else {
        ESP_LOGI(kTag,
                 "GAP %s conn_handle=%u peer_params=null",
                 gap_event_name(event->type),
                 static_cast<unsigned>(event->conn_update_req.conn_handle));
      }
      return 0;
    case BLE_GAP_EVENT_TERM_FAILURE: {
      ESP_LOGW(kTag,
               "GAP %s status=%d conn_handle=%u",
               gap_event_name(event->type),
               event->term_failure.status,
               static_cast<unsigned>(event->term_failure.conn_handle));
      bool matched_recovery = false;
      portENTER_CRITICAL(&connection_power_mux_);
      matched_recovery = owner_recovery_.note_term_failure(
          event->term_failure.conn_handle,
          static_cast<std::uint64_t>(esp_timer_get_time()));
      portEXIT_CRITICAL(&connection_power_mux_);
      if (matched_recovery) {
        ESP_LOGE(kTag,
                 "HID owner terminate failed; host reset scheduled on main task");
      }
      return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
      ESP_LOGI(kTag,
               "GAP %s status=%d conn_handle=%u",
               gap_event_name(event->type),
               event->enc_change.status,
               static_cast<unsigned>(event->enc_change.conn_handle));
      log_connection_desc("enc_change", event->enc_change.conn_handle);
      portENTER_CRITICAL(&connection_power_mux_);
      const bool encrypted_active_handle =
          event->enc_change.conn_handle == active_conn_handle_;
      portEXIT_CRITICAL(&connection_power_mux_);
      if (event->enc_change.status == 0 && encrypted_active_handle) {
        cache_connection_status(event->enc_change.conn_handle, 0);
        reconcile_connection_power_profile("enc_change");
      }
      return 0;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      ESP_LOGI(kTag,
               "GAP %s action=%u conn_handle=%u",
               gap_event_name(event->type),
               static_cast<unsigned>(event->passkey.params.action),
               static_cast<unsigned>(event->passkey.conn_handle));
      return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
      ESP_LOGI(kTag,
               "GAP %s conn_handle=%u attr_handle=%u reason=%u notify=%u->%u indicate=%u->%u",
               gap_event_name(event->type),
               static_cast<unsigned>(event->subscribe.conn_handle),
               static_cast<unsigned>(event->subscribe.attr_handle),
               static_cast<unsigned>(event->subscribe.reason),
               static_cast<unsigned>(event->subscribe.prev_notify),
               static_cast<unsigned>(event->subscribe.cur_notify),
               static_cast<unsigned>(event->subscribe.prev_indicate),
               static_cast<unsigned>(event->subscribe.cur_indicate));
      sync_hid_owner("gap_subscribe");
      return 0;
    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(kTag,
               "GAP %s conn_handle=%u channel=%u value=%u",
               gap_event_name(event->type),
               static_cast<unsigned>(event->mtu.conn_handle),
               static_cast<unsigned>(event->mtu.channel_id),
               static_cast<unsigned>(event->mtu.value));
      return 0;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED: {
      char peer[32] = {};
      format_addr(event->identity_resolved.peer_id_addr, peer, sizeof(peer));
      ESP_LOGI(kTag,
               "GAP %s conn_handle=%u peer_id=%s",
               gap_event_name(event->type),
               static_cast<unsigned>(event->identity_resolved.conn_handle),
               peer);
      return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      ESP_LOGW(kTag,
               "GAP %s conn_handle=%u current(auth=%u sc=%u key=%u) new(auth=%u sc=%u bond=%u key=%u)",
               gap_event_name(event->type),
               static_cast<unsigned>(event->repeat_pairing.conn_handle),
               static_cast<unsigned>(event->repeat_pairing.cur_authenticated),
               static_cast<unsigned>(event->repeat_pairing.cur_sc),
               static_cast<unsigned>(event->repeat_pairing.cur_key_size),
               static_cast<unsigned>(event->repeat_pairing.new_authenticated),
               static_cast<unsigned>(event->repeat_pairing.new_sc),
               static_cast<unsigned>(event->repeat_pairing.new_bonding),
               static_cast<unsigned>(event->repeat_pairing.new_key_size));
      {
        ble_gap_conn_desc desc = {};
        const int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
          const int delete_rc = ble_store_util_delete_peer(&desc.peer_id_addr);
          ESP_LOGW(kTag, "GAP repeat_pairing delete_peer rc=%d", delete_rc);
        } else {
          ESP_LOGW(kTag, "GAP repeat_pairing conn_desc unavailable rc=%d", rc);
        }
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_NOTIFY_TX:
      ESP_LOGD(kTag,
               "GAP %s status=%d conn_handle=%u attr_handle=%u indication=%u",
               gap_event_name(event->type),
               event->notify_tx.status,
               static_cast<unsigned>(event->notify_tx.conn_handle),
               static_cast<unsigned>(event->notify_tx.attr_handle),
               static_cast<unsigned>(event->notify_tx.indication));
      return 0;
    default:
      ESP_LOGD(kTag, "GAP %s type=%u", gap_event_name(event->type), event->type);
      return 0;
  }
}

int BleHidTransport::handle_config_access(std::uint16_t conn_handle,
                                          std::uint16_t attr_handle,
                                          ble_gatt_access_ctxt* ctxt) {
  if (ctxt == nullptr) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    std::array<char, ai_keyboard::kConfigStatusGattSafeLen> status{};
    std::size_t status_len = 0;
    if (!copy_status_json_for_read(conn_handle,
                                   ctxt->offset,
                                   status.data(),
                                   status.size(),
                                   &status_len)) {
      ESP_LOGD(kTag,
               "CONFIG status read rejected conn_handle=%u offset=%u",
               static_cast<unsigned>(conn_handle),
               static_cast<unsigned>(ctxt->offset));
      return BLE_ATT_ERR_INVALID_OFFSET;
    }

    const bool remote_read = conn_handle != BLE_HS_CONN_HANDLE_NONE;
    if (remote_read && ctxt->offset == 0 && status_read_callback_ != nullptr) {
      status_read_callback_(status_read_context_);
    }
    ESP_LOGD(kTag,
             "CONFIG status read conn_handle=%u offset=%u bytes=%u remote=%u",
             static_cast<unsigned>(conn_handle),
             static_cast<unsigned>(ctxt->offset),
             static_cast<unsigned>(status_len),
             remote_read ? 1U : 0U);
    const int rc = os_mbuf_append(ctxt->om, status.data(), status_len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    note_control_connection(conn_handle);
    const int auth_error = config_write_authorization_error(conn_handle);
    if (auth_error != 0) {
      return auth_error;
    }

    const int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(len));
    const int rc = os_mbuf_copydata(ctxt->om, 0, len, data.data());
    if (rc != 0) {
      return BLE_ATT_ERR_UNLIKELY;
    }
    if (attr_handle == s_agent_status_write_handle) {
      if (!receive_agent_status_report(data.data(), data.size())) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      }
    } else {
      auto origin_owner = connection_identity();
      if (origin_owner.conn_handle != conn_handle) {
        origin_owner = {};
      }
      receive_config_report(data.data(), data.size(), origin_owner);
    }
    return 0;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

void BleHidTransport::note_control_connection(std::uint16_t conn_handle) {
  if (conn_handle == kInvalidConnHandle) {
    return;
  }

  bool changed = false;
  bool shared_hid = false;
  portENTER_CRITICAL(&connection_power_mux_);
  shared_hid = active_conn_handle_ == conn_handle;
  if (!shared_hid && control_conn_handle_ != conn_handle) {
    control_conn_handle_ = conn_handle;
    changed = true;
  }
  portEXIT_CRITICAL(&connection_power_mux_);

  if (changed) {
    ESP_LOGI(kTag,
             "GATT control channel conn_handle=%u shared_hid=%u",
             static_cast<unsigned>(conn_handle),
             shared_hid ? 1U : 0U);
  }
}

int BleHidTransport::config_write_authorization_error(std::uint16_t conn_handle) const {
  ble_gap_conn_desc desc = {};
  const int rc = ble_gap_conn_find(conn_handle, &desc);
  if (rc != 0) {
    ESP_LOGW(kTag,
             "CONFIG write rejected conn_handle=%u desc_unavailable rc=%d",
             static_cast<unsigned>(conn_handle),
             rc);
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (desc.sec_state.encrypted == 0 || desc.sec_state.bonded == 0) {
    char peer[32] = {};
    format_addr(desc.peer_ota_addr, peer, sizeof(peer));
    ESP_LOGW(kTag,
             "CONFIG write rejected conn_handle=%u peer=%s encrypted=%u bonded=%u",
             static_cast<unsigned>(conn_handle),
             peer,
             static_cast<unsigned>(desc.sec_state.encrypted),
             static_cast<unsigned>(desc.sec_state.bonded));
    if (desc.sec_state.encrypted == 0) {
      const int security_rc = ble_gap_security_initiate(conn_handle);
      ESP_LOGI(kTag,
               "CONFIG write requested security conn_handle=%u rc=%d",
               static_cast<unsigned>(conn_handle),
               security_rc);
    }
    return desc.sec_state.encrypted == 0 ? BLE_ATT_ERR_INSUFFICIENT_ENC
                                         : BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  }

  return 0;
}

esp_err_t BleHidTransport::init_low_level() {
  esp_bt_controller_config_t bt_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
  bt_config.mode = ESP_BT_MODE_BLE;
#endif

  esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "esp_bt_controller_mem_release failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_bt_controller_init(&bt_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_nimble_init();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_nimble_init failed: %s", esp_err_to_name(err));
  }
  return err;
}

esp_err_t BleHidTransport::register_config_service() {
  int rc = ble_gatts_count_cfg(kConfigServices);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gatts_count_cfg config service failed: %d", rc);
    return ESP_FAIL;
  }
  rc = ble_gatts_add_svcs(kConfigServices);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gatts_add_svcs config service failed: %d", rc);
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t BleHidTransport::prepare_identity_address() {
  if (identity_address_ready_) {
    return ESP_OK;
  }

  std::array<std::uint8_t, 6> source_mac = {};
  const char* source_name = "bt";
  esp_err_t err = esp_read_mac(source_mac.data(), ESP_MAC_BT);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "esp_read_mac ESP_MAC_BT failed: %s", esp_err_to_name(err));
    source_name = "efuse_default";
    err = esp_efuse_mac_get_default(source_mac.data());
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "identity source MAC read failed: %s", esp_err_to_name(err));
    return err;
  }

  derive_static_random_addr_le(source_mac, &identity_address_le_);
  format_addr_le(identity_address_le_, identity_address_text_.data(), identity_address_text_.size());
  identity_address_ready_ = true;

  ESP_LOGI(kTag,
           "BLE HID identity addr=%s source=%s source_mac=%02X:%02X:%02X:%02X:%02X:%02X",
           identity_address_text(),
           source_name,
           source_mac[0],
           source_mac[1],
           source_mac[2],
           source_mac[3],
           source_mac[4],
           source_mac[5]);
  return ESP_OK;
}

esp_err_t BleHidTransport::ensure_identity_set() {
  if (!ble_hs_synced()) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = prepare_identity_address();
  if (err != ESP_OK) {
    return err;
  }

  std::array<std::uint8_t, 6> adapter_identity{};
  int is_nrpa = 0;
  const int copy_rc = ble_hs_id_copy_addr(
      BLE_ADDR_RANDOM, adapter_identity.data(), &is_nrpa);
  if (copy_rc == 0 && is_nrpa == 0 &&
      adapter_identity == identity_address_le_) {
    return ESP_OK;
  }
  if (copy_rc != 0 && copy_rc != BLE_HS_ENOADDR) {
    ESP_LOGW(kTag,
             "ble_hs_id_copy_addr random identity failed: %d",
             copy_rc);
    return ESP_FAIL;
  }

  const int rc = ble_hs_id_set_rnd(identity_address_le_.data());
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_hs_id_set_rnd addr=%s failed: %d", identity_address_text(), rc);
    return ESP_FAIL;
  }
  ESP_LOGI(kTag,
           "BLE HID random identity restored addr=%s previous=%s",
           identity_address_text(),
           copy_rc == BLE_HS_ENOADDR ? "missing" : "different");
  return ESP_OK;
}

const char* BleHidTransport::identity_address_text() const {
  return identity_address_ready_ ? identity_address_text_.data() : "uninitialized";
}

bool BleHidTransport::start_directed_reconnect_advertising() {
  directed_reconnect_attempted_.store(true, std::memory_order_release);

  ble_addr_t peer = {};
  int peer_count = 0;
  if (!bonded_peer_for_reconnect(&peer, &peer_count)) {
    ESP_LOGI(kTag, "BLE HID directed reconnect skipped: bonded_peers=%d", peer_count);
    return false;
  }

  char peer_text[32] = {};
  format_addr(peer, peer_text, sizeof(peer_text));

  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_DIR;
  params.disc_mode = BLE_GAP_DISC_MODE_NON;
  params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
  params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
  params.high_duty_cycle = 0;

  const int rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM,
                                   &peer,
                                   kDirectedReconnectDurationMs,
                                   &params,
                                   gap_event_callback,
                                   this);
  if (rc == BLE_HS_EALREADY) {
    directed_reconnect_active_.store(false, std::memory_order_release);
    ESP_LOGW(kTag,
             "BLE HID directed reconnect found unknown active advertising");
    return false;
  }
  if (rc != 0) {
    directed_reconnect_active_.store(false, std::memory_order_release);
    ESP_LOGW(kTag,
             "BLE HID directed reconnect peer=%s bonded_peers=%d failed: %d",
             peer_text,
             peer_count,
             rc);
    return false;
  }

  directed_reconnect_active_.store(true, std::memory_order_release);
  ESP_LOGI(kTag,
           "BLE HID directed reconnect peer=%s bonded_peers=%d duration_ms=%ld",
           peer_text,
           peer_count,
           static_cast<long>(kDirectedReconnectDurationMs));
  return true;
}

esp_err_t BleHidTransport::start_advertising(
    ai_keyboard::BleAdvertisingMode mode) {
  const esp_err_t identity_err = ensure_identity_set();
  if (identity_err != ESP_OK) {
    return identity_err;
  }

  if (ble_gap_adv_active()) {
    return ESP_ERR_INVALID_STATE;
  }

  const bool connected_now =
      mode == ai_keyboard::BleAdvertisingMode::ControlSlow ||
      mode == ai_keyboard::BleAdvertisingMode::ControlConfig;
  const bool advertise_config =
      mode == ai_keyboard::BleAdvertisingMode::HidConfig ||
      mode == ai_keyboard::BleAdvertisingMode::ControlConfig;
  const bool slow =
      mode == ai_keyboard::BleAdvertisingMode::HidSlow ||
      mode == ai_keyboard::BleAdvertisingMode::ControlSlow;
  if (mode == ai_keyboard::BleAdvertisingMode::Stopped ||
      mode == ai_keyboard::BleAdvertisingMode::Directed) {
    return ESP_ERR_INVALID_ARG;
  }

  ble_hs_adv_fields fields = {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &kConfigServiceUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  if (!connected_now) {
    fields.appearance = kAppearanceHidGeneric;
    fields.appearance_is_present = 1;
    fields.uuids16 = &kHidServiceUuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
  }

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_adv_set_fields failed: %d", rc);
    return ESP_FAIL;
  }

  const bool control_only_advertising = connected_now;
  const char* scan_response_mode = control_only_advertising || advertise_config
                                       ? "control_short_name"
                                       : "complete_name";

  ble_hs_adv_fields response = {};
  response.slave_itvl_range = kPreferredConnectionIntervalLe.data();
  if (control_only_advertising || advertise_config) {
    response.name = reinterpret_cast<const std::uint8_t*>(kBleShortName);
    response.name_len = std::strlen(kBleShortName);
    response.name_is_complete = 0;
  } else {
    response.name = reinterpret_cast<const std::uint8_t*>(kBleDeviceName);
    response.name_len = std::strlen(kBleDeviceName);
    response.name_is_complete = 1;
  }

  rc = ble_gap_adv_rsp_set_fields(&response);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_adv_rsp_set_fields failed: %d", rc);
    return ESP_FAIL;
  }

  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = advertise_config
                        ? kConfigAdvertisingIntervalMin
                        : (slow ? kSlowAdvertisingIntervalMin : BLE_GAP_ADV_FAST_INTERVAL1_MIN);
  params.itvl_max = advertise_config
                        ? kConfigAdvertisingIntervalMax
                        : (slow ? kSlowAdvertisingIntervalMax : BLE_GAP_ADV_FAST_INTERVAL1_MAX);

  const int32_t duration_ms = advertise_config
                                  ? connected_config_window_remaining_ms()
                                  : (slow ? BLE_HS_FOREVER : kFastAdvertisingDurationMs);
  if (duration_ms == 0) {
    return ESP_ERR_INVALID_STATE;
  }
  rc = ble_gap_adv_start(
      BLE_OWN_ADDR_RANDOM, nullptr, duration_ms, &params, gap_event_callback, this);
  if (rc == BLE_HS_EALREADY) {
    return ESP_ERR_INVALID_STATE;
  }
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_adv_start failed: %d", rc);
    return ESP_FAIL;
  }

  ESP_LOGI(kTag,
           "BLE advertising addr=%s mode=%s speed=%s duration_ms=%ld interval=0x%04X-0x%04X scan_response=%s preferred_interval=0x%04X-0x%04X",
           identity_address_text(),
           advertising_mode_name(mode),
           advertising_speed_name(slow),
           static_cast<long>(duration_ms),
           static_cast<unsigned>(params.itvl_min),
           static_cast<unsigned>(params.itvl_max),
           scan_response_mode,
           static_cast<unsigned>(kActiveConnIntervalMin),
           static_cast<unsigned>(kActiveConnIntervalMax));
  return ESP_OK;
}

void BleHidTransport::receive_config_report(
    const std::uint8_t* data,
    std::size_t len,
    ai_keyboard::BleOwnerToken origin_owner) {
  auto result = config_receiver_.receive(data, len);
  ESP_LOGI(kTag,
           "CONFIG rx len=%u status=%s owner=%u/%lu",
           static_cast<unsigned>(len),
           receive_status_name(result.status),
           static_cast<unsigned>(origin_owner.conn_handle),
           static_cast<unsigned long>(origin_owner.generation));
  if (result.status == ai_keyboard::ConfigReceiveStatus::Complete) {
    queue_completed_config(std::move(result.json), origin_owner);
  }
}

bool BleHidTransport::receive_agent_status_report(const std::uint8_t* data,
                                                  std::size_t len) {
  ai_keyboard::AgentStatusCommand command{};
  if (!ai_keyboard::decode_agent_status(data, len, &command)) {
    return false;
  }

  portENTER_CRITICAL(&pending_agent_status_mux_);
  pending_agent_status_ = command;
  pending_agent_status_ready_ = true;
  portEXIT_CRITICAL(&pending_agent_status_mux_);
  ESP_LOGI(kTag,
           "BLE AGENT state=%u seq=%lu ttl=%lums",
           static_cast<unsigned>(command.state),
           static_cast<unsigned long>(command.sequence),
           static_cast<unsigned long>(command.ttl_ms));
  return true;
}

void BleHidTransport::queue_completed_config(
    std::string json,
    ai_keyboard::BleOwnerToken origin_owner) {
  if (json.empty() || json.size() > ai_keyboard::kConfigMaxJsonLen) {
    return;
  }
  portENTER_CRITICAL(&pending_config_mux_);
  pending_config_json_.swap(json);
  pending_config_owner_ = origin_owner;
  pending_config_ready_ = true;
  portEXIT_CRITICAL(&pending_config_mux_);
}

bool BleHidTransport::send_hotkey_report(const std::string& hotkey, bool pressed) {
  if (!pressed) {
    return send_keyboard_report(0, 0);
  }

  const auto report = ai_keyboard::hid_report_for_hotkey(hotkey);
  if (!report.valid) {
    ESP_LOGW(kTag, "HID unsupported_hotkey %s", hotkey.c_str());
    return false;
  }
  return send_keyboard_report(report.modifier, report.keycodes, report.apple_fn);
}

bool BleHidTransport::tap_hotkey(const std::string& hotkey) {
  if (!send_hotkey_report(hotkey, true)) {
    return false;
  }
  vTaskDelay(delay_ticks(15));
  return send_hotkey_report(hotkey, false);
}

void BleHidTransport::send_config_ack(std::uint8_t phase_code,
                                      bool ok,
                                      std::uint16_t bytes,
                                      std::uint16_t crc16,
                                      bool saved) {
  (void)send_config_ack_for_owner(
      phase_code, ok, bytes, crc16, saved, connection_identity());
}

bool BleHidTransport::send_config_ack_for_owner(
    std::uint8_t phase_code,
    bool ok,
    std::uint16_t bytes,
    std::uint16_t crc16,
    bool saved,
    ai_keyboard::BleOwnerToken expected_owner) {
  if (!expected_owner.valid()) {
    return false;
  }
  std::array<std::uint8_t, 7> data{};
  data[0] = phase_code;
  data[1] = ok ? 1 : 0;
  data[2] = static_cast<std::uint8_t>(bytes & 0xFF);
  data[3] = static_cast<std::uint8_t>((bytes >> 8) & 0xFF);
  data[4] = static_cast<std::uint8_t>(crc16 & 0xFF);
  data[5] = static_cast<std::uint8_t>((crc16 >> 8) & 0xFF);
  data[6] = saved ? 1 : 0;
  return send_app_command_report(kAppCommandKindConfigAck,
                                 0,
                                 1,
                                 data.data(),
                                 data.size(),
                                 expected_owner);
}

bool BleHidTransport::send_app_command_report(std::uint8_t command_kind,
                                              std::uint8_t chunk_index,
                                              std::uint8_t total_chunks,
                                              const std::uint8_t* data,
                                              std::size_t len,
                                              ai_keyboard::BleOwnerToken expected_owner,
                                              ai_keyboard::HidReportClass report_class) {
  if (!connected()) {
    return false;
  }
  if (len > kAppCommandChunkDataLen) {
    ESP_LOGW(kTag,
             "APP_COMMAND chunk_too_large kind=%u len=%u",
             static_cast<unsigned>(command_kind),
             static_cast<unsigned>(len));
    return false;
  }

  std::array<std::uint8_t, kAppCommandReportPayloadLen> report{};
  report[0] = command_kind;
  report[1] = chunk_index;
  report[2] = total_chunks;
  report[3] = static_cast<std::uint8_t>(len);
  if (len > 0 && data != nullptr) {
    std::copy_n(data, len, report.data() + kAppCommandHeaderLen);
  }

  return send_input_report(kReportIdAppCommand,
                           report.data(),
                           report.size(),
                           "app command",
                           report_class,
                           expected_owner);
}

bool BleHidTransport::send_fixed_text_command(
    const std::string& text,
    ai_keyboard::BleOwnerToken expected_owner) {
  if (text.empty()) {
    return true;
  }
  if (text.size() > ai_keyboard::kFixedTextMaxUtf8Bytes) {
    ESP_LOGW(kTag,
             "APP_COMMAND fixed_text_too_large bytes=%u max=%u",
             static_cast<unsigned>(text.size()),
             static_cast<unsigned>(
                 ai_keyboard::kFixedTextMaxUtf8Bytes));
    return false;
  }

  apply_deferred_input_reset("connection_reset_before_fixed_text");
  refresh_connection_identity();
  apply_deferred_input_reset("identity_refresh_before_fixed_text");
  const auto current_owner = connection_identity();
  if (!current_owner.valid() ||
      (expected_owner.valid() && expected_owner != current_owner)) {
    return false;
  }
  if (!expected_owner.valid()) {
    expected_owner = current_owner;
  }

  const auto start_status =
      fixed_text_stream_.start(text, expected_owner);
  switch (start_status) {
    case ai_keyboard::BleFixedTextStartStatus::Started:
      prepare_for_input_delivery();
      poll_input_delivery(monotonic_ms());
      return true;
    case ai_keyboard::BleFixedTextStartStatus::Empty:
      return true;
    case ai_keyboard::BleFixedTextStartStatus::InvalidOwner:
      ESP_LOGW(kTag, "APP_COMMAND fixed_text rejected without owner");
      return false;
    case ai_keyboard::BleFixedTextStartStatus::TooLarge:
      ESP_LOGW(kTag,
               "APP_COMMAND fixed_text rejected bytes=%u",
               static_cast<unsigned>(text.size()));
      return false;
    case ai_keyboard::BleFixedTextStartStatus::Busy:
      ESP_LOGW(kTag,
               "APP_COMMAND fixed_text busy pending_owner=%u/%lu",
               static_cast<unsigned>(
                   fixed_text_stream_.owner().conn_handle),
               static_cast<unsigned long>(
                   fixed_text_stream_.owner().generation));
      return false;
  }
  return false;
}

bool BleHidTransport::send_hotkey_app_command(
    const std::string& hotkey,
    bool pressed,
    ai_keyboard::BleOwnerToken expected_owner) {
  if (hotkey.empty() || hotkey.size() > kAppCommandChunkDataLen - 1) {
    ESP_LOGW(kTag,
             "APP_COMMAND hotkey_invalid bytes=%u",
             static_cast<unsigned>(hotkey.size()));
    return false;
  }

  std::array<std::uint8_t, kAppCommandChunkDataLen> payload{};
  payload[0] = pressed ? kAppCommandHotkeyPressed : kAppCommandHotkeyReleased;
  std::copy(hotkey.begin(), hotkey.end(), payload.begin() + 1);
  ESP_LOGI(kTag,
           "APP_COMMAND hotkey %s %s",
           hotkey.c_str(),
           pressed ? "pressed" : "released");
  return send_app_command_report(kAppCommandKindHotkey,
                                 0,
                                 1,
                                 payload.data(),
                                 hotkey.size() + 1,
                                 expected_owner,
                                 pressed
                                     ? ai_keyboard::HidReportClass::AppCommandStatefulPress
                                     : ai_keyboard::HidReportClass::AppCommandStatefulRelease);
}

std::string BleHidTransport::status_json_for_publish(
    const std::string& status_json) const {
  std::string status = status_json;
  if (status.size() < 2 || status.front() != '{' || status.back() != '}') {
    return status;
  }

  ConnectionPowerProfile preferred_profile = ConnectionPowerProfile::Unknown;
  ConnectionPowerProfile requested_profile = ConnectionPowerProfile::Unknown;
  bool update_in_flight = false;
  std::uint16_t conn_handle = kInvalidConnHandle;
  bool params_valid = false;
  std::uint16_t conn_interval = 0;
  std::uint16_t conn_latency = 0;
  std::uint16_t supervision_timeout = 0;
  std::int32_t update_status = 0;
  portENTER_CRITICAL(&connection_power_mux_);
  preferred_profile = preferred_connection_profile_;
  requested_profile = requested_connection_profile_;
  update_in_flight = connection_update_in_flight_;
  conn_handle = active_conn_handle_;
  params_valid = actual_connection_params_valid_;
  conn_interval = actual_conn_interval_;
  conn_latency = actual_conn_latency_;
  supervision_timeout = actual_conn_supervision_timeout_;
  update_status = last_conn_update_status_;
  portEXIT_CRITICAL(&connection_power_mux_);

  const bool battery_status = status.find("\"phase\":\"battery\"") != std::string::npos;
  if (battery_status) {
    const bool connected_now = conn_handle != kInvalidConnHandle;
    return ai_keyboard::append_ble_status_wire_json(
        std::move(status),
        {
            connected_now,
            preferred_profile != ConnectionPowerProfile::Active,
            connection_profile_name(requested_profile),
            params_valid,
            conn_interval,
            conn_latency,
            supervision_timeout,
        });
  }

  std::size_t queued_reports = 0;
  std::size_t queued_wheel_reports = 0;
  std::uint32_t enqueued_reports = 0;
  std::uint32_t transmitted_reports = 0;
  std::uint32_t dropped_reports = 0;
  std::uint32_t enqueued_wheel_reports = 0;
  std::uint32_t coalesced_wheel_reports = 0;
  std::uint32_t transmitted_wheel_reports = 0;
  std::uint32_t dropped_wheel_reports = 0;
  std::uint32_t retryable_reports = 0;
  std::uint32_t hid_queue_high_watermark = 0;
  queued_reports = pending_input_report_count_.load(std::memory_order_relaxed);
  queued_wheel_reports =
      pending_wheel_report_count_.load(std::memory_order_relaxed);
  enqueued_reports = queued_input_report_count_.load(std::memory_order_relaxed);
  transmitted_reports =
      transmitted_input_report_count_.load(std::memory_order_relaxed);
  dropped_reports = dropped_input_report_count_.load(std::memory_order_relaxed);
  enqueued_wheel_reports =
      queued_wheel_report_count_.load(std::memory_order_relaxed);
  coalesced_wheel_reports =
      coalesced_wheel_report_count_.load(std::memory_order_relaxed);
  transmitted_wheel_reports =
      transmitted_wheel_report_count_.load(std::memory_order_relaxed);
  dropped_wheel_reports =
      dropped_wheel_report_count_.load(std::memory_order_relaxed);
  retryable_reports =
      retryable_input_report_count_.load(std::memory_order_relaxed);
  hid_queue_high_watermark =
      hid_queue_high_watermark_.load(std::memory_order_relaxed);

  std::array<char, 448> ble_fragment{};
  const bool connected_now = conn_handle != kInvalidConnHandle;
  const int fragment_len = std::snprintf(
      ble_fragment.data(),
      ble_fragment.size(),
      ",\"ble\":{\"connected\":%u,\"idle\":%u,\"profile\":\"%s\",\"pending\":%u,\"handle\":%u,\"valid\":%u,\"itvl\":%u,\"latency\":%u,\"timeout\":%u,\"upd\":%ld,\"hidq\":%u,\"hid_enq\":%lu,\"hid_tx\":%lu,\"hid_drop\":%lu,\"hid_retry\":%lu,\"hid_hwm\":%lu,\"wheelq\":%u,\"wheel_enq\":%lu,\"wheel_merge\":%lu,\"wheel_tx\":%lu,\"wheel_drop\":%lu}",
      connected_now ? 1U : 0U,
      preferred_profile == ConnectionPowerProfile::Active ? 0U : 1U,
      connection_profile_name(requested_profile),
      update_in_flight ? 1U : 0U,
      connected_now ? static_cast<unsigned>(conn_handle) : 0U,
      params_valid ? 1U : 0U,
      params_valid ? static_cast<unsigned>(conn_interval) : 0U,
      params_valid ? static_cast<unsigned>(conn_latency) : 0U,
      params_valid ? static_cast<unsigned>(supervision_timeout) : 0U,
      static_cast<long>(update_status),
      static_cast<unsigned>(queued_reports),
      static_cast<unsigned long>(enqueued_reports),
      static_cast<unsigned long>(transmitted_reports),
      static_cast<unsigned long>(dropped_reports),
      static_cast<unsigned long>(retryable_reports),
      static_cast<unsigned long>(hid_queue_high_watermark),
      static_cast<unsigned>(queued_wheel_reports),
      static_cast<unsigned long>(enqueued_wheel_reports),
      static_cast<unsigned long>(coalesced_wheel_reports),
      static_cast<unsigned long>(transmitted_wheel_reports),
      static_cast<unsigned long>(dropped_wheel_reports));
  if (fragment_len <= 0 ||
      static_cast<std::size_t>(fragment_len) >= ble_fragment.size() ||
      status.size() + static_cast<std::size_t>(fragment_len) >
          ai_keyboard::kConfigStatusGattSafeLen) {
    return status;
  }

  status.insert(status.size() - 1, ble_fragment.data(), static_cast<std::size_t>(fragment_len));
  return status;
}

bool BleHidTransport::copy_status_json_for_read(std::uint16_t conn_handle,
                                                std::uint16_t offset,
                                                char* out,
                                                std::size_t out_capacity,
                                                std::size_t* out_len) {
  bool copied = false;
  portENTER_CRITICAL(&status_mux_);
  if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    copied = status_read_cache_.copy_published(out, out_capacity, out_len);
  } else {
    copied = status_read_cache_.copy_for_remote_read(
        conn_handle, offset, out, out_capacity, out_len);
  }
  portEXIT_CRITICAL(&status_mux_);
  return copied;
}

void BleHidTransport::forget_status_read_snapshot(std::uint16_t conn_handle) {
  if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  portENTER_CRITICAL(&status_mux_);
  status_read_cache_.forget(conn_handle);
  portEXIT_CRITICAL(&status_mux_);
}

void BleHidTransport::clear_status_read_snapshots() {
  portENTER_CRITICAL(&status_mux_);
  status_read_cache_.clear_snapshots();
  portEXIT_CRITICAL(&status_mux_);
}

}  // namespace easy_input
