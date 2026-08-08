#include <cassert>
#include <cstdint>

#include "keyboard/peripheral_power_lease.h"

namespace {

using ai_keyboard::PeripheralPowerLeaseSet;
using ai_keyboard::PeripheralPowerOwner;

void owners_hold_the_shared_rail_independently() {
  PeripheralPowerLeaseSet leases;
  assert(!leases.power_required());
  assert(leases.held_mask() == 0);

  assert(leases.acquire(PeripheralPowerOwner::Led));
  assert(leases.acquire(PeripheralPowerOwner::KeyboardMic));
  assert(leases.acquire(PeripheralPowerOwner::Speaker));
  assert(leases.power_required());
  assert(leases.held(PeripheralPowerOwner::Led));
  assert(leases.held(PeripheralPowerOwner::KeyboardMic));
  assert(leases.held(PeripheralPowerOwner::Speaker));
  assert(leases.held_mask() == 0b111);

  assert(leases.release(PeripheralPowerOwner::Led));
  assert(leases.power_required());
  assert(leases.release(PeripheralPowerOwner::KeyboardMic));
  assert(leases.power_required());
  assert(leases.release(PeripheralPowerOwner::Speaker));
  assert(!leases.power_required());
}

void acquire_and_release_are_idempotent_per_owner() {
  PeripheralPowerLeaseSet leases;

  assert(leases.acquire(PeripheralPowerOwner::KeyboardMic));
  assert(!leases.acquire(PeripheralPowerOwner::KeyboardMic));
  assert(leases.held_mask() == 0b010);

  assert(leases.release(PeripheralPowerOwner::KeyboardMic));
  assert(!leases.release(PeripheralPowerOwner::KeyboardMic));
  assert(!leases.power_required());
}

void one_owner_cannot_release_another_owners_lease() {
  PeripheralPowerLeaseSet leases;
  assert(leases.acquire(PeripheralPowerOwner::Led));
  assert(leases.acquire(PeripheralPowerOwner::Speaker));

  assert(!leases.release(PeripheralPowerOwner::KeyboardMic));
  assert(leases.held(PeripheralPowerOwner::Led));
  assert(leases.held(PeripheralPowerOwner::Speaker));
  assert(leases.power_required());

  leases.clear();
  assert(!leases.held(PeripheralPowerOwner::Led));
  assert(!leases.held(PeripheralPowerOwner::Speaker));
  assert(!leases.power_required());
}

void invalid_owner_values_fail_closed() {
  PeripheralPowerLeaseSet leases;
  const auto invalid = static_cast<PeripheralPowerOwner>(99);

  assert(!leases.acquire(invalid));
  assert(!leases.release(invalid));
  assert(!leases.held(invalid));
  assert(!leases.power_required());
}

void device_awake_owns_the_shared_rail_until_deep_sleep() {
  PeripheralPowerLeaseSet leases;

  assert(leases.acquire(PeripheralPowerOwner::DeviceAwake));
  assert(leases.acquire(PeripheralPowerOwner::Led));
  assert(leases.acquire(PeripheralPowerOwner::KeyboardMic));
  assert(leases.acquire(PeripheralPowerOwner::Speaker));

  assert(leases.release(PeripheralPowerOwner::Led));
  assert(leases.release(PeripheralPowerOwner::KeyboardMic));
  assert(leases.release(PeripheralPowerOwner::Speaker));
  assert(leases.held(PeripheralPowerOwner::DeviceAwake));
  assert(leases.power_required());

  // Only the whole-device deep-sleep transition may clear DeviceAwake.
  leases.clear();
  assert(!leases.held(PeripheralPowerOwner::DeviceAwake));
  assert(!leases.power_required());
}

}  // namespace

int main() {
  owners_hold_the_shared_rail_independently();
  acquire_and_release_are_idempotent_per_owner();
  one_owner_cannot_release_another_owners_lease();
  invalid_owner_values_fail_closed();
  device_awake_owns_the_shared_rail_until_deep_sleep();
  return 0;
}
