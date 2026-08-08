#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "keyboard/agent_status.h"
#include "keyboard/config_receiver.h"
#include "keyboard/hid_report_queue.h"
#include "keyboard/keymap.h"
#include "keyboard/status_hid_protocol.h"
#include "keyboard/transport_routing.h"
#include "keyboard/usb_hid_endpoint_arbiter.h"

namespace easy_input {

class UsbHidTransport {
 public:
  using StatusRequestCallback = void (*)(void* context);
  using SpeakerAssetsFrameCallback = bool (*)(
      void* context,
      std::uint32_t endpoint_epoch,
      const std::uint8_t* frame,
      std::size_t length);
  // Runs synchronously while the endpoint lifetime mutex is held. The callback
  // must not call back into UsbHidTransport; it may only retire owner state.
  using SpeakerAssetsResponseAcceptedCallback = bool (*)(
      void* context,
      std::uint32_t endpoint_epoch,
      std::uint32_t runtime_reply_sequence);

  esp_err_t begin();
  bool mounted() const;
  bool ready() const;
  std::uint32_t connection_epoch() const;

  // TinyUSB lifecycle callbacks. They only advance the monotonic endpoint
  // lifetime and request a deferred queue reset; platform queues remain owned
  // by the main firmware task.
  void on_tinyusb_mount();
  void on_tinyusb_unmount();
  // Applies the debounced board VBUS truth source. Physical loss revokes the
  // stale TinyUSB lifetime immediately; recovery still requires a real mount.
  void observe_physical_presence(bool present);
  bool take_pending_config(std::string* out, std::uint32_t* endpoint_epoch);
  bool take_pending_agent_status(ai_keyboard::AgentStatusCommand* out);
  bool take_pending_status_request(ai_keyboard::StatusHidRequest* out,
                                   std::uint32_t* endpoint_epoch);
  void set_status_request_callback(StatusRequestCallback callback, void* context);
  void set_speaker_assets_frame_callback(
      SpeakerAssetsFrameCallback callback,
      void* context);
  void set_speaker_assets_response_accepted_callback(
      SpeakerAssetsResponseAcceptedCallback callback,
      void* context);
  bool queue_speaker_assets_response_for_epoch(
      std::uint32_t runtime_reply_sequence,
      const std::uint8_t* frame,
      std::size_t length,
      std::uint32_t expected_epoch);
  bool take_speaker_assets_response_sent(
      std::uint32_t* runtime_reply_sequence,
      std::uint32_t* endpoint_epoch);
  bool queue_status_response(std::uint32_t request_id, const std::string& status_json);
  bool queue_status_response_for_epoch(std::uint32_t request_id,
                                       const std::string& status_json,
                                       std::uint32_t expected_epoch);
  void poll_pending_reports();
  void poll_status_response();
  bool status_response_pending() const;

  bool send_firmware_event(const char* source, const ai_keyboard::FirmwareEvent& event);
  bool send_firmware_event_for_epoch(
      const char* source,
      const ai_keyboard::FirmwareEvent& event,
      std::uint32_t expected_epoch);
  // USB 配置回执:经 0x11 输入报文回传保存结果与 bytes/crc16 指纹。
  bool send_config_ack(std::uint8_t phase_code,
                       bool ok,
                       std::uint16_t bytes,
                       std::uint16_t crc16,
                       bool saved);
  bool send_config_ack_for_epoch(std::uint8_t phase_code,
                                 bool ok,
                                 std::uint16_t bytes,
                                 std::uint16_t crc16,
                                 bool saved,
                                 std::uint32_t expected_epoch);
  bool queue_keyboard_report(std::uint8_t modifier,
                             const std::array<std::uint8_t, 6>& keycodes,
                             bool apple_fn,
                             ai_keyboard::HidReportClass report_class);
  bool queue_keyboard_report_for_epoch(
      std::uint8_t modifier,
      const std::array<std::uint8_t, 6>& keycodes,
      bool apple_fn,
      ai_keyboard::HidReportClass report_class,
      std::uint32_t expected_epoch);
  void poll_keyboard_reports();
  bool queue_mouse_wheel(std::int8_t vertical,
                         std::int8_t horizontal,
                         bool* coalesced = nullptr);
  bool queue_mouse_wheel_for_epoch(std::int8_t vertical,
                                   std::int8_t horizontal,
                                   std::uint32_t expected_epoch,
                                   bool* coalesced = nullptr);
  void poll_mouse_wheel_reports();
  bool mouse_wheel_report_pending() const;
  void receive_config_report(const std::uint8_t* data, std::size_t len);
  void receive_agent_status_report(const std::uint8_t* data, std::size_t len);
  void receive_status_request_report(const std::uint8_t* data, std::size_t len);
  void receive_speaker_assets_report(
      const std::uint8_t* data,
      std::size_t len);

 private:
  enum class PollAttemptResult : std::uint8_t {
    Empty,
    Accepted,
    RetryLater,
    Dropped,
  };

  enum class SyntheticKeyboardOperationKind : std::uint8_t {
    Tap,
    Text,
  };

  struct SyntheticKeyboardOperation {
    SyntheticKeyboardOperationKind kind =
        SyntheticKeyboardOperationKind::Tap;
    ai_keyboard::UsbHidKeyboardSnapshot tap;
    std::string text;
    std::size_t next_text_byte = 0;
    std::uint32_t usb_epoch = 0;
  };

  static constexpr std::size_t kSyntheticKeyboardOperationCapacity = 16;

  void queue_completed_config(std::string json,
                              std::uint32_t endpoint_epoch);
  PollAttemptResult try_send_keyboard_report();
  PollAttemptResult try_send_mouse_wheel_report();
  PollAttemptResult try_send_app_command_report();
  PollAttemptResult try_send_status_response();
  PollAttemptResult try_send_speaker_assets_response();
  void pump_synthetic_keyboard_reports();
  void clear_pending_reports_on_unmount();
  void apply_pending_lifetime_reset();
  bool lock_current_epoch(std::uint32_t expected_epoch) const;
  void unlock_lifetime() const;
  bool queue_synthetic_tap(const ai_keyboard::UsbHidKeyboardSnapshot& tap,
                           std::uint32_t expected_epoch);
  bool queue_synthetic_text(const std::string& text,
                            std::uint32_t expected_epoch);
  bool push_synthetic_operation(SyntheticKeyboardOperation operation);
  SyntheticKeyboardOperation* front_synthetic_operation();
  void pop_synthetic_operation();
  bool tap_hotkey(const std::string& hotkey,
                  std::uint32_t expected_epoch);
  bool type_text(const std::string& text,
                 std::uint32_t expected_epoch);
  bool queue_app_command_report(std::uint8_t command_kind,
                                std::uint8_t chunk_index,
                                std::uint8_t total_chunks,
                                const std::uint8_t* data,
                                std::size_t len,
                                std::size_t reserved_free_slots,
                                std::uint32_t expected_epoch);
  bool push_app_command_report_locked(std::uint8_t command_kind,
                                      std::uint8_t chunk_index,
                                      std::uint8_t total_chunks,
                                      const std::uint8_t* data,
                                      std::size_t len,
                                      std::size_t reserved_free_slots,
                                      std::uint32_t expected_epoch);
  bool send_app_command_report(std::uint8_t command_kind,
                               std::uint8_t chunk_index,
                               std::uint8_t total_chunks,
                               const std::uint8_t* data,
                               std::size_t len,
                               std::uint32_t expected_epoch);
  bool send_fixed_text_command(const std::string& text,
                               std::uint32_t expected_epoch);
  bool send_hotkey_app_command(const std::string& hotkey,
                               bool pressed,
                               std::uint32_t expected_epoch);
  void reset_status_response();
  void reset_speaker_assets_response();

  struct PendingSpeakerAssetsResponse {
    std::array<std::uint8_t, 63> frame{};
    std::uint32_t runtime_reply_sequence = 0;
    std::uint32_t usb_epoch = 0;
    bool active = false;
  };

  ai_keyboard::EndpointBoundConfigReceiver config_receiver_;
  mutable portMUX_TYPE pending_config_mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::string pending_config_json_;
  std::uint32_t pending_config_epoch_ = 0;
  bool pending_config_ready_ = false;

  mutable portMUX_TYPE pending_agent_status_mux_ = portMUX_INITIALIZER_UNLOCKED;
  ai_keyboard::AgentStatusCommand pending_agent_status_{};
  bool pending_agent_status_ready_ = false;

  mutable portMUX_TYPE pending_status_request_mux_ = portMUX_INITIALIZER_UNLOCKED;
  ai_keyboard::StatusHidRequest pending_status_request_{};
  std::uint32_t pending_status_request_epoch_ = 0;
  bool pending_status_request_ready_ = false;
  StatusRequestCallback status_request_callback_ = nullptr;
  void* status_request_context_ = nullptr;
  SpeakerAssetsFrameCallback speaker_assets_frame_callback_ = nullptr;
  void* speaker_assets_frame_context_ = nullptr;
  SpeakerAssetsResponseAcceptedCallback
      speaker_assets_response_accepted_callback_ = nullptr;
  void* speaker_assets_response_accepted_context_ = nullptr;

  ai_keyboard::StatusHidResponseStream status_response_;
  PendingSpeakerAssetsResponse speaker_assets_response_{};
  std::uint32_t speaker_assets_sent_sequence_ = 0;
  std::uint32_t speaker_assets_sent_epoch_ = 0;
  bool speaker_assets_sent_ready_ = false;
  ai_keyboard::HidReportQueue pending_keyboard_reports_;
  ai_keyboard::HidReportQueue pending_app_command_reports_;
  ai_keyboard::MouseWheelQueue pending_mouse_wheel_reports_;
  ai_keyboard::UsbHidEndpointArbiter endpoint_arbiter_;
  ai_keyboard::UsbHidKeyboardSnapshot queued_physical_keyboard_;
  std::array<SyntheticKeyboardOperation,
             kSyntheticKeyboardOperationCapacity>
      pending_synthetic_keyboard_operations_{};
  std::size_t pending_synthetic_keyboard_head_ = 0;
  std::size_t pending_synthetic_keyboard_size_ = 0;

  mutable SemaphoreHandle_t lifetime_mutex_ = nullptr;
  ai_keyboard::UsbEndpointLifetime endpoint_lifetime_;
  bool lifetime_reset_pending_ = false;
  std::uint32_t status_response_epoch_ = 0;
  bool initialized_ = false;
};

}  // namespace easy_input
