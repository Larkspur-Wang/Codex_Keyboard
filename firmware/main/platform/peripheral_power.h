#pragma once

#include <cstdint>

#include "esp_err.h"
#include "keyboard/peripheral_power_lease.h"

namespace easy_input {

// Sole owner of the V2 GPIO8 rail that powers LED, microphone and speaker.
// Feature leases remain as readiness/diagnostic facts, but DeviceAwake keeps
// the physical rail enabled until the whole device commits to deep sleep.
class PeripheralPowerController {
 public:
  esp_err_t begin_awake();
  bool ready() const;
  bool power_enabled() const;

  esp_err_t set_audio_power_hold(bool enabled);
  esp_err_t set_speaker_power_hold(bool enabled);

  // Call only after LED, microphone and speaker have become quiescent. This
  // puts every command pin into a non-backfeeding state and drives GPIO8 low.
  esp_err_t prepare_for_deep_sleep();

 private:
  esp_err_t set_owner_hold(ai_keyboard::PeripheralPowerOwner owner,
                           bool enabled,
                           const char* label);
  esp_err_t configure_command_pins_safe_for_rail_transition();
  esp_err_t apply_power_state();
  esp_err_t set_power_enabled(bool enabled);

  ai_keyboard::PeripheralPowerLeaseSet power_leases_;
  bool initialized_ = false;
  bool ready_ = false;
  bool power_enabled_ = false;
};

}  // namespace easy_input
