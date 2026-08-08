#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "keyboard/hid_keycode.h"
#include "keyboard/keymap.h"

namespace ai_keyboard {

// A complete keyboard input report without a report ID. Unlike
// HidKeyboardReport, an empty snapshot is valid and represents releasing every
// keyboard usage.
struct HidKeyboardSnapshot {
  std::uint8_t modifier = 0;
  bool apple_fn = false;
  std::array<std::uint8_t, 6> keycodes{};

  bool empty() const;
};

bool operator==(const HidKeyboardSnapshot& lhs, const HidKeyboardSnapshot& rhs);
bool operator!=(const HidKeyboardSnapshot& lhs, const HidKeyboardSnapshot& rhs);

enum class HeldKeyboardUpdateStatus {
  Applied,
  NoChange,
  InvalidSource,
  InvalidReport,
  Rollover,
};

struct HeldKeyboardUpdate {
  HeldKeyboardUpdateStatus status = HeldKeyboardUpdateStatus::NoChange;
  HidKeyboardSnapshot snapshot;
  // A source can be added or removed without changing the wire report when
  // another source holds the same modifier/keycode.
  bool state_changed = false;
  bool report_changed = false;
  bool became_empty = false;

  bool accepted() const;
};

// Owns the logical keyboard state by physical input source. Every successful
// mutation produces the full current report, so releasing one source never
// implicitly releases keys still held by another source.
class HeldKeyboardState {
 public:
  HeldKeyboardUpdate press(InputId source, const HidKeyboardReport& report);
  HeldKeyboardUpdate release(InputId source);
  HeldKeyboardUpdate clear();

  HidKeyboardSnapshot current() const;
  bool active(InputId source) const;
  bool empty() const;
  std::size_t active_source_count() const;

 private:
  static constexpr std::size_t kSourceCount =
      static_cast<std::size_t>(InputId::Count);

  static bool valid_source(InputId source);
  static bool usable_report(const HidKeyboardReport& report);
  static bool compose(
      const std::array<HidKeyboardReport, kSourceCount>& reports,
      const std::array<bool, kSourceCount>& active,
      HidKeyboardSnapshot* out);

  std::array<HidKeyboardReport, kSourceCount> reports_{};
  std::array<bool, kSourceCount> active_{};
  HidKeyboardSnapshot current_{};
  std::size_t active_source_count_ = 0;
};

}  // namespace ai_keyboard
