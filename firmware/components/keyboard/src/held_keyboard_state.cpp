#include "keyboard/held_keyboard_state.h"

#include <algorithm>

namespace ai_keyboard {
namespace {

std::size_t source_index(InputId source) {
  return static_cast<std::size_t>(source);
}

bool has_keycode(const std::array<std::uint8_t, 6>& keycodes,
                 std::uint8_t keycode) {
  return std::find(keycodes.begin(), keycodes.end(), keycode) != keycodes.end();
}

bool add_keycode(HidKeyboardSnapshot* snapshot,
                 std::size_t* keycode_count,
                 std::uint8_t keycode) {
  if (keycode == 0 || has_keycode(snapshot->keycodes, keycode)) {
    return true;
  }
  if (*keycode_count >= snapshot->keycodes.size()) {
    return false;
  }
  snapshot->keycodes[*keycode_count] = keycode;
  ++(*keycode_count);
  return true;
}

}  // namespace

bool HidKeyboardSnapshot::empty() const {
  return modifier == 0 && !apple_fn &&
         std::all_of(keycodes.begin(), keycodes.end(),
                     [](std::uint8_t keycode) { return keycode == 0; });
}

bool operator==(const HidKeyboardSnapshot& lhs,
                const HidKeyboardSnapshot& rhs) {
  return lhs.modifier == rhs.modifier &&
         lhs.apple_fn == rhs.apple_fn &&
         lhs.keycodes == rhs.keycodes;
}

bool operator!=(const HidKeyboardSnapshot& lhs,
                const HidKeyboardSnapshot& rhs) {
  return !(lhs == rhs);
}

bool HeldKeyboardUpdate::accepted() const {
  return status == HeldKeyboardUpdateStatus::Applied ||
         status == HeldKeyboardUpdateStatus::NoChange;
}

HeldKeyboardUpdate HeldKeyboardState::press(
    InputId source,
    const HidKeyboardReport& report) {
  HeldKeyboardUpdate update;
  update.snapshot = current_;

  if (!valid_source(source)) {
    update.status = HeldKeyboardUpdateStatus::InvalidSource;
    return update;
  }
  if (!usable_report(report)) {
    update.status = HeldKeyboardUpdateStatus::InvalidReport;
    return update;
  }

  const auto index = source_index(source);
  // GPIO bounce or a duplicated producer event must not replace the report
  // captured for the original physical press.
  if (active_[index]) {
    update.status = HeldKeyboardUpdateStatus::NoChange;
    return update;
  }

  auto candidate_reports = reports_;
  auto candidate_active = active_;
  candidate_reports[index] = report;
  candidate_active[index] = true;

  HidKeyboardSnapshot candidate;
  if (!compose(candidate_reports, candidate_active, &candidate)) {
    update.status = HeldKeyboardUpdateStatus::Rollover;
    return update;
  }

  const auto previous = current_;
  reports_ = candidate_reports;
  active_ = candidate_active;
  current_ = candidate;
  ++active_source_count_;

  update.status = HeldKeyboardUpdateStatus::Applied;
  update.snapshot = current_;
  update.state_changed = true;
  update.report_changed = current_ != previous;
  update.became_empty = false;
  return update;
}

HeldKeyboardUpdate HeldKeyboardState::release(InputId source) {
  HeldKeyboardUpdate update;
  update.snapshot = current_;

  if (!valid_source(source)) {
    update.status = HeldKeyboardUpdateStatus::InvalidSource;
    return update;
  }

  const auto index = source_index(source);
  if (!active_[index]) {
    update.status = HeldKeyboardUpdateStatus::NoChange;
    return update;
  }

  const auto previous = current_;
  auto candidate_reports = reports_;
  auto candidate_active = active_;
  candidate_reports[index] = {};
  candidate_active[index] = false;

  HidKeyboardSnapshot candidate;
  // Removing a source cannot introduce a new 6KRO overflow, but keep the
  // mutation atomic if that invariant is ever violated by a future change.
  if (!compose(candidate_reports, candidate_active, &candidate)) {
    update.status = HeldKeyboardUpdateStatus::Rollover;
    return update;
  }

  reports_ = candidate_reports;
  active_ = candidate_active;
  current_ = candidate;
  --active_source_count_;

  update.status = HeldKeyboardUpdateStatus::Applied;
  update.snapshot = current_;
  update.state_changed = true;
  update.report_changed = current_ != previous;
  update.became_empty = current_.empty();
  return update;
}

HeldKeyboardUpdate HeldKeyboardState::clear() {
  HeldKeyboardUpdate update;
  update.snapshot = current_;
  if (active_source_count_ == 0) {
    update.status = HeldKeyboardUpdateStatus::NoChange;
    return update;
  }

  const auto previous = current_;
  reports_ = {};
  active_ = {};
  current_ = {};
  active_source_count_ = 0;

  update.status = HeldKeyboardUpdateStatus::Applied;
  update.snapshot = current_;
  update.state_changed = true;
  update.report_changed = current_ != previous;
  update.became_empty = true;
  return update;
}

HidKeyboardSnapshot HeldKeyboardState::current() const {
  return current_;
}

bool HeldKeyboardState::active(InputId source) const {
  return valid_source(source) && active_[source_index(source)];
}

bool HeldKeyboardState::empty() const {
  return active_source_count_ == 0;
}

std::size_t HeldKeyboardState::active_source_count() const {
  return active_source_count_;
}

bool HeldKeyboardState::valid_source(InputId source) {
  return source_index(source) < kSourceCount;
}

bool HeldKeyboardState::usable_report(const HidKeyboardReport& report) {
  if (!report.valid) {
    return false;
  }
  if (report.modifier != 0 || report.apple_fn || report.keycode != 0) {
    return true;
  }
  return std::any_of(report.keycodes.begin(), report.keycodes.end(),
                     [](std::uint8_t keycode) { return keycode != 0; });
}

bool HeldKeyboardState::compose(
    const std::array<HidKeyboardReport, kSourceCount>& reports,
    const std::array<bool, kSourceCount>& active,
    HidKeyboardSnapshot* out) {
  if (out == nullptr) {
    return false;
  }

  HidKeyboardSnapshot candidate;
  std::size_t keycode_count = 0;
  for (std::size_t index = 0; index < kSourceCount; ++index) {
    if (!active[index]) {
      continue;
    }

    const auto& report = reports[index];
    candidate.modifier |= report.modifier;
    candidate.apple_fn = candidate.apple_fn || report.apple_fn;

    bool report_contains_keycode = false;
    for (const auto keycode : report.keycodes) {
      if (keycode == 0) {
        continue;
      }
      report_contains_keycode = true;
      if (!add_keycode(&candidate, &keycode_count, keycode)) {
        return false;
      }
    }
    // HidKeyboardReport::keycode is retained for compatibility with callers
    // constructing a single-key report without filling keycodes[].
    if (!report_contains_keycode &&
        !add_keycode(&candidate, &keycode_count, report.keycode)) {
      return false;
    }
  }

  *out = candidate;
  return true;
}

}  // namespace ai_keyboard
