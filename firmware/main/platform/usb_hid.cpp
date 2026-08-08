#include "platform/usb_hid.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard/board_pins.h"
#include "keyboard/config_receiver.h"
#include "keyboard/fixed_text_protocol.h"
#include "keyboard/hid_keycode.h"
#include "keyboard/status_hid_protocol.h"
#include "tinyusb.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

namespace easy_input {
namespace {

constexpr const char* kTag = "usb_hid";
constexpr std::uint8_t kReportIdKeyboard = 1;
constexpr std::uint8_t kReportIdMouse = 2;
constexpr std::uint8_t kReportIdConfig = ai_keyboard::kConfigReportId;
constexpr std::uint8_t kReportIdAppCommand = 0x11;
constexpr std::uint8_t kReportIdAgentStatus = ai_keyboard::kAgentStatusReportId;
constexpr std::uint8_t kReportIdStatusRequest = ai_keyboard::kStatusRequestReportId;
constexpr std::uint8_t kReportIdSpeakerAssetsRequest = 0x14;
constexpr std::uint8_t kReportIdSpeakerAssetsResponse = 0x15;
constexpr std::uint8_t kAppCommandKindFixedText = 0x01;
constexpr std::uint8_t kAppCommandKindHotkey = 0x02;
constexpr std::uint8_t kAppCommandKindConfigAck = 0x03;
constexpr std::uint8_t kAppCommandHotkeyPressed = 0x01;
constexpr std::uint8_t kAppCommandHotkeyReleased = 0x02;
constexpr std::size_t kAppCommandReportPayloadLen = 63;
constexpr std::size_t kAppCommandHeaderLen = 4;
constexpr std::size_t kAppCommandChunkDataLen =
    kAppCommandReportPayloadLen - kAppCommandHeaderLen;
constexpr std::size_t kSpeakerAssetsReportPayloadLen = 63;
static_assert(kReportIdAppCommand ==
              ai_keyboard::kFixedTextAppCommandReportId);
static_assert(kAppCommandKindFixedText ==
              ai_keyboard::kFixedTextAppCommandKind);
static_assert(kAppCommandReportPayloadLen ==
              ai_keyboard::kFixedTextAppCommandPayloadLen);
static_assert(kAppCommandHeaderLen ==
              ai_keyboard::kFixedTextAppCommandHeaderLen);
static_assert(kAppCommandChunkDataLen ==
              ai_keyboard::kFixedTextAppCommandChunkDataLen);
constexpr std::uint16_t kUsbVid = 0x303A;
constexpr std::uint16_t kUsbPid = 0x1006;
constexpr std::uint16_t kUsbBcdDevice = 0x010A;

#define USB_HID_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

const std::uint8_t kHidReportDescriptor[] = {
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
    0x05, 0xFF,        //   Usage Page (AppleVendor Top Case)
    0x09, 0x03,        //   Usage (Keyboard Fn)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (Reserved)
    0x29, 0x65,        //   Usage Maximum (Keyboard Application)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x00,        //   Input (Data,Array,Abs)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Const,Var,Abs)
    0xC0,              // End Collection
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(kReportIdMouse)),
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x02,        // Usage (Vendor Usage 2)
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
    0x85, 0x12,        //   Report ID (18)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x10,        //   Report Count (16)
    0x09, 0x03,        //   Usage (Vendor Usage 3)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x13,        //   Report ID (19)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x10,        //   Report Count (16)
    0x09, 0x04,        //   Usage (Vendor Usage 4)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x14,        //   Report ID (20)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x09, 0x05,        //   Usage (Vendor Usage 5)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x15,        //   Report ID (21)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)
    0x09, 0x06,        //   Usage (Vendor Usage 6)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0xC0,              // End Collection
};

const std::uint8_t kHidConfigurationDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1,
                          1,
                          0,
                          USB_HID_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(kHidReportDescriptor), 0x81, 64, 10),
};

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = kUsbVid,
    .idProduct = kUsbPid,
    .bcdDevice = kUsbBcdDevice,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const char kLangId[] = {0x09, 0x04};
const char* kStringDescriptor[] = {
    kLangId,
    "AIOTWAN",
    "EasyInput AI",
    ai_keyboard::kUsbSerialNumber,
    "EasyInput AI HID",
};

TickType_t delay_ticks(std::uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks == 0 ? 1 : ticks;
}

bool can_type_text_as_keyboard(const std::string& text) {
  for (const char ch : text) {
    if (!ai_keyboard::hid_report_for_ascii_char(ch).valid) {
      return false;
    }
  }
  return true;
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

UsbHidTransport* s_active_transport = nullptr;

bool board_usb_vbus_present() {
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    return gpio_get_level(
               static_cast<gpio_num_t>(
                   ai_keyboard::kExternalPowerSensePin)) ==
           ai_keyboard::kExternalPowerSenseActiveLevel;
  }
  return true;
}

}  // namespace

esp_err_t UsbHidTransport::begin() {
  if (initialized_) {
    return ESP_OK;
  }

  lifetime_mutex_ = xSemaphoreCreateMutex();
  if (lifetime_mutex_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    endpoint_lifetime_.enable_physical_presence_monitor(
        board_usb_vbus_present());
  }
  initialized_ = true;
  s_active_transport = this;

  const tinyusb_config_t tusb_cfg = {
      .device_descriptor = &kDeviceDescriptor,
      .string_descriptor = kStringDescriptor,
      .string_descriptor_count = sizeof(kStringDescriptor) / sizeof(kStringDescriptor[0]),
      .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
      .fs_configuration_descriptor = kHidConfigurationDescriptor,
      .hs_configuration_descriptor = kHidConfigurationDescriptor,
      .qualifier_descriptor = nullptr,
#else
      .configuration_descriptor = kHidConfigurationDescriptor,
#endif
      .self_powered = false,
      .vbus_monitor_io = ai_keyboard::kUsbVbusSensePin,
  };

  const esp_err_t err = tinyusb_driver_install(&tusb_cfg);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "TinyUSB HID init failed: %s", esp_err_to_name(err));
    s_active_transport = nullptr;
    initialized_ = false;
    endpoint_lifetime_ = ai_keyboard::UsbEndpointLifetime{};
    lifetime_reset_pending_ = false;
    vSemaphoreDelete(lifetime_mutex_);
    lifetime_mutex_ = nullptr;
    return err;
  }

  // SET_CONFIGURATION always delivers tud_mount_cb after the active transport
  // and lifetime mutex above are installed. Do not synthesize a mount from a
  // tud_mounted() sample here: every real mount callback is a new endpoint
  // lifetime, so racing a synthetic mount with that callback would count the
  // same enumeration twice and erase reports accepted between them.
  ESP_LOGI(kTag, "TinyUSB HID keyboard initialized vid=0x%04X pid=0x%04X", kUsbVid, kUsbPid);
  return ESP_OK;
}

bool UsbHidTransport::mounted() const {
  return connection_epoch() != 0;
}

bool UsbHidTransport::ready() const {
  return mounted() && tud_hid_ready();
}

std::uint32_t UsbHidTransport::connection_epoch() const {
  if (!initialized_ || lifetime_mutex_ == nullptr ||
      xSemaphoreTake(lifetime_mutex_, portMAX_DELAY) != pdTRUE) {
    return 0;
  }
  const auto epoch =
      endpoint_lifetime_.mounted() && tud_mounted()
          ? endpoint_lifetime_.epoch()
          : 0;
  xSemaphoreGive(lifetime_mutex_);
  return epoch;
}

void UsbHidTransport::on_tinyusb_mount() {
  if (lifetime_mutex_ == nullptr ||
      xSemaphoreTake(lifetime_mutex_, portMAX_DELAY) != pdTRUE) {
    return;
  }
  bool physical_present = true;
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    // The main loop can be in a 750 ms battery-idle cadence when a cable is
    // attached. Sample SEN_VIN in the real mount callback so a completed host
    // enumeration is never rejected merely because that loop has not run yet.
    physical_present = board_usb_vbus_present();
  }
  const bool mount_accepted =
      endpoint_lifetime_.on_mount_with_physical_presence(physical_present);
  if (mount_accepted) {
    lifetime_reset_pending_ = true;
  }
  xSemaphoreGive(lifetime_mutex_);
  if (!mount_accepted) {
    ESP_LOGW(kTag,
             "TinyUSB mount ignored without physical VBUS present=%u",
             physical_present ? 1U : 0U);
  }
}

void UsbHidTransport::on_tinyusb_unmount() {
  if (lifetime_mutex_ == nullptr ||
      xSemaphoreTake(lifetime_mutex_, portMAX_DELAY) != pdTRUE) {
    return;
  }
  if (endpoint_lifetime_.on_unmount()) {
    lifetime_reset_pending_ = true;
  }
  xSemaphoreGive(lifetime_mutex_);
}

void UsbHidTransport::observe_physical_presence(bool present) {
  if (lifetime_mutex_ == nullptr ||
      xSemaphoreTake(lifetime_mutex_, portMAX_DELAY) != pdTRUE) {
    return;
  }
  const bool invalidated =
      endpoint_lifetime_.observe_physical_presence(present);
  const auto epoch = endpoint_lifetime_.epoch();
  if (invalidated) {
    lifetime_reset_pending_ = true;
  }
  xSemaphoreGive(lifetime_mutex_);

  if (invalidated) {
    ESP_LOGI(kTag,
             "USB endpoint invalidated by physical VBUS loss epoch=%lu",
             static_cast<unsigned long>(epoch));
  }
}

bool UsbHidTransport::lock_current_epoch(
    std::uint32_t expected_epoch) const {
  if (!initialized_ || expected_epoch == 0 || lifetime_mutex_ == nullptr ||
      xSemaphoreTake(lifetime_mutex_, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (!endpoint_lifetime_.mounted() ||
      endpoint_lifetime_.epoch() != expected_epoch ||
      !tud_mounted()) {
    xSemaphoreGive(lifetime_mutex_);
    return false;
  }
  return true;
}

void UsbHidTransport::unlock_lifetime() const {
  xSemaphoreGive(lifetime_mutex_);
}

void UsbHidTransport::apply_pending_lifetime_reset() {
  if (lifetime_mutex_ == nullptr ||
      xSemaphoreTake(lifetime_mutex_, portMAX_DELAY) != pdTRUE) {
    return;
  }
  if (lifetime_reset_pending_) {
    // Queue state is owned by the firmware task. Keep the lifetime mutex held
    // while clearing it so a remount callback cannot publish a new epoch/reset
    // between consuming this flag and the actual clear. Otherwise an enqueue
    // could return success for the new epoch and then be erased by a leftover
    // reset flag on the next poll.
    clear_pending_reports_on_unmount();
    lifetime_reset_pending_ = false;
  }
  xSemaphoreGive(lifetime_mutex_);
}

bool UsbHidTransport::queue_keyboard_report(
    std::uint8_t modifier,
    const std::array<std::uint8_t, 6>& keycodes,
    bool apple_fn,
    ai_keyboard::HidReportClass report_class) {
  return queue_keyboard_report_for_epoch(
      modifier, keycodes, apple_fn, report_class, connection_epoch());
}

bool UsbHidTransport::queue_keyboard_report_for_epoch(
    std::uint8_t modifier,
    const std::array<std::uint8_t, 6>& keycodes,
    bool apple_fn,
    ai_keyboard::HidReportClass report_class,
    std::uint32_t expected_epoch) {
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }
  if (report_class != ai_keyboard::HidReportClass::KeyboardPress &&
      report_class != ai_keyboard::HidReportClass::KeyboardRelease &&
      report_class != ai_keyboard::HidReportClass::KeyboardAllReleased) {
    unlock_lifetime();
    ESP_LOGW(kTag, "USB keyboard queue rejected non-keyboard report class");
    return false;
  }

  std::array<std::uint8_t, ai_keyboard::kKeyboardSnapshotPayloadSize> report{};
  report[0] = modifier;
  report[1] = apple_fn ? 0x01 : 0x00;
  std::copy(keycodes.begin(), keycodes.end(), report.begin() + 2);

  const auto now_ms =
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
  const auto result = pending_keyboard_reports_.push_classified(
      kReportIdKeyboard,
      report.data(),
      report.size(),
      now_ms,
      report_class,
      {},
      expected_epoch);
  unlock_lifetime();
  if (!result.accepted()) {
    if (result.status == ai_keyboard::HidQueuePushStatus::Invalid) {
      ESP_LOGE(kTag,
               "USB keyboard queue invalid class=%u status=%u",
               static_cast<unsigned>(report_class),
               static_cast<unsigned>(result.status));
    } else {
      ESP_LOGD(kTag,
               "USB keyboard queue full class=%u depth=%u",
               static_cast<unsigned>(report_class),
               static_cast<unsigned>(pending_keyboard_reports_.size()));
    }
    return false;
  }
  queued_physical_keyboard_.modifier = modifier;
  queued_physical_keyboard_.apple_fn = apple_fn;
  queued_physical_keyboard_.keycodes = keycodes;
  return true;
}

void UsbHidTransport::poll_keyboard_reports() {
  poll_pending_reports();
}

UsbHidTransport::PollAttemptResult
UsbHidTransport::try_send_keyboard_report() {
  ai_keyboard::QueuedHidReport report{};
  if (!pending_keyboard_reports_.front(&report)) {
    return PollAttemptResult::Empty;
  }
  if (report.report_id != kReportIdKeyboard ||
      report.len != ai_keyboard::kKeyboardSnapshotPayloadSize ||
      report.usb_epoch == 0) {
    ESP_LOGE(kTag,
             "USB keyboard queue invalid report id=%u len=%u",
             static_cast<unsigned>(report.report_id),
             static_cast<unsigned>(report.len));
    pending_keyboard_reports_.pop_if_sequence(report.sequence);
    return PollAttemptResult::Dropped;
  }

  if (!lock_current_epoch(report.usb_epoch)) {
    pending_keyboard_reports_.pop_if_sequence(report.sequence);
    return PollAttemptResult::Dropped;
  }
  const bool accepted =
      tud_hid_report(report.report_id, report.data.data(), report.len);
  unlock_lifetime();
  if (!accepted) {
    return PollAttemptResult::RetryLater;
  }
  pending_keyboard_reports_.pop_if_sequence(report.sequence);
  return PollAttemptResult::Accepted;
}

void UsbHidTransport::clear_pending_reports_on_unmount() {
  if (!pending_keyboard_reports_.empty()) {
    ESP_LOGD(kTag,
             "USB keyboard queue cleared on unmount depth=%u",
             static_cast<unsigned>(pending_keyboard_reports_.size()));
    pending_keyboard_reports_.clear();
  }
  if (!pending_mouse_wheel_reports_.empty()) {
    ESP_LOGD(kTag,
             "USB mouse wheel queue cleared on unmount depth=%u",
             static_cast<unsigned>(pending_mouse_wheel_reports_.size()));
    pending_mouse_wheel_reports_.clear();
  }
  if (status_response_pending()) {
    ESP_LOGW(kTag,
             "USB STATUS response cancelled request=%08lx reason=unmounted",
             static_cast<unsigned long>(status_response_.request_id()));
    reset_status_response();
  }
  status_response_epoch_ = 0;
  if (speaker_assets_response_.active) {
    ESP_LOGD(
        kTag,
        "USB speaker-assets response cancelled sequence=%lu reason=unmounted",
        static_cast<unsigned long>(
            speaker_assets_response_.runtime_reply_sequence));
  }
  reset_speaker_assets_response();
  speaker_assets_sent_sequence_ = 0;
  speaker_assets_sent_epoch_ = 0;
  speaker_assets_sent_ready_ = false;
  if (!pending_app_command_reports_.empty()) {
    ESP_LOGD(kTag,
             "USB app command queue cleared on unmount depth=%u",
             static_cast<unsigned>(pending_app_command_reports_.size()));
    pending_app_command_reports_.clear();
  }
  for (auto& operation : pending_synthetic_keyboard_operations_) {
    operation = {};
  }
  pending_synthetic_keyboard_head_ = 0;
  pending_synthetic_keyboard_size_ = 0;
  queued_physical_keyboard_ = {};
  endpoint_arbiter_ = {};
}

void UsbHidTransport::poll_pending_reports() {
  apply_pending_lifetime_reset();
  if (!mounted()) {
    clear_pending_reports_on_unmount();
    return;
  }
  pump_synthetic_keyboard_reports();
  if (!tud_hid_ready()) {
    return;
  }

  constexpr std::size_t kReportKindCount =
      static_cast<std::size_t>(
          ai_keyboard::UsbHidEndpointReportKind::Count);
  for (std::size_t checked = 0; checked < kReportKindCount; ++checked) {
    const ai_keyboard::UsbHidEndpointPending pending{
        !pending_keyboard_reports_.empty(),
        !pending_mouse_wheel_reports_.empty(),
        !pending_app_command_reports_.empty(),
        status_response_pending(),
        speaker_assets_response_.active,
    };
    const auto kind = endpoint_arbiter_.select(pending);
    if (kind == ai_keyboard::UsbHidEndpointReportKind::Count) {
      return;
    }

    PollAttemptResult result = PollAttemptResult::Empty;
    switch (kind) {
      case ai_keyboard::UsbHidEndpointReportKind::Keyboard:
        result = try_send_keyboard_report();
        break;
      case ai_keyboard::UsbHidEndpointReportKind::MouseWheel:
        result = try_send_mouse_wheel_report();
        break;
      case ai_keyboard::UsbHidEndpointReportKind::AppCommand:
        result = try_send_app_command_report();
        break;
      case ai_keyboard::UsbHidEndpointReportKind::StatusResponse:
        result = try_send_status_response();
        break;
      case ai_keyboard::UsbHidEndpointReportKind::SpeakerAssets:
        result = try_send_speaker_assets_response();
        break;
      case ai_keyboard::UsbHidEndpointReportKind::Count:
        return;
    }

    if (result == PollAttemptResult::Accepted) {
      endpoint_arbiter_.mark_accepted(kind);
      return;
    }
    if (result == PollAttemptResult::RetryLater) {
      // No endpoint credit was consumed. Keep this report class preferred for
      // the next poll instead of falsely rotating on a busy return.
      return;
    }
    // A malformed report can be removed without consuming endpoint credit.
    // Re-evaluate the remaining queues in this same bounded call.
  }
}

void UsbHidTransport::pump_synthetic_keyboard_reports() {
  // Physical snapshots and synthetic snapshots share one FIFO. Only append a
  // synthetic press/restore pair when that FIFO is empty, so the last queued
  // physical snapshot is already the host-visible baseline and a later
  // physical transition can only be appended after the matching restore.
  if (!pending_keyboard_reports_.empty()) {
    return;
  }
  auto* operation = front_synthetic_operation();
  if (operation == nullptr) {
    return;
  }
  const auto expected_epoch = operation->usb_epoch;
  if (expected_epoch == 0) {
    ESP_LOGE(kTag, "USB synthetic keyboard operation missing endpoint epoch");
    pop_synthetic_operation();
    return;
  }

  ai_keyboard::UsbHidKeyboardSnapshot synthetic{};
  if (operation->kind == SyntheticKeyboardOperationKind::Tap) {
    synthetic = operation->tap;
  } else {
    if (operation->next_text_byte >= operation->text.size()) {
      pop_synthetic_operation();
      return;
    }
    const auto report = ai_keyboard::hid_report_for_ascii_char(
        operation->text[operation->next_text_byte]);
    if (!report.valid) {
      ESP_LOGE(kTag,
               "USB synthetic text queue contains unsupported byte index=%u",
               static_cast<unsigned>(operation->next_text_byte));
      pop_synthetic_operation();
      return;
    }
    synthetic.modifier = report.modifier;
    synthetic.apple_fn = report.apple_fn;
    synthetic.keycodes = report.keycodes;
  }

  ai_keyboard::UsbHidSyntheticTapPair pair{};
  if (!ai_keyboard::compose_usb_hid_synthetic_tap(
          queued_physical_keyboard_, synthetic, &pair)) {
    // Six physically held keys leave no 6KRO slot. Preserve both states and
    // retry after a physical release instead of replacing the held snapshot.
    return;
  }

  const auto now_ms =
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
  std::array<std::uint8_t, ai_keyboard::kKeyboardSnapshotPayloadSize>
      pressed_report{};
  pressed_report[0] = pair.pressed.modifier;
  pressed_report[1] = pair.pressed.apple_fn ? 0x01 : 0x00;
  std::copy(pair.pressed.keycodes.begin(),
            pair.pressed.keycodes.end(),
            pressed_report.begin() + 2);
  std::array<std::uint8_t, ai_keyboard::kKeyboardSnapshotPayloadSize>
      restored_report{};
  restored_report[0] = pair.restored.modifier;
  restored_report[1] = pair.restored.apple_fn ? 0x01 : 0x00;
  std::copy(pair.restored.keycodes.begin(),
            pair.restored.keycodes.end(),
            restored_report.begin() + 2);

  // The queue is empty, so both validated reports fit atomically under the
  // press/release reservation contract.
  if (!lock_current_epoch(expected_epoch)) {
    // This operation belonged to an endpoint that no longer exists. A fresh
    // host must never receive stale text or a stale tap.
    pop_synthetic_operation();
    return;
  }
  const auto pressed = pending_keyboard_reports_.push_classified(
      kReportIdKeyboard,
      pressed_report.data(),
      pressed_report.size(),
      now_ms,
      ai_keyboard::HidReportClass::KeyboardPress,
      {},
      expected_epoch);
  const auto restored = pending_keyboard_reports_.push_classified(
      kReportIdKeyboard,
      restored_report.data(),
      restored_report.size(),
      now_ms,
      pair.restored.empty()
          ? ai_keyboard::HidReportClass::KeyboardAllReleased
          : ai_keyboard::HidReportClass::KeyboardRelease,
      {},
      expected_epoch);
  unlock_lifetime();
  if (!pressed.accepted() || !restored.accepted()) {
    ESP_LOGE(kTag,
             "USB synthetic keyboard pair invariant failed press=%u restore=%u",
             static_cast<unsigned>(pressed.status),
             static_cast<unsigned>(restored.status));
    pending_keyboard_reports_.clear();
    return;
  }

  if (operation->kind == SyntheticKeyboardOperationKind::Text) {
    ++operation->next_text_byte;
    if (operation->next_text_byte < operation->text.size()) {
      return;
    }
  }
  pop_synthetic_operation();
}

bool UsbHidTransport::queue_mouse_wheel(std::int8_t vertical,
                                        std::int8_t horizontal,
                                        bool* coalesced) {
  return queue_mouse_wheel_for_epoch(
      vertical, horizontal, connection_epoch(), coalesced);
}

bool UsbHidTransport::queue_mouse_wheel_for_epoch(
    std::int8_t vertical,
    std::int8_t horizontal,
    std::uint32_t expected_epoch,
    bool* coalesced) {
  if (coalesced != nullptr) {
    *coalesced = false;
  }
  if (vertical == 0 && horizontal == 0) {
    return true;
  }
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }

  bool saturated = false;
  const auto now_ms =
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
  const bool queued = pending_mouse_wheel_reports_.push(
      vertical,
      horizontal,
      now_ms,
      nullptr,
      coalesced,
      &saturated,
      {},
      expected_epoch);
  unlock_lifetime();
  if (!queued) {
    ESP_LOGD(kTag,
             "USB mouse wheel queue full depth=%u vertical=%d horizontal=%d",
             static_cast<unsigned>(pending_mouse_wheel_reports_.size()),
             static_cast<int>(vertical),
             static_cast<int>(horizontal));
    return false;
  }
  if (saturated) {
    ESP_LOGD(kTag,
             "USB mouse wheel queue saturated depth=%u vertical=%d horizontal=%d",
             static_cast<unsigned>(pending_mouse_wheel_reports_.size()),
             static_cast<int>(vertical),
             static_cast<int>(horizontal));
  }

  // A mounted USB interface owns this wheel movement even if the shared HID
  // endpoint is momentarily busy. Poll once for the low-latency path; if the
  // endpoint has no credit, the bounded queue retains the movement.
  poll_mouse_wheel_reports();
  return true;
}

void UsbHidTransport::poll_mouse_wheel_reports() {
  poll_pending_reports();
}

UsbHidTransport::PollAttemptResult
UsbHidTransport::try_send_mouse_wheel_report() {
  ai_keyboard::QueuedMouseWheel report{};
  if (!pending_mouse_wheel_reports_.front(&report)) {
    return PollAttemptResult::Empty;
  }
  const auto vertical = static_cast<std::int8_t>(
      std::clamp<std::int32_t>(report.vertical, -127, 127));
  const auto horizontal = static_cast<std::int8_t>(
      std::clamp<std::int32_t>(report.horizontal, -127, 127));
  if (vertical == 0 && horizontal == 0) {
    pending_mouse_wheel_reports_.pop_if_sequence(report.sequence);
    return PollAttemptResult::Dropped;
  }

  // TinyUSB accepting the report is the consumption boundary. A busy endpoint
  // leaves the exact queued displacement intact for the next poll.
  if (report.usb_epoch == 0 ||
      !lock_current_epoch(report.usb_epoch)) {
    pending_mouse_wheel_reports_.pop_if_sequence(report.sequence);
    return PollAttemptResult::Dropped;
  }
  const bool accepted =
      tud_hid_mouse_report(kReportIdMouse, 0, 0, 0, vertical, horizontal);
  unlock_lifetime();
  if (!accepted) {
    return PollAttemptResult::RetryLater;
  }
  pending_mouse_wheel_reports_.consume_if_sequence(
      report.sequence, vertical, horizontal);
  return PollAttemptResult::Accepted;
}

bool UsbHidTransport::mouse_wheel_report_pending() const {
  return !pending_mouse_wheel_reports_.empty();
}

bool UsbHidTransport::take_pending_config(
    std::string* out,
    std::uint32_t* endpoint_epoch) {
  if (out == nullptr || endpoint_epoch == nullptr) {
    return false;
  }

  std::string completed_json;
  std::uint32_t completed_epoch = 0;
  bool ready = false;

  portENTER_CRITICAL(&pending_config_mux_);
  if (pending_config_ready_) {
    completed_json.swap(pending_config_json_);
    completed_epoch = pending_config_epoch_;
    pending_config_ready_ = false;
    pending_config_epoch_ = 0;
    ready = true;
  }
  portEXIT_CRITICAL(&pending_config_mux_);

  if (!ready || completed_json.empty() || completed_epoch == 0) {
    return false;
  }
  out->swap(completed_json);
  *endpoint_epoch = completed_epoch;
  return true;
}

bool UsbHidTransport::take_pending_agent_status(ai_keyboard::AgentStatusCommand* out) {
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

bool UsbHidTransport::take_pending_status_request(
    ai_keyboard::StatusHidRequest* out,
    std::uint32_t* endpoint_epoch) {
  if (out == nullptr || endpoint_epoch == nullptr) {
    return false;
  }

  bool ready = false;
  portENTER_CRITICAL(&pending_status_request_mux_);
  if (pending_status_request_ready_) {
    *out = pending_status_request_;
    *endpoint_epoch = pending_status_request_epoch_;
    pending_status_request_ = {};
    pending_status_request_epoch_ = 0;
    pending_status_request_ready_ = false;
    ready = true;
  }
  portEXIT_CRITICAL(&pending_status_request_mux_);
  return ready;
}

void UsbHidTransport::set_status_request_callback(StatusRequestCallback callback,
                                                  void* context) {
  status_request_callback_ = callback;
  status_request_context_ = context;
}

void UsbHidTransport::set_speaker_assets_frame_callback(
    SpeakerAssetsFrameCallback callback,
    void* context) {
  speaker_assets_frame_callback_ = callback;
  speaker_assets_frame_context_ = context;
}

void UsbHidTransport::set_speaker_assets_response_accepted_callback(
    SpeakerAssetsResponseAcceptedCallback callback,
    void* context) {
  speaker_assets_response_accepted_callback_ = callback;
  speaker_assets_response_accepted_context_ = context;
}

bool UsbHidTransport::queue_speaker_assets_response_for_epoch(
    std::uint32_t runtime_reply_sequence,
    const std::uint8_t* frame,
    std::size_t length,
    std::uint32_t expected_epoch) {
  if (runtime_reply_sequence == 0 || frame == nullptr ||
      length != kSpeakerAssetsReportPayloadLen) {
    return false;
  }
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }
  if (speaker_assets_response_.active || speaker_assets_sent_ready_) {
    unlock_lifetime();
    return false;
  }
  speaker_assets_response_ = {};
  std::copy_n(
      frame, length, speaker_assets_response_.frame.begin());
  speaker_assets_response_.runtime_reply_sequence =
      runtime_reply_sequence;
  speaker_assets_response_.usb_epoch = expected_epoch;
  speaker_assets_response_.active = true;
  unlock_lifetime();
  return true;
}

bool UsbHidTransport::take_speaker_assets_response_sent(
    std::uint32_t* runtime_reply_sequence,
    std::uint32_t* endpoint_epoch) {
  if (runtime_reply_sequence == nullptr || endpoint_epoch == nullptr ||
      !speaker_assets_sent_ready_ ||
      speaker_assets_sent_sequence_ == 0 ||
      speaker_assets_sent_epoch_ == 0) {
    return false;
  }
  *runtime_reply_sequence = speaker_assets_sent_sequence_;
  *endpoint_epoch = speaker_assets_sent_epoch_;
  speaker_assets_sent_sequence_ = 0;
  speaker_assets_sent_epoch_ = 0;
  speaker_assets_sent_ready_ = false;
  return true;
}

bool UsbHidTransport::queue_status_response(std::uint32_t request_id,
                                            const std::string& status_json) {
  return queue_status_response_for_epoch(
      request_id, status_json, connection_epoch());
}

bool UsbHidTransport::queue_status_response_for_epoch(
    std::uint32_t request_id,
    const std::string& status_json,
    std::uint32_t expected_epoch) {
  const auto total_chunks =
      ai_keyboard::status_hid_response_chunk_count(status_json.size());
  if (request_id == 0 || total_chunks == 0) {
    ESP_LOGW(kTag,
             "USB STATUS response invalid request=%08lx bytes=%u",
             static_cast<unsigned long>(request_id),
             static_cast<unsigned>(status_json.size()));
    return false;
  }
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }

  if (status_response_.pending()) {
    ESP_LOGW(kTag,
             "USB STATUS response superseded active_request=%08lx new_request=%08lx",
             static_cast<unsigned long>(status_response_.request_id()),
             static_cast<unsigned long>(request_id));
  }
  if (!status_response_.replace(request_id, status_json)) {
    unlock_lifetime();
    return false;
  }
  status_response_epoch_ = expected_epoch;
  unlock_lifetime();
  ESP_LOGI(kTag,
           "USB STATUS response queued request=%08lx bytes=%u chunks=%u",
           static_cast<unsigned long>(request_id),
           static_cast<unsigned>(status_json.size()),
           static_cast<unsigned>(total_chunks));
  return true;
}

void UsbHidTransport::poll_status_response() {
  poll_pending_reports();
}

UsbHidTransport::PollAttemptResult
UsbHidTransport::try_send_app_command_report() {
  ai_keyboard::QueuedHidReport report{};
  if (!pending_app_command_reports_.front(&report)) {
    return PollAttemptResult::Empty;
  }
  if (report.report_id != kReportIdAppCommand ||
      report.len != kAppCommandReportPayloadLen ||
      report.usb_epoch == 0) {
    ESP_LOGE(kTag,
             "USB app command queue invalid report id=%u len=%u",
             static_cast<unsigned>(report.report_id),
             static_cast<unsigned>(report.len));
    pending_app_command_reports_.pop_if_sequence(report.sequence);
    return PollAttemptResult::Dropped;
  }

  // TinyUSB acceptance is the only consumption boundary. Endpoint busy keeps
  // this exact FIFO head and arbiter preference for the next poll.
  if (!lock_current_epoch(report.usb_epoch)) {
    pending_app_command_reports_.pop_if_sequence(report.sequence);
    return PollAttemptResult::Dropped;
  }
  const bool accepted =
      tud_hid_report(report.report_id, report.data.data(), report.len);
  unlock_lifetime();
  if (!accepted) {
    return PollAttemptResult::RetryLater;
  }
  pending_app_command_reports_.pop_if_sequence(report.sequence);
  return PollAttemptResult::Accepted;
}

UsbHidTransport::PollAttemptResult
UsbHidTransport::try_send_status_response() {
  if (!status_response_pending()) {
    return PollAttemptResult::Empty;
  }

  std::array<std::uint8_t, ai_keyboard::kStatusAppCommandPayloadLen> report{};
  if (!status_response_.encode_next(&report)) {
    ESP_LOGE(kTag,
             "USB STATUS response encode failed request=%08lx chunk=%u",
             static_cast<unsigned long>(status_response_.request_id()),
             static_cast<unsigned>(status_response_.next_chunk()));
    reset_status_response();
    return PollAttemptResult::Dropped;
  }
  if (status_response_epoch_ == 0 ||
      !lock_current_epoch(status_response_epoch_)) {
    reset_status_response();
    status_response_epoch_ = 0;
    return PollAttemptResult::Dropped;
  }
  const bool accepted =
      tud_hid_report(kReportIdAppCommand, report.data(), report.size());
  unlock_lifetime();
  if (!accepted) {
    return PollAttemptResult::RetryLater;
  }

  const auto completed_request_id = status_response_.request_id();
  const auto completed_json_size = status_response_.json_size();
  const auto completed_chunks = status_response_.total_chunks();
  if (status_response_.mark_next_sent()) {
    status_response_epoch_ = 0;
    ESP_LOGI(kTag,
             "USB STATUS response complete request=%08lx bytes=%u chunks=%u",
             static_cast<unsigned long>(completed_request_id),
             static_cast<unsigned>(completed_json_size),
             static_cast<unsigned>(completed_chunks));
  }
  return PollAttemptResult::Accepted;
}

UsbHidTransport::PollAttemptResult
UsbHidTransport::try_send_speaker_assets_response() {
  if (!speaker_assets_response_.active) {
    return PollAttemptResult::Empty;
  }
  if (speaker_assets_response_.runtime_reply_sequence == 0 ||
      speaker_assets_response_.usb_epoch == 0) {
    reset_speaker_assets_response();
    return PollAttemptResult::Dropped;
  }
  if (!lock_current_epoch(speaker_assets_response_.usb_epoch)) {
    reset_speaker_assets_response();
    return PollAttemptResult::Dropped;
  }
  const auto sent_sequence =
      speaker_assets_response_.runtime_reply_sequence;
  const auto sent_epoch = speaker_assets_response_.usb_epoch;
  const bool accepted = tud_hid_report(
      kReportIdSpeakerAssetsResponse,
      speaker_assets_response_.frame.data(),
      speaker_assets_response_.frame.size());
  if (!accepted) {
    unlock_lifetime();
    return PollAttemptResult::RetryLater;
  }

  // Keep the endpoint lifetime locked until the supervisor has discharged the
  // exact logical-request admission. A following SET_REPORT takes this same
  // lock, so it cannot observe the old reservation after the host-visible
  // response has entered TinyUSB's endpoint queue.
  const bool retired_synchronously =
      speaker_assets_response_accepted_callback_ != nullptr &&
      speaker_assets_response_accepted_callback_(
          speaker_assets_response_accepted_context_,
          sent_epoch,
          sent_sequence);
  if (!retired_synchronously) {
    // Preserve the owner-task polling path as a fail-closed fallback for an
    // absent callback or an unexpected sequence/epoch mismatch.
    speaker_assets_sent_sequence_ = sent_sequence;
    speaker_assets_sent_epoch_ = sent_epoch;
    speaker_assets_sent_ready_ = true;
  }
  reset_speaker_assets_response();
  unlock_lifetime();
  return PollAttemptResult::Accepted;
}

bool UsbHidTransport::status_response_pending() const {
  return status_response_.pending();
}

void UsbHidTransport::receive_config_report(const std::uint8_t* data, std::size_t len) {
  const auto endpoint_epoch = connection_epoch();
  if (!lock_current_epoch(endpoint_epoch)) {
    ESP_LOGW(kTag,
             "USB CONFIG ignored without current endpoint len=%u",
             static_cast<unsigned>(len));
    return;
  }
  auto result = config_receiver_.receive(endpoint_epoch, data, len);
  if (result.status == ai_keyboard::ConfigReceiveStatus::Complete) {
    queue_completed_config(std::move(result.json), endpoint_epoch);
  }
  unlock_lifetime();
  ESP_LOGI(kTag,
           "USB CONFIG rx len=%u status=%d epoch=%lu",
           static_cast<unsigned>(len),
           static_cast<int>(result.status),
           static_cast<unsigned long>(endpoint_epoch));
}

void UsbHidTransport::receive_agent_status_report(const std::uint8_t* data,
                                                  std::size_t len) {
  const auto endpoint_epoch = connection_epoch();
  if (!lock_current_epoch(endpoint_epoch)) {
    ESP_LOGW(kTag,
             "USB AGENT ignored without current endpoint len=%u",
             static_cast<unsigned>(len));
    return;
  }
  ai_keyboard::AgentStatusCommand command{};
  if (!ai_keyboard::decode_agent_status(data, len, &command)) {
    unlock_lifetime();
    ESP_LOGW(kTag, "USB AGENT invalid len=%u", static_cast<unsigned>(len));
    return;
  }

  portENTER_CRITICAL(&pending_agent_status_mux_);
  pending_agent_status_ = command;
  pending_agent_status_ready_ = true;
  portEXIT_CRITICAL(&pending_agent_status_mux_);
  unlock_lifetime();
  ESP_LOGI(kTag,
           "USB AGENT state=%u seq=%lu ttl=%lums epoch=%lu",
           static_cast<unsigned>(command.state),
           static_cast<unsigned long>(command.sequence),
           static_cast<unsigned long>(command.ttl_ms),
           static_cast<unsigned long>(endpoint_epoch));
}

void UsbHidTransport::receive_status_request_report(const std::uint8_t* data,
                                                    std::size_t len) {
  ai_keyboard::StatusHidRequest request{};
  if (!ai_keyboard::decode_status_hid_request(data, len, &request)) {
    ESP_LOGW(kTag, "USB STATUS invalid len=%u", static_cast<unsigned>(len));
    return;
  }
  const auto endpoint_epoch = connection_epoch();
  if (!lock_current_epoch(endpoint_epoch)) {
    ESP_LOGW(kTag,
             "USB STATUS request ignored without mounted endpoint request=%08lx",
             static_cast<unsigned long>(request.request_id));
    return;
  }

  bool replaced = false;
  portENTER_CRITICAL(&pending_status_request_mux_);
  replaced = pending_status_request_ready_;
  pending_status_request_ = request;
  pending_status_request_epoch_ = endpoint_epoch;
  pending_status_request_ready_ = true;
  portEXIT_CRITICAL(&pending_status_request_mux_);
  unlock_lifetime();

  ESP_LOGI(kTag,
           "USB STATUS request accepted request=%08lx flags=%02x replaced_pending=%u",
           static_cast<unsigned long>(request.request_id),
           static_cast<unsigned>(request.flags),
           replaced ? 1U : 0U);
  if (status_request_callback_ != nullptr) {
    status_request_callback_(status_request_context_);
  }
}

void UsbHidTransport::receive_speaker_assets_report(
    const std::uint8_t* data,
    std::size_t len) {
  if (data == nullptr || speaker_assets_frame_callback_ == nullptr) {
    return;
  }

  const std::uint8_t* frame = data;
  std::size_t frame_length = len;
  if (len == kSpeakerAssetsReportPayloadLen + 1U &&
      data[0] == kReportIdSpeakerAssetsRequest) {
    frame = data + 1U;
    frame_length = len - 1U;
  }
  if (frame_length != kSpeakerAssetsReportPayloadLen) {
    ESP_LOGW(kTag,
             "USB speaker-assets invalid len=%u",
             static_cast<unsigned>(len));
    return;
  }

  const auto endpoint_epoch = connection_epoch();
  if (!lock_current_epoch(endpoint_epoch)) {
    return;
  }
  const bool accepted = speaker_assets_frame_callback_(
      speaker_assets_frame_context_,
      endpoint_epoch,
      frame,
      frame_length);
  unlock_lifetime();
  if (!accepted) {
    ESP_LOGD(
        kTag,
        "USB speaker-assets request backpressured epoch=%lu",
        static_cast<unsigned long>(endpoint_epoch));
  }
}

void UsbHidTransport::queue_completed_config(
    std::string json,
    std::uint32_t endpoint_epoch) {
  if (endpoint_epoch == 0 || json.empty() ||
      json.size() > ai_keyboard::kConfigMaxJsonLen) {
    return;
  }
  portENTER_CRITICAL(&pending_config_mux_);
  pending_config_json_.swap(json);
  pending_config_epoch_ = endpoint_epoch;
  pending_config_ready_ = true;
  portEXIT_CRITICAL(&pending_config_mux_);
}

bool UsbHidTransport::send_firmware_event(const char* source,
                                          const ai_keyboard::FirmwareEvent& event) {
  return send_firmware_event_for_epoch(
      source, event, connection_epoch());
}

bool UsbHidTransport::send_firmware_event_for_epoch(
    const char* source,
    const ai_keyboard::FirmwareEvent& event,
    std::uint32_t expected_epoch) {
  if (event.kind == ai_keyboard::FirmwareEventKind::None) {
    return true;
  }
  if (expected_epoch == 0) {
    return false;
  }

  switch (event.kind) {
    case ai_keyboard::FirmwareEventKind::None:
      return true;
    case ai_keyboard::FirmwareEventKind::HidKeyDown:
      ESP_LOGI(kTag, "ACTION %s hid_down %s", source, event.value.c_str());
      if (event.bridge_app_hotkey && should_bridge_hotkey_to_app_command(event.value)) {
        return send_hotkey_app_command(event.value, true, expected_epoch);
      }
      ESP_LOGE(kTag,
               "USB stateful key down bypassed HeldKeyboardState value=%s",
               event.value.c_str());
      return false;
    case ai_keyboard::FirmwareEventKind::HidKeyUp:
      ESP_LOGI(kTag, "ACTION %s hid_up %s", source, event.value.c_str());
      if (event.bridge_app_hotkey && should_bridge_hotkey_to_app_command(event.value)) {
        return send_hotkey_app_command(event.value, false, expected_epoch);
      }
      ESP_LOGE(kTag,
               "USB stateful key up bypassed HeldKeyboardState value=%s",
               event.value.c_str());
      return false;
    case ai_keyboard::FirmwareEventKind::HidTap:
      ESP_LOGI(kTag, "ACTION %s hid_tap %s", source, event.value.c_str());
      if (event.bridge_app_hotkey && should_bridge_hotkey_to_app_command(event.value)) {
        if (!send_hotkey_app_command(event.value, true, expected_epoch)) {
          return false;
        }
        vTaskDelay(delay_ticks(15));
        return send_hotkey_app_command(event.value, false, expected_epoch);
      }
      return tap_hotkey(event.value, expected_epoch);
    case ai_keyboard::FirmwareEventKind::FixedText:
      ESP_LOGI(kTag,
               "ACTION %s fixed_text_usb bytes=%u",
               source,
               static_cast<unsigned>(event.value.size()));
      if (can_type_text_as_keyboard(event.value)) {
        return type_text(event.value, expected_epoch);
      }
      return send_fixed_text_command(event.value, expected_epoch);
    case ai_keyboard::FirmwareEventKind::AppCommand:
      return false;
  }
  return false;
}

bool UsbHidTransport::tap_hotkey(const std::string& hotkey,
                                 std::uint32_t expected_epoch) {
  const auto report = ai_keyboard::hid_report_for_hotkey(hotkey);
  if (!report.valid) {
    ESP_LOGW(kTag, "USB HID unsupported_hotkey %s", hotkey.c_str());
    return false;
  }

  ai_keyboard::UsbHidKeyboardSnapshot tap{};
  tap.modifier = report.modifier;
  tap.apple_fn = report.apple_fn;
  tap.keycodes = report.keycodes;
  return queue_synthetic_tap(tap, expected_epoch);
}

bool UsbHidTransport::type_text(const std::string& text,
                                std::uint32_t expected_epoch) {
  if (expected_epoch == 0) {
    return false;
  }

  for (const char ch : text) {
    if (!ai_keyboard::hid_report_for_ascii_char(ch).valid) {
      ESP_LOGW(kTag, "USB HID fixed_text contains unsupported byte 0x%02X",
               static_cast<unsigned>(static_cast<unsigned char>(ch)));
      return false;
    }
  }
  return queue_synthetic_text(text, expected_epoch);
}

bool UsbHidTransport::queue_synthetic_tap(
    const ai_keyboard::UsbHidKeyboardSnapshot& tap,
    std::uint32_t expected_epoch) {
  if (tap.empty()) {
    return false;
  }
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }
  SyntheticKeyboardOperation operation{};
  operation.kind = SyntheticKeyboardOperationKind::Tap;
  operation.tap = tap;
  operation.usb_epoch = expected_epoch;
  const bool queued = push_synthetic_operation(std::move(operation));
  unlock_lifetime();
  if (!queued) {
    ESP_LOGD(kTag,
             "USB synthetic keyboard queue full kind=tap depth=%u",
             static_cast<unsigned>(pending_synthetic_keyboard_size_));
    return false;
  }
  poll_pending_reports();
  return true;
}

bool UsbHidTransport::queue_synthetic_text(
    const std::string& text,
    std::uint32_t expected_epoch) {
  if (expected_epoch == 0) {
    return false;
  }
  if (text.empty()) {
    return true;
  }
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }
  SyntheticKeyboardOperation operation{};
  operation.kind = SyntheticKeyboardOperationKind::Text;
  operation.text = text;
  operation.usb_epoch = expected_epoch;
  const bool queued = push_synthetic_operation(std::move(operation));
  unlock_lifetime();
  if (!queued) {
    ESP_LOGD(kTag,
             "USB synthetic keyboard queue full kind=text depth=%u",
             static_cast<unsigned>(pending_synthetic_keyboard_size_));
    return false;
  }
  poll_pending_reports();
  return true;
}

bool UsbHidTransport::push_synthetic_operation(
    SyntheticKeyboardOperation operation) {
  if (pending_synthetic_keyboard_size_ >=
      pending_synthetic_keyboard_operations_.size()) {
    return false;
  }
  const auto tail =
      (pending_synthetic_keyboard_head_ + pending_synthetic_keyboard_size_) %
      pending_synthetic_keyboard_operations_.size();
  pending_synthetic_keyboard_operations_[tail] = std::move(operation);
  ++pending_synthetic_keyboard_size_;
  return true;
}

UsbHidTransport::SyntheticKeyboardOperation*
UsbHidTransport::front_synthetic_operation() {
  if (pending_synthetic_keyboard_size_ == 0) {
    return nullptr;
  }
  return &pending_synthetic_keyboard_operations_[
      pending_synthetic_keyboard_head_];
}

void UsbHidTransport::pop_synthetic_operation() {
  if (pending_synthetic_keyboard_size_ == 0) {
    return;
  }
  pending_synthetic_keyboard_operations_[pending_synthetic_keyboard_head_] =
      {};
  pending_synthetic_keyboard_head_ =
      (pending_synthetic_keyboard_head_ + 1) %
      pending_synthetic_keyboard_operations_.size();
  --pending_synthetic_keyboard_size_;
}

bool UsbHidTransport::send_config_ack(std::uint8_t phase_code,
                                      bool ok,
                                      std::uint16_t bytes,
                                      std::uint16_t crc16,
                                      bool saved) {
  return send_config_ack_for_epoch(
      phase_code, ok, bytes, crc16, saved, connection_epoch());
}

bool UsbHidTransport::send_config_ack_for_epoch(
    std::uint8_t phase_code,
    bool ok,
    std::uint16_t bytes,
    std::uint16_t crc16,
    bool saved,
    std::uint32_t expected_epoch) {
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
                                 expected_epoch);
}

bool UsbHidTransport::send_app_command_report(std::uint8_t command_kind,
                                              std::uint8_t chunk_index,
                                              std::uint8_t total_chunks,
                                              const std::uint8_t* data,
                                              std::size_t len,
                                              std::uint32_t expected_epoch) {
  if (!queue_app_command_report(
          command_kind,
          chunk_index,
          total_chunks,
          data,
          len,
          2,
          expected_epoch)) {
    return false;
  }
  poll_pending_reports();
  return true;
}

bool UsbHidTransport::queue_app_command_report(
    std::uint8_t command_kind,
    std::uint8_t chunk_index,
    std::uint8_t total_chunks,
    const std::uint8_t* data,
    std::size_t len,
    std::size_t reserved_free_slots,
    std::uint32_t expected_epoch) {
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }
  const bool queued = push_app_command_report_locked(
      command_kind,
      chunk_index,
      total_chunks,
      data,
      len,
      reserved_free_slots,
      expected_epoch);
  unlock_lifetime();
  return queued;
}

bool UsbHidTransport::push_app_command_report_locked(
    std::uint8_t command_kind,
    std::uint8_t chunk_index,
    std::uint8_t total_chunks,
    const std::uint8_t* data,
    std::size_t len,
    std::size_t reserved_free_slots,
    std::uint32_t expected_epoch) {
  if (len > kAppCommandChunkDataLen) {
    ESP_LOGW(kTag,
             "USB APP_COMMAND chunk_too_large kind=%u len=%u",
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
  const auto now_ms =
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
  const bool queued = pending_app_command_reports_.push(
          kReportIdAppCommand,
          report.data(),
          report.size(),
          now_ms,
          nullptr,
          reserved_free_slots,
          {},
          expected_epoch);
  if (!queued) {
    ESP_LOGD(kTag,
             "USB APP_COMMAND queue full kind=%u depth=%u",
             static_cast<unsigned>(command_kind),
             static_cast<unsigned>(pending_app_command_reports_.size()));
    return false;
  }
  return true;
}

bool UsbHidTransport::send_fixed_text_command(
    const std::string& text,
    std::uint32_t expected_epoch) {
  if (expected_epoch == 0) {
    return false;
  }
  if (text.empty()) {
    return true;
  }
  if (text.size() > ai_keyboard::kFixedTextMaxUtf8Bytes) {
    ESP_LOGW(kTag,
             "USB APP_COMMAND fixed_text_too_large bytes=%u max=%u",
             static_cast<unsigned>(text.size()),
             static_cast<unsigned>(
                 ai_keyboard::kFixedTextMaxUtf8Bytes));
    return false;
  }

  const auto total_chunks = (text.size() + kAppCommandChunkDataLen - 1) / kAppCommandChunkDataLen;
  if (total_chunks == 0 || total_chunks > 255) {
    ESP_LOGW(kTag,
             "USB APP_COMMAND fixed_text_too_large bytes=%u chunks=%u",
             static_cast<unsigned>(text.size()),
             static_cast<unsigned>(total_chunks));
    return false;
  }
  constexpr std::size_t kAppCommandStatefulReserve = 2;
  apply_pending_lifetime_reset();
  if (!lock_current_epoch(expected_epoch)) {
    return false;
  }
  if (pending_app_command_reports_.size() + total_chunks >
      ai_keyboard::kHidReportQueueCapacity - kAppCommandStatefulReserve) {
    ESP_LOGD(kTag,
             "USB APP_COMMAND fixed_text_queue_full bytes=%u chunks=%u depth=%u",
             static_cast<unsigned>(text.size()),
             static_cast<unsigned>(total_chunks),
             static_cast<unsigned>(pending_app_command_reports_.size()));
    unlock_lifetime();
    return false;
  }

  const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
  for (std::size_t index = 0; index < total_chunks; ++index) {
    const auto offset = index * kAppCommandChunkDataLen;
    const auto len = std::min(kAppCommandChunkDataLen, text.size() - offset);
    if (!push_app_command_report_locked(
            kAppCommandKindFixedText,
            static_cast<std::uint8_t>(index),
            static_cast<std::uint8_t>(total_chunks),
            bytes + offset,
            len,
            kAppCommandStatefulReserve,
            expected_epoch)) {
      // The capacity preflight and lifetime lock make partial insertion
      // impossible unless an internal queue invariant is violated.
      ESP_LOGE(kTag,
               "USB APP_COMMAND fixed text atomic enqueue invariant failed chunk=%u",
               static_cast<unsigned>(index));
      unlock_lifetime();
      return false;
    }
  }
  unlock_lifetime();
  poll_pending_reports();
  return true;
}

bool UsbHidTransport::send_hotkey_app_command(
    const std::string& hotkey,
    bool pressed,
    std::uint32_t expected_epoch) {
  if (expected_epoch == 0) {
    return false;
  }
  if (hotkey.empty() || hotkey.size() > kAppCommandChunkDataLen - 1) {
    ESP_LOGW(kTag,
             "USB APP_COMMAND hotkey_invalid bytes=%u",
             static_cast<unsigned>(hotkey.size()));
    return false;
  }

  std::array<std::uint8_t, kAppCommandChunkDataLen> payload{};
  payload[0] = pressed ? kAppCommandHotkeyPressed : kAppCommandHotkeyReleased;
  std::copy(hotkey.begin(), hotkey.end(), payload.begin() + 1);
  ESP_LOGI(kTag,
           "USB APP_COMMAND hotkey %s %s",
           hotkey.c_str(),
           pressed ? "pressed" : "released");
  // Leave one immediate FIFO slot after a press when possible. Correctness
  // does not depend on this optimization: AppContext retains each source's
  // accepted press and retries its matching release until this queue accepts
  // it, including when multiple bridged sources overlap.
  if (!queue_app_command_report(kAppCommandKindHotkey,
                                0,
                                1,
                                payload.data(),
                                hotkey.size() + 1,
                                pressed ? 1 : 0,
                                expected_epoch)) {
    return false;
  }
  poll_pending_reports();
  return true;
}

void UsbHidTransport::reset_status_response() {
  status_response_.reset();
  status_response_epoch_ = 0;
}

void UsbHidTransport::reset_speaker_assets_response() {
  speaker_assets_response_ = {};
}

}  // namespace easy_input

extern "C" void tud_mount_cb(void) {
  if (easy_input::s_active_transport != nullptr) {
    easy_input::s_active_transport->on_tinyusb_mount();
  }
}

extern "C" void tud_umount_cb(void) {
  if (easy_input::s_active_transport != nullptr) {
    easy_input::s_active_transport->on_tinyusb_unmount();
  }
}

extern "C" uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return easy_input::kHidReportDescriptor;
}

extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance,
                                           uint8_t report_id,
                                           hid_report_type_t report_type,
                                           uint8_t* buffer,
                                           uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}

extern "C" void tud_hid_set_report_cb(uint8_t instance,
                                       uint8_t report_id,
                                       hid_report_type_t report_type,
                                       uint8_t const* buffer,
                                       uint16_t bufsize) {
  (void)instance;
  if (report_id == easy_input::kReportIdConfig && easy_input::s_active_transport != nullptr) {
    easy_input::s_active_transport->receive_config_report(buffer, bufsize);
  } else if (report_id == easy_input::kReportIdAgentStatus &&
             easy_input::s_active_transport != nullptr) {
    easy_input::s_active_transport->receive_agent_status_report(buffer, bufsize);
  } else if (report_type == HID_REPORT_TYPE_FEATURE &&
             easy_input::s_active_transport != nullptr &&
             (report_id == easy_input::kReportIdStatusRequest ||
              (bufsize == ai_keyboard::kStatusRequestPayloadLen + 1 &&
               buffer != nullptr &&
               buffer[0] == easy_input::kReportIdStatusRequest))) {
    easy_input::s_active_transport->receive_status_request_report(buffer, bufsize);
  } else if (report_type == HID_REPORT_TYPE_FEATURE &&
             easy_input::s_active_transport != nullptr &&
             (report_id == easy_input::kReportIdSpeakerAssetsRequest ||
              (bufsize == easy_input::kSpeakerAssetsReportPayloadLen + 1U &&
               buffer != nullptr &&
               buffer[0] ==
                   easy_input::kReportIdSpeakerAssetsRequest))) {
    easy_input::s_active_transport->receive_speaker_assets_report(
        buffer, bufsize);
  }
}
