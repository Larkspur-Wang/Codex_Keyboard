#include "keyboard/input_feedback.h"

#include <array>

namespace ai_keyboard {
namespace {

constexpr std::array<FeedbackColor, 8> kLightBarKeyColors{{
    {28, 0, 0},    // KEY1 red
    {28, 10, 0},   // KEY2 orange
    {26, 20, 0},   // KEY3 yellow
    {0, 28, 0},    // KEY4 green
    {0, 22, 22},   // KEY5 cyan
    {0, 0, 28},    // KEY6 blue
    {18, 0, 28},   // KEY7 purple
    {22, 22, 22},  // KEY8 white
}};

FeedbackColor color_for_key(InputId input) {
  const auto index = static_cast<std::size_t>(input);
  if (index < kLightBarKeyColors.size()) {
    return kLightBarKeyColors[index];
  }
  return {};
}

}  // namespace

InputActivityFeedback feedback_for_input_activity(InputActivityKind kind) {
  switch (kind) {
    case InputActivityKind::KeyPressed:
      return {true, FeedbackEffectKind::LightBarRipple, FeedbackDirection::None, {22, 22, 22}, 140, 35};
    case InputActivityKind::KeyReleased:
      return {};
    case InputActivityKind::EncoderTurn:
      return {true, FeedbackEffectKind::DirectionalFlow, FeedbackDirection::Right, {0, 22, 22}, 160, 40};
    case InputActivityKind::EncoderPressed:
      return {true, FeedbackEffectKind::ConfirmPulse, FeedbackDirection::None, {22, 18, 10}, 300, 60};
    case InputActivityKind::EncoderReleased:
      return {};
  }
  return {};
}

InputActivityFeedback feedback_for_input_event(InputId input, InputPhase phase) {
  if (phase == InputPhase::Released) {
    return {};
  }
  switch (input) {
    case InputId::Key1:
    case InputId::Key2:
    case InputId::Key3:
    case InputId::Key4:
    case InputId::Key5:
    case InputId::Key6:
    case InputId::Key7:
    case InputId::Key8:
      return {true,
              FeedbackEffectKind::LightBarRipple,
              FeedbackDirection::None,
              color_for_key(input),
              140,
              35};
    case InputId::EncoderLeft:
      return {true,
              FeedbackEffectKind::DirectionalFlow,
              FeedbackDirection::Left,
              {0, 0, 28},
              160,
              40};
    case InputId::EncoderRight:
      return {true,
              FeedbackEffectKind::DirectionalFlow,
              FeedbackDirection::Right,
              {0, 22, 22},
              160,
              40};
    case InputId::EncoderPress:
      return {true,
              FeedbackEffectKind::ConfirmPulse,
              FeedbackDirection::None,
              {22, 18, 10},
              300,
              60};
    case InputId::Count:
      return {};
  }
  return {};
}

}  // namespace ai_keyboard
