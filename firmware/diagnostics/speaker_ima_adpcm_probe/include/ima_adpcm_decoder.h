#pragma once

#include <cstddef>
#include <cstdint>

namespace easy_input {

inline constexpr std::uint8_t kImaAdpcmAssetVersion = 1;
inline constexpr std::uint8_t kImaAdpcmAssetChannels = 1;
inline constexpr std::uint32_t kImaAdpcmAssetSampleRate = 48000;
inline constexpr std::uint16_t kImaAdpcmFrameSamples = 480;
inline constexpr std::size_t kImaAdpcmAssetHeaderBytes = 20;
inline constexpr std::size_t kImaAdpcmFrameHeaderBytes = 6;

enum class ImaAdpcmDecoderStatus : std::uint8_t {
  Ok,
  End,
  InvalidArgument,
  InvalidAsset,
  OutputTooSmall,
  NotReady,
};

struct ImaAdpcmAssetInfo {
  std::uint32_t sample_rate = 0;
  std::uint32_t total_samples = 0;
  std::uint16_t frame_samples = 0;
  std::uint16_t frame_count = 0;
  std::uint8_t channels = 0;
};

// Allocation-free decoder for EasyInput's diagnostic EIAD v1 container.
// Each frame carries its own predictor and step index, so every 10 ms frame
// can be decoded independently. The decoder never owns or copies asset bytes.
class ImaAdpcmDecoder {
 public:
  ImaAdpcmDecoderStatus open(const std::uint8_t* encoded,
                             std::size_t encoded_size);
  ImaAdpcmDecoderStatus reset();
  ImaAdpcmDecoderStatus decode_next(std::int16_t* output,
                                    std::size_t output_capacity_samples,
                                    std::size_t* output_samples);

  bool ready() const;
  const ImaAdpcmAssetInfo& info() const;
  std::size_t encoded_size() const;
  std::uint16_t next_frame_index() const;
  std::uint32_t decoded_samples() const;

 private:
  void clear();

  const std::uint8_t* encoded_ = nullptr;
  std::size_t encoded_size_ = 0;
  std::size_t next_offset_ = 0;
  ImaAdpcmAssetInfo info_{};
  std::uint32_t decoded_samples_ = 0;
  std::uint16_t next_frame_index_ = 0;
  bool ready_ = false;
};

static_assert(sizeof(ImaAdpcmDecoder) <= 48U);

}  // namespace easy_input
