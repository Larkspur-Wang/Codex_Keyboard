#include "keyboard/peripheral_power_lease.h"

namespace ai_keyboard {
namespace {

constexpr std::uint8_t owner_bit(PeripheralPowerOwner owner) {
  switch (owner) {
    case PeripheralPowerOwner::Led:
      return 1U << 0;
    case PeripheralPowerOwner::KeyboardMic:
      return 1U << 1;
    case PeripheralPowerOwner::Speaker:
      return 1U << 2;
    case PeripheralPowerOwner::DeviceAwake:
      return 1U << 3;
  }
  return 0;
}

}  // namespace

bool PeripheralPowerLeaseSet::acquire(PeripheralPowerOwner owner) {
  const std::uint8_t bit = owner_bit(owner);
  if (bit == 0 || (held_mask_ & bit) != 0) {
    return false;
  }
  held_mask_ |= bit;
  return true;
}

bool PeripheralPowerLeaseSet::release(PeripheralPowerOwner owner) {
  const std::uint8_t bit = owner_bit(owner);
  if (bit == 0 || (held_mask_ & bit) == 0) {
    return false;
  }
  held_mask_ &= static_cast<std::uint8_t>(~bit);
  return true;
}

void PeripheralPowerLeaseSet::clear() {
  held_mask_ = 0;
}

bool PeripheralPowerLeaseSet::held(PeripheralPowerOwner owner) const {
  const std::uint8_t bit = owner_bit(owner);
  return bit != 0 && (held_mask_ & bit) != 0;
}

bool PeripheralPowerLeaseSet::power_required() const {
  return held_mask_ != 0;
}

std::uint8_t PeripheralPowerLeaseSet::held_mask() const {
  return held_mask_;
}

}  // namespace ai_keyboard
