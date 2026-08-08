#include "speaker_ima_adpcm_probe.h"

#include "assets/easyinput_boot_probe_eiad.h"

namespace easy_input {

esp_err_t SpeakerImaAdpcmProbe::begin() {
  if (ready_) {
    return ESP_OK;
  }
  const auto status = decoder_.open(
      ima_adpcm_assets::kEasyInputBootProbeEiad.data(),
      ima_adpcm_assets::kEasyInputBootProbeEiad.size());
  if (status != ImaAdpcmDecoderStatus::Ok) {
    last_error_code_ = status_to_esp(status);
    return static_cast<esp_err_t>(last_error_code_);
  }
  ready_ = true;
  last_error_code_ = 0;
  return ESP_OK;
}

esp_err_t SpeakerImaAdpcmProbe::reset() {
  if (!ready_) {
    last_error_code_ = ESP_ERR_INVALID_STATE;
    return ESP_ERR_INVALID_STATE;
  }
  const auto status = decoder_.reset();
  last_error_code_ = status_to_esp(status);
  return static_cast<esp_err_t>(last_error_code_);
}

SpeakerImaAdpcmDecodeStatus SpeakerImaAdpcmProbe::decode_next(
    std::int16_t* output,
    std::size_t output_capacity_samples,
    std::size_t* output_samples) {
  if (!ready_) {
    if (output_samples != nullptr) {
      *output_samples = 0;
    }
    last_error_code_ = ESP_ERR_INVALID_STATE;
    return SpeakerImaAdpcmDecodeStatus::Failed;
  }
  const auto status =
      decoder_.decode_next(output, output_capacity_samples, output_samples);
  if (status == ImaAdpcmDecoderStatus::Ok) {
    last_error_code_ = 0;
    return SpeakerImaAdpcmDecodeStatus::Frame;
  }
  if (status == ImaAdpcmDecoderStatus::End) {
    last_error_code_ = 0;
    return SpeakerImaAdpcmDecodeStatus::End;
  }
  last_error_code_ = status_to_esp(status);
  return SpeakerImaAdpcmDecodeStatus::Failed;
}

bool SpeakerImaAdpcmProbe::ready() const {
  return ready_;
}

std::size_t SpeakerImaAdpcmProbe::encoded_size() const {
  return ima_adpcm_assets::kEasyInputBootProbeEiad.size();
}

std::uint32_t SpeakerImaAdpcmProbe::total_samples() const {
  return ready_ ? decoder_.info().total_samples : 0;
}

std::int32_t SpeakerImaAdpcmProbe::last_error_code() const {
  return last_error_code_;
}

esp_err_t SpeakerImaAdpcmProbe::status_to_esp(
    ImaAdpcmDecoderStatus status) {
  switch (status) {
    case ImaAdpcmDecoderStatus::Ok:
    case ImaAdpcmDecoderStatus::End:
      return ESP_OK;
    case ImaAdpcmDecoderStatus::InvalidArgument:
      return ESP_ERR_INVALID_ARG;
    case ImaAdpcmDecoderStatus::InvalidAsset:
      return ESP_ERR_INVALID_RESPONSE;
    case ImaAdpcmDecoderStatus::OutputTooSmall:
      return ESP_ERR_INVALID_SIZE;
    case ImaAdpcmDecoderStatus::NotReady:
      return ESP_ERR_INVALID_STATE;
  }
  return ESP_FAIL;
}

}  // namespace easy_input
