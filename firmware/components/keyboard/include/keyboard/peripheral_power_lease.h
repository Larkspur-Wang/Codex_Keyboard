#pragma once

#include <cstdint>

namespace ai_keyboard {

enum class PeripheralPowerOwner : std::uint8_t {
  Led = 0,
  KeyboardMic,
  Speaker,
  DeviceAwake,
};

class PeripheralPowerLeaseSet {
 public:
  bool acquire(PeripheralPowerOwner owner);
  bool release(PeripheralPowerOwner owner);
  void clear();

  bool held(PeripheralPowerOwner owner) const;
  bool power_required() const;
  std::uint8_t held_mask() const;

 private:
  std::uint8_t held_mask_ = 0;
};

}  // namespace ai_keyboard
