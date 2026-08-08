#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

constexpr std::size_t kEncoderStepQueueCapacity = 32;

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
