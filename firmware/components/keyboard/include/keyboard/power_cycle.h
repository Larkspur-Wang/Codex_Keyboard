#pragma once

#include <cstdint>

#include "keyboard/power_policy.h"

namespace ai_keyboard {

enum class PowerCycleWakeReason : std::uint8_t {
  Unknown = 0,
  Input,
  Key,
  LightSleepKey,
  DeepSleepKey,
  StatusRead,
  Config,
  WifiAudio,
  Timer,
  ExternalPower,
  Other,
};

inline constexpr std::uint8_t kPowerCycleStageIdle = 1U << 0;
inline constexpr std::uint8_t kPowerCycleStageDeepIdle = 1U << 1;
inline constexpr std::uint8_t kPowerCycleStageDeepSleep = 1U << 2;

struct PowerCycleSnapshot {
  std::uint32_t sequence = 0;
  std::uint32_t idle_ms = 0;
  std::uint32_t deep_idle_ms = 0;
  std::uint8_t stage_flags = 0;
  PowerCycleWakeReason wake_reason = PowerCycleWakeReason::Unknown;

  bool valid() const { return sequence != 0; }
};

PowerCycleSnapshot build_power_cycle_snapshot(
    std::uint32_t sequence,
    std::uint32_t elapsed_idle_ms,
    PowerCycleWakeReason wake_reason,
    bool reached_deep_sleep,
    const PowerPolicyConfig& config = kDefaultPowerPolicy);

PowerCycleWakeReason power_cycle_wake_reason(const char* reason);
const char* power_cycle_wake_reason_name(PowerCycleWakeReason reason);

}  // namespace ai_keyboard
