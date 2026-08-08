#include "keyboard/battery_estimator.h"

#include <algorithm>
#include <array>

namespace ai_keyboard {
namespace {

constexpr int kBatteryEmptyMv = 3300;
constexpr int kBatteryNominalFullMv = 4200;
constexpr int kMinimumPlausibleFullMv = 4000;
constexpr int kMaximumPlausibleFullMv = 4400;
constexpr int kFullLearningMaxSpanMv = 40;
constexpr std::uint32_t kFullLearningDurationMs = 2 * 60 * 1000;
constexpr std::uint32_t kFullLearningMinimumSamples = 3;

struct BatteryCurvePoint {
  int mv;
  std::uint8_t percent;
};

constexpr std::array<BatteryCurvePoint, 12> kBatteryCurve = {{
    {kBatteryEmptyMv, 0},
    {3500, 5},
    {3600, 10},
    {3650, 20},
    {3700, 30},
    {3740, 40},
    {3790, 50},
    {3850, 60},
    {3920, 70},
    {4000, 80},
    {4100, 90},
    {kBatteryNominalFullMv, 100},
}};

bool plausible_full_anchor(int measured_mv) {
  return measured_mv >= kMinimumPlausibleFullMv &&
         measured_mv <= kMaximumPlausibleFullMv;
}

}  // namespace

const char* battery_power_state_name(BatteryPowerState state) {
  switch (state) {
    case BatteryPowerState::Battery:
      return "battery";
    case BatteryPowerState::UsbUnknown:
      return "usb_unknown";
    case BatteryPowerState::Charging:
      return "charging";
    case BatteryPowerState::Full:
      return "full";
  }
  return "unknown";
}

BatteryPowerState battery_power_state_from_signals(bool external_power_active,
                                                   int charge_status_level,
                                                   int charging_level) {
  if (!external_power_active) {
    return BatteryPowerState::Battery;
  }
  if (charge_status_level < 0) {
    return BatteryPowerState::UsbUnknown;
  }
  return charge_status_level == charging_level ? BatteryPowerState::Charging
                                                : BatteryPowerState::Full;
}

std::uint8_t battery_percent_from_mv(int rail_mv) {
  if (rail_mv <= 0 || rail_mv <= kBatteryCurve.front().mv) {
    return kBatteryCurve.front().percent;
  }
  if (rail_mv >= kBatteryCurve.back().mv) {
    return kBatteryCurve.back().percent;
  }

  for (std::size_t index = 1; index < kBatteryCurve.size(); ++index) {
    const auto& low = kBatteryCurve[index - 1];
    const auto& high = kBatteryCurve[index];
    if (rail_mv > high.mv) {
      continue;
    }
    const int mv_span = high.mv - low.mv;
    const int percent_span = high.percent - low.percent;
    const int percent =
        low.percent + ((rail_mv - low.mv) * percent_span + mv_span / 2) / mv_span;
    return static_cast<std::uint8_t>(std::clamp(percent, 0, 100));
  }

  return kBatteryCurve.back().percent;
}

bool BatteryEstimator::set_full_anchor_mv(int measured_full_mv) {
  if (!plausible_full_anchor(measured_full_mv)) {
    return false;
  }
  full_anchor_mv_ = measured_full_mv;
  reset_full_learning();
  return true;
}

int BatteryEstimator::full_anchor_mv() const {
  return full_anchor_mv_;
}

BatteryEstimate BatteryEstimator::update(int measured_mv,
                                         BatteryPowerState power_state,
                                         std::uint32_t now_ms) {
  BatteryEstimate estimate;
  estimate.measured_mv = measured_mv;
  if (measured_mv <= 0) {
    return estimate;
  }

  bool anchor_updated = false;
  if (power_state == BatteryPowerState::Full && full_anchor_mv_ == 0) {
    anchor_updated = learn_full_anchor(measured_mv, now_ms);
  } else if (power_state != BatteryPowerState::Full) {
    reset_full_learning();
  }

  const int adjusted_mv = corrected_mv(measured_mv);
  if (!estimate_ready_ || anchor_updated) {
    filtered_mv_ = adjusted_mv;
  } else {
    filtered_mv_ = (filtered_mv_ * 3 + adjusted_mv + 2) / 4;
  }

  const auto candidate_percent = battery_percent_from_mv(filtered_mv_);
  if (!estimate_ready_) {
    percent_ = candidate_percent;
    estimate_ready_ = true;
  }

  switch (power_state) {
    case BatteryPowerState::Full:
      percent_ = 100;
      break;
    case BatteryPowerState::Charging:
    case BatteryPowerState::UsbUnknown:
      percent_ = std::max<std::uint8_t>(percent_, std::min<std::uint8_t>(candidate_percent, 99));
      break;
    case BatteryPowerState::Battery:
      percent_ = std::min(percent_, candidate_percent);
      break;
  }

  estimate.valid = true;
  estimate.corrected_mv = filtered_mv_;
  estimate.percent = percent_;
  estimate.full_anchor_ready = full_anchor_mv_ != 0;
  estimate.full_anchor_updated = anchor_updated;
  estimate.full_anchor_mv = full_anchor_mv_;
  return estimate;
}

bool BatteryEstimator::learn_full_anchor(int measured_mv, std::uint32_t now_ms) {
  if (!plausible_full_anchor(measured_mv)) {
    reset_full_learning();
    return false;
  }

  if (full_sample_count_ == 0) {
    full_learning_started_ms_ = now_ms;
    full_sample_count_ = 1;
    full_sample_total_mv_ = measured_mv;
    full_sample_min_mv_ = measured_mv;
    full_sample_max_mv_ = measured_mv;
    return false;
  }

  const int next_min = std::min(full_sample_min_mv_, measured_mv);
  const int next_max = std::max(full_sample_max_mv_, measured_mv);
  if (next_max - next_min > kFullLearningMaxSpanMv) {
    reset_full_learning();
    full_learning_started_ms_ = now_ms;
    full_sample_count_ = 1;
    full_sample_total_mv_ = measured_mv;
    full_sample_min_mv_ = measured_mv;
    full_sample_max_mv_ = measured_mv;
    return false;
  }

  ++full_sample_count_;
  full_sample_total_mv_ += measured_mv;
  full_sample_min_mv_ = next_min;
  full_sample_max_mv_ = next_max;
  const auto learning_ms = now_ms - full_learning_started_ms_;
  if (full_sample_count_ < kFullLearningMinimumSamples ||
      learning_ms < kFullLearningDurationMs) {
    return false;
  }

  const int learned_mv = static_cast<int>(full_sample_total_mv_ / full_sample_count_);
  if (!set_full_anchor_mv(learned_mv)) {
    reset_full_learning();
    return false;
  }
  return true;
}

int BatteryEstimator::corrected_mv(int measured_mv) const {
  if (full_anchor_mv_ == 0) {
    return measured_mv;
  }
  const auto scaled =
      (static_cast<std::int64_t>(measured_mv) * kBatteryNominalFullMv + full_anchor_mv_ / 2) /
      full_anchor_mv_;
  return static_cast<int>(std::clamp<std::int64_t>(scaled, 2500, 4500));
}

void BatteryEstimator::reset_full_learning() {
  full_learning_started_ms_ = 0;
  full_sample_count_ = 0;
  full_sample_total_mv_ = 0;
  full_sample_min_mv_ = 0;
  full_sample_max_mv_ = 0;
}

}  // namespace ai_keyboard
