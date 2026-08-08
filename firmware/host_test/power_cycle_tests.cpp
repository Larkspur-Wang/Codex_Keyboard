#include <cassert>
#include <cstring>

#include "keyboard/power_cycle.h"

int main() {
  using namespace ai_keyboard;

  const auto too_short = build_power_cycle_snapshot(
      1, kDefaultPowerPolicy.idle_after_ms - 1, PowerCycleWakeReason::Input, false);
  assert(!too_short.valid());

  const auto idle = build_power_cycle_snapshot(
      3,
      kDefaultPowerPolicy.idle_after_ms + 12'000,
      PowerCycleWakeReason::StatusRead,
      false);
  assert(idle.valid());
  assert(idle.sequence == 3);
  assert(idle.stage_flags == kPowerCycleStageIdle);
  assert(idle.idle_ms == 12'000);
  assert(idle.deep_idle_ms == 0);
  assert(idle.wake_reason == PowerCycleWakeReason::StatusRead);

  const auto deep_idle = build_power_cycle_snapshot(
      4,
      kDefaultPowerPolicy.deep_idle_after_ms + 45'000,
      PowerCycleWakeReason::LightSleepKey,
      false);
  assert((deep_idle.stage_flags & kPowerCycleStageIdle) != 0);
  assert((deep_idle.stage_flags & kPowerCycleStageDeepIdle) != 0);
  assert((deep_idle.stage_flags & kPowerCycleStageDeepSleep) == 0);
  assert(deep_idle.idle_ms ==
         kDefaultPowerPolicy.deep_idle_after_ms - kDefaultPowerPolicy.idle_after_ms);
  assert(deep_idle.deep_idle_ms == 45'000);

  const auto deep_sleep = build_power_cycle_snapshot(
      5,
      kDefaultPowerPolicy.deep_sleep_after_ms,
      PowerCycleWakeReason::Unknown,
      true);
  assert((deep_sleep.stage_flags & kPowerCycleStageDeepSleep) != 0);
  assert(deep_sleep.deep_idle_ms ==
         kDefaultPowerPolicy.deep_sleep_after_ms - kDefaultPowerPolicy.deep_idle_after_ms);

  assert(power_cycle_wake_reason("input_edge") == PowerCycleWakeReason::Input);
  assert(power_cycle_wake_reason("deep_sleep_key_wake") ==
         PowerCycleWakeReason::DeepSleepKey);
  assert(power_cycle_wake_reason("status_read") == PowerCycleWakeReason::StatusRead);
  assert(power_cycle_wake_reason("unknown") == PowerCycleWakeReason::Unknown);
  assert(power_cycle_wake_reason("unexpected") == PowerCycleWakeReason::Other);
  assert(std::strcmp(power_cycle_wake_reason_name(PowerCycleWakeReason::WifiAudio),
                     "wifi_audio") == 0);
  return 0;
}
