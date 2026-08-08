#include "keyboard/platform_selection.h"

namespace ai_keyboard {
namespace {

constexpr std::uint8_t kKey1Mask = 1U << 0;
constexpr std::uint8_t kKey2Mask = 1U << 1;
constexpr std::uint8_t kConflictMask = kKey1Mask | kKey2Mask;

std::uint8_t platform_key_mask(InputId input) {
  if (input == InputId::Key1) return kKey1Mask;
  if (input == InputId::Key2) return kKey2Mask;
  return 0;
}

bool deadline_reached(std::uint32_t now_ms, std::uint32_t deadline_ms) {
  return static_cast<std::int32_t>(now_ms - deadline_ms) >= 0;
}

}  // namespace

void PlatformSelectionController::arm(std::uint32_t now_ms,
                                      std::uint32_t timeout_ms) {
  active_ = true;
  timeout_deadline_ms_ = now_ms + timeout_ms;
  decision_deadline_ms_ = 0;
  seen_mask_ = 0;
  down_mask_ = 0;
  drain_mask_ = 0;
}

PlatformSelectionResult PlatformSelectionController::handle_event(
    InputId input,
    InputPhase phase,
    std::uint32_t now_ms) {
  const auto key_mask = platform_key_mask(input);
  if (!active_) {
    if (key_mask != 0 && (drain_mask_ & key_mask) != 0) {
      if (phase == InputPhase::Released) {
        drain_mask_ = static_cast<std::uint8_t>(drain_mask_ & ~key_mask);
      }
      return {true, PlatformSelectionOutcome::None};
    }
    return {};
  }

  // Resolve an already-expired guard/timeout before interpreting this new
  // edge.  This keeps correctness independent of main-loop polling latency:
  // a genuinely later KEY2 press cannot retroactively turn a completed KEY1
  // choice into a chord conflict.
  PlatformSelectionResult elapsed_result;
  if (deadline_reached(now_ms, timeout_deadline_ms_)) {
    elapsed_result = finish(PlatformSelectionOutcome::TimedOut, false);
  } else if (decision_deadline_ms_ != 0 &&
             deadline_reached(now_ms, decision_deadline_ms_)) {
    elapsed_result = finish(seen_mask_ == kKey2Mask
                                ? PlatformSelectionOutcome::Windows
                                : PlatformSelectionOutcome::MacOS,
                            false);
  }
  if (elapsed_result.outcome != PlatformSelectionOutcome::None) {
    if (key_mask != 0 && (drain_mask_ & key_mask) != 0) {
      if (phase == InputPhase::Released) {
        drain_mask_ = static_cast<std::uint8_t>(drain_mask_ & ~key_mask);
      }
      elapsed_result.consumed = true;
    }
    return elapsed_result;
  }

  if (key_mask == 0) {
    if (phase == InputPhase::Pressed) {
      return finish(PlatformSelectionOutcome::Cancelled, false);
    }
    return {};
  }

  if (phase == InputPhase::Pressed) {
    seen_mask_ = static_cast<std::uint8_t>(seen_mask_ | key_mask);
    down_mask_ = static_cast<std::uint8_t>(down_mask_ | key_mask);
    decision_deadline_ms_ = 0;
    if (seen_mask_ == kConflictMask) {
      return finish(PlatformSelectionOutcome::Conflict, true);
    }
    return {true, PlatformSelectionOutcome::None};
  }

  down_mask_ = static_cast<std::uint8_t>(down_mask_ & ~key_mask);
  if (seen_mask_ != 0 && down_mask_ == 0) {
    decision_deadline_ms_ = now_ms + kPlatformSelectionChordGuardMs;
  }
  return {true, PlatformSelectionOutcome::None};
}

PlatformSelectionResult PlatformSelectionController::update(
    std::uint32_t now_ms) {
  if (!active_) {
    return {};
  }
  if (deadline_reached(now_ms, timeout_deadline_ms_)) {
    return finish(PlatformSelectionOutcome::TimedOut, false);
  }
  if (decision_deadline_ms_ != 0 &&
      deadline_reached(now_ms, decision_deadline_ms_)) {
    return finish(seen_mask_ == kKey2Mask
                      ? PlatformSelectionOutcome::Windows
                      : PlatformSelectionOutcome::MacOS,
                  false);
  }
  return {};
}

bool PlatformSelectionController::active() const {
  return active_;
}

PlatformSelectionResult PlatformSelectionController::finish(
    PlatformSelectionOutcome outcome,
    bool consumed) {
  drain_mask_ = static_cast<std::uint8_t>(drain_mask_ | down_mask_);
  active_ = false;
  timeout_deadline_ms_ = 0;
  decision_deadline_ms_ = 0;
  seen_mask_ = 0;
  down_mask_ = 0;
  return {consumed, outcome};
}

}  // namespace ai_keyboard
