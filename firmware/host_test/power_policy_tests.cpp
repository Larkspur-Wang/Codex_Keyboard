#include <cassert>
#include <string_view>

#include "keyboard/power_policy.h"

namespace {

ai_keyboard::PowerPolicyInputs ready_inputs(std::uint32_t now_ms) {
  return {
      now_ms,
      0,
      false,
      false,
      true,
      false,
      false,
      false,
      false,
      false,
      false,
      false,
      true,
      true,
      false,
      false,
  };
}

void transitions_at_expected_thresholds() {
  auto inputs = ready_inputs(29'999);
  auto decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::Active);
  assert(decision.poll_ms == 5);

  inputs.now_ms = 30'000;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::Idle);
  assert(decision.poll_ms == 80);

  inputs.now_ms = 180'000;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::DeepIdle);
  assert(decision.poll_ms == 750);
  assert(decision.light_sleep_allowed);

  inputs.now_ms = 1'800'000;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.deep_sleep_allowed);
}

void activity_and_external_power_force_active_mode() {
  auto inputs = ready_inputs(1'800'000);
  inputs.input_active = true;
  auto decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::Active);
  assert(!decision.light_sleep_allowed);
  assert(std::string_view(decision.light_sleep_block) == "mode");
  assert(!decision.deep_sleep_allowed);
  assert(std::string_view(decision.deep_sleep_block) == "input");

  inputs.input_active = false;
  inputs.external_power = true;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::Active);
  assert(std::string_view(decision.deep_sleep_block) == "external_power");
}

void audio_blocks_light_sleep_but_idle_wifi_does_not() {
  auto inputs = ready_inputs(1'800'000);
  inputs.audio_streaming = true;
  auto decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::DeepIdle);
  assert(decision.poll_ms == 5);
  assert(std::string_view(decision.light_sleep_block) == "keyboard_mic");
  assert(std::string_view(decision.deep_sleep_block) == "keyboard_mic");

  inputs.audio_streaming = false;
  inputs.wifi_active = true;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.light_sleep_allowed);
  assert(std::string_view(decision.light_sleep_block) == "ok");
  // A connected idle station may use controlled light sleep. At the deep
  // sleep threshold the policy asks the audio owner to release Wi-Fi first.
  assert(!decision.deep_sleep_allowed);
  assert(decision.wifi_release_required);
  assert(std::string_view(decision.deep_sleep_block) == "wifi_control");

  inputs.config_window_active = true;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(!decision.wifi_release_required);
  assert(std::string_view(decision.deep_sleep_block) == "config_window");

  inputs.config_window_active = false;
  inputs.key_wake_asserted = true;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(!decision.wifi_release_required);
  assert(std::string_view(decision.deep_sleep_block) == "key_wake_low");
}

void speaker_playback_blocks_sleep_and_keyboard_mic_has_priority() {
  auto inputs = ready_inputs(1'800'000);
  inputs.speaker_playback_active = true;
  auto decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::DeepIdle);
  assert(decision.poll_ms == 5);
  assert(std::string_view(decision.light_sleep_block) == "speaker_playback");
  assert(std::string_view(decision.deep_sleep_block) == "speaker_playback");

  inputs.audio_streaming = true;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(std::string_view(decision.light_sleep_block) == "keyboard_mic");
  assert(std::string_view(decision.deep_sleep_block) == "keyboard_mic");
}

void wake_validation_and_backoff_are_enforced() {
  auto inputs = ready_inputs(180'000);
  inputs.key_wake_verified = false;
  auto decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.poll_ms == 250);
  assert(std::string_view(decision.light_sleep_block) == "key_wake_unverified");

  inputs.key_wake_verified = true;
  inputs.key_wake_asserted = true;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(std::string_view(decision.light_sleep_block) == "key_wake_low");

  inputs.key_wake_asserted = false;
  inputs.light_sleep_backoff = true;
  decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(std::string_view(decision.light_sleep_block) == "backoff");
}

void elapsed_time_handles_uptime_wraparound() {
  auto inputs = ready_inputs(100);
  inputs.last_activity_ms = 0xFFFF'FF00U;
  const auto decision = ai_keyboard::evaluate_power_policy(inputs);
  assert(decision.mode == ai_keyboard::PowerPolicyMode::Active);
}

void wifi_power_stages_only_release_for_coordinated_deep_sleep() {
  using ai_keyboard::WifiPowerStage;

  assert(ai_keyboard::evaluate_wifi_power_stage(119'999, false, false) ==
         WifiPowerStage::Active);
  assert(ai_keyboard::evaluate_wifi_power_stage(120'000, false, false) ==
         WifiPowerStage::Throttled);
  assert(ai_keyboard::evaluate_wifi_power_stage(900'000, false, false) ==
         WifiPowerStage::Throttled);
  assert(ai_keyboard::evaluate_wifi_power_stage(1'800'000, false, false) ==
         WifiPowerStage::Throttled);
  assert(ai_keyboard::evaluate_wifi_power_stage(1'800'000, false, true) ==
         WifiPowerStage::Released);
  assert(ai_keyboard::evaluate_wifi_power_stage(1'800'000, true, true) ==
         WifiPowerStage::Active);
}

void wifi_release_waits_for_a_quiet_control_window() {
  assert(!ai_keyboard::wifi_release_ready_for_deep_sleep(4'999, false, true));
  assert(ai_keyboard::wifi_release_ready_for_deep_sleep(5'000, false, true));
  assert(!ai_keyboard::wifi_release_ready_for_deep_sleep(30'000, true, true));
  assert(!ai_keyboard::wifi_release_ready_for_deep_sleep(30'000, false, false));
}

}  // namespace

int main() {
  transitions_at_expected_thresholds();
  activity_and_external_power_force_active_mode();
  audio_blocks_light_sleep_but_idle_wifi_does_not();
  speaker_playback_blocks_sleep_and_keyboard_mic_has_priority();
  wake_validation_and_backoff_are_enforced();
  elapsed_time_handles_uptime_wraparound();
  wifi_power_stages_only_release_for_coordinated_deep_sleep();
  wifi_release_waits_for_a_quiet_control_window();
  return 0;
}
