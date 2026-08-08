#pragma once

#include <cstdint>

namespace ai_keyboard {

enum class PowerPolicyMode : std::uint8_t {
  Active,
  Idle,
  DeepIdle,
};

enum class WifiPowerStage : std::uint8_t {
  Active,
  Throttled,
  Released,
};

struct PowerPolicyConfig {
  std::uint32_t active_poll_ms = 5;
  std::uint32_t idle_connected_poll_ms = 80;
  std::uint32_t deep_idle_connected_poll_ms = 750;
  std::uint32_t deep_idle_unverified_poll_ms = 250;
  std::uint32_t idle_usb_poll_ms = 50;
  std::uint32_t idle_battery_poll_ms = 80;
  std::uint32_t idle_after_ms = 30 * 1000;
  std::uint32_t deep_idle_after_ms = 3 * 60 * 1000;
  std::uint32_t deep_sleep_after_ms = 30 * 60 * 1000;
  std::uint32_t wifi_throttle_after_ms = 2 * 60 * 1000;
  // Matches the App's keyboard-microphone cold-start wait. Once whole-device
  // deep sleep is requested, keep the control socket reachable for this
  // quiescence window so an in-flight PTT/config packet wins over shutdown.
  std::uint32_t wifi_release_quiesce_ms = 5 * 1000;
};

inline constexpr PowerPolicyConfig kDefaultPowerPolicy{};

struct PowerPolicyInputs {
  std::uint32_t now_ms = 0;
  std::uint32_t last_activity_ms = 0;
  bool external_power = false;
  bool input_active = false;
  bool ble_connected = false;
  bool usb_mounted = false;
  bool config_window_active = false;
  bool encoder_press_pending = false;
  bool wheel_report_pending = false;
  bool audio_streaming = false;
  bool speaker_playback_active = false;
  bool wifi_active = false;
  bool wake_source_configured = false;
  bool key_wake_verified = false;
  bool key_wake_asserted = false;
  bool light_sleep_backoff = false;
};

struct PowerPolicyDecision {
  PowerPolicyMode mode = PowerPolicyMode::Active;
  std::uint32_t poll_ms = kDefaultPowerPolicy.active_poll_ms;
  bool light_sleep_allowed = false;
  const char* light_sleep_block = "mode";
  bool deep_sleep_allowed = false;
  bool wifi_release_required = false;
  const char* deep_sleep_block = "idle";
};

PowerPolicyDecision evaluate_power_policy(
    const PowerPolicyInputs& inputs,
    const PowerPolicyConfig& config = kDefaultPowerPolicy);

const char* power_policy_mode_name(PowerPolicyMode mode);

WifiPowerStage evaluate_wifi_power_stage(
    std::uint32_t idle_ms,
    bool session_active,
    bool release_requested,
    const PowerPolicyConfig& config = kDefaultPowerPolicy);

bool wifi_release_ready_for_deep_sleep(
    std::uint32_t request_elapsed_ms,
    bool session_active,
    bool release_requested,
    const PowerPolicyConfig& config = kDefaultPowerPolicy);

const char* wifi_power_stage_name(WifiPowerStage stage);

}  // namespace ai_keyboard
