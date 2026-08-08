#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "assets/easyinput_boot_probe_eiad.h"
#include "ima_adpcm_decoder.h"

namespace {

using easy_input::ImaAdpcmDecoder;
using easy_input::ImaAdpcmDecoderStatus;

constexpr std::array<std::uint8_t, 28> kGoldenAsset{{
    'E', 'I', 'A', 'D', 1, 1,
    0x80, 0xBB, 0x00, 0x00,
    0xE0, 0x01,
    0x01, 0x00,
    0x05, 0x00, 0x00, 0x00,
    0x14, 0x00,
    0x05, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0x10, 0x32,
}};

constexpr std::array<std::uint8_t, 27> kPositiveSaturationAsset{{
    'E', 'I', 'A', 'D', 1, 1,
    0x80, 0xBB, 0x00, 0x00,
    0xE0, 0x01,
    0x01, 0x00,
    0x02, 0x00, 0x00, 0x00,
    0x14, 0x00,
    0x02, 0x00,
    0xF8, 0x7F,
    0x58, 0x00,
    0x07,
}};

constexpr std::array<std::uint8_t, 27> kNegativeSaturationAsset{{
    'E', 'I', 'A', 'D', 1, 1,
    0x80, 0xBB, 0x00, 0x00,
    0xE0, 0x01,
    0x01, 0x00,
    0x02, 0x00, 0x00, 0x00,
    0x14, 0x00,
    0x02, 0x00,
    0x08, 0x80,
    0x58, 0x00,
    0x0F,
}};

std::uint64_t fnv1a_pcm(std::uint64_t hash,
                        const std::int16_t* samples,
                        std::size_t sample_count) {
  for (std::size_t index = 0; index < sample_count; ++index) {
    const auto value = static_cast<std::uint16_t>(samples[index]);
    const std::array<std::uint8_t, 2> bytes{{
        static_cast<std::uint8_t>(value & 0xFFU),
        static_cast<std::uint8_t>(value >> 8U),
    }};
    for (const auto byte : bytes) {
      hash ^= byte;
      hash *= UINT64_C(1099511628211);
    }
  }
  return hash;
}

void golden_nibbles_decode_low_first() {
  ImaAdpcmDecoder decoder;
  assert(decoder.open(kGoldenAsset.data(), kGoldenAsset.size()) ==
         ImaAdpcmDecoderStatus::Ok);
  assert(decoder.ready());
  assert(decoder.info().sample_rate == 48000U);
  assert(decoder.info().channels == 1U);
  assert(decoder.info().frame_samples == 480U);
  assert(decoder.info().frame_count == 1U);
  assert(decoder.info().total_samples == 5U);

  std::array<std::int16_t, 5> output{};
  std::size_t output_samples = 0;
  assert(decoder.decode_next(
             output.data(), output.size(), &output_samples) ==
         ImaAdpcmDecoderStatus::Ok);
  assert(output_samples == output.size());
  assert((output == std::array<std::int16_t, 5>{{0, 0, 1, 4, 8}}));
  assert(decoder.decode_next(
             output.data(), output.size(), &output_samples) ==
         ImaAdpcmDecoderStatus::End);
  assert(output_samples == 0U);
}

void predictor_and_index_saturate_without_overflow() {
  ImaAdpcmDecoder decoder;
  assert(decoder.open(
             kPositiveSaturationAsset.data(),
             kPositiveSaturationAsset.size()) ==
         ImaAdpcmDecoderStatus::Ok);
  std::array<std::int16_t, 2> output{};
  std::size_t output_samples = 0;
  assert(decoder.decode_next(
             output.data(), output.size(), &output_samples) ==
         ImaAdpcmDecoderStatus::Ok);
  assert((output == std::array<std::int16_t, 2>{{32760, 32767}}));
  assert(decoder.open(
             kNegativeSaturationAsset.data(),
             kNegativeSaturationAsset.size()) ==
         ImaAdpcmDecoderStatus::Ok);
  assert(decoder.decode_next(
             output.data(), output.size(), &output_samples) ==
         ImaAdpcmDecoderStatus::Ok);
  assert((output == std::array<std::int16_t, 2>{{-32760, -32768}}));
}

void output_too_small_does_not_advance_decoder_state() {
  ImaAdpcmDecoder decoder;
  assert(decoder.open(kGoldenAsset.data(), kGoldenAsset.size()) ==
         ImaAdpcmDecoderStatus::Ok);
  std::array<std::int16_t, 5> output{};
  std::size_t output_samples = 99;
  assert(decoder.decode_next(
             output.data(), output.size() - 1U, &output_samples) ==
         ImaAdpcmDecoderStatus::OutputTooSmall);
  assert(output_samples == 0U);
  assert(decoder.next_frame_index() == 0U);
  assert(decoder.decoded_samples() == 0U);
  assert(decoder.decode_next(
             output.data(), output.size(), &output_samples) ==
         ImaAdpcmDecoderStatus::Ok);
  assert(output_samples == output.size());
}

void malformed_assets_are_rejected_before_playback() {
  ImaAdpcmDecoder decoder;
  assert(decoder.open(nullptr, 0) ==
         ImaAdpcmDecoderStatus::InvalidArgument);
  assert(decoder.open(
             kGoldenAsset.data(),
             easy_input::kImaAdpcmAssetHeaderBytes - 1U) ==
         ImaAdpcmDecoderStatus::InvalidAsset);

  auto malformed = kGoldenAsset;
  malformed[0] = 'X';
  assert(decoder.open(malformed.data(), malformed.size()) ==
         ImaAdpcmDecoderStatus::InvalidAsset);
  malformed = kGoldenAsset;
  malformed[4] = 2;
  assert(decoder.open(malformed.data(), malformed.size()) ==
         ImaAdpcmDecoderStatus::InvalidAsset);
  malformed = kGoldenAsset;
  malformed[24] = 89;
  assert(decoder.open(malformed.data(), malformed.size()) ==
         ImaAdpcmDecoderStatus::InvalidAsset);
  malformed = kGoldenAsset;
  malformed[25] = 1;
  assert(decoder.open(malformed.data(), malformed.size()) ==
         ImaAdpcmDecoderStatus::InvalidAsset);
  auto malformed_padding = kPositiveSaturationAsset;
  malformed_padding[26] |= 0xF0U;
  assert(decoder.open(
             malformed_padding.data(), malformed_padding.size()) ==
         ImaAdpcmDecoderStatus::InvalidAsset);
  malformed = kGoldenAsset;
  malformed[14] = 6;
  assert(decoder.open(malformed.data(), malformed.size()) ==
         ImaAdpcmDecoderStatus::InvalidAsset);
  assert(decoder.open(
             kGoldenAsset.data(), kGoldenAsset.size() - 1U) ==
         ImaAdpcmDecoderStatus::InvalidAsset);
}

std::uint64_t decode_fixture(ImaAdpcmDecoder* decoder,
                             std::uint32_t* decoded_samples,
                             std::uint32_t* absolute_peak,
                             std::uint64_t* sum_squares) {
  assert(decoder != nullptr);
  std::array<std::int16_t, easy_input::kImaAdpcmFrameSamples> frame{};
  std::uint64_t hash = UINT64_C(14695981039346656037);
  *decoded_samples = 0;
  *absolute_peak = 0;
  *sum_squares = 0;
  while (true) {
    std::size_t frame_samples = 0;
    const auto status = decoder->decode_next(
        frame.data(), frame.size(), &frame_samples);
    if (status == ImaAdpcmDecoderStatus::End) {
      break;
    }
    assert(status == ImaAdpcmDecoderStatus::Ok);
    assert(frame_samples > 0U);
    assert(frame_samples <= frame.size());
    hash = fnv1a_pcm(hash, frame.data(), frame_samples);
    *decoded_samples += static_cast<std::uint32_t>(frame_samples);
    for (std::size_t index = 0; index < frame_samples; ++index) {
      const auto sample = static_cast<std::int32_t>(frame[index]);
      const auto magnitude = static_cast<std::uint32_t>(
          sample < 0 ? -sample : sample);
      if (magnitude > *absolute_peak) {
        *absolute_peak = magnitude;
      }
      *sum_squares += static_cast<std::uint64_t>(
          sample * static_cast<std::int64_t>(sample));
    }
  }
  return hash;
}

void fixed_fixture_is_small_audible_and_reset_deterministic() {
  const auto& fixture =
      easy_input::ima_adpcm_assets::kEasyInputBootProbeEiad;
  static_assert(fixture.size() == 6872U);
  ImaAdpcmDecoder decoder;
  assert(decoder.open(fixture.data(), fixture.size()) ==
         ImaAdpcmDecoderStatus::Ok);
  assert(decoder.info().sample_rate == 48000U);
  assert(decoder.info().channels == 1U);
  assert(decoder.info().frame_samples == 480U);
  assert(decoder.info().frame_count == 28U);
  assert(decoder.info().total_samples == 13369U);
  assert(fixture.size() * 100U <=
         decoder.info().total_samples * sizeof(std::int16_t) * 26U);

  std::uint32_t sample_count = 0;
  std::uint32_t peak = 0;
  std::uint64_t sum_squares = 0;
  const auto first_hash = decode_fixture(
      &decoder, &sample_count, &peak, &sum_squares);
  assert(sample_count == 13369U);
  assert(peak == 15465U);
  assert(sum_squares == UINT64_C(160475123025));
  assert(first_hash == UINT64_C(0x7abb5b0344f7014d));

  assert(decoder.reset() == ImaAdpcmDecoderStatus::Ok);
  std::uint32_t repeated_samples = 0;
  std::uint32_t repeated_peak = 0;
  std::uint64_t repeated_sum_squares = 0;
  const auto repeated_hash = decode_fixture(
      &decoder,
      &repeated_samples,
      &repeated_peak,
      &repeated_sum_squares);
  assert(repeated_hash == first_hash);
  assert(repeated_samples == sample_count);
  assert(repeated_peak == peak);
  assert(repeated_sum_squares == sum_squares);
}

}  // namespace

int main() {
  golden_nibbles_decode_low_first();
  predictor_and_index_saturate_without_overflow();
  output_too_small_does_not_advance_decoder_state();
  malformed_assets_are_rejected_before_playback();
  fixed_fixture_is_small_audible_and_reset_deterministic();
  return 0;
}
