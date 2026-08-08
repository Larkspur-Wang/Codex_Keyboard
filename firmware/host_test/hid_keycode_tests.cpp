#include <cassert>

#include "keyboard/hid_keycode.h"

void maps_named_keys_to_keyboard_usage_ids() {
  const auto f12 = ai_keyboard::hid_report_for_hotkey("F12");
  const auto enter = ai_keyboard::hid_report_for_hotkey("Return");
  const auto escape = ai_keyboard::hid_report_for_hotkey("Escape");
  const auto arrow_right = ai_keyboard::hid_report_for_hotkey("ArrowRight");
  const auto arrow_up = ai_keyboard::hid_report_for_hotkey("Up");

  assert(f12.valid);
  assert(f12.modifier == 0x00);
  assert(f12.keycode == 0x45);
  assert(f12.keycodes[0] == 0x45);
  assert(enter.valid);
  assert(enter.keycode == 0x28);
  assert(escape.valid);
  assert(escape.keycode == 0x29);
  assert(arrow_right.valid);
  assert(arrow_right.keycode == 0x4F);
  assert(arrow_up.valid);
  assert(arrow_up.keycode == 0x52);
}

void maps_modifier_only_hotkeys() {
  const auto right_meta = ai_keyboard::hid_report_for_hotkey("RightMeta");
  const auto alt_gr = ai_keyboard::hid_report_for_hotkey("AltGr");

  assert(right_meta.valid);
  assert(right_meta.modifier == 0x80);
  assert(right_meta.keycode == 0x00);
  assert(alt_gr.valid);
  assert(alt_gr.modifier == 0x40);
  assert(alt_gr.keycode == 0x00);
}

void rejects_apple_fn_hotkeys() {
  const auto fn = ai_keyboard::hid_report_for_hotkey("Fn");
  const auto fn_space = ai_keyboard::hid_report_for_hotkey("Fn+Space");

  assert(!fn.valid);
  assert(!fn.apple_fn);
  assert(!fn_space.valid);
  assert(!fn_space.apple_fn);
}

void maps_modifier_chords() {
  const auto report = ai_keyboard::hid_report_for_hotkey("Ctrl+Alt+Space");

  assert(report.valid);
  assert(report.modifier == 0x05);
  assert(report.keycode == 0x2C);
  assert(report.keycodes[0] == 0x2C);
}

void maps_meta_and_command_or_control_chords() {
  const auto meta = ai_keyboard::hid_report_for_hotkey("Meta+C");
  const auto command_or_control = ai_keyboard::hid_report_for_hotkey("CommandOrControl+Shift+E");

  assert(meta.valid);
  assert(meta.modifier == 0x08);
  assert(meta.keycode == 0x06);
  assert(command_or_control.valid);
  assert(command_or_control.modifier == 0x03);
  assert(command_or_control.keycode == 0x08);
}

void maps_multiple_non_modifier_keys() {
  const auto report = ai_keyboard::hid_report_for_hotkey("A+B");

  assert(report.valid);
  assert(report.modifier == 0x00);
  assert(report.keycode == 0x04);
  assert(report.keycodes[0] == 0x04);
  assert(report.keycodes[1] == 0x05);
}

void rejects_unknown_hotkeys() {
  const auto report = ai_keyboard::hid_report_for_hotkey("LaunchWarpDrive");
  const auto duplicate = ai_keyboard::hid_report_for_hotkey("A+A");

  assert(!report.valid);
  assert(!duplicate.valid);
}

void maps_ascii_text_characters() {
  const auto lower = ai_keyboard::hid_report_for_ascii_char('a');
  const auto upper = ai_keyboard::hid_report_for_ascii_char('A');
  const auto digit = ai_keyboard::hid_report_for_ascii_char('1');
  const auto bang = ai_keyboard::hid_report_for_ascii_char('!');
  const auto space = ai_keyboard::hid_report_for_ascii_char(' ');
  const auto unknown = ai_keyboard::hid_report_for_ascii_char(static_cast<char>(0xE4));

  assert(lower.valid);
  assert(lower.modifier == 0x00);
  assert(lower.keycode == 0x04);
  assert(upper.valid);
  assert(upper.modifier == 0x02);
  assert(upper.keycode == 0x04);
  assert(digit.valid);
  assert(digit.keycode == 0x1E);
  assert(bang.valid);
  assert(bang.modifier == 0x02);
  assert(bang.keycode == 0x1E);
  assert(space.valid);
  assert(space.keycode == 0x2C);
  assert(!unknown.valid);
}

int main() {
  maps_named_keys_to_keyboard_usage_ids();
  maps_modifier_only_hotkeys();
  rejects_apple_fn_hotkeys();
  maps_modifier_chords();
  maps_meta_and_command_or_control_chords();
  maps_multiple_non_modifier_keys();
  rejects_unknown_hotkeys();
  maps_ascii_text_characters();
  return 0;
}
