#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

constexpr std::size_t kEncoderStepQueueCapacity = 32;
constexpr std::uint8_t kSpeakerVolumeMinimum = 1;
constexpr std::uint8_t kSpeakerVolumeMaximum = 10;
constexpr std::uint8_t kSpeakerVolumeDefault = 7;

std::uint8_t adjust_speaker_volume_for_wired_encoder_step(
    std::uint8_t current_level,
    int encoder_step);
std::uint16_t speaker_volume_gain_per_mille(std::uint8_t level);
std::int16_t scale_speaker_sample(std::int16_t sample,
                                  std::uint8_t level);

class EncoderStepQueue {
 public:
  bool push(int step);
  bool pop(int* step);
  void clear();
  bool empty() const;
  std::size_t size() const;

 private:
  std::array<int, kEncoderStepQueueCapacity> steps_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

class EncoderDecoder {
 public:
  void reset(std::uint8_t state);
  int update(std::uint8_t state);
  std::uint32_t invalid_transition_count() const;
  std::uint32_t partial_reset_count() const;

 private:
  std::uint8_t previous_state_ = 0;
  int accumulator_ = 0;
  bool armed_ = true;
  std::uint32_t invalid_transition_count_ = 0;
  std::uint32_t partial_reset_count_ = 0;
};

}  // namespace ai_keyboard
