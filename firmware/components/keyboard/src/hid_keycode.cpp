#include "keyboard/hid_keycode.h"

#include <array>
#include <cctype>
#include <string>

namespace ai_keyboard {
namespace {

constexpr std::uint8_t kModifierLeftCtrl = 0x01;
constexpr std::uint8_t kModifierLeftShift = 0x02;
constexpr std::uint8_t kModifierLeftAlt = 0x04;
constexpr std::uint8_t kModifierLeftGui = 0x08;
constexpr std::uint8_t kModifierRightCtrl = 0x10;
constexpr std::uint8_t kModifierRightShift = 0x20;
constexpr std::uint8_t kModifierRightAlt = 0x40;
constexpr std::uint8_t kModifierRightGui = 0x80;

std::string normalize(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const auto ch : value) {
    if (ch == ' ' || ch == '_' || ch == '-') {
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool modifier_for_token(const std::string& token, std::uint8_t* modifier) {
  if (token == "ctrl" || token == "control" || token == "commandorcontrol" ||
      token == "leftctrl" || token == "leftcontrol") {
    *modifier |= kModifierLeftCtrl;
    return true;
  }
  if (token == "shift" || token == "leftshift") {
    *modifier |= kModifierLeftShift;
    return true;
  }
  if (token == "alt" || token == "option" || token == "leftalt" || token == "leftoption") {
    *modifier |= kModifierLeftAlt;
    return true;
  }
  if (token == "meta" || token == "gui" || token == "cmd" || token == "command" ||
      token == "leftmeta" || token == "leftgui" || token == "leftcmd" || token == "leftcommand") {
    *modifier |= kModifierLeftGui;
    return true;
  }
  if (token == "altgr" || token == "rightalt" || token == "rightoption") {
    *modifier |= kModifierRightAlt;
    return true;
  }
  if (token == "rightmeta" || token == "rightgui" || token == "rightcmd" || token == "rightcommand") {
    *modifier |= kModifierRightGui;
    return true;
  }
  if (token == "rightctrl" || token == "rightcontrol") {
    *modifier |= kModifierRightCtrl;
    return true;
  }
  if (token == "rightshift") {
    *modifier |= kModifierRightShift;
    return true;
  }
  return false;
}

bool keycode_for_token(const std::string& token, std::uint8_t* keycode) {
  if (token.size() == 1 && token[0] >= 'a' && token[0] <= 'z') {
    *keycode = static_cast<std::uint8_t>(0x04 + (token[0] - 'a'));
    return true;
  }
  if (token.size() == 1 && token[0] >= '1' && token[0] <= '9') {
    *keycode = static_cast<std::uint8_t>(0x1E + (token[0] - '1'));
    return true;
  }
  if (token == "0") {
    *keycode = 0x27;
    return true;
  }
  if (token == "enter" || token == "return") {
    *keycode = 0x28;
    return true;
  }
  if (token == "escape" || token == "esc") {
    *keycode = 0x29;
    return true;
  }
  if (token == "backspace") {
    *keycode = 0x2A;
    return true;
  }
  if (token == "tab") {
    *keycode = 0x2B;
    return true;
  }
  if (token == "space" || token == "spacebar") {
    *keycode = 0x2C;
    return true;
  }
  if (token == "arrowright" || token == "right") {
    *keycode = 0x4F;
    return true;
  }
  if (token == "arrowleft" || token == "left") {
    *keycode = 0x50;
    return true;
  }
  if (token == "arrowdown" || token == "down") {
    *keycode = 0x51;
    return true;
  }
  if (token == "arrowup" || token == "up") {
    *keycode = 0x52;
    return true;
  }
  if (token.size() >= 2 && token[0] == 'f') {
    int number = 0;
    for (std::size_t i = 1; i < token.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
        return false;
      }
      number = (number * 10) + (token[i] - '0');
    }
    if (number >= 1 && number <= 12) {
      *keycode = static_cast<std::uint8_t>(0x3A + (number - 1));
      return true;
    }
  }
  return false;
}

bool unsupported_hotkey_token(const std::string& token) {
  return token == "fn" || token == "function";
}

HidKeyboardReport report_for_key(std::uint8_t keycode, std::uint8_t modifier = 0) {
  HidKeyboardReport report;
  report.valid = true;
  report.modifier = modifier;
  report.keycode = keycode;
  report.keycodes[0] = keycode;
  return report;
}

bool add_keycode(HidKeyboardReport* report, std::uint8_t keycode) {
  for (const auto existing : report->keycodes) {
    if (existing == keycode) {
      return false;
    }
  }
  for (auto& existing : report->keycodes) {
    if (existing == 0) {
      existing = keycode;
      if (report->keycode == 0) {
        report->keycode = keycode;
      }
      return true;
    }
  }
  return false;
}

}  // namespace

HidKeyboardReport hid_report_for_hotkey(std::string_view hotkey) {
  HidKeyboardReport report;
  std::size_t begin = 0;
  bool saw_token = false;

  while (begin <= hotkey.size()) {
    const auto end = hotkey.find('+', begin);
    const auto raw = hotkey.substr(begin, end == std::string_view::npos ? hotkey.size() - begin : end - begin);
    const auto token = normalize(raw);
    if (!token.empty()) {
      saw_token = true;
      if (unsupported_hotkey_token(token)) {
        return {};
      }
      if (!modifier_for_token(token, &report.modifier)) {
        std::uint8_t keycode = 0;
        if (!keycode_for_token(token, &keycode) || !add_keycode(&report, keycode)) {
          return {};
        }
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }

  report.valid = saw_token && (report.apple_fn || report.modifier != 0 || report.keycode != 0);
  return report;
}

HidKeyboardReport hid_report_for_ascii_char(char ch) {
  const auto value = static_cast<unsigned char>(ch);
  if (value >= 'a' && value <= 'z') {
    return report_for_key(static_cast<std::uint8_t>(0x04 + (value - 'a')));
  }
  if (value >= 'A' && value <= 'Z') {
    return report_for_key(static_cast<std::uint8_t>(0x04 + (value - 'A')), kModifierLeftShift);
  }
  if (value >= '1' && value <= '9') {
    return report_for_key(static_cast<std::uint8_t>(0x1E + (value - '1')));
  }

  switch (value) {
    case '0':
      return report_for_key(0x27);
    case '\n':
    case '\r':
      return report_for_key(0x28);
    case '\b':
      return report_for_key(0x2A);
    case '\t':
      return report_for_key(0x2B);
    case ' ':
      return report_for_key(0x2C);
    case '-':
      return report_for_key(0x2D);
    case '_':
      return report_for_key(0x2D, kModifierLeftShift);
    case '=':
      return report_for_key(0x2E);
    case '+':
      return report_for_key(0x2E, kModifierLeftShift);
    case '[':
      return report_for_key(0x2F);
    case '{':
      return report_for_key(0x2F, kModifierLeftShift);
    case ']':
      return report_for_key(0x30);
    case '}':
      return report_for_key(0x30, kModifierLeftShift);
    case '\\':
      return report_for_key(0x31);
    case '|':
      return report_for_key(0x31, kModifierLeftShift);
    case ';':
      return report_for_key(0x33);
    case ':':
      return report_for_key(0x33, kModifierLeftShift);
    case '\'':
      return report_for_key(0x34);
    case '"':
      return report_for_key(0x34, kModifierLeftShift);
    case '`':
      return report_for_key(0x35);
    case '~':
      return report_for_key(0x35, kModifierLeftShift);
    case ',':
      return report_for_key(0x36);
    case '<':
      return report_for_key(0x36, kModifierLeftShift);
    case '.':
      return report_for_key(0x37);
    case '>':
      return report_for_key(0x37, kModifierLeftShift);
    case '/':
      return report_for_key(0x38);
    case '?':
      return report_for_key(0x38, kModifierLeftShift);
    case '!':
      return report_for_key(0x1E, kModifierLeftShift);
    case '@':
      return report_for_key(0x1F, kModifierLeftShift);
    case '#':
      return report_for_key(0x20, kModifierLeftShift);
    case '$':
      return report_for_key(0x21, kModifierLeftShift);
    case '%':
      return report_for_key(0x22, kModifierLeftShift);
    case '^':
      return report_for_key(0x23, kModifierLeftShift);
    case '&':
      return report_for_key(0x24, kModifierLeftShift);
    case '*':
      return report_for_key(0x25, kModifierLeftShift);
    case '(':
      return report_for_key(0x26, kModifierLeftShift);
    case ')':
      return report_for_key(0x27, kModifierLeftShift);
    default:
      return {};
  }
}

}  // namespace ai_keyboard
