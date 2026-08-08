#include <cassert>
#include <set>
#include <string>

#include "keyboard/input_feedback.h"

using ai_keyboard::FeedbackDirection;
using ai_keyboard::FeedbackEffectKind;
using ai_keyboard::InputActivityKind;
using ai_keyboard::InputId;
using ai_keyboard::InputPhase;

namespace {

std::string color_key(const ai_keyboard::FeedbackColor& color) {
  return std::to_string(color.red) + "," +
         std::to_string(color.green) + "," +
         std::to_string(color.blue);
}

std::uint8_t max_channel(const ai_keyboard::FeedbackColor& color) {
  auto max = color.red > color.green ? color.red : color.green;
  return max > color.blue ? max : color.blue;
}

}  // namespace

void eight_keys_use_unique_dim_colors() {
  const InputId keys[] = {
      InputId::Key1,
      InputId::Key2,
      InputId::Key3,
      InputId::Key4,
      InputId::Key5,
      InputId::Key6,
      InputId::Key7,
      InputId::Key8,
  };
  const std::string expected[] = {
      "28,0,0",
      "28,10,0",
      "26,20,0",
      "0,28,0",
      "0,22,22",
      "0,0,28",
      "18,0,28",
      "22,22,22",
  };
  std::set<std::string> seen;

  for (std::size_t index = 0; index < 8; ++index) {
    const auto key = keys[index];
    const auto feedback = ai_keyboard::feedback_for_input_event(key, InputPhase::Pressed);
    assert(feedback.active);
    assert(feedback.effect == FeedbackEffectKind::LightBarRipple);
    assert(feedback.duration_ms <= 160);
    assert(feedback.frame_interval_ms > 0);
    assert(max_channel(feedback.color) <= 28);
    assert(color_key(feedback.color) == expected[index]);
    seen.insert(color_key(feedback.color));
  }
  assert(seen.size() == 8);
}

void key_release_returns_to_quiet_idle() {
  const auto feedback = ai_keyboard::feedback_for_input_event(InputId::Key1, InputPhase::Released);

  assert(!feedback.active);
  assert(feedback.effect == FeedbackEffectKind::None);
}

void encoder_turns_have_directional_flows() {
  const auto left = ai_keyboard::feedback_for_input_event(InputId::EncoderLeft, InputPhase::Pressed);
  const auto right = ai_keyboard::feedback_for_input_event(InputId::EncoderRight, InputPhase::Pressed);

  assert(left.active);
  assert(right.active);
  assert(left.effect == FeedbackEffectKind::DirectionalFlow);
  assert(right.effect == FeedbackEffectKind::DirectionalFlow);
  assert(left.direction == FeedbackDirection::Left);
  assert(right.direction == FeedbackDirection::Right);
  assert(color_key(left.color) == "0,0,28");
  assert(color_key(right.color) == "0,22,22");
  assert(left.duration_ms <= 180);
  assert(right.duration_ms <= 180);
  assert(left.frame_interval_ms <= 50);
  assert(right.frame_interval_ms <= 50);
}

void encoder_press_gets_a_short_confirm_pulse() {
  const auto feedback = ai_keyboard::feedback_for_input_event(InputId::EncoderPress, InputPhase::Pressed);

  assert(feedback.active);
  assert(feedback.effect == FeedbackEffectKind::ConfirmPulse);
  assert(feedback.duration_ms <= 350);
  assert(feedback.frame_interval_ms <= 70);
  assert(max_channel(feedback.color) <= 22);
}

int main() {
  eight_keys_use_unique_dim_colors();
  key_release_returns_to_quiet_idle();
  encoder_turns_have_directional_flows();
  encoder_press_gets_a_short_confirm_pulse();
  return 0;
}
