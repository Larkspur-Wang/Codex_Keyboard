#include "keyboard/config_payload.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "keyboard/fixed_text_protocol.h"

namespace ai_keyboard {
namespace {

struct ValueRange {
  std::size_t begin = 0;
  std::size_t end = 0;
};

constexpr std::size_t input_index(InputId input) {
  return static_cast<std::size_t>(input);
}

constexpr int kDefaultAudioPort = 17333;

std::string_view trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

void skip_ws(std::string_view json, std::size_t& pos) {
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
    ++pos;
  }
}

int hex_digit_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return 10 + value - 'a';
  }
  if (value >= 'A' && value <= 'F') {
    return 10 + value - 'A';
  }
  return -1;
}

bool read_utf16_code_unit(std::string_view json,
                          std::size_t& pos,
                          std::uint16_t* out) {
  if (out == nullptr || pos + 4 > json.size()) {
    return false;
  }
  std::uint16_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const auto digit = hex_digit_value(json[pos + index]);
    if (digit < 0) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        (value << 4) | static_cast<std::uint16_t>(digit));
  }
  pos += 4;
  *out = value;
  return true;
}

bool append_utf8_code_point(std::uint32_t code_point, std::string* out) {
  if (out == nullptr || code_point > 0x10FFFF ||
      (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    return false;
  }
  if (code_point <= 0x7F) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(
        static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(
        static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(
        static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
  return true;
}

bool append_literal_utf8(std::string_view json,
                         std::size_t& pos,
                         std::string* out) {
  if (out == nullptr || pos >= json.size()) {
    return false;
  }
  const auto lead = static_cast<std::uint8_t>(json[pos]);
  if (lead <= 0x7F) {
    // JSON strings require U+0000..U+001F to use an escape sequence.
    if (lead < 0x20) {
      return false;
    }
    out->push_back(json[pos++]);
    return true;
  }

  std::size_t sequence_len = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    sequence_len = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    sequence_len = 3;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    sequence_len = 4;
  } else {
    return false;
  }
  if (pos + sequence_len > json.size()) {
    return false;
  }
  for (std::size_t offset = 1; offset < sequence_len; ++offset) {
    const auto continuation =
        static_cast<std::uint8_t>(json[pos + offset]);
    if ((continuation & 0xC0) != 0x80) {
      return false;
    }
  }

  const auto second = static_cast<std::uint8_t>(json[pos + 1]);
  if ((lead == 0xE0 && second < 0xA0) ||
      (lead == 0xED && second > 0x9F) ||
      (lead == 0xF0 && second < 0x90) ||
      (lead == 0xF4 && second > 0x8F)) {
    return false;
  }
  out->append(json.data() + pos, sequence_len);
  pos += sequence_len;
  return true;
}

bool parse_string(std::string_view json, std::size_t& pos, std::string* out) {
  skip_ws(json, pos);
  if (pos >= json.size() || json[pos] != '"') {
    return false;
  }
  ++pos;

  std::string parsed;
  while (pos < json.size()) {
    const auto ch = json[pos];
    if (ch == '"') {
      ++pos;
      if (out != nullptr) {
        *out = std::move(parsed);
      }
      return true;
    }
    if (ch != '\\') {
      if (!append_literal_utf8(json, pos, &parsed)) {
        return false;
      }
      continue;
    }
    ++pos;
    if (pos >= json.size()) {
      return false;
    }
    const auto escaped = json[pos++];
    switch (escaped) {
      case '"':
      case '\\':
      case '/':
        parsed.push_back(escaped);
        break;
      case 'b':
        parsed.push_back('\b');
        break;
      case 'f':
        parsed.push_back('\f');
        break;
      case 'n':
        parsed.push_back('\n');
        break;
      case 'r':
        parsed.push_back('\r');
        break;
      case 't':
        parsed.push_back('\t');
        break;
      case 'u': {
        std::uint16_t first = 0;
        if (!read_utf16_code_unit(json, pos, &first)) {
          return false;
        }

        std::uint32_t code_point = first;
        if (first >= 0xD800 && first <= 0xDBFF) {
          if (pos + 2 > json.size() ||
              json[pos] != '\\' || json[pos + 1] != 'u') {
            return false;
          }
          pos += 2;
          std::uint16_t second = 0;
          if (!read_utf16_code_unit(json, pos, &second) ||
              second < 0xDC00 || second > 0xDFFF) {
            return false;
          }
          code_point =
              0x10000 +
              ((static_cast<std::uint32_t>(first) - 0xD800) << 10) +
              (static_cast<std::uint32_t>(second) - 0xDC00);
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
          return false;
        }
        if (!append_utf8_code_point(code_point, &parsed)) {
          return false;
        }
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

bool skip_composite(std::string_view json, std::size_t& pos, char open, char close) {
  if (pos >= json.size() || json[pos] != open) {
    return false;
  }

  int depth = 0;
  while (pos < json.size()) {
    const auto ch = json[pos];
    if (ch == '"') {
      std::string ignored;
      if (!parse_string(json, pos, &ignored)) {
        return false;
      }
      continue;
    }
    if (ch == open) {
      ++depth;
    } else if (ch == close) {
      --depth;
      if (depth == 0) {
        ++pos;
        return true;
      }
    }
    ++pos;
  }
  return false;
}

std::optional<ValueRange> read_value_range(std::string_view json, std::size_t& pos) {
  skip_ws(json, pos);
  if (pos >= json.size()) {
    return std::nullopt;
  }

  const auto begin = pos;
  if (json[pos] == '"') {
    std::string ignored;
    if (!parse_string(json, pos, &ignored)) {
      return std::nullopt;
    }
    return ValueRange{begin, pos};
  }
  if (json[pos] == '{') {
    if (!skip_composite(json, pos, '{', '}')) {
      return std::nullopt;
    }
    return ValueRange{begin, pos};
  }
  if (json[pos] == '[') {
    if (!skip_composite(json, pos, '[', ']')) {
      return std::nullopt;
    }
    return ValueRange{begin, pos};
  }

  while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']') {
    ++pos;
  }
  return ValueRange{begin, pos};
}

std::optional<std::string_view> field_value(std::string_view object, std::string_view field) {
  object = trim(object);
  if (object.size() < 2 || object.front() != '{' || object.back() != '}') {
    return std::nullopt;
  }

  std::size_t pos = 1;
  while (pos < object.size() - 1) {
    skip_ws(object, pos);
    if (pos < object.size() && object[pos] == '}') {
      break;
    }

    std::string key;
    if (!parse_string(object, pos, &key)) {
      return std::nullopt;
    }
    skip_ws(object, pos);
    if (pos >= object.size() || object[pos] != ':') {
      return std::nullopt;
    }
    ++pos;

    const auto range = read_value_range(object, pos);
    if (!range.has_value()) {
      return std::nullopt;
    }
    if (key == field) {
      return object.substr(range->begin, range->end - range->begin);
    }

    skip_ws(object, pos);
    if (pos < object.size() && object[pos] == ',') {
      ++pos;
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> first_array_value(std::string_view array) {
  array = trim(array);
  if (array.size() < 2 || array.front() != '[' || array.back() != ']') {
    return std::nullopt;
  }

  std::size_t pos = 1;
  skip_ws(array, pos);
  if (pos < array.size() && array[pos] == ']') {
    return std::nullopt;
  }

  const auto range = read_value_range(array, pos);
  if (!range.has_value()) {
    return std::nullopt;
  }
  return array.substr(range->begin, range->end - range->begin);
}

std::optional<std::string> string_value(std::string_view value) {
  std::size_t pos = 0;
  std::string parsed;
  if (!parse_string(value, pos, &parsed)) {
    return std::nullopt;
  }
  skip_ws(value, pos);
  if (pos != value.size()) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<bool> bool_value(std::string_view value) {
  value = trim(value);
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  return std::nullopt;
}

std::optional<int> int_value(std::string_view value) {
  value = trim(value);
  if (value.empty()) {
    return std::nullopt;
  }

  int sign = 1;
  std::size_t pos = 0;
  if (value[pos] == '-') {
    sign = -1;
    ++pos;
  }
  if (pos >= value.size()) {
    return std::nullopt;
  }

  int parsed = 0;
  for (; pos < value.size(); ++pos) {
    const auto ch = value[pos];
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    parsed = parsed * 10 + (ch - '0');
  }
  return parsed * sign;
}

int clamp_scroll_speed(int speed) {
  if (speed < 1) {
    return 1;
  }
  if (speed > 8) {
    return 8;
  }
  return speed;
}

std::optional<EncoderScrollAxis> parse_scroll_axis(const std::string& axis) {
  if (axis == "vertical") {
    return EncoderScrollAxis::Vertical;
  }
  if (axis == "horizontal") {
    return EncoderScrollAxis::Horizontal;
  }
  if (axis == "toggle") {
    return EncoderScrollAxis::Toggle;
  }
  return std::nullopt;
}

std::optional<EncoderRotationMode> parse_rotation_mode(const std::string& mode) {
  if (mode == "scroll") {
    return EncoderRotationMode::Scroll;
  }
  if (mode == "cursor") {
    return EncoderRotationMode::Cursor;
  }
  return std::nullopt;
}

std::optional<PttMode> parse_ptt_mode(const std::string& mode) {
  if (mode == "hold") {
    return PttMode::Hold;
  }
  if (mode == "toggle") {
    return PttMode::Toggle;
  }
  return std::nullopt;
}

std::optional<HostPlatform> parse_host_platform(const std::string& value) {
  if (value == "macos") return HostPlatform::MacOS;
  if (value == "windows") return HostPlatform::Windows;
  return std::nullopt;
}

std::optional<HotkeySource> parse_hotkey_source(const std::string& value) {
  if (value == "platform_default") return HotkeySource::PlatformDefault;
  if (value == "custom") return HotkeySource::Custom;
  return std::nullopt;
}

bool is_legacy_factory_ptt_hotkey(const std::string& value) {
  return value == "RightMeta" ||
         value == "Ctrl+Shift+Space" ||
         value == "CommandOrControl+Shift+Space";
}

bool is_legacy_factory_edit_ptt_hotkey(const std::string& value) {
  return value == "RightOption" ||
         value == "AltGr" ||
         value == "Ctrl+Shift+E" ||
         value == "CommandOrControl+Shift+E";
}

bool is_valid_audio_source(const std::string& source) {
  return source == "unavailable" || source == "wifi_udp" || source == "computer";
}

bool is_valid_microphone_source(const std::string& source) {
  return source == "auto" || source == "keyboard" || source == "computer";
}

bool is_valid_audio_port(int port) {
  return port >= 1024 && port <= 65535;
}

bool is_valid_speaker_sync_key(std::string_view key) {
  if (key.size() != 64U) {
    return false;
  }
  bool nonzero = false;
  for (const auto value : key) {
    const auto digit = hex_digit_value(value);
    if (digit < 0) {
      return false;
    }
    nonzero = nonzero || digit != 0;
  }
  return nonzero;
}

std::optional<ActionKind> named_action_kind(const std::string& action) {
  if (action == "disabled") {
    return ActionKind::Disabled;
  }
  if (action == "voice_ptt_hold") {
    return ActionKind::VoicePttHold;
  }
  if (action == "edit_ptt_hold") {
    return ActionKind::EditPttHold;
  }
  if (action == "paste_last") {
    return ActionKind::PasteLast;
  }
  if (action == "history") {
    return ActionKind::OpenHistory;
  }
  if (action == "toggle_profile") {
    return ActionKind::ToggleProfile;
  }
  if (action == "settings") {
    return ActionKind::Settings;
  }
  if (action == "previous_profile") {
    return ActionKind::PreviousProfile;
  }
  if (action == "next_profile") {
    return ActionKind::NextProfile;
  }
  if (action == "scroll_axis_toggle") {
    return ActionKind::ScrollAxisToggle;
  }
  if (action == "select_all") return ActionKind::SelectAll;
  if (action == "copy") return ActionKind::Copy;
  if (action == "paste") return ActionKind::Paste;
  if (action == "undo") return ActionKind::Undo;
  return std::nullopt;
}

ConfigParseStatus parse_action(std::string_view value, Action* action) {
  if (const auto named = string_value(value); named.has_value()) {
    const auto kind = named_action_kind(*named);
    if (!kind.has_value()) {
      return ConfigParseStatus::UnknownAction;
    }
    *action = {*kind, ""};
    return ConfigParseStatus::Ok;
  }

  const auto hotkey_value = field_value(value, "hotkey");
  if (hotkey_value.has_value()) {
    const auto hotkey = string_value(*hotkey_value);
    if (!hotkey.has_value() || hotkey->empty()) {
      return ConfigParseStatus::UnknownAction;
    }
    *action = {ActionKind::Hotkey, *hotkey};
    return ConfigParseStatus::Ok;
  }

  const auto text_value = field_value(value, "text");
  if (text_value.has_value()) {
    const auto text = string_value(*text_value);
    if (!text.has_value() || text->empty()) {
      return ConfigParseStatus::UnknownAction;
    }
    if (text->size() > kFixedTextMaxUtf8Bytes) {
      return ConfigParseStatus::FixedTextTooLarge;
    }
    *action = {ActionKind::FixedText, "", *text};
    return ConfigParseStatus::Ok;
  }

  return ConfigParseStatus::UnknownAction;
}

ConfigParseStatus parse_binding(std::string_view bindings,
                                std::string_view input_name,
                                Action* action) {
  const auto binding = field_value(bindings, input_name);
  if (!binding.has_value()) {
    return ConfigParseStatus::MissingBinding;
  }
  const auto press = field_value(*binding, "press");
  if (!press.has_value()) {
    return ConfigParseStatus::MissingBinding;
  }
  return parse_action(*press, action);
}

ConfigParseStatus parse_encoder_action(std::string_view encoder,
                                       std::string_view field,
                                       Action* action) {
  const auto value = field_value(encoder, field);
  if (!value.has_value()) {
    return ConfigParseStatus::MissingBinding;
  }
  return parse_action(*value, action);
}

ConfigParseStatus parse_encoder_scroll_config(std::string_view encoder,
                                              EncoderScrollConfig* config) {
  const auto scroll = field_value(encoder, "scroll");
  if (!scroll.has_value()) {
    return ConfigParseStatus::Ok;
  }

  const auto scroll_object = trim(*scroll);
  if (scroll_object.size() < 2 || scroll_object.front() != '{' || scroll_object.back() != '}') {
    return ConfigParseStatus::InvalidJson;
  }

  EncoderScrollConfig parsed;
  if (const auto enabled = field_value(*scroll, "enabled"); enabled.has_value()) {
    const auto value = bool_value(*enabled);
    if (!value.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    parsed.enabled = *value;
  }
  if (const auto mode = field_value(*scroll, "mode"); mode.has_value()) {
    const auto value = string_value(*mode);
    if (!value.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    const auto parsed_mode = parse_rotation_mode(*value);
    if (!parsed_mode.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    parsed.mode = *parsed_mode;
  }
  if (const auto axis = field_value(*scroll, "axis"); axis.has_value()) {
    const auto value = string_value(*axis);
    if (!value.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    const auto parsed_axis = parse_scroll_axis(*value);
    if (!parsed_axis.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    parsed.axis = *parsed_axis;
  }
  if (const auto speed = field_value(*scroll, "speed"); speed.has_value()) {
    const auto value = int_value(*speed);
    if (!value.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    parsed.speed = clamp_scroll_speed(*value);
  }
  if (const auto reverse = field_value(*scroll, "reverse_vertical"); reverse.has_value()) {
    const auto value = bool_value(*reverse);
    if (!value.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    parsed.reverse_vertical = *value;
  }
  if (const auto reverse = field_value(*scroll, "reverse_horizontal"); reverse.has_value()) {
    const auto value = bool_value(*reverse);
    if (!value.has_value()) {
      return ConfigParseStatus::InvalidJson;
    }
    parsed.reverse_horizontal = *value;
  }
  parsed.macos_reverse_vertical = parsed.reverse_vertical;
  parsed.macos_reverse_horizontal = parsed.reverse_horizontal;
  parsed.windows_reverse_vertical = parsed.reverse_vertical;
  parsed.windows_reverse_horizontal = parsed.reverse_horizontal;
  const auto parse_platform_reverse = [&](const char* field, bool* destination) {
    const auto raw = field_value(*scroll, field);
    if (!raw.has_value()) return true;
    const auto value = bool_value(*raw);
    if (!value.has_value()) return false;
    *destination = *value;
    return true;
  };
  if (!parse_platform_reverse("macos_reverse_vertical", &parsed.macos_reverse_vertical) ||
      !parse_platform_reverse("macos_reverse_horizontal", &parsed.macos_reverse_horizontal) ||
      !parse_platform_reverse("windows_reverse_vertical", &parsed.windows_reverse_vertical) ||
      !parse_platform_reverse("windows_reverse_horizontal", &parsed.windows_reverse_horizontal)) {
    return ConfigParseStatus::InvalidJson;
  }

  *config = parsed;
  return ConfigParseStatus::Ok;
}

}  // namespace

ConfigParseResult parse_config_payload(const std::string& json_storage) {
  const std::string_view json = json_storage;
  ConfigParseResult result;
  // Payloads persisted before hotkey_mode existed used hold semantics. Keep
  // that legacy omission distinct from the Toggle default of a fresh device.
  result.config.ptt_mode = PttMode::Hold;

  if (const auto raw = field_value(json, "target_platform"); raw.has_value()) {
    const auto value = string_value(*raw);
    const auto platform = value.has_value() ? parse_host_platform(*value) : std::nullopt;
    if (!platform.has_value()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.target_platform = *platform;
    result.config.target_platform_explicit = true;
  }

  const auto schema_value = field_value(json, "schema");
  const auto schema = schema_value.has_value() ? string_value(*schema_value) : std::nullopt;
  if (!schema.has_value() || *schema != "ai_keyboard.v1") {
    result.status = ConfigParseStatus::InvalidSchema;
    return result;
  }

  if (const auto audio_source_value = field_value(json, "audio_source"); audio_source_value.has_value()) {
    const auto audio_source = string_value(*audio_source_value);
    if (!audio_source.has_value() || !is_valid_audio_source(*audio_source)) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
  }

  if (const auto microphone_source_value = field_value(json, "microphone_source");
      microphone_source_value.has_value()) {
    const auto microphone_source = string_value(*microphone_source_value);
    if (!microphone_source.has_value() || !is_valid_microphone_source(*microphone_source)) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
  }

  // Legacy source fields are accepted for old payload/NVS compatibility only.
  // V2 hardware always captures from the keyboard and transports over Wi-Fi UDP.

  if (const auto wifi_ssid_value = field_value(json, "wifi_ssid"); wifi_ssid_value.has_value()) {
    const auto wifi_ssid = string_value(*wifi_ssid_value);
    if (!wifi_ssid.has_value()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.wifi_ssid = *wifi_ssid;
  }

  if (const auto wifi_password_value = field_value(json, "wifi_password"); wifi_password_value.has_value()) {
    const auto wifi_password = string_value(*wifi_password_value);
    if (!wifi_password.has_value()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.wifi_password = *wifi_password;
  }

  if (const auto audio_host_value = field_value(json, "audio_host"); audio_host_value.has_value()) {
    const auto audio_host = string_value(*audio_host_value);
    if (!audio_host.has_value()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.audio_host = *audio_host;
  }

  if (const auto audio_port_value = field_value(json, "audio_port"); audio_port_value.has_value()) {
    const auto audio_port = int_value(*audio_port_value);
    if (!audio_port.has_value() || !is_valid_audio_port(*audio_port)) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.audio_port = *audio_port;
  } else {
    result.config.audio_port = kDefaultAudioPort;
  }

  const auto speaker_sync_key_value =
      field_value(json, "speaker_sync_key");
  const auto speaker_sync_epoch_value =
      field_value(json, "speaker_sync_key_epoch");
  if (speaker_sync_key_value.has_value() !=
      speaker_sync_epoch_value.has_value()) {
    result.status = ConfigParseStatus::InvalidJson;
    return result;
  }
  if (speaker_sync_key_value.has_value()) {
    const auto speaker_sync_key =
        string_value(*speaker_sync_key_value);
    const auto speaker_sync_epoch =
        int_value(*speaker_sync_epoch_value);
    if (!speaker_sync_key.has_value() ||
        !is_valid_speaker_sync_key(*speaker_sync_key) ||
        !speaker_sync_epoch.has_value() ||
        *speaker_sync_epoch <= 0 ||
        *speaker_sync_epoch > 65535) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.speaker_sync_key = *speaker_sync_key;
    result.config.speaker_sync_key_epoch =
        static_cast<std::uint16_t>(*speaker_sync_epoch);
  }

  if (const auto audio_enabled_value = field_value(json, "audio_enabled"); audio_enabled_value.has_value()) {
    const auto audio_enabled = bool_value(*audio_enabled_value);
    if (!audio_enabled.has_value()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    // 仅验证旧线协议字段类型。运行时能力由 ConfigState 从完整端点推导，
    // 这里不保存该历史来源状态，避免它再次成为第二个事实源。
  }

  if (const auto ptt_hotkey_value = field_value(json, "ptt_hotkey"); ptt_hotkey_value.has_value()) {
    const auto ptt_hotkey = string_value(*ptt_hotkey_value);
    if (!ptt_hotkey.has_value() || ptt_hotkey->empty()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.ptt_hotkey = *ptt_hotkey;
    result.config.ptt_hotkey_source = HotkeySource::Custom;
  }
  if (const auto raw = field_value(json, "ptt_hotkey_source"); raw.has_value()) {
    const auto value = string_value(*raw);
    const auto source = value.has_value() ? parse_hotkey_source(*value) : std::nullopt;
    if (!source.has_value()) { result.status = ConfigParseStatus::InvalidJson; return result; }
    result.config.ptt_hotkey_source = *source;
    result.config.ptt_hotkey_source_explicit = true;
  }

  if (const auto edit_ptt_hotkey_value = field_value(json, "edit_ptt_hotkey");
      edit_ptt_hotkey_value.has_value()) {
    const auto edit_ptt_hotkey = string_value(*edit_ptt_hotkey_value);
    if (!edit_ptt_hotkey.has_value() || edit_ptt_hotkey->empty()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.edit_ptt_hotkey = *edit_ptt_hotkey;
    result.config.edit_ptt_hotkey_source = HotkeySource::Custom;
  }
  if (const auto raw = field_value(json, "edit_ptt_hotkey_source"); raw.has_value()) {
    const auto value = string_value(*raw);
    const auto source = value.has_value() ? parse_hotkey_source(*value) : std::nullopt;
    if (!source.has_value()) { result.status = ConfigParseStatus::InvalidJson; return result; }
    result.config.edit_ptt_hotkey_source = *source;
    result.config.edit_ptt_hotkey_source_explicit = true;
  }

  if (const auto hotkey_mode_value = field_value(json, "hotkey_mode"); hotkey_mode_value.has_value()) {
    const auto hotkey_mode = string_value(*hotkey_mode_value);
    if (!hotkey_mode.has_value()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    const auto ptt_mode = parse_ptt_mode(*hotkey_mode);
    if (!ptt_mode.has_value()) {
      result.status = ConfigParseStatus::InvalidJson;
      return result;
    }
    result.config.ptt_mode = *ptt_mode;
  }

  const auto profiles = field_value(json, "profiles");
  if (!profiles.has_value()) {
    result.status = ConfigParseStatus::MissingProfile;
    return result;
  }
  const auto profile = first_array_value(*profiles);
  if (!profile.has_value()) {
    result.status = ConfigParseStatus::MissingProfile;
    return result;
  }
  const auto keys = field_value(*profile, "keys");
  const auto encoder = field_value(*profile, "encoder");
  if (!keys.has_value() || !encoder.has_value()) {
    result.status = ConfigParseStatus::MissingBinding;
    return result;
  }

  std::array<Action, static_cast<std::size_t>(InputId::Count)> actions{};
  const std::array<std::pair<const char*, InputId>, 8> key_inputs{{
      {"KEY1", InputId::Key1},
      {"KEY2", InputId::Key2},
      {"KEY3", InputId::Key3},
      {"KEY4", InputId::Key4},
      {"KEY5", InputId::Key5},
      {"KEY6", InputId::Key6},
      {"KEY7", InputId::Key7},
      {"KEY8", InputId::Key8},
  }};

  for (const auto& [name, input] : key_inputs) {
    const auto status = parse_binding(*keys, name, &actions[input_index(input)]);
    if (status != ConfigParseStatus::Ok) {
      result.status = status;
      return result;
    }
  }

  const auto action_matches = [&actions](InputId input,
                                         ActionKind kind,
                                         const char* hotkey = "") {
    const auto& action = actions[input_index(input)];
    return action.kind == kind && action.hotkey == hotkey;
  };
  const bool factory_keymap_prefix =
      action_matches(InputId::Key1, ActionKind::VoicePttHold) &&
      action_matches(InputId::Key2, ActionKind::Hotkey, "Return") &&
      action_matches(InputId::Key3, ActionKind::EditPttHold) &&
      action_matches(InputId::Key4, ActionKind::Hotkey, "Backspace");
  const bool legacy_macos_factory_tail =
      action_matches(InputId::Key5, ActionKind::Hotkey, "Meta+A") &&
      action_matches(InputId::Key6, ActionKind::Hotkey, "Meta+C") &&
      action_matches(InputId::Key7, ActionKind::Hotkey, "Meta+V") &&
      (action_matches(InputId::Key8, ActionKind::Hotkey, "Ctrl+Tab") ||
       action_matches(InputId::Key8, ActionKind::Disabled));
  const bool expanded_macos_factory_tail =
      action_matches(InputId::Key5, ActionKind::Hotkey, "Meta+A") &&
      action_matches(InputId::Key6, ActionKind::Hotkey, "Meta+C") &&
      action_matches(InputId::Key7, ActionKind::Hotkey, "Meta+V") &&
      action_matches(InputId::Key8, ActionKind::Hotkey, "Meta+Z");
  const bool expanded_windows_factory_tail =
      action_matches(InputId::Key5, ActionKind::Hotkey, "Ctrl+A") &&
      action_matches(InputId::Key6, ActionKind::Hotkey, "Ctrl+C") &&
      action_matches(InputId::Key7, ActionKind::Hotkey, "Ctrl+V") &&
      action_matches(InputId::Key8, ActionKind::Hotkey, "Ctrl+Z");
  const bool platform_default_expanded_keymap =
      factory_keymap_prefix &&
      (legacy_macos_factory_tail || expanded_macos_factory_tail ||
       expanded_windows_factory_tail);
  if (platform_default_expanded_keymap) {
    actions[input_index(InputId::Key5)] = {ActionKind::SelectAll, ""};
    actions[input_index(InputId::Key6)] = {ActionKind::Copy, ""};
    actions[input_index(InputId::Key7)] = {ActionKind::Paste, ""};
    actions[input_index(InputId::Key8)] = {ActionKind::Undo, ""};
  }

  // Older App payloads did not record the source field.  Recognize only the
  // historical factory presets in that case, independent of whether K5-K8
  // have already been migrated to semantic actions.  An explicit `custom`
  // source always wins, even when its value happens to equal a factory preset.
  if (!result.config.ptt_hotkey_source_explicit &&
      is_legacy_factory_ptt_hotkey(result.config.ptt_hotkey)) {
    result.config.ptt_hotkey_source = HotkeySource::PlatformDefault;
  }
  if (!result.config.edit_ptt_hotkey_source_explicit &&
      is_legacy_factory_edit_ptt_hotkey(result.config.edit_ptt_hotkey)) {
    result.config.edit_ptt_hotkey_source = HotkeySource::PlatformDefault;
  }

  struct EncoderField {
    const char* name;
    InputId input;
  };
  const std::array<EncoderField, 3> encoder_inputs{{
      {"left", InputId::EncoderLeft},
      {"right", InputId::EncoderRight},
      {"press", InputId::EncoderPress},
  }};

  for (const auto& field : encoder_inputs) {
    const auto status = parse_encoder_action(*encoder, field.name, &actions[input_index(field.input)]);
    if (status != ConfigParseStatus::Ok) {
      result.status = status;
      return result;
    }
  }

  const auto scroll_status = parse_encoder_scroll_config(*encoder, &result.config.encoder_scroll);
  if (scroll_status != ConfigParseStatus::Ok) {
    result.status = scroll_status;
    return result;
  }

  result.config.keymap = Keymap(actions);
  if (result.config.ptt_hotkey_source == HotkeySource::PlatformDefault) {
    result.config.ptt_hotkey = default_ptt_hotkey(result.config.target_platform);
  }
  if (result.config.edit_ptt_hotkey_source == HotkeySource::PlatformDefault) {
    result.config.edit_ptt_hotkey = default_edit_ptt_hotkey(result.config.target_platform);
  }
  result.config.encoder_scroll.reverse_vertical = result.config.target_platform == HostPlatform::Windows
      ? result.config.encoder_scroll.windows_reverse_vertical
      : result.config.encoder_scroll.macos_reverse_vertical;
  result.config.encoder_scroll.reverse_horizontal = result.config.target_platform == HostPlatform::Windows
      ? result.config.encoder_scroll.windows_reverse_horizontal
      : result.config.encoder_scroll.macos_reverse_horizontal;
  result.status = ConfigParseStatus::Ok;
  return result;
}

const char* default_ptt_hotkey(HostPlatform platform) {
  return platform == HostPlatform::Windows ? "Ctrl+Shift+Space" : "RightMeta";
}

const char* default_edit_ptt_hotkey(HostPlatform platform) {
  return platform == HostPlatform::Windows ? "Ctrl+Shift+E" : "RightOption";
}

}  // namespace ai_keyboard
