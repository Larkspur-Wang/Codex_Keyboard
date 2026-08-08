#include "keyboard/keyboard_snapshot_delivery.h"

#include <algorithm>
#include <limits>

namespace ai_keyboard {
namespace {

bool contains_keycode(const HidKeyboardSnapshot& snapshot,
                      std::uint8_t keycode) {
  return keycode != 0 &&
         std::find(snapshot.keycodes.begin(),
                   snapshot.keycodes.end(),
                   keycode) != snapshot.keycodes.end();
}

bool removes_keyboard_usage(const HidKeyboardSnapshot& accepted,
                            const HidKeyboardSnapshot& desired) {
  if ((accepted.modifier & static_cast<std::uint8_t>(~desired.modifier)) != 0) {
    return true;
  }
  if (accepted.apple_fn && !desired.apple_fn) {
    return true;
  }
  return std::any_of(
      accepted.keycodes.begin(),
      accepted.keycodes.end(),
      [&desired](std::uint8_t keycode) {
        return keycode != 0 && !contains_keycode(desired, keycode);
      });
}

}  // namespace

bool PendingKeyboardSnapshot::valid() const {
  return generation != 0;
}

void KeyboardSnapshotDelivery::set_desired(
    const HidKeyboardSnapshot& snapshot) {
  if (snapshot == desired_) {
    return;
  }

  desired_ = snapshot;
  if (desired_ == accepted_) {
    pending_generation_ = 0;
    return;
  }
  advance_generation();
}

PendingKeyboardSnapshot KeyboardSnapshotDelivery::pending_snapshot() const {
  if (!pending()) {
    return {};
  }
  return {
      desired_,
      classify_transition(accepted_, desired_),
      pending_generation_,
  };
}

bool KeyboardSnapshotDelivery::mark_accepted(std::uint32_t generation) {
  if (!pending() || generation == 0 ||
      generation != pending_generation_) {
    return false;
  }
  accepted_ = desired_;
  pending_generation_ = 0;
  return true;
}

void KeyboardSnapshotDelivery::reset(
    const HidKeyboardSnapshot& accepted) {
  desired_ = accepted;
  accepted_ = accepted;
  pending_generation_ = 0;
}

bool KeyboardSnapshotDelivery::pending() const {
  return pending_generation_ != 0;
}

HidKeyboardSnapshot KeyboardSnapshotDelivery::desired() const {
  return desired_;
}

HidKeyboardSnapshot KeyboardSnapshotDelivery::accepted() const {
  return accepted_;
}

HidReportClass KeyboardSnapshotDelivery::classify_transition(
    const HidKeyboardSnapshot& accepted,
    const HidKeyboardSnapshot& desired) {
  if (desired.empty()) {
    return HidReportClass::KeyboardAllReleased;
  }
  return removes_keyboard_usage(accepted, desired)
             ? HidReportClass::KeyboardRelease
             : HidReportClass::KeyboardPress;
}

void KeyboardSnapshotDelivery::advance_generation() {
  if (generation_counter_ == std::numeric_limits<std::uint32_t>::max()) {
    generation_counter_ = 1;
  } else {
    ++generation_counter_;
    if (generation_counter_ == 0) {
      generation_counter_ = 1;
    }
  }
  pending_generation_ = generation_counter_;
}

}  // namespace ai_keyboard
