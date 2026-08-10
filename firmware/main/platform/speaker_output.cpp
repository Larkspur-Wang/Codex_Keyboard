#include "platform/speaker_output.h"

#include <algorithm>
#include <array>
#include <limits>

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
#include "esp_timer.h"
#endif
#include "esp_log.h"
#include "keyboard/board_pins.h"
#include "keyboard/speaker_audio_contract.h"

namespace easy_input {
namespace {

const char* const kTag = "speaker_output";
constexpr std::uint32_t kSampleRate =
    ai_keyboard::kSpeakerPlaybackSampleRate;
constexpr i2s_port_t kSpeakerI2sController = I2S_NUM_1;
constexpr std::uint32_t kFrameMs =
    ai_keyboard::kSpeakerPlaybackFrameMilliseconds;
constexpr std::size_t kSamplesPerFrame = kSampleRate * kFrameMs / 1000;
constexpr std::size_t kDmaDescriptorCount =
    ai_keyboard::kSpeakerPlaybackDmaDescriptorCount;
constexpr std::size_t kToneBurstFrames = 25;
constexpr std::size_t kToneGapFrames = 10;
constexpr std::size_t kToneFrames =
    (kToneBurstFrames * 2U) + kToneGapFrames;
constexpr std::size_t kTailZeroFrames =
    ai_keyboard::kSpeakerPlaybackTailZeroFrames;
constexpr std::size_t kNormalDrainZeroFrames =
    ai_keyboard::speaker_normal_drain_zero_frames(
        kDmaDescriptorCount, kTailZeroFrames);
constexpr std::size_t kFadeSamples = kSampleRate * 15 / 1000;
constexpr std::uint32_t kWriteTimeoutMs = 60;
constexpr std::uint32_t kPowerHandshakeTimeoutMs = 200;
constexpr std::uint32_t kInternalHeapCaps =
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
// Espressif documents about 20 KiB of task stack when the complete decoder
// family is enabled. This isolated probe registers only Ogg + Opus, but starts
// conservatively and records the real high-water mark for the Go/No-Go audit.
constexpr std::uint32_t kWorkerStackBytes = 20U * 1024U;
#else
constexpr std::uint32_t kWorkerStackBytes = 4096;
#endif

static_assert(kSamplesPerFrame == 480U);
static_assert(kFadeSamples == 720U);
static_assert(kToneFrames == 60U);
static_assert(kNormalDrainZeroFrames == 6U);

// Two 250 ms 1 kHz bursts separated by 100 ms are easier to distinguish from
// ambient noise than the original 200 ms / 500 Hz probe. The 25% PCM peak
// remains deliberately conservative, and every burst keeps its own fade.
constexpr std::array<std::int16_t, 48> kTone1kHz{{
    0,     1069,  2120,  3135,  4096,  4987,  5793,  6499,
    7094,  7568,  7913,  8122,  8192,  8122,  7913,  7568,
    7094,  6499,  5793,  4987,  4096,  3135,  2120,  1069,
    0,    -1069, -2120, -3135, -4096, -4987, -5793, -6499,
   -7094, -7568, -7913, -8122, -8192, -8122, -7913, -7568,
   -7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069,
}};
static_assert(kTone1kHz.size() * 1000U == kSampleRate);

TickType_t delay_ticks(std::uint32_t milliseconds) {
  const auto ticks = pdMS_TO_TICKS(milliseconds);
  return ticks == 0 ? 1 : ticks;
}

#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
std::uint32_t integer_sqrt(std::uint64_t value) {
  std::uint64_t result = 0;
  std::uint64_t bit = std::uint64_t{1} << 62U;
  while (bit > value) {
    bit >>= 2U;
  }
  while (bit != 0) {
    if (value >= result + bit) {
      value -= result + bit;
      result = (result >> 1U) + bit;
    } else {
      result >>= 1U;
    }
    bit >>= 2U;
  }
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(
          result, std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t pcm_rms_permille(std::uint64_t sum_squares,
                               std::uint64_t sample_count) {
  if (sample_count == 0) {
    return 0;
  }
  const auto mean_square = sum_squares / sample_count;
  // sqrt(mean_square * 1,000,000) is RMS in PCM units multiplied by 1,000.
  // mean_square is bounded by 32768^2, so this multiplication cannot overflow
  // uint64_t. Divide by signed-16 full scale to report 0..1000 permille.
  const auto scaled_rms = integer_sqrt(mean_square * 1000000ULL);
  return std::min<std::uint32_t>(1000U, scaled_rms / 32768U);
}

std::uint32_t conservative_first_pcm_latency_us(
    std::uint32_t request_time_us) {
  const auto submitted_latency =
      static_cast<std::uint32_t>(esp_timer_get_time()) - request_time_us;
  const auto queue_bound =
      ai_keyboard::speaker_first_pcm_queue_upper_bound_us(
          kDmaDescriptorCount, kFrameMs);
  if (submitted_latency >
      std::numeric_limits<std::uint32_t>::max() - queue_bound) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return submitted_latency + queue_bound;
}

void accumulate_pcm_amplitude(const std::int16_t* samples,
                              std::size_t sample_count,
                              std::uint64_t* sum_squares,
                              std::uint64_t* total_samples,
                              std::uint32_t* absolute_peak) {
  if (samples == nullptr || sum_squares == nullptr ||
      total_samples == nullptr || absolute_peak == nullptr) {
    return;
  }
  for (std::size_t index = 0; index < sample_count; ++index) {
    const auto sample = static_cast<std::int32_t>(samples[index]);
    const auto magnitude = static_cast<std::uint32_t>(
        sample < 0 ? -sample : sample);
    *absolute_peak = std::max(*absolute_peak, magnitude);
    *sum_squares +=
        static_cast<std::uint64_t>(sample * static_cast<std::int64_t>(sample));
  }
  *total_samples += sample_count;
}
#endif

}  // namespace

void SpeakerOutput::mark_boot_pending(
    std::uint32_t microphone_generation_value) {
  ai_keyboard::SpeakerProbeSnapshot snapshot;
  snapshot.present = true;
  snapshot.version = ai_keyboard::kSpeakerProbeStatusVersion;
  snapshot.stage = ai_keyboard::SpeakerProbeStage::BootPending;
  snapshot.result = ai_keyboard::SpeakerProbeResult::Pending;
  snapshot.microphone_generation = microphone_generation_value;
  portENTER_CRITICAL(&probe_mux_);
  probe_snapshot_ = snapshot;
  portEXIT_CRITICAL(&probe_mux_);
}

ai_keyboard::SpeakerProbeSnapshot SpeakerOutput::probe_snapshot() const {
  portENTER_CRITICAL(&probe_mux_);
  const auto snapshot = probe_snapshot_;
  portEXIT_CRITICAL(&probe_mux_);
  return snapshot;
}

esp_err_t SpeakerOutput::begin(
    TaskHandle_t supervisor_task,
    ai_keyboard::AudioIoArbiter* audio_io_arbiter) {
  const auto heap_begin = static_cast<std::uint32_t>(
      heap_caps_get_free_size(kInternalHeapCaps));
  ai_keyboard::SpeakerProbeSnapshot begin_snapshot;
  begin_snapshot.present = true;
  begin_snapshot.version = ai_keyboard::kSpeakerProbeStatusVersion;
  begin_snapshot.generation = 0;
  begin_snapshot.stage = ai_keyboard::SpeakerProbeStage::Begin;
  begin_snapshot.result = ai_keyboard::SpeakerProbeResult::Running;
  begin_snapshot.microphone_generation =
      audio_io_arbiter == nullptr ? 0
                                  : audio_io_arbiter->microphone_generation();
  begin_snapshot.heap_begin_free = heap_begin;
  portENTER_CRITICAL(&probe_mux_);
  probe_snapshot_ = begin_snapshot;
  portEXIT_CRITICAL(&probe_mux_);

  if (ready_) {
    record_probe_state(ai_keyboard::SpeakerProbeStage::Ready,
                       ai_keyboard::SpeakerProbeResult::Pending,
                       ai_keyboard::SpeakerProbeError::None,
                       0,
                       0);
    return ESP_OK;
  }
  if (worker_task_ != nullptr || tx_channel_ != nullptr ||
      shutdown_requested_.load(std::memory_order_acquire) ||
      worker_quiesced_.load(std::memory_order_acquire)) {
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::Begin,
                          ai_keyboard::SpeakerProbeResult::Failed,
                          ai_keyboard::SpeakerProbeError::InvalidArgument,
                          ESP_ERR_INVALID_STATE,
                          0);
    return ESP_ERR_INVALID_STATE;
  }
  if (supervisor_task == nullptr || audio_io_arbiter == nullptr) {
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::Begin,
                          ai_keyboard::SpeakerProbeResult::Failed,
                          ai_keyboard::SpeakerProbeError::InvalidArgument,
                          ESP_ERR_INVALID_ARG,
                          0);
    return ESP_ERR_INVALID_ARG;
  }
  if constexpr (ai_keyboard::kSpkI2sBclkPin < 0 ||
                ai_keyboard::kSpkI2sWsPin < 0 ||
                ai_keyboard::kSpkI2sDataOutPin < 0) {
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::Begin,
                          ai_keyboard::SpeakerProbeResult::Failed,
                          ai_keyboard::SpeakerProbeError::Unsupported,
                          ESP_ERR_NOT_SUPPORTED,
                          0);
    return ESP_ERR_NOT_SUPPORTED;
  }

  audio_io_arbiter_ = audio_io_arbiter;
  record_probe_state(ai_keyboard::SpeakerProbeStage::I2sNew,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     0);
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(kSpeakerI2sController, I2S_ROLE_MASTER);
  channel_config.dma_desc_num = kDmaDescriptorCount;
  channel_config.dma_frame_num = kSamplesPerFrame;
  channel_config.auto_clear_after_cb = true;
  esp_err_t err = i2s_new_channel(&channel_config, &tx_channel_, nullptr);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "i2s_new_channel TX failed: %s", esp_err_to_name(err));
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::I2sNew,
                          ai_keyboard::SpeakerProbeResult::Failed,
                          ai_keyboard::SpeakerProbeError::I2sNew,
                          static_cast<std::int32_t>(err),
                          0);
    return err;
  }

  record_probe_state(ai_keyboard::SpeakerProbeStage::I2sConfig,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     0);
  i2s_std_config_t standard_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = static_cast<gpio_num_t>(ai_keyboard::kSpkI2sBclkPin),
          .ws = static_cast<gpio_num_t>(ai_keyboard::kSpkI2sWsPin),
          .dout = static_cast<gpio_num_t>(ai_keyboard::kSpkI2sDataOutPin),
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  err = i2s_channel_init_std_mode(tx_channel_, &standard_config);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "i2s_channel_init_std_mode TX failed: %s", esp_err_to_name(err));
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::I2sConfig,
                          ai_keyboard::SpeakerProbeResult::Failed,
                          ai_keyboard::SpeakerProbeError::I2sConfig,
                          static_cast<std::int32_t>(err),
                          0);
    const esp_err_t cleanup_err = i2s_del_channel(tx_channel_);
    if (cleanup_err != ESP_OK) {
      record_cleanup_failure(0, static_cast<std::int32_t>(cleanup_err));
    } else {
      tx_channel_ = nullptr;
    }
    return err;
  }

#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  record_probe_state(ai_keyboard::SpeakerProbeStage::OpusInit,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     0);
  err = opus_probe_.begin();
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "Ogg Opus probe initialization failed: %s",
             esp_err_to_name(err));
    const auto raw_error = opus_probe_.last_error_code() == 0
        ? static_cast<std::int32_t>(err)
        : opus_probe_.last_error_code();
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::OpusInit,
                          ai_keyboard::SpeakerProbeResult::Failed,
                          ai_keyboard::SpeakerProbeError::OpusInit,
                          raw_error,
                          0);
    const esp_err_t cleanup_err = i2s_del_channel(tx_channel_);
    if (cleanup_err != ESP_OK) {
      record_cleanup_failure(0, static_cast<std::int32_t>(cleanup_err));
    } else {
      tx_channel_ = nullptr;
    }
    return err;
  }
#endif
#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  err = ima_probe_.begin();
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "IMA-ADPCM probe initialization failed: %s",
             esp_err_to_name(err));
    record_probe_terminal(
        ai_keyboard::SpeakerProbeStage::DecodeReset,
        ai_keyboard::SpeakerProbeResult::Failed,
        ai_keyboard::SpeakerProbeError::DecodeReset,
        ima_probe_.last_error_code() == 0
            ? static_cast<std::int32_t>(err)
            : ima_probe_.last_error_code(),
        0);
    const esp_err_t cleanup_err = i2s_del_channel(tx_channel_);
    if (cleanup_err != ESP_OK) {
      record_cleanup_failure(0, static_cast<std::int32_t>(cleanup_err));
    } else {
      tx_channel_ = nullptr;
    }
    return err;
  }
#endif

  supervisor_task_ = supervisor_task;
  shutdown_requested_.store(false, std::memory_order_release);
  worker_quiesced_.store(false, std::memory_order_release);
  record_probe_state(ai_keyboard::SpeakerProbeStage::TaskAlloc,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     0);
  const auto task_status = xTaskCreatePinnedToCore(
      &SpeakerOutput::task_entry,
      "speaker_diag",
      kWorkerStackBytes,
      this,
      tskIDLE_PRIORITY + 1,
      &worker_task_,
      1);
  if (task_status != pdPASS) {
    worker_task_ = nullptr;
    supervisor_task_ = nullptr;
    ESP_LOGW(kTag, "speaker worker allocation failed");
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::TaskAlloc,
                          ai_keyboard::SpeakerProbeResult::Failed,
                          ai_keyboard::SpeakerProbeError::TaskAlloc,
                          ESP_ERR_NO_MEM,
                          0);
    const esp_err_t cleanup_err = i2s_del_channel(tx_channel_);
    if (cleanup_err != ESP_OK) {
      record_cleanup_failure(0, static_cast<std::int32_t>(cleanup_err));
    } else {
      tx_channel_ = nullptr;
    }
    audio_io_arbiter_ = nullptr;
    return ESP_ERR_NO_MEM;
  }

  ready_ = true;
  record_probe_state(ai_keyboard::SpeakerProbeStage::Ready,
                     ai_keyboard::SpeakerProbeResult::Pending,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     0);
  ESP_LOGI(kTag,
           "diagnostic speaker ready rate=%luHz controller=1 bclk=GPIO%d ws=GPIO%d dout=GPIO%d channel=left",
           static_cast<unsigned long>(kSampleRate),
           static_cast<int>(ai_keyboard::kSpkI2sBclkPin),
           static_cast<int>(ai_keyboard::kSpkI2sWsPin),
           static_cast<int>(ai_keyboard::kSpkI2sDataOutPin));
  return ESP_OK;
}

bool SpeakerOutput::ready() const {
  return ready_ &&
         !shutdown_requested_.load(std::memory_order_acquire);
}

bool SpeakerOutput::request_diagnostic_tone() {
  if (!ready() || worker_task_ == nullptr || tx_channel_ == nullptr) {
    const auto current_generation = probe_snapshot().generation;
    record_probe_terminal(ai_keyboard::SpeakerProbeStage::RequestReject,
                          ai_keyboard::SpeakerProbeResult::Rejected,
                          ai_keyboard::SpeakerProbeError::NotReady,
                          ESP_ERR_INVALID_STATE,
                          current_generation);
    return false;
  }
  const auto ticket = playback_.request();
  if (!ticket.accepted) {
    // The one-slot player already has an active generation. A duplicate API
    // call must not mutate that generation's diagnostic snapshot or turn a
    // healthy in-flight playback into a false PlaybackBusy failure.
    ESP_LOGW(kTag, "diagnostic sound request rejected: playback busy");
    return false;
  }
  reset_probe_run(ticket.generation);
  if (!audio_io_arbiter_->try_begin_speaker(ticket.generation)) {
    playback_.cancel(ticket.generation);
    playback_.mark_drained(ticket.generation);
    record_probe_terminal(
        ai_keyboard::SpeakerProbeStage::RequestReject,
        ai_keyboard::SpeakerProbeResult::Rejected,
        audio_io_arbiter_->microphone_requested()
            ? ai_keyboard::SpeakerProbeError::MicrophoneBusy
            : ai_keyboard::SpeakerProbeError::PlaybackBusy,
        ESP_ERR_INVALID_STATE,
        ticket.generation);
    return false;
  }
  reserved_generation_ = ticket.generation;
  completed_reservation_generation_ = 0;

  clock_primed_ = false;
  power_ready_sent_ = false;
  cancel_generation_.store(0, std::memory_order_release);
  clock_ready_generation_.store(0, std::memory_order_release);
  power_ready_generation_.store(0, std::memory_order_release);
  started_generation_.store(0, std::memory_order_release);
  completed_result_.store(
      static_cast<std::uint8_t>(WorkerResult::None), std::memory_order_relaxed);
  completed_generation_.store(0, std::memory_order_release);
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  request_time_us_.store(
      static_cast<std::uint32_t>(esp_timer_get_time()),
      std::memory_order_release);
#endif
  requested_kind_.store(
      static_cast<std::uint8_t>(RequestKind::Diagnostic),
      std::memory_order_release);
  requested_generation_.store(ticket.generation, std::memory_order_release);
  xTaskNotifyGive(worker_task_);
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  ESP_LOGI(kTag,
           "diagnostic Ogg Opus sound requested generation=%lu encoded=%u",
           static_cast<unsigned long>(ticket.generation),
           static_cast<unsigned>(opus_probe_.encoded_size()));
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  ESP_LOGI(kTag,
           "diagnostic IMA-ADPCM sound requested generation=%lu encoded=%u samples=%lu",
           static_cast<unsigned long>(ticket.generation),
           static_cast<unsigned>(ima_probe_.encoded_size()),
           static_cast<unsigned long>(ima_probe_.total_samples()));
#else
  ESP_LOGI(kTag,
           "diagnostic tone requested generation=%lu",
           static_cast<unsigned long>(ticket.generation));
#endif
  return true;
}

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
bool SpeakerOutput::submit_open_asset_request(
    std::uint32_t generation) {
  reset_probe_run(generation);
  if (!audio_io_arbiter_->try_begin_speaker(generation)) {
    last_request_failure_ = audio_io_arbiter_->microphone_requested()
        ? SpeakerRequestFailure::MicrophoneBusy
        : SpeakerRequestFailure::OwnershipBusy;
    asset_decoder_.close();
    playback_.cancel(generation);
    playback_.mark_drained(generation);
    return false;
  }
  reserved_generation_ = generation;
  completed_reservation_generation_ = 0U;
  clock_primed_ = false;
  power_ready_sent_ = false;
  cancel_generation_.store(0U, std::memory_order_release);
  clock_ready_generation_.store(0U, std::memory_order_release);
  power_ready_generation_.store(0U, std::memory_order_release);
  started_generation_.store(0U, std::memory_order_release);
  completed_result_.store(
      static_cast<std::uint8_t>(WorkerResult::None),
      std::memory_order_relaxed);
  completed_generation_.store(0U, std::memory_order_release);
  requested_kind_.store(
      static_cast<std::uint8_t>(RequestKind::Asset),
      std::memory_order_release);
  requested_generation_.store(generation, std::memory_order_release);
  xTaskNotifyGive(worker_task_);
  return true;
}

bool SpeakerOutput::request_asset(
    speaker_assets::SoundBankStorage& storage,
    const speaker_assets::SoundReadLease& lease,
    const speaker_assets::SoundResolvedAsset& asset) {
  if (!ready() || worker_task_ == nullptr || tx_channel_ == nullptr) {
    return false;
  }
  const auto ticket = playback_.request();
  if (!ticket.accepted) {
    return false;
  }
  if (asset_decoder_.open(storage, lease, asset) !=
      speaker_assets::SoundAssetReadResult::Ok) {
    playback_.cancel(ticket.generation);
    playback_.mark_drained(ticket.generation);
    return false;
  }
  if (!submit_open_asset_request(ticket.generation)) {
    return false;
  }
  ESP_LOGI(kTag,
           "asset sound requested generation=%lu bank=%u asset=%u frames=%u",
           static_cast<unsigned long>(ticket.generation),
           static_cast<unsigned>(asset.bank),
           static_cast<unsigned>(asset.resource_index),
           static_cast<unsigned>(asset.frame_count));
  return true;
}

bool SpeakerOutput::request_embedded_asset(
    const std::uint8_t* encoded,
    std::size_t encoded_bytes) {
  last_request_failure_ = SpeakerRequestFailure::None;
  if (!ready() || worker_task_ == nullptr || tx_channel_ == nullptr) {
    last_request_failure_ = SpeakerRequestFailure::NotReady;
    return false;
  }
  const auto ticket = playback_.request();
  if (!ticket.accepted) {
    last_request_failure_ = SpeakerRequestFailure::PlaybackBusy;
    return false;
  }
  if (asset_decoder_.open_embedded(encoded, encoded_bytes) !=
      speaker_assets::SoundAssetReadResult::Ok) {
    last_request_failure_ = SpeakerRequestFailure::InvalidAsset;
    playback_.cancel(ticket.generation);
    playback_.mark_drained(ticket.generation);
    return false;
  }
  if (!submit_open_asset_request(ticket.generation)) {
    return false;
  }
  ESP_LOGI(
      kTag,
      "factory asset sound requested generation=%lu encoded=%u",
      static_cast<unsigned long>(ticket.generation),
      static_cast<unsigned>(encoded_bytes));
  return true;
}

bool SpeakerOutput::request_streaming_asset(
    const std::uint8_t* encoded_header,
    std::size_t encoded_bytes,
    std::uint64_t expected_samples,
    speaker_assets::SoundAssetStreamingRead read,
    void* read_context) {
  last_request_failure_ = SpeakerRequestFailure::None;
  if (!ready() || worker_task_ == nullptr || tx_channel_ == nullptr) {
    last_request_failure_ = SpeakerRequestFailure::NotReady;
    return false;
  }
  const auto ticket = playback_.request();
  if (!ticket.accepted) {
    last_request_failure_ = SpeakerRequestFailure::PlaybackBusy;
    return false;
  }
  if (asset_decoder_.open_streaming(
          encoded_header, encoded_bytes, read, read_context) !=
          speaker_assets::SoundAssetReadResult::Ok ||
      expected_samples == 0U ||
      expected_samples > speaker_assets::kEmbeddedSoundAssetMaximumSamples ||
      asset_decoder_.asset().decoded_samples != expected_samples) {
    last_request_failure_ = SpeakerRequestFailure::InvalidAsset;
    playback_.cancel(ticket.generation);
    playback_.mark_drained(ticket.generation);
    return false;
  }
  if (!submit_open_asset_request(ticket.generation)) {
    return false;
  }
  ESP_LOGI(
      kTag,
      "streaming asset sound requested generation=%lu encoded=%u",
      static_cast<unsigned long>(ticket.generation),
      static_cast<unsigned>(encoded_bytes));
  return true;
}
#endif

void SpeakerOutput::poll(bool playback_allowed) {
  if (!ready_) {
    // A failed begin can leave a valid channel when the first delete attempt
    // reports a transient driver error. Preserve and retire that exact handle
    // here instead of falsely declaring shutdown complete or leaking I2S1.
    if (worker_task_ == nullptr && tx_channel_ != nullptr) {
      const auto disable_result = i2s_channel_disable(tx_channel_);
      if (disable_result != ESP_OK &&
          disable_result != ESP_ERR_INVALID_STATE) {
        return;
      }
      const auto delete_result = i2s_del_channel(tx_channel_);
      if (delete_result == ESP_OK) {
        tx_channel_ = nullptr;
        audio_io_arbiter_ = nullptr;
      }
    }
    return;
  }

  const auto generation = playback_.active_generation();
  if (generation != 0) {
    const auto clock_generation =
        clock_ready_generation_.load(std::memory_order_acquire);
    if (clock_generation == generation) {
      clock_primed_ = true;
    }

    const auto started = started_generation_.load(std::memory_order_acquire);
    if (started == generation &&
        playback_.phase() == ai_keyboard::SpeakerPlaybackPhase::Starting) {
      playback_.mark_started(generation);
    }

    const auto completed = completed_generation_.load(std::memory_order_acquire);
    if (completed == generation) {
      const auto result = static_cast<WorkerResult>(
          completed_result_.load(std::memory_order_relaxed));
      if (playback_.phase() != ai_keyboard::SpeakerPlaybackPhase::Draining) {
        switch (result) {
          case WorkerResult::Succeeded:
            playback_.finish(generation);
            break;
          case WorkerResult::Cancelled:
            playback_.cancel(generation);
            break;
          case WorkerResult::Failed:
          case WorkerResult::None:
            playback_.fail(generation);
            break;
        }
      } else if (result == WorkerResult::Failed || result == WorkerResult::None) {
        playback_.fail(generation);
      }
      if (playback_.mark_drained(generation)) {
        completed_reservation_generation_ = generation;
      }
      clock_primed_ = false;
      power_ready_sent_ = false;
      ESP_LOGI(kTag,
               "diagnostic tone complete generation=%lu result=%s",
               static_cast<unsigned long>(generation),
               worker_result_name(result));
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
      ESP_LOGI(
          kTag,
          "codec metrics first_pcm_us=%lu decode_us=%lu max_decode_call_us=%lu frames=%lu pcm_bytes=%lu stack_hwm=%lu heap_before=%lu recovered=%lu largest=%lu minimum=%lu",
          static_cast<unsigned long>(
              first_pcm_latency_us_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              decode_time_us_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              maximum_decode_call_us_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              decoded_frame_count_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              decoded_pcm_bytes_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              worker_stack_high_water_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              playback_heap_before_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              playback_heap_after_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              playback_largest_block_.load(std::memory_order_relaxed)),
          static_cast<unsigned long>(
              playback_minimum_heap_.load(std::memory_order_relaxed)));
#endif
    }
  }

  if (!playback_allowed && playback_.active()) {
    cancel_active();
  }

  if (shutdown_requested_.load(std::memory_order_acquire) &&
      worker_quiesced_.load(std::memory_order_acquire)) {
    // The worker publishes quiesced before suspending so it can wake us, but
    // Core 0 must not infer suspension from that atomic alone. Wait for the
    // scheduler's exact state before touching its I2S resource or TCB.
    if (worker_task_ != nullptr &&
        eTaskGetState(worker_task_) != eSuspended) {
      return;
    }
    if (tx_channel_ != nullptr) {
      const auto disable_result = i2s_channel_disable(tx_channel_);
      if (disable_result != ESP_OK &&
          disable_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag,
                 "speaker I2S stop deferred: %s",
                 esp_err_to_name(disable_result));
        return;
      }
      const auto delete_result = i2s_del_channel(tx_channel_);
      if (delete_result != ESP_OK) {
        ESP_LOGW(kTag,
                 "speaker I2S release deferred: %s",
                 esp_err_to_name(delete_result));
        return;
      }
      tx_channel_ = nullptr;
    }
    if (worker_task_ != nullptr) {
      vTaskDelete(worker_task_);
      worker_task_ = nullptr;
    }
    ready_ = false;
    clock_primed_ = false;
    power_ready_sent_ = false;
    supervisor_task_ = nullptr;
    audio_io_arbiter_ = nullptr;
    requested_generation_.store(0U, std::memory_order_release);
    requested_kind_.store(
        static_cast<std::uint8_t>(RequestKind::None),
        std::memory_order_release);
    worker_quiesced_.store(false, std::memory_order_release);
    shutdown_requested_.store(false, std::memory_order_release);
    ESP_LOGI(
        kTag,
        "speaker startup resources released heap=%lu largest=%lu",
        static_cast<unsigned long>(
            heap_caps_get_free_size(kInternalHeapCaps)),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(kInternalHeapCaps)));
  }
}

bool SpeakerOutput::request_shutdown() {
  if (shutdown_complete()) {
    return true;
  }
  if (!ready_ || worker_task_ == nullptr || tx_channel_ == nullptr ||
      busy()) {
    return false;
  }
  bool expected = false;
  if (shutdown_requested_.compare_exchange_strong(
          expected, true,
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    xTaskNotifyGive(worker_task_);
  }
  return true;
}

bool SpeakerOutput::shutdown_complete() const {
  return !ready_ && worker_task_ == nullptr && tx_channel_ == nullptr;
}

bool SpeakerOutput::power_lease_required() const {
  return ready_ && clock_primed_ && playback_.power_required();
}

void SpeakerOutput::notify_power_ready() {
  if (!power_lease_required() || power_ready_sent_ || worker_task_ == nullptr) {
    return;
  }
  const auto generation = playback_.active_generation();
  if (generation == 0) {
    return;
  }
  power_ready_sent_ = true;
  power_ready_generation_.store(generation, std::memory_order_release);
  record_probe_state(ai_keyboard::SpeakerProbeStage::PowerReady,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  xTaskNotifyGive(worker_task_);
}

bool SpeakerOutput::complete_power_handoff() {
  const auto generation = completed_reservation_generation_;
  if (generation == 0) {
    return true;
  }
  if (audio_io_arbiter_ == nullptr ||
      !audio_io_arbiter_->finish_speaker(generation)) {
    return false;
  }
  completed_reservation_generation_ = 0;
  if (reserved_generation_ == generation) {
    reserved_generation_ = 0;
  }
  return true;
}

bool SpeakerOutput::busy() const {
  return playback_.active() || reserved_generation_ != 0;
}

bool SpeakerOutput::sleep_blocked() const {
  return playback_.sleep_blocked() || reserved_generation_ != 0 ||
         (shutdown_requested_.load(std::memory_order_acquire) &&
          !shutdown_complete());
}

ai_keyboard::SpeakerPlaybackPhase SpeakerOutput::phase() const {
  return playback_.phase();
}

ai_keyboard::SpeakerPlaybackResult SpeakerOutput::last_result() const {
  return playback_.last_result();
}

SpeakerRequestFailure SpeakerOutput::last_request_failure() const {
  return last_request_failure_;
}

void SpeakerOutput::task_entry(void* context) {
  static_cast<SpeakerOutput*>(context)->run();
}

const char* SpeakerOutput::worker_result_name(WorkerResult result) {
  switch (result) {
    case WorkerResult::None:
      return "none";
    case WorkerResult::Succeeded:
      return "succeeded";
    case WorkerResult::Cancelled:
      return "cancelled";
    case WorkerResult::Failed:
      return "failed";
  }
  return "unknown";
}

void SpeakerOutput::run() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (shutdown_requested_.load(std::memory_order_acquire) &&
        requested_generation_.load(std::memory_order_acquire) == 0U) {
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
      asset_decoder_.close();
#endif
      const auto stack_high_water =
          static_cast<std::uint32_t>(
              uxTaskGetStackHighWaterMark(nullptr));
      worker_quiesced_.store(true, std::memory_order_release);
      wake_supervisor();
      ESP_LOGI(
          kTag,
          "speaker worker quiesced stack_hwm=%lu",
          static_cast<unsigned long>(stack_high_water));
      vTaskSuspend(nullptr);
      // Only the platform owner may delete this exact suspended task. If an
      // unexpected resume occurs, remain quiesced and never touch I2S again.
      for (;;) {
        vTaskSuspend(nullptr);
      }
    }
    const auto generation =
        requested_generation_.exchange(0, std::memory_order_acq_rel);
    if (generation == 0) {
      continue;
    }
    const auto request_kind = static_cast<RequestKind>(
        requested_kind_.exchange(
            static_cast<std::uint8_t>(RequestKind::None),
            std::memory_order_acq_rel));
    const auto result = play_sound(generation, request_kind);
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
    if (request_kind == RequestKind::Asset) {
      // Completion is the release/acquire proof that the borrowed storage
      // identity is gone before the platform may return the exact read lease.
      asset_decoder_.close();
    }
#endif
    publish_completed(generation, result);
  }
}

SpeakerOutput::WorkerResult SpeakerOutput::play_sound(
    std::uint32_t generation,
    RequestKind request_kind) {
  if (cancelled(generation)) {
    record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Cancelled,
                       0,
                       generation);
    return WorkerResult::Cancelled;
  }

  // Reuse this one 10 ms buffer for DMA priming, playback, gap and drain.
  // At 48 kHz it occupies 960 bytes; a second nested zero buffer would
  // unnecessarily consume another quarter of the fixed 4 KiB worker stack.
  std::array<std::int16_t, kSamplesPerFrame> frame{};
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  SpeakerOpusPcmFrame first_opus_frame{};
#endif
  record_probe_state(ai_keyboard::SpeakerProbeStage::Dma,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  esp_err_t err = preload_zero_dma(frame.data(), frame.size());
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "zero DMA preload failed: %s", esp_err_to_name(err));
    record_probe_state(ai_keyboard::SpeakerProbeStage::Dma,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Dma,
                       static_cast<std::int32_t>(err),
                       generation);
    return WorkerResult::Failed;
  }

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  const bool asset_request = request_kind == RequestKind::Asset;
  if (asset_request) {
    std::size_t first_asset_samples = 0U;
    err = prepare_asset_first_frame(
        generation,
        frame.data(),
        frame.size(),
        &first_asset_samples);
    if (err != ESP_OK) {
      return cancelled(generation) ? WorkerResult::Cancelled
                                   : WorkerResult::Failed;
    }
  } else
#else
  static_cast<void>(request_kind);
#endif
  {
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  // Decode the first 20 ms while I2S clocks and the shared GPIO8 amplifier
  // rail are still off. The unavoidable official-parser lazy allocation is
  // therefore measured without holding power needed by the microphone.
  err = prepare_opus_first_frame(generation, &first_opus_frame);
  if (err != ESP_OK) {
    finish_opus_probe_metrics(generation);
    const auto snapshot = probe_snapshot();
    return snapshot.error == ai_keyboard::SpeakerProbeError::Cancelled
        ? WorkerResult::Cancelled
        : WorkerResult::Failed;
  }
  if (cancelled(generation)) {
    const esp_err_t finish_err = finish_opus_probe_metrics(generation);
    const auto cleanup = ai_keyboard::observe_speaker_probe_cleanup(
        {}, finish_err == ESP_OK ? 0 : static_cast<std::int32_t>(finish_err));
    record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Cancelled,
                       0,
                       generation);
    return cleanup.failed ? WorkerResult::Failed
                          : WorkerResult::Cancelled;
  }
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  // The zero DMA preload copied this buffer into the driver. Reuse the same
  // 960-byte frame for the first allocation-free ADPCM decode while I2S
  // clocks and the GPIO8 amplifier rail are still off.
  std::size_t first_ima_samples = 0;
  err = prepare_ima_first_frame(generation,
                                frame.data(),
                                frame.size(),
                                &first_ima_samples);
  if (err != ESP_OK) {
    finish_ima_probe_metrics(generation);
    const auto snapshot = probe_snapshot();
    return snapshot.error == ai_keyboard::SpeakerProbeError::Cancelled
        ? WorkerResult::Cancelled
        : WorkerResult::Failed;
  }
  if (cancelled(generation)) {
    const esp_err_t finish_err = finish_ima_probe_metrics(generation);
    const auto cleanup = ai_keyboard::observe_speaker_probe_cleanup(
        {}, finish_err == ESP_OK ? 0 : static_cast<std::int32_t>(finish_err));
    record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Cancelled,
                       0,
                       generation);
    return cleanup.failed ? WorkerResult::Failed
                          : WorkerResult::Cancelled;
  }
#endif
  }

  record_probe_state(ai_keyboard::SpeakerProbeStage::Clock,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  err = i2s_channel_enable(tx_channel_);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "i2s_channel_enable TX failed: %s", esp_err_to_name(err));
    record_probe_state(ai_keyboard::SpeakerProbeStage::Clock,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Clock,
                       static_cast<std::int32_t>(err),
                       generation);
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
    finish_opus_probe_metrics(generation);
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
    finish_ima_probe_metrics(generation);
#endif
    return WorkerResult::Failed;
  }
  publish_clock_ready(generation);

  record_probe_state(ai_keyboard::SpeakerProbeStage::PowerWait,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  const auto handshake_start = xTaskGetTickCount();
  while (power_ready_generation_.load(std::memory_order_acquire) != generation) {
    if (cancelled(generation)) {
      ai_keyboard::SpeakerProbeCleanupOutcome cleanup;
      const esp_err_t disable_err = i2s_channel_disable(tx_channel_);
      if (disable_err != ESP_OK && disable_err != ESP_ERR_INVALID_STATE) {
        record_cleanup_failure(generation,
                               static_cast<std::int32_t>(disable_err));
        cleanup = ai_keyboard::observe_speaker_probe_cleanup(
            cleanup, static_cast<std::int32_t>(disable_err));
      }
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
      const esp_err_t finish_err = finish_opus_probe_metrics(generation);
      if (finish_err != ESP_OK) {
        cleanup = ai_keyboard::observe_speaker_probe_cleanup(
            cleanup, static_cast<std::int32_t>(finish_err));
      }
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
      const esp_err_t finish_err = finish_ima_probe_metrics(generation);
      if (finish_err != ESP_OK) {
        cleanup = ai_keyboard::observe_speaker_probe_cleanup(
            cleanup, static_cast<std::int32_t>(finish_err));
      }
#endif
      record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                         ai_keyboard::SpeakerProbeResult::Running,
                         ai_keyboard::SpeakerProbeError::Cancelled,
                         0,
                         generation);
      return cleanup.failed ? WorkerResult::Failed
                            : WorkerResult::Cancelled;
    }
    const auto elapsed_ms = static_cast<std::uint32_t>(
        (xTaskGetTickCount() - handshake_start) * portTICK_PERIOD_MS);
    if (elapsed_ms >= kPowerHandshakeTimeoutMs) {
      ESP_LOGW(kTag,
               "speaker power handshake timed out generation=%lu",
               static_cast<unsigned long>(generation));
      record_probe_state(ai_keyboard::SpeakerProbeStage::PowerWait,
                         ai_keyboard::SpeakerProbeResult::Running,
                         ai_keyboard::SpeakerProbeError::PowerTimeout,
                         ESP_ERR_TIMEOUT,
                         generation);
      const esp_err_t disable_err = i2s_channel_disable(tx_channel_);
      if (disable_err != ESP_OK && disable_err != ESP_ERR_INVALID_STATE) {
        record_cleanup_failure(generation,
                               static_cast<std::int32_t>(disable_err));
      }
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
      finish_opus_probe_metrics(generation);
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
      finish_ima_probe_metrics(generation);
#endif
      return WorkerResult::Failed;
    }
    ulTaskNotifyTake(pdTRUE, delay_ticks(kFrameMs));
  }
  record_probe_state(ai_keyboard::SpeakerProbeStage::PowerReady,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);

  err = write_samples(frame.data(), frame.size());
  if (err != ESP_OK) {
    record_probe_state(ai_keyboard::SpeakerProbeStage::Write,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Write,
                       static_cast<std::int32_t>(err),
                       generation);
    const esp_err_t disable_err = i2s_channel_disable(tx_channel_);
    if (disable_err != ESP_OK && disable_err != ESP_ERR_INVALID_STATE) {
      record_cleanup_failure(generation,
                             static_cast<std::int32_t>(disable_err));
    }
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
    finish_opus_probe_metrics(generation);
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
    finish_ima_probe_metrics(generation);
#endif
    return WorkerResult::Failed;
  }
#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  const auto requested = request_time_us_.load(std::memory_order_acquire);
  first_pcm_latency_us_.store(
      conservative_first_pcm_latency_us(requested),
      std::memory_order_relaxed);
  record_probe_state(ai_keyboard::SpeakerProbeStage::FirstSubmit,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
#endif
  publish_started(generation);

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (asset_request) {
    err = play_asset_frames(
        generation, frame.data(), frame.size());
  } else
#endif
  {
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
    err = play_opus_frames(generation, first_opus_frame);
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
    err = play_ima_frames(generation, frame.data(), frame.size());
#else
  const auto burst_samples = kToneBurstFrames * kSamplesPerFrame;
  for (std::size_t frame_index = 0; frame_index < kToneFrames; ++frame_index) {
    if (cancelled(generation)) {
      break;
    }
    const bool in_gap =
        frame_index >= kToneBurstFrames &&
        frame_index < (kToneBurstFrames + kToneGapFrames);
    if (in_gap) {
      frame.fill(0);
    }
    for (std::size_t index = 0; index < frame.size(); ++index) {
      if (in_gap) {
        continue;
      }
      const auto burst_frame =
          frame_index < kToneBurstFrames
              ? frame_index
              : frame_index - kToneBurstFrames - kToneGapFrames;
      const auto sample_index = burst_frame * frame.size() + index;
      const auto fade_in = std::min(sample_index, kFadeSamples);
      const auto remaining = burst_samples - 1U - sample_index;
      const auto fade_out = std::min(remaining, kFadeSamples);
      const auto envelope = std::min(fade_in, fade_out);
      const auto base = static_cast<std::int32_t>(
          kTone1kHz[sample_index % kTone1kHz.size()]);
      frame[index] = static_cast<std::int16_t>(
          (base * static_cast<std::int32_t>(envelope)) /
          static_cast<std::int32_t>(kFadeSamples));
    }
    err = write_samples(frame.data(), frame.size());
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "diagnostic tone write failed: %s", esp_err_to_name(err));
      record_probe_state(ai_keyboard::SpeakerProbeStage::Write,
                         ai_keyboard::SpeakerProbeResult::Running,
                         ai_keyboard::SpeakerProbeError::Write,
                         static_cast<std::int32_t>(err),
                         generation);
      break;
    }
  }
#endif
  }

  bool cancellation_seen = cancelled(generation);
  if (err == ESP_OK && !cancellation_seen) {
    record_probe_state(ai_keyboard::SpeakerProbeStage::Cleanup,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::None,
                       0,
                       generation);
  }
  frame.fill(0);
  ai_keyboard::SpeakerProbeCleanupOutcome cleanup;
  if (err == ESP_OK && !cancellation_seen) {
    // Four full-ring zero writes guarantee that every previously queued audio
    // descriptor has reached EOF. Two additional frames then establish 20 ms
    // of real silence before stopping clocks. A microphone cancellation skips
    // this normal drain and disables I2S immediately below.
    for (std::size_t index = 0;
         index < kNormalDrainZeroFrames;
         ++index) {
      if (cancelled(generation)) {
        cancellation_seen = true;
        break;
      }
      const esp_err_t drain_err =
          write_samples(frame.data(), frame.size());
      if (drain_err != ESP_OK) {
        record_cleanup_failure(generation,
                               static_cast<std::int32_t>(drain_err));
        cleanup = ai_keyboard::observe_speaker_probe_cleanup(
            cleanup, static_cast<std::int32_t>(drain_err));
        break;
      }
    }
  }
  const esp_err_t disable_err = i2s_channel_disable(tx_channel_);
  if (disable_err != ESP_OK && disable_err != ESP_ERR_INVALID_STATE) {
    record_cleanup_failure(generation,
                           static_cast<std::int32_t>(disable_err));
    cleanup = ai_keyboard::observe_speaker_probe_cleanup(
        cleanup, static_cast<std::int32_t>(disable_err));
  }

#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
  const esp_err_t rewind_err = finish_opus_probe_metrics(generation);
  if (rewind_err != ESP_OK) {
    cleanup = ai_keyboard::observe_speaker_probe_cleanup(
        cleanup, static_cast<std::int32_t>(rewind_err));
  }
#elif defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  const esp_err_t rewind_err = finish_ima_probe_metrics(generation);
  if (rewind_err != ESP_OK) {
    cleanup = ai_keyboard::observe_speaker_probe_cleanup(
        cleanup, static_cast<std::int32_t>(rewind_err));
  }
#endif

  if (err != ESP_OK || cleanup.failed) {
    if (err != ESP_OK) {
      record_cleanup_failure(generation, static_cast<std::int32_t>(err));
    }
    return WorkerResult::Failed;
  }
  if (cancellation_seen || cancelled(generation)) {
    record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Cancelled,
                       0,
                       generation);
    return WorkerResult::Cancelled;
  }
  return WorkerResult::Succeeded;
}

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
esp_err_t SpeakerOutput::prepare_asset_first_frame(
    std::uint32_t generation,
    std::int16_t* output,
    std::size_t output_capacity,
    std::size_t* output_samples) {
  if (output == nullptr || output_samples == nullptr ||
      output_capacity < kSamplesPerFrame) {
    return ESP_ERR_INVALID_ARG;
  }
  const auto status = asset_decoder_.decode_next(
      output, output_capacity, output_samples);
  if (status != speaker_assets::SoundAssetReadResult::Ok ||
      *output_samples == 0U ||
      *output_samples > output_capacity) {
    ESP_LOGW(kTag,
             "asset first frame decode failed status=%u generation=%lu",
             static_cast<unsigned>(status),
             static_cast<unsigned long>(generation));
    return ESP_ERR_INVALID_RESPONSE;
  }
  std::fill(
      output + *output_samples, output + output_capacity, 0);
  return cancelled(generation) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t SpeakerOutput::play_asset_frames(
    std::uint32_t generation,
    std::int16_t* frame,
    std::size_t frame_capacity) {
  if (frame == nullptr || frame_capacity < kSamplesPerFrame) {
    return ESP_ERR_INVALID_ARG;
  }
  while (!cancelled(generation)) {
    std::size_t decoded_samples = 0U;
    const auto status = asset_decoder_.decode_next(
        frame, frame_capacity, &decoded_samples);
    if (status == speaker_assets::SoundAssetReadResult::End) {
      return ESP_OK;
    }
    if (status != speaker_assets::SoundAssetReadResult::Ok ||
        decoded_samples == 0U ||
        decoded_samples > frame_capacity) {
      ESP_LOGW(kTag,
               "asset stream decode failed status=%u generation=%lu",
               static_cast<unsigned>(status),
               static_cast<unsigned long>(generation));
      return ESP_FAIL;
    }
    std::fill(
        frame + decoded_samples, frame + frame_capacity, 0);
    const auto write_result =
        write_samples(frame, frame_capacity);
    if (write_result != ESP_OK) {
      return write_result;
    }
  }
  return ESP_OK;
}
#endif

#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
esp_err_t SpeakerOutput::prepare_opus_first_frame(
    std::uint32_t generation,
    SpeakerOpusPcmFrame* first_frame) {
  if (first_frame == nullptr || cancelled(generation)) {
    const bool was_cancelled = cancelled(generation);
    record_probe_state(was_cancelled
                           ? ai_keyboard::SpeakerProbeStage::Cancel
                           : ai_keyboard::SpeakerProbeStage::DecodeFirst,
                       ai_keyboard::SpeakerProbeResult::Running,
                       was_cancelled
                           ? ai_keyboard::SpeakerProbeError::Cancelled
                           : ai_keyboard::SpeakerProbeError::InvalidArgument,
                       was_cancelled ? 0 : ESP_ERR_INVALID_ARG,
                       generation);
    return ESP_ERR_INVALID_ARG;
  }
  record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeReset,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  esp_err_t err = opus_probe_.reset();
  if (err != ESP_OK) {
    record_probe_state(
        ai_keyboard::SpeakerProbeStage::DecodeReset,
        ai_keyboard::SpeakerProbeResult::Running,
        ai_keyboard::SpeakerProbeError::DecodeReset,
        opus_probe_.last_error_code() == 0
            ? static_cast<std::int32_t>(err)
            : opus_probe_.last_error_code(),
        generation);
    return err;
  }
  playback_heap_before_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_free_size(kInternalHeapCaps)),
      std::memory_order_relaxed);

  const auto decode_start =
      static_cast<std::uint32_t>(esp_timer_get_time());
  record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeFirst,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  const auto status = opus_probe_.decode_next(first_frame);
  const auto elapsed =
      static_cast<std::uint32_t>(esp_timer_get_time()) - decode_start;
  decode_time_us_.store(elapsed, std::memory_order_relaxed);
  maximum_decode_call_us_.store(elapsed, std::memory_order_relaxed);
  if (status != SpeakerOpusDecodeStatus::Frame ||
      first_frame->samples == nullptr ||
      first_frame->sample_count == 0) {
    const auto raw_error = opus_probe_.last_error_code() == 0
        ? static_cast<std::int32_t>(ESP_ERR_INVALID_RESPONSE)
        : opus_probe_.last_error_code();
    record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeFirst,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::DecodeFirst,
                       raw_error,
                       generation);
    return ESP_ERR_INVALID_RESPONSE;
  }
  decoded_frame_count_.store(1, std::memory_order_relaxed);
  decoded_pcm_bytes_.store(
      static_cast<std::uint32_t>(
          first_frame->sample_count * sizeof(first_frame->samples[0])),
      std::memory_order_relaxed);
  std::uint64_t sum_squares = 0;
  std::uint64_t sample_count = 0;
  std::uint32_t absolute_peak = 0;
  accumulate_pcm_amplitude(first_frame->samples,
                           first_frame->sample_count,
                           &sum_squares,
                           &sample_count,
                           &absolute_peak);
  decoded_abs_peak_.store(absolute_peak, std::memory_order_relaxed);
  decoded_rms_permille_.store(
      pcm_rms_permille(sum_squares, sample_count),
      std::memory_order_relaxed);
  if (cancelled(generation)) {
    record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Cancelled,
                       0,
                       generation);
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t SpeakerOutput::play_opus_frames(
    std::uint32_t generation,
    const SpeakerOpusPcmFrame& first_frame) {
  std::uint32_t decode_time_us =
      decode_time_us_.load(std::memory_order_relaxed);
  std::uint32_t maximum_decode_call_us =
      maximum_decode_call_us_.load(std::memory_order_relaxed);
  std::uint32_t frame_count = 0;
  std::uint32_t pcm_bytes = 0;
  std::uint64_t amplitude_sum_squares = 0;
  std::uint64_t amplitude_sample_count = 0;
  std::uint32_t absolute_peak = 0;
  bool first_pcm = true;

  const auto publish_metrics = [&]() {
    decode_time_us_.store(decode_time_us, std::memory_order_relaxed);
    maximum_decode_call_us_.store(
        maximum_decode_call_us, std::memory_order_relaxed);
    decoded_frame_count_.store(frame_count, std::memory_order_relaxed);
    decoded_pcm_bytes_.store(pcm_bytes, std::memory_order_relaxed);
    decoded_abs_peak_.store(absolute_peak, std::memory_order_relaxed);
    decoded_rms_permille_.store(
        pcm_rms_permille(amplitude_sum_squares, amplitude_sample_count),
        std::memory_order_relaxed);
  };
  const auto write_decoded_frame =
      [&](const SpeakerOpusPcmFrame& decoded) -> esp_err_t {
    if (decoded.samples == nullptr || decoded.sample_count == 0) {
      record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeStream,
                         ai_keyboard::SpeakerProbeResult::Running,
                         ai_keyboard::SpeakerProbeError::DecodeStream,
                         ESP_ERR_INVALID_RESPONSE,
                         generation);
      return ESP_ERR_INVALID_RESPONSE;
    }
    ++frame_count;
    pcm_bytes += static_cast<std::uint32_t>(
        decoded.sample_count * sizeof(decoded.samples[0]));
    accumulate_pcm_amplitude(decoded.samples,
                             decoded.sample_count,
                             &amplitude_sum_squares,
                             &amplitude_sample_count,
                             &absolute_peak);
    std::size_t offset = 0;
    while (offset < decoded.sample_count) {
      if (cancelled(generation)) {
        return ESP_OK;
      }
      const auto chunk =
          std::min(kSamplesPerFrame, decoded.sample_count - offset);
      const esp_err_t write_err =
          write_samples(decoded.samples + offset, chunk);
      if (write_err != ESP_OK) {
        record_probe_state(ai_keyboard::SpeakerProbeStage::Write,
                           ai_keyboard::SpeakerProbeResult::Running,
                           ai_keyboard::SpeakerProbeError::Write,
                           static_cast<std::int32_t>(write_err),
                           generation);
        return write_err;
      }
      if (first_pcm) {
        const auto requested =
            request_time_us_.load(std::memory_order_acquire);
        first_pcm_latency_us_.store(
            conservative_first_pcm_latency_us(requested),
            std::memory_order_relaxed);
        first_pcm = false;
        record_probe_state(ai_keyboard::SpeakerProbeStage::FirstSubmit,
                           ai_keyboard::SpeakerProbeResult::Running,
                           ai_keyboard::SpeakerProbeError::None,
                           0,
                           generation);
      }
      if (cancelled(generation)) {
        return ESP_OK;
      }
      offset += chunk;
    }
    return ESP_OK;
  };

  esp_err_t err = write_decoded_frame(first_frame);
  if (err != ESP_OK || cancelled(generation)) {
    publish_metrics();
    return err;
  }

  while (!cancelled(generation)) {
    SpeakerOpusPcmFrame decoded{};
    record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeStream,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::None,
                       0,
                       generation);
    const auto decode_start =
        static_cast<std::uint32_t>(esp_timer_get_time());
    const auto status = opus_probe_.decode_next(&decoded);
    const auto elapsed =
        static_cast<std::uint32_t>(esp_timer_get_time()) - decode_start;
    decode_time_us += elapsed;
    maximum_decode_call_us = std::max(maximum_decode_call_us, elapsed);
    if (status != SpeakerOpusDecodeStatus::Frame &&
        status != SpeakerOpusDecodeStatus::End) {
      publish_metrics();
      const auto raw_error = opus_probe_.last_error_code() == 0
          ? static_cast<std::int32_t>(ESP_FAIL)
          : opus_probe_.last_error_code();
      record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeStream,
                         ai_keyboard::SpeakerProbeResult::Running,
                         ai_keyboard::SpeakerProbeError::DecodeStream,
                         raw_error,
                         generation);
      return ESP_FAIL;
    }
    if (cancelled(generation) ||
        status == SpeakerOpusDecodeStatus::End) {
      break;
    }
    err = write_decoded_frame(decoded);
    if (err != ESP_OK) {
      publish_metrics();
      return err;
    }
  }
  publish_metrics();
  return ESP_OK;
}

esp_err_t SpeakerOutput::finish_opus_probe_metrics(
    std::uint32_t generation) {
  // Ogg reset closes the official parser's nested decoder. Measure recovered
  // heap after each run and keep the next trigger independent from retained
  // per-stream allocations. The outer simple-decoder handle and our 20 ms PCM
  // buffer remain allocated for the firmware lifetime.
  const esp_err_t reset_err = opus_probe_.reset();
  playback_heap_after_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_free_size(kInternalHeapCaps)),
      std::memory_order_relaxed);
  playback_largest_block_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_largest_free_block(kInternalHeapCaps)),
      std::memory_order_relaxed);
  playback_minimum_heap_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_minimum_free_size(kInternalHeapCaps)),
      std::memory_order_relaxed);
  worker_stack_high_water_.store(
      static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
      std::memory_order_relaxed);
  if (reset_err != ESP_OK) {
    record_cleanup_failure(
        generation,
        opus_probe_.last_error_code() == 0
            ? static_cast<std::int32_t>(reset_err)
            : opus_probe_.last_error_code());
  }
  return reset_err;
}
#endif

#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
esp_err_t SpeakerOutput::prepare_ima_first_frame(
    std::uint32_t generation,
    std::int16_t* output,
    std::size_t output_capacity,
    std::size_t* output_samples) {
  if (output == nullptr || output_samples == nullptr ||
      output_capacity < kSamplesPerFrame) {
    return ESP_ERR_INVALID_ARG;
  }

  record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeReset,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  esp_err_t err = ima_probe_.reset();
  if (err != ESP_OK) {
    record_probe_state(
        ai_keyboard::SpeakerProbeStage::DecodeReset,
        ai_keyboard::SpeakerProbeResult::Running,
        ai_keyboard::SpeakerProbeError::DecodeReset,
        ima_probe_.last_error_code() == 0
            ? static_cast<std::int32_t>(err)
            : ima_probe_.last_error_code(),
        generation);
    return err;
  }

  playback_heap_before_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_free_size(kInternalHeapCaps)),
      std::memory_order_relaxed);
  record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeFirst,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::None,
                     0,
                     generation);
  const auto decode_start =
      static_cast<std::uint32_t>(esp_timer_get_time());
  const auto status =
      ima_probe_.decode_next(output, output_capacity, output_samples);
  const auto elapsed =
      static_cast<std::uint32_t>(esp_timer_get_time()) - decode_start;
  decode_time_us_.store(elapsed, std::memory_order_relaxed);
  maximum_decode_call_us_.store(elapsed, std::memory_order_relaxed);
  if (status != SpeakerImaAdpcmDecodeStatus::Frame ||
      *output_samples == 0 ||
      *output_samples > output_capacity) {
    const auto raw_error = ima_probe_.last_error_code() == 0
        ? static_cast<std::int32_t>(ESP_ERR_INVALID_RESPONSE)
        : ima_probe_.last_error_code();
    record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeFirst,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::DecodeFirst,
                       raw_error,
                       generation);
    return ESP_ERR_INVALID_RESPONSE;
  }

  std::uint64_t sum_squares = 0;
  std::uint64_t sample_count = 0;
  std::uint32_t absolute_peak = 0;
  accumulate_pcm_amplitude(output,
                           *output_samples,
                           &sum_squares,
                           &sample_count,
                           &absolute_peak);
  decoded_frame_count_.store(1, std::memory_order_relaxed);
  decoded_pcm_bytes_.store(
      static_cast<std::uint32_t>(
          *output_samples * sizeof(output[0])),
      std::memory_order_relaxed);
  decoded_abs_peak_.store(absolute_peak, std::memory_order_relaxed);
  decoded_rms_permille_.store(
      pcm_rms_permille(sum_squares, sample_count),
      std::memory_order_relaxed);
  // Keep every I2S submission descriptor-aligned even when the EIAD asset's
  // final frame is shorter than 10 ms. Metrics continue to describe decoded
  // samples only; the padded tail is intentional silence.
  std::fill(output + *output_samples, output + output_capacity, 0);

  if (cancelled(generation)) {
    record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::Cancelled,
                       0,
                       generation);
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t SpeakerOutput::play_ima_frames(
    std::uint32_t generation,
    std::int16_t* frame,
    std::size_t frame_capacity) {
  if (frame == nullptr || frame_capacity < kSamplesPerFrame) {
    return ESP_ERR_INVALID_ARG;
  }

  std::uint32_t decode_time_us =
      decode_time_us_.load(std::memory_order_relaxed);
  std::uint32_t maximum_decode_call_us =
      maximum_decode_call_us_.load(std::memory_order_relaxed);
  std::uint32_t frame_count =
      decoded_frame_count_.load(std::memory_order_relaxed);
  std::uint32_t pcm_bytes =
      decoded_pcm_bytes_.load(std::memory_order_relaxed);
  std::uint64_t amplitude_sum_squares = 0;
  std::uint64_t amplitude_sample_count = 0;
  std::uint32_t absolute_peak = 0;
  const auto first_samples =
      static_cast<std::size_t>(pcm_bytes / sizeof(frame[0]));
  accumulate_pcm_amplitude(frame,
                           first_samples,
                           &amplitude_sum_squares,
                           &amplitude_sample_count,
                           &absolute_peak);

  const auto publish_metrics = [&]() {
    decode_time_us_.store(decode_time_us, std::memory_order_relaxed);
    maximum_decode_call_us_.store(
        maximum_decode_call_us, std::memory_order_relaxed);
    decoded_frame_count_.store(frame_count, std::memory_order_relaxed);
    decoded_pcm_bytes_.store(pcm_bytes, std::memory_order_relaxed);
    decoded_abs_peak_.store(absolute_peak, std::memory_order_relaxed);
    decoded_rms_permille_.store(
        pcm_rms_permille(amplitude_sum_squares, amplitude_sample_count),
        std::memory_order_relaxed);
  };

  while (!cancelled(generation)) {
    std::size_t decoded_samples = 0;
    record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeStream,
                       ai_keyboard::SpeakerProbeResult::Running,
                       ai_keyboard::SpeakerProbeError::None,
                       0,
                       generation);
    const auto decode_start =
        static_cast<std::uint32_t>(esp_timer_get_time());
    const auto status =
        ima_probe_.decode_next(frame,
                               frame_capacity,
                               &decoded_samples);
    const auto elapsed =
        static_cast<std::uint32_t>(esp_timer_get_time()) - decode_start;
    decode_time_us += elapsed;
    maximum_decode_call_us = std::max(maximum_decode_call_us, elapsed);
    if (status == SpeakerImaAdpcmDecodeStatus::End) {
      break;
    }
    if (status != SpeakerImaAdpcmDecodeStatus::Frame ||
        decoded_samples == 0 ||
        decoded_samples > frame_capacity) {
      publish_metrics();
      const auto raw_error = ima_probe_.last_error_code() == 0
          ? static_cast<std::int32_t>(ESP_FAIL)
          : ima_probe_.last_error_code();
      record_probe_state(ai_keyboard::SpeakerProbeStage::DecodeStream,
                         ai_keyboard::SpeakerProbeResult::Running,
                         ai_keyboard::SpeakerProbeError::DecodeStream,
                         raw_error,
                         generation);
      return ESP_FAIL;
    }

    ++frame_count;
    pcm_bytes += static_cast<std::uint32_t>(
        decoded_samples * sizeof(frame[0]));
    accumulate_pcm_amplitude(frame,
                             decoded_samples,
                             &amplitude_sum_squares,
                             &amplitude_sample_count,
                             &absolute_peak);
    if (cancelled(generation)) {
      break;
    }
    // A short final EIAD frame must not leave the driver's descriptor cursor
    // mid-buffer. Pad it to a complete 10 ms DMA frame so the subsequent
    // full-ring drain calculation remains exact.
    std::fill(frame + decoded_samples, frame + frame_capacity, 0);
    const esp_err_t write_err =
        write_samples(frame, frame_capacity);
    if (write_err != ESP_OK) {
      publish_metrics();
      record_probe_state(ai_keyboard::SpeakerProbeStage::Write,
                         ai_keyboard::SpeakerProbeResult::Running,
                         ai_keyboard::SpeakerProbeError::Write,
                         static_cast<std::int32_t>(write_err),
                         generation);
      return write_err;
    }
  }

  publish_metrics();
  return ESP_OK;
}

esp_err_t SpeakerOutput::finish_ima_probe_metrics(
    std::uint32_t generation) {
  const esp_err_t reset_err = ima_probe_.reset();
  playback_heap_after_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_free_size(kInternalHeapCaps)),
      std::memory_order_relaxed);
  playback_largest_block_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_largest_free_block(kInternalHeapCaps)),
      std::memory_order_relaxed);
  playback_minimum_heap_.store(
      static_cast<std::uint32_t>(
          heap_caps_get_minimum_free_size(kInternalHeapCaps)),
      std::memory_order_relaxed);
  worker_stack_high_water_.store(
      static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
      std::memory_order_relaxed);
  if (reset_err != ESP_OK) {
    record_cleanup_failure(
        generation,
        ima_probe_.last_error_code() == 0
            ? static_cast<std::int32_t>(reset_err)
            : ima_probe_.last_error_code());
  }
  return reset_err;
}
#endif

esp_err_t SpeakerOutput::preload_zero_dma(const std::int16_t* samples,
                                          std::size_t sample_count) {
  if (samples == nullptr || sample_count == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  static_assert(ai_keyboard::kSpeakerPlaybackPreloadZeroFrames == 1U);
  const auto expected_bytes = sample_count * sizeof(samples[0]);
  std::size_t bytes_loaded = 0;
  const esp_err_t err =
      i2s_channel_preload_data(tx_channel_,
                              samples,
                              expected_bytes,
                              &bytes_loaded);
  if (err != ESP_OK) {
    return err;
  }
  return bytes_loaded == expected_bytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t SpeakerOutput::write_samples(const std::int16_t* samples,
                                       std::size_t sample_count) {
  if (samples == nullptr || sample_count == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples);
  const auto total_bytes = sample_count * sizeof(samples[0]);
  std::size_t offset = 0;
  while (offset < total_bytes) {
    std::size_t written = 0;
    const esp_err_t err =
        i2s_channel_write(tx_channel_,
                          bytes + offset,
                          total_bytes - offset,
                          &written,
                          kWriteTimeoutMs);
    if (err != ESP_OK) {
      return err;
    }
    if (written == 0) {
      return ESP_ERR_TIMEOUT;
    }
    offset += written;
  }
  return ESP_OK;
}

bool SpeakerOutput::cancelled(std::uint32_t generation) const {
  return cancel_generation_.load(std::memory_order_acquire) == generation ||
         (audio_io_arbiter_ != nullptr &&
          audio_io_arbiter_->microphone_requested());
}

void SpeakerOutput::publish_started(std::uint32_t generation) {
  started_generation_.store(generation, std::memory_order_release);
  wake_supervisor();
}

void SpeakerOutput::publish_clock_ready(std::uint32_t generation) {
  clock_ready_generation_.store(generation, std::memory_order_release);
  wake_supervisor();
}

void SpeakerOutput::publish_completed(std::uint32_t generation,
                                      WorkerResult result) {
  finalize_probe(generation, result);
  completed_result_.store(
      static_cast<std::uint8_t>(result), std::memory_order_relaxed);
  completed_generation_.store(generation, std::memory_order_release);
  wake_supervisor();
}

void SpeakerOutput::wake_supervisor() const {
  if (supervisor_task_ != nullptr) {
    xTaskNotifyGive(supervisor_task_);
  }
}

void SpeakerOutput::cancel_active() {
  const auto generation = playback_.active_generation();
  if (generation == 0) {
    return;
  }
  if (playback_.phase() != ai_keyboard::SpeakerPlaybackPhase::Draining) {
    playback_.cancel(generation);
  }
  record_probe_state(ai_keyboard::SpeakerProbeStage::Cancel,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::Cancelled,
                     0,
                     generation);
  cancel_generation_.store(generation, std::memory_order_release);
  if (worker_task_ != nullptr) {
    xTaskNotifyGive(worker_task_);
  }
}

void SpeakerOutput::record_probe_state(
    ai_keyboard::SpeakerProbeStage stage,
    ai_keyboard::SpeakerProbeResult result,
    ai_keyboard::SpeakerProbeError error,
    std::int32_t raw_error,
    std::uint32_t expected_generation) {
  const auto mic_generation = microphone_generation();
  portENTER_CRITICAL(&probe_mux_);
  if (!probe_snapshot_.present && expected_generation == 0) {
    probe_snapshot_ = {};
    probe_snapshot_.present = true;
    probe_snapshot_.version = ai_keyboard::kSpeakerProbeStatusVersion;
  }
  if (ai_keyboard::apply_speaker_probe_state(&probe_snapshot_,
                                             expected_generation,
                                             stage,
                                             result,
                                             error,
                                             raw_error)) {
    probe_snapshot_.microphone_generation = mic_generation;
    refresh_probe_metrics_locked();
  }
  portEXIT_CRITICAL(&probe_mux_);
}

void SpeakerOutput::record_probe_terminal(
    ai_keyboard::SpeakerProbeStage stage,
    ai_keyboard::SpeakerProbeResult result,
    ai_keyboard::SpeakerProbeError error,
    std::int32_t raw_error,
    std::uint32_t expected_generation) {
  const auto mic_generation = microphone_generation();
  const auto terminal_free = static_cast<std::uint32_t>(
      heap_caps_get_free_size(kInternalHeapCaps));
  const auto largest_block = static_cast<std::uint32_t>(
      heap_caps_get_largest_free_block(kInternalHeapCaps));
  const auto minimum_free = static_cast<std::uint32_t>(
      heap_caps_get_minimum_free_size(kInternalHeapCaps));
  portENTER_CRITICAL(&probe_mux_);
  if (!probe_snapshot_.present && expected_generation == 0) {
    probe_snapshot_ = {};
    probe_snapshot_.present = true;
    probe_snapshot_.version = ai_keyboard::kSpeakerProbeStatusVersion;
  }
  if (ai_keyboard::apply_speaker_probe_state(&probe_snapshot_,
                                             expected_generation,
                                             stage,
                                             result,
                                             error,
                                             raw_error)) {
    probe_snapshot_.microphone_generation = mic_generation;
    probe_snapshot_.heap_terminal_free = terminal_free;
    probe_snapshot_.heap_largest_block = largest_block;
    probe_snapshot_.heap_minimum_free = minimum_free;
    refresh_probe_metrics_locked();
  }
  portEXIT_CRITICAL(&probe_mux_);
}

void SpeakerOutput::record_cleanup_failure(std::uint32_t generation,
                                           std::int32_t raw_error) {
  record_probe_state(ai_keyboard::SpeakerProbeStage::Cleanup,
                     ai_keyboard::SpeakerProbeResult::Running,
                     ai_keyboard::SpeakerProbeError::Cleanup,
                     raw_error == 0 ? static_cast<std::int32_t>(ESP_FAIL)
                                    : raw_error,
                     generation);
}

void SpeakerOutput::reset_probe_run(std::uint32_t generation) {
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  request_time_us_.store(0, std::memory_order_relaxed);
  first_pcm_latency_us_.store(0, std::memory_order_relaxed);
  decode_time_us_.store(0, std::memory_order_relaxed);
  maximum_decode_call_us_.store(0, std::memory_order_relaxed);
  decoded_frame_count_.store(0, std::memory_order_relaxed);
  decoded_pcm_bytes_.store(0, std::memory_order_relaxed);
  decoded_abs_peak_.store(0, std::memory_order_relaxed);
  decoded_rms_permille_.store(0, std::memory_order_relaxed);
  worker_stack_high_water_.store(0, std::memory_order_relaxed);
  playback_heap_before_.store(0, std::memory_order_relaxed);
  playback_heap_after_.store(0, std::memory_order_relaxed);
  playback_largest_block_.store(0, std::memory_order_relaxed);
  playback_minimum_heap_.store(0, std::memory_order_relaxed);
#endif
  const auto mic_generation = microphone_generation();
  portENTER_CRITICAL(&probe_mux_);
  const auto heap_begin = probe_snapshot_.heap_begin_free;
  probe_snapshot_ = {};
  probe_snapshot_.present = true;
  probe_snapshot_.version = ai_keyboard::kSpeakerProbeStatusVersion;
  probe_snapshot_.stage = ai_keyboard::SpeakerProbeStage::Request;
  probe_snapshot_.result = ai_keyboard::SpeakerProbeResult::Running;
  probe_snapshot_.generation = generation;
  probe_snapshot_.microphone_generation = mic_generation;
  probe_snapshot_.heap_begin_free = heap_begin;
  portEXIT_CRITICAL(&probe_mux_);
}

void SpeakerOutput::finalize_probe(std::uint32_t generation,
                                   WorkerResult result) {
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  // ESP-IDF's FreeRTOS port reports stack high-water in bytes (unlike
  // upstream FreeRTOS ports that commonly report StackType_t units).
  worker_stack_high_water_.store(
      static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
      std::memory_order_relaxed);
#endif
  const auto current = probe_snapshot();
  ai_keyboard::SpeakerProbeResult worker_result =
      ai_keyboard::SpeakerProbeResult::Failed;
  switch (result) {
    case WorkerResult::Succeeded:
      worker_result = ai_keyboard::SpeakerProbeResult::Ok;
      break;
    case WorkerResult::Cancelled:
      worker_result = ai_keyboard::SpeakerProbeResult::Cancelled;
      break;
    case WorkerResult::Failed:
    case WorkerResult::None:
      worker_result = ai_keyboard::SpeakerProbeResult::Failed;
      break;
  }
  const auto terminal = ai_keyboard::resolve_speaker_probe_terminal(
      current, worker_result, static_cast<std::int32_t>(ESP_FAIL));
  record_probe_terminal(terminal.stage,
                        terminal.result,
                        terminal.error,
                        terminal.raw_error,
                        generation);
}

void SpeakerOutput::refresh_probe_metrics_locked() {
#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  probe_snapshot_.first_submit_us =
      first_pcm_latency_us_.load(std::memory_order_relaxed);
  probe_snapshot_.decode_total_us =
      decode_time_us_.load(std::memory_order_relaxed);
  probe_snapshot_.decode_max_us =
      maximum_decode_call_us_.load(std::memory_order_relaxed);
  probe_snapshot_.decoded_frames =
      decoded_frame_count_.load(std::memory_order_relaxed);
  probe_snapshot_.decoded_pcm_bytes =
      decoded_pcm_bytes_.load(std::memory_order_relaxed);
  probe_snapshot_.stack_high_water_bytes =
      worker_stack_high_water_.load(std::memory_order_relaxed);
  probe_snapshot_.decoded_abs_peak =
      decoded_abs_peak_.load(std::memory_order_relaxed);
  probe_snapshot_.decoded_rms_permille =
      decoded_rms_permille_.load(std::memory_order_relaxed);
#endif
}

std::uint32_t SpeakerOutput::microphone_generation() const {
  return audio_io_arbiter_ == nullptr
      ? 0
      : audio_io_arbiter_->microphone_generation();
}

}  // namespace easy_input
