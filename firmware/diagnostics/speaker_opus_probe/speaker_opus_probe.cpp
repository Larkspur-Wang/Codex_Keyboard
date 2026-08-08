#include "speaker_opus_probe.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "decoder/esp_audio_dec_reg.h"
#include "decoder/impl/esp_opus_dec.h"
#include "esp_audio_simple_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "simple_dec/impl/esp_ogg_dec.h"

namespace easy_input {
namespace {

const char* const kTag = "speaker_opus_probe";
constexpr std::uint32_t kExpectedSampleRate = 48000;
constexpr std::uint8_t kExpectedChannels = 1;
constexpr std::uint8_t kExpectedBitsPerSample = 16;
constexpr std::uint32_t kExpectedFrameMs = 20;
constexpr std::size_t kMaximumDecodedFrameBytes =
    kExpectedSampleRate * kExpectedFrameMs / 1000U * sizeof(std::int16_t);
constexpr std::size_t kMaximumParserStepsPerFrame = 64;
constexpr std::uint32_t kInternalHeapCaps =
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static_assert(kMaximumDecodedFrameBytes == 1920U);

extern const std::uint8_t kEncodedStart[]
    asm("_binary_easyinput_boot_probe_ogg_start");
extern const std::uint8_t kEncodedEnd[]
    asm("_binary_easyinput_boot_probe_ogg_end");

bool registration_succeeded(esp_audio_err_t result, bool* owns_registration) {
  if (owns_registration == nullptr) {
    return false;
  }
  if (result == ESP_AUDIO_ERR_OK) {
    *owns_registration = true;
    return true;
  }
  if (result == ESP_AUDIO_ERR_ALREADY_EXIST) {
    *owns_registration = false;
    return true;
  }
  return false;
}

}  // namespace

SpeakerOpusProbe::~SpeakerOpusProbe() {
  release();
}

esp_err_t SpeakerOpusProbe::begin() {
  if (ready_) {
    return ESP_OK;
  }
  last_error_code_ = 0;
  if (!validate_container()) {
    last_error_code_ = ESP_ERR_INVALID_RESPONSE;
    ESP_LOGE(kTag, "embedded fixture is not an Ogg Opus stream");
    return ESP_ERR_INVALID_RESPONSE;
  }

  const auto heap_before = heap_caps_get_free_size(kInternalHeapCaps);
  pcm_buffer_ = static_cast<std::uint8_t*>(
      heap_caps_malloc(kMaximumDecodedFrameBytes, kInternalHeapCaps));
  if (pcm_buffer_ == nullptr) {
    last_error_code_ = ESP_ERR_NO_MEM;
    ESP_LOGE(kTag,
             "failed to reserve %u-byte internal PCM buffer",
             static_cast<unsigned>(kMaximumDecodedFrameBytes));
    return ESP_ERR_NO_MEM;
  }

  const auto opus_result = esp_opus_dec_register();
  if (!registration_succeeded(opus_result, &owns_opus_registration_)) {
    last_error_code_ = static_cast<std::int32_t>(opus_result);
    ESP_LOGE(kTag,
             "Opus decoder registration failed: %d",
             static_cast<int>(opus_result));
    release();
    return ESP_FAIL;
  }

  const auto ogg_result = esp_ogg_dec_register();
  if (!registration_succeeded(ogg_result, &owns_ogg_registration_)) {
    last_error_code_ = static_cast<std::int32_t>(ogg_result);
    ESP_LOGE(kTag,
             "Ogg decoder registration failed: %d",
             static_cast<int>(ogg_result));
    release();
    return ESP_FAIL;
  }

  esp_audio_simple_dec_cfg_t config{};
  config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
  config.use_frame_dec = false;
  esp_audio_simple_dec_handle_t decoder = nullptr;
  const auto open_result = esp_audio_simple_dec_open(&config, &decoder);
  if (open_result != ESP_AUDIO_ERR_OK || decoder == nullptr) {
    last_error_code_ = static_cast<std::int32_t>(open_result);
    ESP_LOGE(kTag,
             "Ogg Opus simple decoder open failed: %d",
             static_cast<int>(open_result));
    release();
    return open_result == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
  }

  decoder_ = decoder;
  encoded_offset_ = 0;
  ready_ = true;
  format_validated_ = false;
  last_error_code_ = 0;
  const auto heap_after = heap_caps_get_free_size(kInternalHeapCaps);
  ESP_LOGI(kTag,
           "ready encoded=%u pcm_buffer=%u internal_heap_before=%u after=%u largest=%u minimum=%u",
           static_cast<unsigned>(encoded_size()),
           static_cast<unsigned>(kMaximumDecodedFrameBytes),
           static_cast<unsigned>(heap_before),
           static_cast<unsigned>(heap_after),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(kInternalHeapCaps)),
           static_cast<unsigned>(
               heap_caps_get_minimum_free_size(kInternalHeapCaps)));
  return ESP_OK;
}

esp_err_t SpeakerOpusProbe::reset() {
  if (!ready_ || decoder_ == nullptr || pcm_buffer_ == nullptr) {
    last_error_code_ = ESP_ERR_INVALID_STATE;
    return ESP_ERR_INVALID_STATE;
  }
  const auto result = esp_audio_simple_dec_reset(
      static_cast<esp_audio_simple_dec_handle_t>(decoder_));
  if (result != ESP_AUDIO_ERR_OK) {
    last_error_code_ = static_cast<std::int32_t>(result);
    ESP_LOGE(kTag,
             "simple decoder reset failed: %d",
             static_cast<int>(result));
    return ESP_FAIL;
  }
  encoded_offset_ = 0;
  format_validated_ = false;
  last_error_code_ = 0;
  return ESP_OK;
}

SpeakerOpusDecodeStatus SpeakerOpusProbe::decode_next(
    SpeakerOpusPcmFrame* frame) {
  if (frame == nullptr || !ready_ || decoder_ == nullptr ||
      pcm_buffer_ == nullptr) {
    last_error_code_ = frame == nullptr ? ESP_ERR_INVALID_ARG
                                        : ESP_ERR_INVALID_STATE;
    return SpeakerOpusDecodeStatus::Failed;
  }
  frame->samples = nullptr;
  frame->sample_count = 0;

  for (std::size_t step = 0;
       step < kMaximumParserStepsPerFrame;
       ++step) {
    const auto total_size = encoded_size();
    if (encoded_offset_ >= total_size) {
      last_error_code_ = 0;
      return SpeakerOpusDecodeStatus::End;
    }

    const auto remaining = total_size - encoded_offset_;
    esp_audio_simple_dec_raw_t input{};
    input.buffer = const_cast<std::uint8_t*>(kEncodedStart + encoded_offset_);
    input.len = static_cast<std::uint32_t>(remaining);
    input.eos = true;

    esp_audio_simple_dec_out_t output{};
    output.buffer = pcm_buffer_;
    output.len = static_cast<std::uint32_t>(kMaximumDecodedFrameBytes);

    const auto result = esp_audio_simple_dec_process(
        static_cast<esp_audio_simple_dec_handle_t>(decoder_),
        &input,
        &output);
    if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      last_error_code_ = static_cast<std::int32_t>(result);
      ESP_LOGE(kTag,
               "decoded frame needs %u bytes; fixed buffer has %u",
               static_cast<unsigned>(output.needed_size),
               static_cast<unsigned>(kMaximumDecodedFrameBytes));
      return SpeakerOpusDecodeStatus::Failed;
    }
    if (result != ESP_AUDIO_ERR_OK) {
      last_error_code_ = static_cast<std::int32_t>(result);
      ESP_LOGE(kTag,
               "Ogg Opus decode failed at offset=%u: %d",
               static_cast<unsigned>(encoded_offset_),
               static_cast<int>(result));
      return SpeakerOpusDecodeStatus::Failed;
    }
    if (input.consumed > remaining) {
      last_error_code_ = ESP_ERR_INVALID_RESPONSE;
      ESP_LOGE(kTag,
               "decoder reported invalid consumed bytes=%u remaining=%u",
               static_cast<unsigned>(input.consumed),
               static_cast<unsigned>(remaining));
      return SpeakerOpusDecodeStatus::Failed;
    }
    encoded_offset_ += input.consumed;

    if (output.decoded_size != 0) {
      if ((output.decoded_size % sizeof(std::int16_t)) != 0 ||
          output.decoded_size > kMaximumDecodedFrameBytes ||
          !validate_format()) {
        if (last_error_code_ == 0) {
          last_error_code_ = ESP_ERR_INVALID_RESPONSE;
        }
        return SpeakerOpusDecodeStatus::Failed;
      }
      frame->samples =
          reinterpret_cast<const std::int16_t*>(pcm_buffer_);
      frame->sample_count =
          output.decoded_size / sizeof(std::int16_t);
      last_error_code_ = 0;
      return SpeakerOpusDecodeStatus::Frame;
    }
    if (input.consumed == 0) {
      last_error_code_ = ESP_ERR_INVALID_RESPONSE;
      ESP_LOGE(kTag,
               "decoder stalled at encoded offset=%u",
               static_cast<unsigned>(encoded_offset_));
      return SpeakerOpusDecodeStatus::Failed;
    }
  }

  last_error_code_ = ESP_ERR_INVALID_RESPONSE;
  ESP_LOGE(kTag, "decoder exceeded bounded parser steps");
  return SpeakerOpusDecodeStatus::Failed;
}

bool SpeakerOpusProbe::ready() const {
  return ready_;
}

std::size_t SpeakerOpusProbe::encoded_size() const {
  return static_cast<std::size_t>(kEncodedEnd - kEncodedStart);
}

std::int32_t SpeakerOpusProbe::last_error_code() const {
  return last_error_code_;
}

void SpeakerOpusProbe::release() {
  ready_ = false;
  format_validated_ = false;
  encoded_offset_ = 0;
  if (decoder_ != nullptr) {
    esp_audio_simple_dec_close(
        static_cast<esp_audio_simple_dec_handle_t>(decoder_));
    decoder_ = nullptr;
  }
  if (owns_ogg_registration_) {
    esp_ogg_dec_unregister();
    owns_ogg_registration_ = false;
  }
  if (owns_opus_registration_) {
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_OPUS);
    owns_opus_registration_ = false;
  }
  if (pcm_buffer_ != nullptr) {
    heap_caps_free(pcm_buffer_);
    pcm_buffer_ = nullptr;
  }
}

bool SpeakerOpusProbe::validate_container() const {
  constexpr std::array<std::uint8_t, 4> kOggMagic{{'O', 'g', 'g', 'S'}};
  constexpr std::array<std::uint8_t, 8> kOpusHead{{
      'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
  }};
  const auto size = encoded_size();
  if (size < kOggMagic.size() + kOpusHead.size() ||
      !std::equal(kOggMagic.begin(), kOggMagic.end(), kEncodedStart)) {
    return false;
  }
  return std::search(kEncodedStart,
                     kEncodedEnd,
                     kOpusHead.begin(),
                     kOpusHead.end()) != kEncodedEnd;
}

bool SpeakerOpusProbe::validate_format() {
  if (format_validated_) {
    return true;
  }
  esp_audio_simple_dec_info_t info{};
  const auto result = esp_audio_simple_dec_get_info(
      static_cast<esp_audio_simple_dec_handle_t>(decoder_),
      &info);
  if (result != ESP_AUDIO_ERR_OK) {
    last_error_code_ = static_cast<std::int32_t>(result);
    ESP_LOGE(kTag,
             "decoded format unavailable: %d",
             static_cast<int>(result));
    return false;
  }
  if (info.sample_rate != kExpectedSampleRate ||
      info.channel != kExpectedChannels ||
      info.bits_per_sample != kExpectedBitsPerSample) {
    last_error_code_ = ESP_ERR_INVALID_RESPONSE;
    ESP_LOGE(kTag,
             "fixture format mismatch rate=%u channels=%u bits=%u",
             static_cast<unsigned>(info.sample_rate),
             static_cast<unsigned>(info.channel),
             static_cast<unsigned>(info.bits_per_sample));
    return false;
  }
  format_validated_ = true;
  last_error_code_ = 0;
  ESP_LOGI(kTag,
           "decoded format rate=%u channels=%u bits=%u bitrate=%u frame=%u",
           static_cast<unsigned>(info.sample_rate),
           static_cast<unsigned>(info.channel),
           static_cast<unsigned>(info.bits_per_sample),
           static_cast<unsigned>(info.bitrate),
           static_cast<unsigned>(info.frame_size));
  return true;
}

}  // namespace easy_input
