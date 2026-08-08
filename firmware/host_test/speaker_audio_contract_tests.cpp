#include <cassert>

#include "keyboard/speaker_audio_contract.h"

namespace {

void playback_rate_is_valid_for_the_amplifier_and_opus() {
  static_assert(ai_keyboard::kSpeakerPlaybackSampleRate == 48000U);
  static_assert(ai_keyboard::max98357a_supports_lrclk(
      ai_keyboard::kSpeakerPlaybackSampleRate));
  static_assert(ai_keyboard::opus_supports_pcm_rate(
      ai_keyboard::kSpeakerPlaybackSampleRate));
}

void unsupported_rate_sets_do_not_drift_together() {
  static_assert(ai_keyboard::opus_supports_pcm_rate(24000U));
  static_assert(!ai_keyboard::max98357a_supports_lrclk(24000U));
  static_assert(ai_keyboard::max98357a_supports_lrclk(44100U));
  static_assert(!ai_keyboard::opus_supports_pcm_rate(44100U));

  assert(ai_keyboard::max98357a_supports_lrclk(8000U));
  assert(ai_keyboard::max98357a_supports_lrclk(16000U));
  assert(ai_keyboard::max98357a_supports_lrclk(48000U));
  assert(!ai_keyboard::max98357a_supports_lrclk(12000U));
}

void dma_plan_bounds_first_sound_and_drains_the_whole_ring() {
  static_assert(
      ai_keyboard::kSpeakerPlaybackFrameMilliseconds == 10U);
  static_assert(
      ai_keyboard::kSpeakerPlaybackDmaDescriptorCount == 4U);
  static_assert(
      ai_keyboard::kSpeakerPlaybackPreloadZeroFrames == 1U);
  static_assert(
      ai_keyboard::kSpeakerPlaybackTailZeroFrames == 2U);
  static_assert(ai_keyboard::speaker_first_pcm_queue_upper_bound_us(
                    ai_keyboard::kSpeakerPlaybackDmaDescriptorCount,
                    ai_keyboard::kSpeakerPlaybackFrameMilliseconds) ==
                30000U);
  static_assert(ai_keyboard::speaker_normal_drain_zero_frames(
                    ai_keyboard::kSpeakerPlaybackDmaDescriptorCount,
                    ai_keyboard::kSpeakerPlaybackTailZeroFrames) == 6U);

  assert(ai_keyboard::speaker_first_pcm_queue_upper_bound_us(1U, 10U) ==
         0U);
  assert(ai_keyboard::speaker_first_pcm_queue_upper_bound_us(2U, 10U) ==
         10000U);
  assert(ai_keyboard::speaker_normal_drain_zero_frames(4U, 0U) == 4U);
  assert(ai_keyboard::speaker_normal_drain_zero_frames(4U, 2U) == 6U);
}

}  // namespace

int main() {
  playback_rate_is_valid_for_the_amplifier_and_opus();
  unsupported_rate_sets_do_not_drift_together();
  dma_plan_bounds_first_sound_and_drains_the_whole_ring();
  return 0;
}
