#pragma once

#include <cstdint>

#include "keyboard/held_keyboard_state.h"
#include "keyboard/hid_report_queue.h"

namespace ai_keyboard {

// Immutable description of the one full keyboard state that should be
// offered to a transport next. generation protects the state machine from a
// stale acceptance if a newer desired state supersedes this attempt.
struct PendingKeyboardSnapshot {
  HidKeyboardSnapshot snapshot;
  HidReportClass report_class = HidReportClass::KeyboardPress;
  std::uint32_t generation = 0;

  bool valid() const;
};

// Tracks the latest logical keyboard state independently from transport queue
// availability. Producers may update desired() at any rate; at most one
// complete snapshot remains pending. accepted() advances only after the
// selected transport has accepted that snapshot into its ordered queue.
//
// This is intentionally fixed-size and non-blocking. Under sustained
// backpressure intermediate states may be coalesced, but the latest desired
// state (especially all-released) remains retryable until accepted.
class KeyboardSnapshotDelivery {
 public:
  void set_desired(const HidKeyboardSnapshot& snapshot);
  PendingKeyboardSnapshot pending_snapshot() const;
  bool mark_accepted(std::uint32_t generation);

  // Establishes a known transport baseline and cancels any pending attempt.
  // This is for explicit transport/state resets, not queue-full recovery.
  void reset(const HidKeyboardSnapshot& accepted = {});

  bool pending() const;
  HidKeyboardSnapshot desired() const;
  HidKeyboardSnapshot accepted() const;

 private:
  static HidReportClass classify_transition(
      const HidKeyboardSnapshot& accepted,
      const HidKeyboardSnapshot& desired);
  void advance_generation();

  HidKeyboardSnapshot desired_{};
  HidKeyboardSnapshot accepted_{};
  std::uint32_t generation_counter_ = 0;
  std::uint32_t pending_generation_ = 0;
};

}  // namespace ai_keyboard
