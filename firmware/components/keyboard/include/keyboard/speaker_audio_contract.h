#pragma once

#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

// The MAX98357A and the standard Opus API do not support the same complete
// sample-rate set. Keep the playback rate in their shared set so an App-created
// asset can be decoded directly into hardware-valid I2S frames.
constexpr bool max98357a_supports_lrclk(std::uint32_t sample_rate) {
  return sample_rate == 8000U ||
         sample_rate == 16000U ||
         sample_rate == 32000U ||
         sample_rate == 44100U ||
         sample_rate == 48000U ||
         sample_rate == 88200U ||
         sample_rate == 96000U;
}

constexpr bool opus_supports_pcm_rate(std::uint32_t sample_rate) {
  return sample_rate == 8000U ||
         sample_rate == 12000U ||
         sample_rate == 16000U ||
         sample_rate == 24000U ||
         sample_rate == 48000U;
}

constexpr std::uint32_t kSpeakerPlaybackSampleRate = 48000U;
constexpr std::uint32_t kSpeakerPlaybackFrameMilliseconds = 10U;
constexpr std::size_t kSpeakerPlaybackDmaDescriptorCount = 4U;
constexpr std::size_t kSpeakerPlaybackPreloadZeroFrames = 1U;
constexpr std::size_t kSpeakerPlaybackTailZeroFrames = 2U;

// ESP-IDF's TX write API returns after copying a frame into a free DMA
// descriptor, not after that descriptor reaches the wire. With one leading
// zero frame and a circular descriptor ring, a freshly queued PCM frame can
// still be behind at most desc_count - 1 frames. Report this conservative
// upper bound instead of presenting the copy-complete timestamp as audible
// latency.
constexpr std::uint32_t speaker_first_pcm_queue_upper_bound_us(
    std::size_t descriptor_count,
    std::uint32_t frame_milliseconds) {
  return descriptor_count <= 1U
      ? 0U
      : static_cast<std::uint32_t>(descriptor_count - 1U) *
            frame_milliseconds * 1000U;
}

// A full-depth sequence of zero writes forces every previously queued audio
// descriptor to reach TX EOF. The extra tail frames then establish the
// required audible silence before I2S is disabled. Cancellation does not use
// this plan: it stops the channel immediately so queued audio cannot continue
// while the higher-priority microphone waits.
constexpr std::size_t speaker_normal_drain_zero_frames(
    std::size_t descriptor_count,
    std::size_t tail_zero_frames) {
  return descriptor_count + tail_zero_frames;
}

static_assert(max98357a_supports_lrclk(kSpeakerPlaybackSampleRate),
              "Speaker LRCLK must be supported by MAX98357A");
static_assert(opus_supports_pcm_rate(kSpeakerPlaybackSampleRate),
              "Speaker PCM rate must be supported by standard Opus");
static_assert(kSpeakerPlaybackPreloadZeroFrames == 1U);
static_assert(
    speaker_normal_drain_zero_frames(
        kSpeakerPlaybackDmaDescriptorCount,
        kSpeakerPlaybackTailZeroFrames) == 6U);
static_assert(
    speaker_first_pcm_queue_upper_bound_us(
        kSpeakerPlaybackDmaDescriptorCount,
        kSpeakerPlaybackFrameMilliseconds) == 30000U);

}  // namespace ai_keyboard
