#include "keyboard/debounce.h"

namespace ai_keyboard {

DebouncedInput::DebouncedInput(std::uint32_t interval_ms)
    : DebouncedInput(interval_ms, interval_ms) {}

DebouncedInput::DebouncedInput(std::uint32_t press_interval_ms,
                               std::uint32_t release_interval_ms)
    : press_interval_ms_(press_interval_ms),
      release_interval_ms_(release_interval_ms) {}

std::uint32_t DebouncedInput::interval_for(bool state) const {
  return state ? press_interval_ms_ : release_interval_ms_;
}

void DebouncedInput::reset(bool raw_state, std::uint32_t now_ms) {
  initialized_ = true;
  stable_state_ = raw_state;
  candidate_state_ = raw_state;
  candidate_since_ms_ = now_ms;
}

DebounceResult DebouncedInput::update(bool raw_state, std::uint32_t now_ms) {
  if (!initialized_) {
    reset(raw_state, now_ms);
    return {false, stable_state_};
  }

  if (raw_state != candidate_state_) {
    DebounceResult result{false, stable_state_};
    if (candidate_state_ != stable_state_) {
      if (static_cast<std::uint32_t>(now_ms - candidate_since_ms_) >=
          interval_for(candidate_state_)) {
        stable_state_ = candidate_state_;
        result = {true, stable_state_};
      } else {
        result.filtered_transition = true;
      }
    }
    candidate_state_ = raw_state;
    candidate_since_ms_ = now_ms;
    return result;
  }

  if (candidate_state_ == stable_state_) {
    return {false, stable_state_};
  }

  if (static_cast<std::uint32_t>(now_ms - candidate_since_ms_) <
      interval_for(candidate_state_)) {
    return {false, stable_state_};
  }

  stable_state_ = candidate_state_;
  return {true, stable_state_};
}

DebounceResult DebouncedInput::latch_wake_state(bool raw_state, std::uint32_t now_ms) {
  if (!initialized_) {
    reset(raw_state, now_ms);
    return {false, stable_state_};
  }

  const bool changed = stable_state_ != raw_state;
  reset(raw_state, now_ms);
  return {changed, stable_state_};
}

bool DebouncedInput::stable_state() const {
  return stable_state_;
}

}  // namespace ai_keyboard
