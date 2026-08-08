#include "keyboard/transport_routing.h"

namespace ai_keyboard {

FirmwareTransportRoute route_for_firmware_event(const FirmwareEvent& event, bool ble_connected) {
  (void)event;
  (void)ble_connected;
  return FirmwareTransportRoute::UsbFirst;
}

UsbPhysicalPresenceMonitor::UsbPhysicalPresenceMonitor(
    std::uint32_t disconnect_confirm_ms)
    : disconnect_confirm_ms_(disconnect_confirm_ms) {}

void UsbPhysicalPresenceMonitor::reset(bool present,
                                       std::uint32_t now_ms) {
  initialized_ = true;
  present_ = present;
  disconnect_pending_ = false;
  disconnect_candidate_since_ms_ = now_ms;
}

bool UsbPhysicalPresenceMonitor::update(bool raw_present,
                                        std::uint32_t now_ms) {
  if (!initialized_) {
    reset(raw_present, now_ms);
    return false;
  }

  if (raw_present) {
    disconnect_pending_ = false;
    if (present_) {
      return false;
    }
    present_ = true;
    return true;
  }

  if (!present_) {
    disconnect_pending_ = false;
    return false;
  }

  if (!disconnect_pending_) {
    disconnect_pending_ = true;
    disconnect_candidate_since_ms_ = now_ms;
  }
  if (static_cast<std::uint32_t>(
          now_ms - disconnect_candidate_since_ms_) <
      disconnect_confirm_ms_) {
    return false;
  }

  present_ = false;
  disconnect_pending_ = false;
  return true;
}

bool UsbPhysicalPresenceMonitor::present() const {
  return present_;
}

bool UsbPhysicalPresenceMonitor::disconnect_pending() const {
  return disconnect_pending_;
}

UsbEndpointLifetime::UsbEndpointLifetime(std::uint32_t initial_epoch)
    : epoch_(initial_epoch) {}

bool UsbEndpointLifetime::on_mount() {
  if (physical_presence_monitor_enabled_ && !physical_present_) {
    return false;
  }
  tinyusb_mounted_ = true;
  fresh_mount_required_ = false;
  advance_epoch();
  return true;
}

bool UsbEndpointLifetime::on_mount_with_physical_presence(bool present) {
  if (physical_presence_monitor_enabled_) {
    if (!present) {
      return false;
    }
    physical_present_ = true;
  }
  return on_mount();
}

bool UsbEndpointLifetime::on_unmount() {
  if (!tinyusb_mounted_) {
    return false;
  }
  tinyusb_mounted_ = false;
  advance_epoch();
  return true;
}

void UsbEndpointLifetime::enable_physical_presence_monitor(
    bool initially_present) {
  physical_presence_monitor_enabled_ = true;
  physical_present_ = initially_present;
  fresh_mount_required_ = !initially_present;
}

bool UsbEndpointLifetime::observe_physical_presence(bool present) {
  if (!physical_presence_monitor_enabled_) {
    return false;
  }

  const bool was_mounted = mounted();
  physical_present_ = present;
  if (!present) {
    // Keep TinyUSB's callback-owned mounted state and epoch intact, but never
    // let their stale values become routable merely because VBUS returns.
    fresh_mount_required_ = true;
  }
  return was_mounted && !mounted();
}

bool UsbEndpointLifetime::mounted() const {
  return tinyusb_mounted_ &&
         (!physical_presence_monitor_enabled_ ||
          (physical_present_ && !fresh_mount_required_));
}

std::uint32_t UsbEndpointLifetime::epoch() const {
  return epoch_;
}

void UsbEndpointLifetime::advance_epoch() {
  ++epoch_;
  if (epoch_ == 0) {
    epoch_ = 1;
  }
}

KeyboardTransportOwner KeyboardTransportLatch::select_for_snapshot(
    bool all_released,
    bool usb_mounted,
    std::uint32_t usb_epoch,
    bool ble_connected,
    std::uint32_t ble_epoch) {
  observe_transport_state(
      usb_mounted, usb_epoch, ble_connected, ble_epoch);
  const bool usb_available = usb_mounted && usb_epoch != 0;
  const bool ble_available = ble_connected && ble_epoch != 0;

  if (all_released) {
    return (owner_ == KeyboardTransportOwner::Usb && usb_available) ||
                   (owner_ == KeyboardTransportOwner::Ble && ble_available)
               ? owner_
               : KeyboardTransportOwner::None;
  }

  if (owner_ == KeyboardTransportOwner::None) {
    if (usb_available) {
      owner_ = KeyboardTransportOwner::Usb;
      owner_epoch_ = usb_epoch;
    } else if (ble_available) {
      owner_ = KeyboardTransportOwner::Ble;
      owner_epoch_ = ble_epoch;
    } else {
      owner_ = KeyboardTransportOwner::Suppressed;
      owner_epoch_ = 0;
    }
  }
  return owner_;
}

bool KeyboardTransportLatch::observe_transport_state(
    bool usb_mounted,
    std::uint32_t usb_epoch,
    bool ble_connected,
    std::uint32_t ble_epoch) {
  const bool usb_available = usb_mounted && usb_epoch != 0;
  const bool ble_available = ble_connected && ble_epoch != 0;
  const bool usb_lifetime_lost =
      owner_ == KeyboardTransportOwner::Usb &&
      (!usb_available || owner_epoch_ != usb_epoch);
  const bool ble_lifetime_lost =
      owner_ == KeyboardTransportOwner::Ble &&
      (!ble_available || owner_epoch_ != ble_epoch);
  if (!usb_lifetime_lost && !ble_lifetime_lost) {
    return false;
  }

  // A chord that lost its exact host instance is never migrated to another
  // instance, including a reconnect that reuses the same numeric BLE handle.
  // The host OS releases keys on disconnect; wait for all physical sources to
  // release before selecting a transport for a new chord.
  owner_ = KeyboardTransportOwner::Suppressed;
  owner_epoch_ = 0;
  return true;
}

void KeyboardTransportLatch::commit_snapshot(bool all_released) {
  if (all_released) {
    owner_ = KeyboardTransportOwner::None;
    owner_epoch_ = 0;
  }
}

void KeyboardTransportLatch::reset() {
  owner_ = KeyboardTransportOwner::None;
  owner_epoch_ = 0;
}

KeyboardTransportOwner KeyboardTransportLatch::owner() const {
  return owner_;
}

std::uint32_t KeyboardTransportLatch::owner_epoch() const {
  return owner_epoch_;
}

bool BridgedHotkeyDelivery::set_desired(bool pressed,
                                        const std::string& hotkey) {
  const std::string next_hotkey = pressed ? hotkey : std::string{};
  if (desired_.pressed == pressed && desired_.hotkey == next_hotkey) {
    return false;
  }
  desired_.pressed = pressed;
  desired_.hotkey = next_hotkey;
  ++desired_generation_;
  if (desired_generation_ == 0) {
    desired_generation_ = 1;
  }
  return true;
}

BridgedHotkeyTransition BridgedHotkeyDelivery::pending_transition() const {
  // A changed hotkey while the same physical source remains held is serialized
  // as release(old) followed by press(new); it is never rewritten in place.
  if (accepted_.pressed &&
      (!desired_.pressed || accepted_.hotkey != desired_.hotkey)) {
    return {
        true,
        false,
        accepted_.hotkey,
        desired_generation_,
    };
  }
  if (!accepted_.pressed && desired_.pressed) {
    return {
        true,
        true,
        desired_.hotkey,
        desired_generation_,
    };
  }
  return {};
}

bool BridgedHotkeyDelivery::mark_accepted(
    const BridgedHotkeyTransition& transition) {
  if (!transition.valid ||
      transition.desired_generation != desired_generation_) {
    return false;
  }
  const auto current = pending_transition();
  if (!current.valid ||
      current.pressed != transition.pressed ||
      current.hotkey != transition.hotkey) {
    return false;
  }

  accepted_.pressed = transition.pressed;
  accepted_.hotkey = transition.pressed ? transition.hotkey : std::string{};
  return true;
}

void BridgedHotkeyDelivery::reset_to_desired() {
  accepted_ = desired_;
  ++desired_generation_;
  if (desired_generation_ == 0) {
    desired_generation_ = 1;
  }
}

void BridgedHotkeyDelivery::reset() {
  desired_ = {};
  accepted_ = {};
  ++desired_generation_;
  if (desired_generation_ == 0) {
    desired_generation_ = 1;
  }
}

bool BridgedHotkeyDelivery::pending() const {
  return pending_transition().valid;
}

bool BridgedHotkeyDelivery::desired_pressed() const {
  return desired_.pressed;
}

bool BridgedHotkeyDelivery::accepted_pressed() const {
  return accepted_.pressed;
}

}  // namespace ai_keyboard
