#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard/audio_io_arbiter.h"
#include "keyboard/codex_playback_wire.h"
#include "keyboard/codex_slot_state.h"
#include "platform/keyboard_audio.h"
#include "platform/speaker_output.h"

namespace easy_input {

// Main-task-owned LAN playback receiver. Network packets are authenticated
// before allocation or state mutation. A complete bounded EIAD generation is
// held in PSRAM while the existing streaming decoder/I2S worker consumes it.
class CodexLanPlayback {
 public:
  esp_err_t begin(KeyboardAudioLink* audio,
                  SpeakerOutput* speaker,
                  ai_keyboard::AudioIoArbiter* audio_io_arbiter,
                  easy_codex::CodexSlotState* slots,
                  TaskHandle_t supervisor_task);
  bool request(std::uint8_t slot,
               std::uint32_t request_generation,
               std::uint32_t connection_generation);
  void preempt(const easy_codex::PlaybackIdentity& identity);
  void poll();
  bool active() const;
  bool sleep_blocked() const;

 private:
  enum class Phase : std::uint8_t {
    Idle,
    AwaitBegin,
    Receiving,
    Playing,
    FinishedPendingAck,
    Cancelling,
  };

  bool open_socket(const KeyboardWifiServiceSnapshot& snapshot);
  bool send_request();
  bool send_ack(std::uint8_t status);
  bool send_finished();
  void receive_packets();
  void handle_begin(const std::uint8_t* packet, std::size_t length);
  void handle_data(const std::uint8_t* packet, std::size_t length);
  void handle_finished_ack(const std::uint8_t* packet, std::size_t length);
  bool begin_speaker_playback();
  void fail(const char* reason);
  void cleanup(bool abort_slot);
  bool source_is_host(std::uint32_t address) const;
  easy_codex::PlaybackIdentity slot_identity() const;

  KeyboardAudioLink* audio_ = nullptr;
  SpeakerOutput* speaker_ = nullptr;
  ai_keyboard::AudioIoArbiter* audio_io_arbiter_ = nullptr;
  easy_codex::CodexSlotState* slots_ = nullptr;
  TaskHandle_t supervisor_task_ = nullptr;
  KeyboardWifiServiceLease wifi_lease_{};
  std::array<std::uint8_t, 32> key_{};
  std::uint32_t host_ipv4_ = 0U;
  std::uint16_t host_port_ = 0U;
  int socket_ = -1;
  Phase phase_ = Phase::Idle;
  easy_codex::PlaybackWireRequest request_{};
  easy_codex::PlaybackWireBegin begin_{};
  std::uint8_t* encoded_ = nullptr;
  std::array<std::uint8_t, easy_codex::kPlaybackChunkBytes> decrypted_chunk_{};
  std::size_t received_bytes_ = 0U;
  std::uint32_t last_send_ms_ = 0U;
  std::uint8_t request_retries_ = 0U;
  std::uint8_t finished_retries_ = 0U;
  std::uint8_t cancel_retries_ = 0U;
  bool cancel_acknowledged_ = false;
  const char* last_failure_ = "none";
  std::uint8_t deferred_slot_ = 0U;
  std::uint32_t deferred_request_generation_ = 0U;
  std::uint32_t deferred_connection_generation_ = 0U;
};

}  // namespace easy_input
