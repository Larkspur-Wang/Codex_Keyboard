#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ai_keyboard {

struct HidKeyboardReport {
  bool valid = false;
  bool apple_fn = false;
  std::uint8_t modifier = 0;
  std::uint8_t keycode = 0;
  std::array<std::uint8_t, 6> keycodes{};
};

HidKeyboardReport hid_report_for_hotkey(std::string_view hotkey);
HidKeyboardReport hid_report_for_ascii_char(char ch);

}  // namespace ai_keyboard
