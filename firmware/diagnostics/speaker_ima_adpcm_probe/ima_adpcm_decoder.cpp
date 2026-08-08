#include "ima_adpcm_decoder.h"

#include <algorithm>
#include <array>
#include <limits>

namespace easy_input {
namespace {

constexpr std::array<std::int32_t, 89> kStepTable{{
    7,     8,     9,     10,    11,    12,    13,    14,    16,
    17,    19,    21,    23,    25,    28,    31,    34,    37,
    41,    45,    50,    55,    60,    66,    73,    80,    88,
    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,
    1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,
    3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}};

constexpr std::array<std::int8_t, 16> kIndexDelta{{
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
}};

constexpr std::array<std::uint8_t, 4> kMagic{{'E', 'I', 'A', 'D'}};

std::uint16_t read_le16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t read_le32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::int32_t read_signed_le16(const std::uint8_t* data) {
  const auto raw = read_le16(data);
  return (raw & 0x8000U) == 0
      ? static_cast<std::int32_t>(raw)
      : static_cast<std::int32_t>(raw) - 65536;
}

std::size_t payload_bytes(std::uint16_t sample_count) {
  // The first sample is the frame predictor. Each remaining sample consumes
  // one nibble; an unused high nibble is zero when the count is even.
  return static_cast<std::size_t>(sample_count) / 2U;
}

bool has_bytes(std::size_t offset,
               std::size_t length,
               std::size_t total_size) {
  return offset <= total_size && length <= total_size - offset;
}

std::int16_t decode_nibble(std::uint8_t code,
                           std::int32_t* predictor,
                           std::int32_t* step_index) {
  const auto step = kStepTable[static_cast<std::size_t>(*step_index)];
  std::int32_t difference = step >> 3U;
  if ((code & 0x01U) != 0) {
    difference += step >> 2U;
  }
  if ((code & 0x02U) != 0) {
    difference += step >> 1U;
  }
  if ((code & 0x04U) != 0) {
    difference += step;
  }

  *predictor += (code & 0x08U) != 0 ? -difference : difference;
  *predictor = std::clamp<std::int32_t>(
      *predictor,
      std::numeric_limits<std::int16_t>::min(),
      std::numeric_limits<std::int16_t>::max());
  *step_index = std::clamp<std::int32_t>(
      *step_index + kIndexDelta[code],
      0,
      static_cast<std::int32_t>(kStepTable.size() - 1U));
  return static_cast<std::int16_t>(*predictor);
}

}  // namespace

ImaAdpcmDecoderStatus ImaAdpcmDecoder::open(
    const std::uint8_t* encoded,
    std::size_t encoded_size) {
  clear();
  if (encoded == nullptr) {
    return ImaAdpcmDecoderStatus::InvalidArgument;
  }
  if (!has_bytes(0, kImaAdpcmAssetHeaderBytes, encoded_size) ||
      !std::equal(kMagic.begin(), kMagic.end(), encoded) ||
      encoded[4] != kImaAdpcmAssetVersion ||
      encoded[5] != kImaAdpcmAssetChannels ||
      read_le32(encoded + 6U) != kImaAdpcmAssetSampleRate ||
      read_le16(encoded + 10U) != kImaAdpcmFrameSamples ||
      read_le16(encoded + 18U) != kImaAdpcmAssetHeaderBytes) {
    return ImaAdpcmDecoderStatus::InvalidAsset;
  }

  const auto frame_count = read_le16(encoded + 12U);
  const auto total_samples = read_le32(encoded + 14U);
  if (frame_count == 0 || total_samples == 0) {
    return ImaAdpcmDecoderStatus::InvalidAsset;
  }

  std::size_t offset = kImaAdpcmAssetHeaderBytes;
  std::uint64_t observed_samples = 0;
  for (std::uint16_t frame_index = 0;
       frame_index < frame_count;
       ++frame_index) {
    if (!has_bytes(offset, kImaAdpcmFrameHeaderBytes, encoded_size)) {
      return ImaAdpcmDecoderStatus::InvalidAsset;
    }
    const auto sample_count = read_le16(encoded + offset);
    const auto step_index = encoded[offset + 4U];
    const auto reserved = encoded[offset + 5U];
    if (sample_count == 0 || sample_count > kImaAdpcmFrameSamples ||
        (frame_index + 1U < frame_count &&
         sample_count != kImaAdpcmFrameSamples) ||
        step_index >= kStepTable.size() || reserved != 0) {
      return ImaAdpcmDecoderStatus::InvalidAsset;
    }

    const auto encoded_samples = payload_bytes(sample_count);
    const auto payload_offset = offset + kImaAdpcmFrameHeaderBytes;
    if (!has_bytes(payload_offset, encoded_samples, encoded_size)) {
      return ImaAdpcmDecoderStatus::InvalidAsset;
    }
    if ((sample_count % 2U) == 0U && encoded_samples != 0U &&
        (encoded[payload_offset + encoded_samples - 1U] & 0xF0U) != 0U) {
      return ImaAdpcmDecoderStatus::InvalidAsset;
    }

    observed_samples += sample_count;
    if (observed_samples > std::numeric_limits<std::uint32_t>::max()) {
      return ImaAdpcmDecoderStatus::InvalidAsset;
    }
    offset = payload_offset + encoded_samples;
  }
  if (offset != encoded_size || observed_samples != total_samples) {
    return ImaAdpcmDecoderStatus::InvalidAsset;
  }

  encoded_ = encoded;
  encoded_size_ = encoded_size;
  info_.sample_rate = kImaAdpcmAssetSampleRate;
  info_.total_samples = total_samples;
  info_.frame_samples = kImaAdpcmFrameSamples;
  info_.frame_count = frame_count;
  info_.channels = kImaAdpcmAssetChannels;
  ready_ = true;
  return reset();
}

ImaAdpcmDecoderStatus ImaAdpcmDecoder::reset() {
  if (!ready_) {
    return ImaAdpcmDecoderStatus::NotReady;
  }
  next_offset_ = kImaAdpcmAssetHeaderBytes;
  decoded_samples_ = 0;
  next_frame_index_ = 0;
  return ImaAdpcmDecoderStatus::Ok;
}

ImaAdpcmDecoderStatus ImaAdpcmDecoder::decode_next(
    std::int16_t* output,
    std::size_t output_capacity_samples,
    std::size_t* output_samples) {
  if (output_samples == nullptr) {
    return ImaAdpcmDecoderStatus::InvalidArgument;
  }
  *output_samples = 0;
  if (!ready_) {
    return ImaAdpcmDecoderStatus::NotReady;
  }
  if (next_frame_index_ >= info_.frame_count) {
    return ImaAdpcmDecoderStatus::End;
  }
  if (output == nullptr) {
    return ImaAdpcmDecoderStatus::InvalidArgument;
  }
  if (!has_bytes(next_offset_,
                 kImaAdpcmFrameHeaderBytes,
                 encoded_size_)) {
    return ImaAdpcmDecoderStatus::InvalidAsset;
  }

  const auto sample_count = read_le16(encoded_ + next_offset_);
  if (output_capacity_samples < sample_count) {
    return ImaAdpcmDecoderStatus::OutputTooSmall;
  }
  const auto encoded_samples = payload_bytes(sample_count);
  const auto payload_offset =
      next_offset_ + kImaAdpcmFrameHeaderBytes;
  if (sample_count == 0 || sample_count > kImaAdpcmFrameSamples ||
      !has_bytes(payload_offset, encoded_samples, encoded_size_) ||
      encoded_[next_offset_ + 4U] >= kStepTable.size() ||
      encoded_[next_offset_ + 5U] != 0) {
    return ImaAdpcmDecoderStatus::InvalidAsset;
  }

  std::int32_t predictor =
      read_signed_le16(encoded_ + next_offset_ + 2U);
  std::int32_t step_index = encoded_[next_offset_ + 4U];
  output[0] = static_cast<std::int16_t>(predictor);
  for (std::size_t sample_index = 1;
       sample_index < sample_count;
       ++sample_index) {
    const auto nibble_index = sample_index - 1U;
    const auto packed = encoded_[
        payload_offset + (nibble_index / 2U)];
    const auto code = static_cast<std::uint8_t>(
        (nibble_index % 2U) == 0U ? packed & 0x0FU : packed >> 4U);
    output[sample_index] =
        decode_nibble(code, &predictor, &step_index);
  }

  next_offset_ = payload_offset + encoded_samples;
  decoded_samples_ += sample_count;
  ++next_frame_index_;
  *output_samples = sample_count;
  return ImaAdpcmDecoderStatus::Ok;
}

bool ImaAdpcmDecoder::ready() const {
  return ready_;
}

const ImaAdpcmAssetInfo& ImaAdpcmDecoder::info() const {
  return info_;
}

std::size_t ImaAdpcmDecoder::encoded_size() const {
  return encoded_size_;
}

std::uint16_t ImaAdpcmDecoder::next_frame_index() const {
  return next_frame_index_;
}

std::uint32_t ImaAdpcmDecoder::decoded_samples() const {
  return decoded_samples_;
}

void ImaAdpcmDecoder::clear() {
  encoded_ = nullptr;
  encoded_size_ = 0;
  next_offset_ = 0;
  info_ = {};
  decoded_samples_ = 0;
  next_frame_index_ = 0;
  ready_ = false;
}

}  // namespace easy_input
