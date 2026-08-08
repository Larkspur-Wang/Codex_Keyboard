#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/i2s_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard/audio_io_arbiter.h"
#include "keyboard/speaker_playback.h"
#include "keyboard/speaker_probe_status.h"
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
#include "speaker_opus_probe.h"
#endif
#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
#include "speaker_ima_adpcm_probe.h"
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
#include "speaker_assets/sound_asset_reader.h"
#endif

namespace easy_input {

class SpeakerOutput {
 public:
  esp_err_t begin(TaskHandle_t supervisor_task,
                  ai_keyboard::AudioIoArbiter* audio_io_arbiter);
  bool ready() const;
  void mark_boot_pending(std::uint32_t microphone_generation);
  ai_keyboard::SpeakerProbeSnapshot probe_snapshot() const;

  // Stage-1 only: one fixed, low-volume PCM tone. Product sounds and their
  // transport are intentionally not part of this diagnostic interface.
  bool request_diagnostic_tone();

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  // Starts one already-resolved, bank-pinned asset. The caller retains and
  // releases the SoundReadLease only after busy() becomes false.
  bool request_asset(
      speaker_assets::SoundBankStorage& storage,
      const speaker_assets::SoundReadLease& lease,
      const speaker_assets::SoundResolvedAsset& asset);
  // Starts the immutable factory EIAD resource from the application image.
  // It shares the exact worker, I2S, power, cancellation, and microphone
  // arbitration path used by a bank-backed user asset.
  bool request_embedded_asset(
      const std::uint8_t* encoded,
      std::size_t encoded_bytes);
#endif

  // Called only by the platform/main task. Worker events are folded into the
  // playback core state machine here, never from the worker task. The separate
  // diagnostic snapshot is copied under a short fixed critical section.
  void poll(bool playback_allowed);

  // Product startup sound is a one-shot capability. Once playback and the
  // exact audio-arbiter generation are fully drained, the platform owner asks
  // the worker to close its decoder and quiesce. poll() waits until FreeRTOS
  // reports that exact task Suspended, releases I2S, then deletes the task.
  bool request_shutdown();
  bool shutdown_complete() const;

  // The worker first primes zero-filled DMA and enables clocks while V_SPK is
  // still off. Only after that event may the platform owner acquire GPIO8.
  bool power_lease_required() const;
  void notify_power_ready();
  bool complete_power_handoff();

  bool busy() const;
  bool sleep_blocked() const;
  ai_keyboard::SpeakerPlaybackPhase phase() const;
  ai_keyboard::SpeakerPlaybackResult last_result() const;

 private:
  enum class RequestKind : std::uint8_t {
    None,
    Diagnostic,
    Asset,
  };

  enum class WorkerResult : std::uint8_t {
    None,
    Succeeded,
    Cancelled,
    Failed,
  };

  static void task_entry(void* context);
  static const char* worker_result_name(WorkerResult result);
  void run();
  WorkerResult play_sound(std::uint32_t generation,
                          RequestKind request_kind);
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  bool submit_open_asset_request(std::uint32_t generation);
  esp_err_t prepare_asset_first_frame(std::uint32_t generation,
                                      std::int16_t* output,
                                      std::size_t output_capacity,
                                      std::size_t* output_samples);
  esp_err_t play_asset_frames(std::uint32_t generation,
                              std::int16_t* frame,
                              std::size_t frame_capacity);
#endif
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  esp_err_t prepare_opus_first_frame(std::uint32_t generation,
                                     SpeakerOpusPcmFrame* first_frame);
  esp_err_t play_opus_frames(std::uint32_t generation,
                             const SpeakerOpusPcmFrame& first_frame);
  esp_err_t finish_opus_probe_metrics(std::uint32_t generation);
#endif
#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  esp_err_t prepare_ima_first_frame(std::uint32_t generation,
                                    std::int16_t* output,
                                    std::size_t output_capacity,
                                    std::size_t* output_samples);
  esp_err_t play_ima_frames(std::uint32_t generation,
                            std::int16_t* frame,
                            std::size_t frame_capacity);
  esp_err_t finish_ima_probe_metrics(std::uint32_t generation);
#endif
  esp_err_t preload_zero_dma(const std::int16_t* samples,
                             std::size_t sample_count);
  esp_err_t write_samples(const std::int16_t* samples, std::size_t sample_count);
  bool cancelled(std::uint32_t generation) const;
  void publish_started(std::uint32_t generation);
  void publish_clock_ready(std::uint32_t generation);
  void publish_completed(std::uint32_t generation, WorkerResult result);
  void wake_supervisor() const;
  void cancel_active();
  void record_probe_state(ai_keyboard::SpeakerProbeStage stage,
                          ai_keyboard::SpeakerProbeResult result,
                          ai_keyboard::SpeakerProbeError error,
                          std::int32_t raw_error,
                          std::uint32_t expected_generation);
  void record_probe_terminal(ai_keyboard::SpeakerProbeStage stage,
                             ai_keyboard::SpeakerProbeResult result,
                             ai_keyboard::SpeakerProbeError error,
                             std::int32_t raw_error,
                             std::uint32_t expected_generation);
  void record_cleanup_failure(std::uint32_t generation,
                              std::int32_t raw_error);
  void reset_probe_run(std::uint32_t generation);
  void finalize_probe(std::uint32_t generation, WorkerResult result);
  void refresh_probe_metrics_locked();
  std::uint32_t microphone_generation() const;

  ai_keyboard::SpeakerPlayback playback_;
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  SpeakerOpusProbe opus_probe_;
#endif
#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  SpeakerImaAdpcmProbe ima_probe_;
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  speaker_assets::SoundAssetStreamDecoder asset_decoder_;
#endif
  i2s_chan_handle_t tx_channel_ = nullptr;
  TaskHandle_t worker_task_ = nullptr;
  TaskHandle_t supervisor_task_ = nullptr;
  ai_keyboard::AudioIoArbiter* audio_io_arbiter_ = nullptr;
  bool ready_ = false;
  bool clock_primed_ = false;
  bool power_ready_sent_ = false;
  std::uint32_t reserved_generation_ = 0;
  std::uint32_t completed_reservation_generation_ = 0;

  std::atomic<std::uint32_t> requested_generation_{0};
  std::atomic<std::uint8_t> requested_kind_{
      static_cast<std::uint8_t>(RequestKind::None)};
  std::atomic<std::uint32_t> cancel_generation_{0};
  std::atomic<std::uint32_t> clock_ready_generation_{0};
  std::atomic<std::uint32_t> power_ready_generation_{0};
  std::atomic<std::uint32_t> started_generation_{0};
  std::atomic<std::uint32_t> completed_generation_{0};
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> worker_quiesced_{false};
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  std::atomic<std::uint32_t> request_time_us_{0};
  std::atomic<std::uint32_t> first_pcm_latency_us_{0};
  std::atomic<std::uint32_t> decode_time_us_{0};
  std::atomic<std::uint32_t> maximum_decode_call_us_{0};
  std::atomic<std::uint32_t> decoded_frame_count_{0};
  std::atomic<std::uint32_t> decoded_pcm_bytes_{0};
  std::atomic<std::uint32_t> decoded_abs_peak_{0};
  std::atomic<std::uint32_t> decoded_rms_permille_{0};
  std::atomic<std::uint32_t> worker_stack_high_water_{0};
  std::atomic<std::uint32_t> playback_heap_before_{0};
  std::atomic<std::uint32_t> playback_heap_after_{0};
  std::atomic<std::uint32_t> playback_largest_block_{0};
  std::atomic<std::uint32_t> playback_minimum_heap_{0};
#endif
  std::atomic<std::uint8_t> completed_result_{
      static_cast<std::uint8_t>(WorkerResult::None)};
  mutable portMUX_TYPE probe_mux_ = portMUX_INITIALIZER_UNLOCKED;
  ai_keyboard::SpeakerProbeSnapshot probe_snapshot_;
};

}  // namespace easy_input
