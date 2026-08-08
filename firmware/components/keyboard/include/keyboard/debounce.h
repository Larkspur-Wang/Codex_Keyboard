#pragma once

#include <cstdint>

namespace ai_keyboard {

struct DebounceResult {
  bool changed = false;
  bool state = false;
  bool filtered_transition = false;
};

class DebouncedInput {
 public:
  explicit DebouncedInput(std::uint32_t interval_ms = 20);
  DebouncedInput(std::uint32_t press_interval_ms,
                 std::uint32_t release_interval_ms);

  void reset(bool raw_state, std::uint32_t now_ms);
  DebounceResult update(bool raw_state, std::uint32_t now_ms);
  DebounceResult latch_wake_state(bool raw_state, std::uint32_t now_ms);
  bool stable_state() const;

 private:
  std::uint32_t interval_for(bool state) const;

  std::uint32_t press_interval_ms_ = 20;
  std::uint32_t release_interval_ms_ = 20;
  bool initialized_ = false;
  bool stable_state_ = false;
  bool candidate_state_ = false;
  std::uint32_t candidate_since_ms_ = 0;
};

}  // namespace ai_keyboard
