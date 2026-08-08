#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "ima_adpcm_decoder.h"

namespace easy_input {

enum class SpeakerImaAdpcmDecodeStatus : std::uint8_t {
  Frame,
  End,
  Failed,
};

// Diagnostic-only asset wrapper. It owns no heap memory: encoded bytes remain
// in flash and callers provide the one 10 ms PCM output frame.
class SpeakerImaAdpcmProbe {
 public:
  esp_err_t begin();
  esp_err_t reset();
  SpeakerImaAdpcmDecodeStatus decode_next(
      std::int16_t* output,
      std::size_t output_capacity_samples,
      std::size_t* output_samples);

  bool ready() const;
  std::size_t encoded_size() const;
  std::uint32_t total_samples() const;
  std::int32_t last_error_code() const;

 private:
  static esp_err_t status_to_esp(ImaAdpcmDecoderStatus status);

  ImaAdpcmDecoder decoder_;
  bool ready_ = false;
  std::int32_t last_error_code_ = 0;
};

}  // namespace easy_input
