#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

enum class UsbHidEndpointReportKind : std::uint8_t {
  Keyboard = 0,
  MouseWheel,
  AppCommand,
  StatusResponse,
  SpeakerAssets,
  Count,
};

struct UsbHidEndpointPending {
  bool keyboard = false;
  bool mouse_wheel = false;
  bool app_command = false;
  bool status_response = false;
  bool speaker_assets = false;

  bool has(UsbHidEndpointReportKind kind) const {
    switch (kind) {
      case UsbHidEndpointReportKind::Keyboard:
        return keyboard;
      case UsbHidEndpointReportKind::MouseWheel:
        return mouse_wheel;
      case UsbHidEndpointReportKind::AppCommand:
        return app_command;
      case UsbHidEndpointReportKind::StatusResponse:
        return status_response;
      case UsbHidEndpointReportKind::SpeakerAssets:
        return speaker_assets;
      case UsbHidEndpointReportKind::Count:
        return false;
    }
    return false;
  }
};

// TinyUSB exposes one shared HID IN endpoint for keyboard, mouse and vendor
// reports. This arbiter assigns each available endpoint credit to one pending
// report class. Selection is side-effect free: a busy endpoint must retry the
// same class, and rotation advances only after TinyUSB accepts a report.
class UsbHidEndpointArbiter {
 public:
  UsbHidEndpointReportKind select(const UsbHidEndpointPending& pending) const {
    constexpr auto count =
        static_cast<std::size_t>(UsbHidEndpointReportKind::Count);
    const auto start = static_cast<std::size_t>(next_preferred_);
    for (std::size_t offset = 0; offset < count; ++offset) {
      const auto kind = static_cast<UsbHidEndpointReportKind>(
          (start + offset) % count);
      if (pending.has(kind)) {
        return kind;
      }
    }
    return UsbHidEndpointReportKind::Count;
  }

  void mark_accepted(UsbHidEndpointReportKind kind) {
    if (kind == UsbHidEndpointReportKind::Count) {
      return;
    }
    constexpr auto count =
        static_cast<std::size_t>(UsbHidEndpointReportKind::Count);
    next_preferred_ = static_cast<UsbHidEndpointReportKind>(
        (static_cast<std::size_t>(kind) + 1) % count);
  }

  UsbHidEndpointReportKind next_preferred() const {
    return next_preferred_;
  }

 private:
  UsbHidEndpointReportKind next_preferred_ =
      UsbHidEndpointReportKind::Keyboard;
};

// Complete keyboard state as encoded by the EasyInput USB report descriptor.
// This deliberately mirrors the wire payload without depending on ESP-IDF so
// the synthetic/physical interaction can be contract-tested on the host.
struct UsbHidKeyboardSnapshot {
  std::uint8_t modifier = 0;
  bool apple_fn = false;
  std::array<std::uint8_t, 6> keycodes{};

  bool empty() const {
    return modifier == 0 && !apple_fn &&
           std::all_of(keycodes.begin(), keycodes.end(), [](std::uint8_t keycode) {
             return keycode == 0;
           });
  }

  bool operator==(const UsbHidKeyboardSnapshot& other) const {
    return modifier == other.modifier &&
           apple_fn == other.apple_fn &&
           keycodes == other.keycodes;
  }
};

struct UsbHidSyntheticTapPair {
  UsbHidKeyboardSnapshot pressed;
  UsbHidKeyboardSnapshot restored;
};

// A FixedText/HidTap report is synthetic: it must never replace the complete
// physical HeldKeyboardState with a standalone key followed by all-zero.
// Compose the temporary key into the last queued physical snapshot, then
// restore that exact snapshot. Both reports are appended consecutively to the
// same keyboard FIFO by UsbHidTransport.
inline bool compose_usb_hid_synthetic_tap(
    const UsbHidKeyboardSnapshot& physical,
    const UsbHidKeyboardSnapshot& synthetic,
    UsbHidSyntheticTapPair* out) {
  if (out == nullptr || synthetic.empty()) {
    return false;
  }

  UsbHidKeyboardSnapshot pressed = physical;
  pressed.modifier |= synthetic.modifier;
  pressed.apple_fn = pressed.apple_fn || synthetic.apple_fn;
  for (const auto keycode : synthetic.keycodes) {
    if (keycode == 0) {
      continue;
    }
    if (std::find(pressed.keycodes.begin(), pressed.keycodes.end(), keycode) !=
        pressed.keycodes.end()) {
      continue;
    }
    const auto empty_slot =
        std::find(pressed.keycodes.begin(), pressed.keycodes.end(), 0);
    if (empty_slot == pressed.keycodes.end()) {
      return false;
    }
    *empty_slot = keycode;
  }

  out->pressed = pressed;
  out->restored = physical;
  return true;
}

}  // namespace ai_keyboard
