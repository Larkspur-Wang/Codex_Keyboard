#include <cassert>
#include <cstdint>

#include "keyboard/held_keyboard_state.h"

namespace {

using ai_keyboard::HeldKeyboardState;
using ai_keyboard::HeldKeyboardUpdateStatus;
using ai_keyboard::InputId;

ai_keyboard::HidKeyboardReport report(const char* hotkey) {
  const auto value = ai_keyboard::hid_report_for_hotkey(hotkey);
  assert(value.valid);
  return value;
}

void basic_press_release_and_clear_are_complete_snapshots() {
  HeldKeyboardState state;
  assert(state.empty());
  assert(state.current().empty());

  const auto down = state.press(InputId::Key1, report("A"));
  assert(down.status == HeldKeyboardUpdateStatus::Applied);
  assert(down.state_changed);
  assert(down.report_changed);
  assert(!down.became_empty);
  assert(down.snapshot.keycodes[0] == 0x04);
  assert(state.active(InputId::Key1));
  assert(state.active_source_count() == 1);

  const auto up = state.release(InputId::Key1);
  assert(up.status == HeldKeyboardUpdateStatus::Applied);
  assert(up.state_changed);
  assert(up.report_changed);
  assert(up.became_empty);
  assert(up.snapshot.empty());
  assert(state.empty());

  const auto empty_clear = state.clear();
  assert(empty_clear.status == HeldKeyboardUpdateStatus::NoChange);
  assert(!empty_clear.became_empty);
}

void overlapping_sources_preserve_other_held_keys() {
  HeldKeyboardState state;
  assert(state.press(InputId::Key5, report("Meta+A")).accepted());
  const auto both = state.press(InputId::Key6, report("Meta+C"));
  assert(both.status == HeldKeyboardUpdateStatus::Applied);
  assert(both.snapshot.modifier == 0x08);
  assert(both.snapshot.keycodes[0] == 0x04);
  assert(both.snapshot.keycodes[1] == 0x06);

  const auto release_first = state.release(InputId::Key5);
  assert(release_first.snapshot.modifier == 0x08);
  assert(release_first.snapshot.keycodes[0] == 0x06);
  assert(release_first.snapshot.keycodes[1] == 0x00);
  assert(!release_first.became_empty);

  const auto release_second = state.release(InputId::Key6);
  assert(release_second.snapshot.empty());
  assert(release_second.became_empty);
}

void duplicate_keycodes_are_reference_counted_by_source() {
  HeldKeyboardState state;
  const auto first = state.press(InputId::Key1, report("Ctrl+A"));
  assert(first.report_changed);

  const auto duplicate = state.press(InputId::Key2, report("Ctrl+A"));
  assert(duplicate.status == HeldKeyboardUpdateStatus::Applied);
  assert(duplicate.state_changed);
  assert(!duplicate.report_changed);
  assert(state.active_source_count() == 2);

  const auto release_first = state.release(InputId::Key1);
  assert(release_first.state_changed);
  assert(!release_first.report_changed);
  assert(release_first.snapshot.modifier == 0x01);
  assert(release_first.snapshot.keycodes[0] == 0x04);

  const auto release_last = state.release(InputId::Key2);
  assert(release_last.report_changed);
  assert(release_last.snapshot.empty());
}

void modifiers_and_apple_fn_are_ored_until_their_sources_release() {
  HeldKeyboardState state;
  auto ctrl = report("Ctrl");
  ai_keyboard::HidKeyboardReport fn_shift_b;
  fn_shift_b.valid = true;
  fn_shift_b.apple_fn = true;
  fn_shift_b.modifier = 0x02;
  fn_shift_b.keycode = 0x05;
  fn_shift_b.keycodes[0] = 0x05;

  assert(state.press(InputId::Key1, ctrl).accepted());
  const auto both = state.press(InputId::Key2, fn_shift_b);
  assert(both.snapshot.modifier == 0x03);
  assert(both.snapshot.apple_fn);
  assert(both.snapshot.keycodes[0] == 0x05);

  const auto release_fn = state.release(InputId::Key2);
  assert(release_fn.snapshot.modifier == 0x01);
  assert(!release_fn.snapshot.apple_fn);
  assert(release_fn.snapshot.keycodes[0] == 0x00);
}

void duplicate_press_and_stray_release_are_idempotent() {
  HeldKeyboardState state;
  assert(state.press(InputId::Key1, report("A")).accepted());

  const auto duplicate = state.press(InputId::Key1, report("B"));
  assert(duplicate.status == HeldKeyboardUpdateStatus::NoChange);
  assert(!duplicate.state_changed);
  assert(!duplicate.report_changed);
  assert(duplicate.snapshot.keycodes[0] == 0x04);

  const auto stray = state.release(InputId::Key2);
  assert(stray.status == HeldKeyboardUpdateStatus::NoChange);
  assert(!stray.state_changed);
  assert(stray.snapshot.keycodes[0] == 0x04);

  assert(state.release(InputId::Key1).became_empty);
}

void six_key_rollover_rejects_the_whole_new_source_atomically() {
  HeldKeyboardState state;
  const auto six = report("A+B+C+D+E+F");
  assert(state.press(InputId::Key1, six).accepted());
  const auto before = state.current();

  const auto overflow = state.press(InputId::Key2, report("G"));
  assert(overflow.status == HeldKeyboardUpdateStatus::Rollover);
  assert(!overflow.accepted());
  assert(!overflow.state_changed);
  assert(!overflow.report_changed);
  assert(overflow.snapshot == before);
  assert(!state.active(InputId::Key2));
  assert(state.active_source_count() == 1);
  assert(state.current() == before);

  assert(state.release(InputId::Key1).became_empty);
}

void clear_releases_all_sources_once() {
  HeldKeyboardState state;
  assert(state.press(InputId::Key1, report("Ctrl+A")).accepted());
  assert(state.press(InputId::Key2, report("Alt+B")).accepted());

  const auto cleared = state.clear();
  assert(cleared.status == HeldKeyboardUpdateStatus::Applied);
  assert(cleared.state_changed);
  assert(cleared.report_changed);
  assert(cleared.became_empty);
  assert(cleared.snapshot.empty());
  assert(state.empty());
  assert(!state.active(InputId::Key1));
  assert(!state.active(InputId::Key2));

  const auto second = state.clear();
  assert(second.status == HeldKeyboardUpdateStatus::NoChange);
  assert(!second.report_changed);
  assert(!second.became_empty);
}

void macos_ptt_modifiers_overlap_without_global_release() {
  HeldKeyboardState state;
  assert(state.press(InputId::Key1, report("RightMeta")).accepted());
  const auto both = state.press(InputId::Key3, report("RightOption"));
  assert(both.snapshot.modifier == 0xC0);
  assert(both.snapshot.keycodes[0] == 0);

  const auto voice_up = state.release(InputId::Key1);
  assert(voice_up.snapshot.modifier == 0x40);
  assert(!voice_up.became_empty);
  assert(state.release(InputId::Key3).became_empty);
}

void windows_ptt_chords_keep_shared_modifiers_and_remaining_key() {
  HeldKeyboardState state;
  assert(state.press(InputId::Key1, report("Ctrl+Shift+Space")).accepted());
  const auto both = state.press(InputId::Key3, report("Ctrl+Shift+E"));
  assert(both.snapshot.modifier == 0x03);
  assert(both.snapshot.keycodes[0] == 0x2C);
  assert(both.snapshot.keycodes[1] == 0x08);

  const auto voice_up = state.release(InputId::Key1);
  assert(voice_up.snapshot.modifier == 0x03);
  assert(voice_up.snapshot.keycodes[0] == 0x08);
  assert(voice_up.snapshot.keycodes[1] == 0x00);
  assert(state.release(InputId::Key3).became_empty);
}

void invalid_source_and_report_do_not_mutate_state() {
  HeldKeyboardState state;
  ai_keyboard::HidKeyboardReport invalid;
  assert(state.press(InputId::Key1, invalid).status ==
         HeldKeyboardUpdateStatus::InvalidReport);
  assert(state.press(InputId::Count, report("A")).status ==
         HeldKeyboardUpdateStatus::InvalidSource);
  assert(state.release(InputId::Count).status ==
         HeldKeyboardUpdateStatus::InvalidSource);
  assert(!state.active(InputId::Count));
  assert(state.empty());
}

}  // namespace

int main() {
  basic_press_release_and_clear_are_complete_snapshots();
  overlapping_sources_preserve_other_held_keys();
  duplicate_keycodes_are_reference_counted_by_source();
  modifiers_and_apple_fn_are_ored_until_their_sources_release();
  duplicate_press_and_stray_release_are_idempotent();
  six_key_rollover_rejects_the_whole_new_source_atomically();
  clear_releases_all_sources_once();
  macos_ptt_modifiers_overlap_without_global_release();
  windows_ptt_chords_keep_shared_modifiers_and_remaining_key();
  invalid_source_and_report_do_not_mutate_state();
  return 0;
}
