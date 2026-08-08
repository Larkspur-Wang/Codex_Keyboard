#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/keyboard_audio.h"
#include "speaker_assets/speaker_assets_protocol.h"
#include "speaker_assets/speaker_assets_wifi_policy.h"
#include "speaker_assets/speaker_assets_wifi_wire.h"

namespace easy_input {

// Authenticated TCP carrier for speaker-asset logical frames. The carrier
// owns sockets and byte-stream framing only; protocol/session/Flash work stays
// in SpeakerAssetsSupervisor and its Store worker.
class SpeakerAssetsWifiCarrier final {
 public:
  using ReceiveFrameCallback = bool (*)(
      void* context,
      const speaker_assets::SpeakerAssetsRouteToken& route,
      const std::uint8_t* frame,
      std::size_t length,
      std::uint32_t received_ms);
  using ObserveRouteCallback = bool (*)(
      void* context,
      const speaker_assets::SpeakerAssetsRouteToken& route,
      bool opened);

  struct AcceptedResponse {
    speaker_assets::SpeakerAssetsRouteToken route{};
    std::uint32_t runtime_reply_sequence = 0U;
  };

  esp_err_t begin(
      KeyboardAudioLink* audio,
      TaskHandle_t supervisor_task,
      ReceiveFrameCallback receive_frame,
      ObserveRouteCallback observe_route,
      void* callback_context);
  // Publish the carrier only after its owner has committed the successful
  // begin. The worker remains behind this gate, so no route callback can race
  // the owner's wifi_started publication.
  void activate();

  void set_assets_ready(bool ready);
  void set_usb_preferred(bool preferred);

  bool queue_response(
      const speaker_assets::SpeakerAssetsRouteToken& route,
      std::uint32_t runtime_reply_sequence,
      const std::uint8_t* payload,
      std::size_t payload_length);
  bool take_response_accepted(AcceptedResponse* accepted);

  bool route_active() const;
  bool response_pending() const;
  void publish_runtime_progress(
      std::uint32_t generation);

 private:
  struct PendingResponse {
    speaker_assets::SpeakerAssetsRouteToken route{};
    std::uint32_t runtime_reply_sequence = 0U;
    std::uint16_t payload_length = 0U;
    std::array<std::uint8_t,
               speaker_assets::kSpeakerAssetsWifiFrameMaxBytes>
        payload{};
    bool ready = false;
  };

  struct ServiceState {
    bool assets_ready = false;
    bool usb_preferred = false;
    bool listener_ready = false;
    bool route_active = false;
    speaker_assets::SpeakerAssetsRouteToken route{};
  };

  static void task_entry(void* context);
  static std::size_t extend_heartbeat(
      void* context,
      std::uint8_t* heartbeat,
      std::size_t base_length,
      std::size_t capacity);

  void run();
  void service_connection(
      int socket,
      const KeyboardWifiServiceSnapshot& snapshot);
  speaker_assets::SpeakerAssetsWifiPolicyInputs policy_inputs(
      const KeyboardWifiServiceSnapshot& snapshot) const;
  bool allow_new_route(
      const KeyboardWifiServiceSnapshot& snapshot,
      bool require_listener) const;
  bool lifecycle_still_exact(
      std::uint32_t generation) const;
  bool read_exact(
      int socket,
      std::uint8_t* destination,
      std::size_t length,
      std::uint32_t timeout_ms,
      std::uint32_t expected_generation);
  bool write_exact(
      int socket,
      const std::uint8_t* source,
      std::size_t length,
      std::uint32_t timeout_ms,
      std::uint32_t expected_generation);
  bool take_pending_response(
      const speaker_assets::SpeakerAssetsRouteToken& route,
      PendingResponse* response);
  bool publish_accepted_response(
      const speaker_assets::SpeakerAssetsRouteToken& route,
      std::uint32_t runtime_reply_sequence);
  void clear_pending_response(
      const speaker_assets::SpeakerAssetsRouteToken& route);
  void set_listener_ready(bool ready);
  void set_active_route(
      const speaker_assets::SpeakerAssetsRouteToken& route);
  void clear_active_route(
      const speaker_assets::SpeakerAssetsRouteToken& route);
  void rotate_endpoint_nonce();
  std::uint32_t next_route_id();
  std::uint32_t next_route_generation();
  void lock() const;
  void unlock() const;

  mutable SemaphoreHandle_t mutex_ = nullptr;
  KeyboardAudioLink* audio_ = nullptr;
  TaskHandle_t task_ = nullptr;
  TaskHandle_t supervisor_task_ = nullptr;
  ReceiveFrameCallback receive_frame_ = nullptr;
  ObserveRouteCallback observe_route_ = nullptr;
  void* callback_context_ = nullptr;
  ServiceState state_{};
  PendingResponse pending_response_{};
  bool response_in_flight_ = false;
  AcceptedResponse accepted_response_{};
  bool accepted_response_ready_ = false;
  std::array<std::uint8_t,
             speaker_assets::kSpeakerAssetsWifiIdentityBytes>
      device_id_{};
  std::array<std::uint8_t,
             speaker_assets::kSpeakerAssetsWifiIdentityBytes>
      endpoint_nonce_{};
  std::uint32_t next_route_id_ = 1U;
  std::uint32_t next_route_generation_ = 1U;
  std::atomic<bool> activated_{false};
  std::atomic<std::uint32_t>
      runtime_progress_generation_{0U};
  bool initialized_ = false;
};

}  // namespace easy_input
