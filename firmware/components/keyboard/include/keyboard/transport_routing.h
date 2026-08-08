#pragma once

#include <cstdint>
#include <string>

#include "keyboard/keymap.h"

namespace ai_keyboard {

enum class FirmwareTransportRoute {
  UsbFirst,
  BleFirst,
};

FirmwareTransportRoute route_for_firmware_event(const FirmwareEvent& event, bool ble_connected);

enum class KeyboardTransportOwner {
  None,
  Usb,
  Ble,
  Suppressed,
};

// Filters the board's physical USB/VBUS signal without delaying re-attach.
//
// A brief absent sample must not strand a still-mounted USB endpoint, so loss
// is confirmed for a bounded interval. Presence is accepted immediately:
// actual USB enumeration can complete faster than the battery-idle main-loop
// cadence, and only a real TinyUSB mount callback is allowed to make the
// endpoint routable again.
class UsbPhysicalPresenceMonitor {
 public:
  explicit UsbPhysicalPresenceMonitor(
      std::uint32_t disconnect_confirm_ms = 25);

  void reset(bool present, std::uint32_t now_ms);
  // Returns true exactly when the stable physical-presence state changes.
  bool update(bool raw_present, std::uint32_t now_ms);
  bool present() const;
  bool disconnect_pending() const;

 private:
  std::uint32_t disconnect_confirm_ms_ = 25;
  std::uint32_t disconnect_candidate_since_ms_ = 0;
  bool initialized_ = false;
  bool present_ = false;
  bool disconnect_pending_ = false;
};

// Tracks one concrete USB HID endpoint lifetime from mount/unmount callbacks.
//
// The epoch advances on every TinyUSB endpoint callback boundary, rather than
// when a slower consumer happens to sample mounted(). A separate physical gate
// can revoke routability while TinyUSB retains stale configured state, but only
// a subsequent real mount callback creates a fresh endpoint epoch.
class UsbEndpointLifetime {
 public:
  explicit UsbEndpointLifetime(std::uint32_t initial_epoch = 0);

  // Every mount callback establishes a fresh configured endpoint lifetime.
  // TinyUSB can deliver a new mount after BUS RESET without first delivering
  // umount, so repeated mount callbacks are not idempotent.
  bool on_mount();
  // Atomically qualifies a TinyUSB mount callback with the board's raw VBUS
  // sample. When monitoring is enabled, an absent sample rejects the callback
  // even if the debounced main-loop state has not changed yet.
  bool on_mount_with_physical_presence(bool present);
  // Repeated unmount callbacks while already unmounted are idempotent.
  bool on_unmount();

  // V2 has an active-low SEN_VIN hardware truth source while legacy boards do
  // not. Enable this before the USB driver starts when that signal exists.
  void enable_physical_presence_monitor(bool initially_present);
  // Returns true only when physical loss invalidates a currently routable
  // endpoint. Physical recovery never synthesizes a mount and a mount callback
  // observed while absent is ignored.
  bool observe_physical_presence(bool present);

  bool mounted() const;
  std::uint32_t epoch() const;

 private:
  void advance_epoch();

  bool tinyusb_mounted_ = false;
  bool physical_presence_monitor_enabled_ = false;
  bool physical_present_ = true;
  bool fresh_mount_required_ = false;
  std::uint32_t epoch_ = 0;
};

// Binds every non-empty keyboard chord to one physical transport. A temporary
// endpoint-busy state or a second transport appearing mid-chord must never
// split key-down and key-up across two hosts.
class KeyboardTransportLatch {
 public:
  // Reconciles the latched endpoint with its current connection lifetime.
  // Returns true exactly when an active chord loses its original endpoint and
  // enters Suppressed. Connection epochs must change for every new endpoint
  // instance, even if a numeric BLE connection handle is reused.
  bool observe_transport_state(bool usb_mounted,
                               std::uint32_t usb_epoch,
                               bool ble_connected,
                               std::uint32_t ble_epoch);
  KeyboardTransportOwner select_for_snapshot(bool all_released,
                                             bool usb_mounted,
                                             std::uint32_t usb_epoch,
                                             bool ble_connected,
                                             std::uint32_t ble_epoch);
  // Selection is intentionally non-destructive for an all-released snapshot:
  // a full transport queue must not lose the owner needed to retry it. Call
  // this only after the selected endpoint accepts the snapshot.
  void commit_snapshot(bool all_released);
  void reset();
  KeyboardTransportOwner owner() const;
  std::uint32_t owner_epoch() const;

 private:
  KeyboardTransportOwner owner_ = KeyboardTransportOwner::None;
  std::uint32_t owner_epoch_ = 0;
};

// One stateful App hotkey transition waiting for admission to the selected
// transport queue. "Accepted" means the bounded transport queue owns the
// transition; it does not claim that the host application has processed it.
struct BridgedHotkeyTransition {
  bool valid = false;
  bool pressed = false;
  std::string hotkey;
  std::uint32_t desired_generation = 0;
};

// Tracks physical desired state separately from the last transition accepted
// by the transport. This is the App-command equivalent of
// KeyboardSnapshotDelivery:
//
// - a rejected press remains pending while the key is held;
// - a press that was accepted always retains its matching release until that
//   release is accepted;
// - a complete press/release that occurs before the press is accepted may be
//   coalesced to a no-op because the host never observed either edge.
//
// The class is transport-independent so BLE and USB use the same guarantee.
class BridgedHotkeyDelivery {
 public:
  bool set_desired(bool pressed, const std::string& hotkey);
  BridgedHotkeyTransition pending_transition() const;
  bool mark_accepted(const BridgedHotkeyTransition& transition);

  // An endpoint lifetime change causes the host to discard its old state.
  // Establish the current physical state as the new delivery baseline so no
  // stale transition is replayed into a fresh endpoint.
  void reset_to_desired();
  void reset();

  bool pending() const;
  bool desired_pressed() const;
  bool accepted_pressed() const;

 private:
  struct State {
    bool pressed = false;
    std::string hotkey;
  };

  State desired_{};
  State accepted_{};
  std::uint32_t desired_generation_ = 1;
};

}  // namespace ai_keyboard
