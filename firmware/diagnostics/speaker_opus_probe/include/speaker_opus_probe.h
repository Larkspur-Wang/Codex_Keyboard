#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace easy_input {

enum class SpeakerOpusDecodeStatus : std::uint8_t {
  Frame,
  End,
  Failed,
};

struct SpeakerOpusPcmFrame {
  const std::int16_t* samples = nullptr;
  std::size_t sample_count = 0;
};

// Phase-2 diagnostic adapter only. It owns one Espressif simple-decoder handle
// and one internal-RAM PCM buffer for the lifetime of the firmware. Playback
// rewinds the outer handle with the official reset API and never allocates an
// application buffer from the request/input path. The official Ogg parser may
// recreate its nested Opus state after reset, so the probe explicitly measures
// first-frame latency and recovered heap instead of claiming zero library
// allocation.
class SpeakerOpusProbe {
 public:
  SpeakerOpusProbe() = default;
  ~SpeakerOpusProbe();

  SpeakerOpusProbe(const SpeakerOpusProbe&) = delete;
  SpeakerOpusProbe& operator=(const SpeakerOpusProbe&) = delete;

  esp_err_t begin();
  esp_err_t reset();
  SpeakerOpusDecodeStatus decode_next(SpeakerOpusPcmFrame* frame);

  bool ready() const;
  std::size_t encoded_size() const;
  std::int32_t last_error_code() const;

 private:
  void release();
  bool validate_container() const;
  bool validate_format();

  void* decoder_ = nullptr;
  std::uint8_t* pcm_buffer_ = nullptr;
  std::size_t encoded_offset_ = 0;
  bool ready_ = false;
  bool format_validated_ = false;
  bool owns_opus_registration_ = false;
  bool owns_ogg_registration_ = false;
  std::int32_t last_error_code_ = 0;
};

}  // namespace easy_input
