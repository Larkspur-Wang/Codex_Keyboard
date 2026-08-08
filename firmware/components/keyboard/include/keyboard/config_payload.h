#pragma once

#include <cstdint>
#include <string>

#include "keyboard/keymap.h"

namespace ai_keyboard {

enum class ConfigParseStatus {
  Ok,
  InvalidJson,
  InvalidSchema,
  UnsupportedAudio,
  MissingProfile,
  MissingBinding,
  UnknownAction,
  FixedTextTooLarge,
};

enum class EncoderScrollAxis {
  Vertical,
  Horizontal,
  Toggle,
};

enum class EncoderRotationMode {
  Scroll,
  Cursor,
};

enum class PttMode {
  Hold,
  Toggle,
};

struct EncoderScrollConfig {
  bool enabled = true;
  EncoderRotationMode mode = EncoderRotationMode::Scroll;
  EncoderScrollAxis axis = EncoderScrollAxis::Vertical;
  int speed = 3;
  bool reverse_vertical = false;
  bool reverse_horizontal = false;
  bool macos_reverse_vertical = false;
  bool macos_reverse_horizontal = false;
  bool windows_reverse_vertical = false;
  bool windows_reverse_horizontal = false;
};

enum class HotkeySource { PlatformDefault, Custom };

struct ParsedKeyboardConfig {
  std::string ptt_hotkey = "RightMeta";
  std::string edit_ptt_hotkey = "RightOption";
  HotkeySource ptt_hotkey_source = HotkeySource::PlatformDefault;
  HotkeySource edit_ptt_hotkey_source = HotkeySource::PlatformDefault;
  HostPlatform target_platform = HostPlatform::MacOS;
  bool ptt_hotkey_source_explicit = false;
  bool edit_ptt_hotkey_source_explicit = false;
  bool target_platform_explicit = false;
  PttMode ptt_mode = PttMode::Toggle;
  std::string wifi_ssid;
  std::string wifi_password;
  std::string audio_host;
  int audio_port = 17333;
  // 32-byte speaker Wi-Fi PSK encoded as exactly 64 hexadecimal characters.
  // Empty + epoch zero is the backward-compatible unprovisioned state.
  std::string speaker_sync_key;
  std::uint16_t speaker_sync_key_epoch = 0U;
  Keymap keymap = DefaultKeymap();
  EncoderScrollConfig encoder_scroll;
};

struct ConfigParseResult {
  ConfigParseStatus status = ConfigParseStatus::InvalidJson;
  ParsedKeyboardConfig config;
};

ConfigParseResult parse_config_payload(const std::string& json);
const char* default_ptt_hotkey(HostPlatform platform);
const char* default_edit_ptt_hotkey(HostPlatform platform);

}  // namespace ai_keyboard
