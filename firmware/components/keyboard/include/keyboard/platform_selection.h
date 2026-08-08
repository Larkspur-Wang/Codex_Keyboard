#pragma once

#include <cstdint>

#include "keyboard/keymap.h"

namespace ai_keyboard {

constexpr std::uint32_t kPlatformSelectionChordGuardMs = 100;

enum class PlatformSelectionOutcome {
  None,
  MacOS,
  Windows,
  Conflict,
  TimedOut,
  Cancelled,
};

struct PlatformSelectionResult {
  bool consumed = false;
  PlatformSelectionOutcome outcome = PlatformSelectionOutcome::None;
};

class PlatformSelectionController {
 public:
  void arm(std::uint32_t now_ms, std::uint32_t timeout_ms);
  PlatformSelectionResult handle_event(InputId input,
                                       InputPhase phase,
                                       std::uint32_t now_ms);
  PlatformSelectionResult update(std::uint32_t now_ms);
  bool active() const;

 private:
  PlatformSelectionResult finish(PlatformSelectionOutcome outcome,
                                 bool consumed);

  bool active_ = false;
  std::uint32_t timeout_deadline_ms_ = 0;
  std::uint32_t decision_deadline_ms_ = 0;
  std::uint8_t seen_mask_ = 0;
  std::uint8_t down_mask_ = 0;
  std::uint8_t drain_mask_ = 0;
};

}  // namespace ai_keyboard
