#pragma once

#include <cstdint>

namespace ai_keyboard {

enum class BatteryPowerState : std::uint8_t {
  Battery,
  UsbUnknown,
  Charging,
  Full,
};

const char* battery_power_state_name(BatteryPowerState state);
BatteryPowerState battery_power_state_from_signals(bool external_power_active,
                                                   int charge_status_level,
                                                   int charging_level);
std::uint8_t battery_percent_from_mv(int rail_mv);

struct BatteryEstimate {
  bool valid = false;
  int measured_mv = 0;
  int corrected_mv = 0;
  std::uint8_t percent = 0;
  bool full_anchor_ready = false;
  bool full_anchor_updated = false;
  int full_anchor_mv = 0;
};

class BatteryEstimator {
 public:
  bool set_full_anchor_mv(int measured_full_mv);
  int full_anchor_mv() const;
  BatteryEstimate update(int measured_mv,
                         BatteryPowerState power_state,
                         std::uint32_t now_ms);

 private:
  bool learn_full_anchor(int measured_mv, std::uint32_t now_ms);
  int corrected_mv(int measured_mv) const;
  void reset_full_learning();

  int full_anchor_mv_ = 0;
  int filtered_mv_ = 0;
  std::uint8_t percent_ = 0;
  bool estimate_ready_ = false;
  std::uint32_t full_learning_started_ms_ = 0;
  std::uint32_t full_sample_count_ = 0;
  std::int64_t full_sample_total_mv_ = 0;
  int full_sample_min_mv_ = 0;
  int full_sample_max_mv_ = 0;
};

}  // namespace ai_keyboard
