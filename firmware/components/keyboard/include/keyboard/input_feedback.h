#pragma once

#include <cstdint>

#include "keyboard/keymap.h"

namespace ai_keyboard {

enum class InputActivityKind {
  KeyPressed,
  KeyReleased,
  EncoderTurn,
  EncoderPressed,
  EncoderReleased,
};

enum class FeedbackEffectKind {
  None,
  Solid,
  LightBarRipple,
  DirectionalFlow,
  ConfirmPulse,
  RainbowMarquee,
};

enum class FeedbackDirection {
  None,
  Left,
  Right,
};

struct FeedbackColor {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

struct InputActivityFeedback {
  bool active = false;
  FeedbackEffectKind effect = FeedbackEffectKind::None;
  FeedbackDirection direction = FeedbackDirection::None;
  FeedbackColor color;
  std::uint32_t duration_ms = 0;
  std::uint32_t frame_interval_ms = 0;
};

InputActivityFeedback feedback_for_input_activity(InputActivityKind kind);
InputActivityFeedback feedback_for_input_event(InputId input, InputPhase phase);

}  // namespace ai_keyboard
