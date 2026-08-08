#include <cassert>

#include "keyboard/battery_estimator.h"

namespace {

void maps_voltage_curve() {
  assert(ai_keyboard::battery_percent_from_mv(3200) == 0);
  assert(ai_keyboard::battery_percent_from_mv(3790) == 50);
  assert(ai_keyboard::battery_percent_from_mv(4200) == 100);
}

void maps_v2_charge_signals() {
  using ai_keyboard::BatteryPowerState;
  assert(ai_keyboard::battery_power_state_from_signals(false, 1, 1) ==
         BatteryPowerState::Battery);
  assert(ai_keyboard::battery_power_state_from_signals(true, -1, 1) ==
         BatteryPowerState::UsbUnknown);
  assert(ai_keyboard::battery_power_state_from_signals(true, 1, 1) ==
         BatteryPowerState::Charging);
  assert(ai_keyboard::battery_power_state_from_signals(true, 0, 1) ==
         BatteryPowerState::Full);
}

void learns_a_stable_full_anchor() {
  ai_keyboard::BatteryEstimator estimator;
  auto estimate = estimator.update(4100, ai_keyboard::BatteryPowerState::Full, 1000);
  assert(!estimate.full_anchor_ready);
  estimate = estimator.update(4110, ai_keyboard::BatteryPowerState::Full, 61000);
  assert(!estimate.full_anchor_ready);
  estimate = estimator.update(4105, ai_keyboard::BatteryPowerState::Full, 121000);
  assert(estimate.full_anchor_ready);
  assert(estimate.full_anchor_updated);
  assert(estimate.full_anchor_mv == 4105);
  assert(estimate.corrected_mv == 4200);
  assert(estimate.percent == 100);
}

void rejects_an_implausible_full_anchor() {
  ai_keyboard::BatteryEstimator estimator;
  estimator.update(3800, ai_keyboard::BatteryPowerState::Full, 0);
  estimator.update(3800, ai_keyboard::BatteryPowerState::Full, 60000);
  const auto estimate = estimator.update(3800, ai_keyboard::BatteryPowerState::Full, 120000);
  assert(!estimate.full_anchor_ready);
}

void applies_a_persisted_full_anchor() {
  ai_keyboard::BatteryEstimator estimator;
  assert(estimator.set_full_anchor_mv(4100));
  const auto estimate = estimator.update(4100, ai_keyboard::BatteryPowerState::Battery, 0);
  assert(estimate.corrected_mv == 4200);
  assert(estimate.full_anchor_ready);
}

void keeps_discharge_percentage_monotonic() {
  ai_keyboard::BatteryEstimator estimator;
  auto estimate = estimator.update(3850, ai_keyboard::BatteryPowerState::Battery, 0);
  const auto initial_percent = estimate.percent;
  estimate = estimator.update(4050, ai_keyboard::BatteryPowerState::Battery, 60000);
  assert(estimate.percent == initial_percent);
  estimate = estimator.update(3500, ai_keyboard::BatteryPowerState::Battery, 120000);
  assert(estimate.percent <= initial_percent);
}

void keeps_charge_percentage_monotonic_and_reserves_full() {
  ai_keyboard::BatteryEstimator estimator;
  auto estimate = estimator.update(3850, ai_keyboard::BatteryPowerState::Charging, 0);
  const auto initial_percent = estimate.percent;
  estimate = estimator.update(3500, ai_keyboard::BatteryPowerState::Charging, 60000);
  assert(estimate.percent >= initial_percent);
  assert(estimate.percent <= 99);
  estimate = estimator.update(4100, ai_keyboard::BatteryPowerState::Full, 120000);
  assert(estimate.percent == 100);
}

}  // namespace

int main() {
  maps_voltage_curve();
  maps_v2_charge_signals();
  learns_a_stable_full_anchor();
  rejects_an_implausible_full_anchor();
  applies_a_persisted_full_anchor();
  keeps_discharge_percentage_monotonic();
  keeps_charge_percentage_monotonic_and_reserves_full();
  return 0;
}
