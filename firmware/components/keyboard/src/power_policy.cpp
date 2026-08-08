#include "keyboard/power_policy.h"

#include <string_view>

namespace ai_keyboard {
namespace {

PowerPolicyMode select_mode(const PowerPolicyInputs& inputs,
                            const PowerPolicyConfig& config,
                            std::uint32_t idle_ms) {
  if (inputs.external_power || inputs.input_active || idle_ms < config.idle_after_ms) {
    return PowerPolicyMode::Active;
  }
  if (idle_ms >= config.deep_idle_after_ms) {
    return PowerPolicyMode::DeepIdle;
  }
  return PowerPolicyMode::Idle;
}

std::uint32_t select_poll_ms(const PowerPolicyInputs& inputs,
                             const PowerPolicyConfig& config,
                             PowerPolicyMode mode) {
  // Microphone, speaker playback, and speaker-asset Flash work all depend on
  // bounded owner-loop permits. Leaving that loop at the 80-750 ms idle
  // cadence turns a healthy 576 KiB erase/readback into a multi-minute
  // operation and can trip transport watchdogs. Keep the loop responsive
  // while either audio direction owns resources; foreground input checks still
  // preempt every individual Flash permit.
  if (mode == PowerPolicyMode::Active ||
      inputs.input_active ||
      inputs.external_power ||
      inputs.audio_streaming ||
      inputs.speaker_playback_active) {
    return config.active_poll_ms;
  }
  if (mode == PowerPolicyMode::DeepIdle && inputs.ble_connected) {
    return inputs.key_wake_verified ? config.deep_idle_connected_poll_ms
                                    : config.deep_idle_unverified_poll_ms;
  }
  if (inputs.ble_connected) {
    return config.idle_connected_poll_ms;
  }
  if (inputs.usb_mounted) {
    return config.idle_usb_poll_ms;
  }
  return config.idle_battery_poll_ms;
}

const char* light_sleep_block_reason(const PowerPolicyInputs& inputs,
                                     PowerPolicyMode mode,
                                     std::uint32_t idle_ms,
                                     const PowerPolicyConfig& config) {
  if (!inputs.wake_source_configured) return "not_configured";
  if (mode != PowerPolicyMode::DeepIdle) return "mode";
  if (idle_ms < config.deep_idle_after_ms) return "idle";
  if (inputs.external_power) return "external_power";
  if (inputs.usb_mounted) return "usb_hid";
  if (inputs.config_window_active) return "config_window";
  if (inputs.input_active) return "input";
  if (inputs.encoder_press_pending) return "encoder_press";
  if (inputs.wheel_report_pending) return "wheel_queue";
  if (inputs.audio_streaming) return "keyboard_mic";
  if (inputs.speaker_playback_active) return "speaker_playback";
  if (!inputs.key_wake_verified) return "key_wake_unverified";
  if (inputs.key_wake_asserted) return "key_wake_low";
  if (inputs.light_sleep_backoff) return "backoff";
  return "ok";
}

const char* deep_sleep_block_reason(const PowerPolicyInputs& inputs,
                                    std::uint32_t idle_ms,
                                    const PowerPolicyConfig& config) {
  if (!inputs.wake_source_configured) return "not_configured";
  if (!inputs.key_wake_verified) return "key_wake_unverified";
  if (idle_ms < config.deep_sleep_after_ms) return "idle";
  if (inputs.external_power) return "external_power";
  if (inputs.usb_mounted) return "usb_hid";
  if (inputs.config_window_active) return "config_window";
  if (inputs.input_active) return "input";
  if (inputs.encoder_press_pending) return "encoder_press";
  if (inputs.wheel_report_pending) return "wheel_queue";
  if (inputs.audio_streaming) return "keyboard_mic";
  if (inputs.speaker_playback_active) return "speaker_playback";
  if (inputs.key_wake_asserted) return "key_wake_low";
  if (inputs.wifi_active) return "wifi_control";
  return "ok";
}

}  // namespace

PowerPolicyDecision evaluate_power_policy(const PowerPolicyInputs& inputs,
                                          const PowerPolicyConfig& config) {
  const std::uint32_t idle_ms = inputs.now_ms - inputs.last_activity_ms;
  PowerPolicyDecision decision;
  decision.mode = select_mode(inputs, config, idle_ms);
  decision.poll_ms = select_poll_ms(inputs, config, decision.mode);
  decision.light_sleep_block =
      light_sleep_block_reason(inputs, decision.mode, idle_ms, config);
  decision.light_sleep_allowed = std::string_view(decision.light_sleep_block) == "ok";
  decision.deep_sleep_block = deep_sleep_block_reason(inputs, idle_ms, config);
  decision.deep_sleep_allowed = std::string_view(decision.deep_sleep_block) == "ok";
  decision.wifi_release_required =
      std::string_view(decision.deep_sleep_block) == "wifi_control";
  return decision;
}

const char* power_policy_mode_name(PowerPolicyMode mode) {
  switch (mode) {
    case PowerPolicyMode::Active:
      return "active";
    case PowerPolicyMode::Idle:
      return "idle";
    case PowerPolicyMode::DeepIdle:
      return "deep_idle";
  }
  return "unknown";
}

WifiPowerStage evaluate_wifi_power_stage(std::uint32_t idle_ms,
                                         bool session_active,
                                         bool release_requested,
                                         const PowerPolicyConfig& config) {
  if (session_active || idle_ms < config.wifi_throttle_after_ms) {
    return WifiPowerStage::Active;
  }
  if (release_requested) {
    return WifiPowerStage::Released;
  }
  return WifiPowerStage::Throttled;
}

bool wifi_release_ready_for_deep_sleep(std::uint32_t request_elapsed_ms,
                                       bool session_active,
                                       bool release_requested,
                                       const PowerPolicyConfig& config) {
  return release_requested && !session_active &&
         request_elapsed_ms >= config.wifi_release_quiesce_ms;
}

const char* wifi_power_stage_name(WifiPowerStage stage) {
  switch (stage) {
    case WifiPowerStage::Active:
      return "active";
    case WifiPowerStage::Throttled:
      return "throttled";
    case WifiPowerStage::Released:
      return "released";
  }
  return "unknown";
}

}  // namespace ai_keyboard
