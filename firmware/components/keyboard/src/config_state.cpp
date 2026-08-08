#include "keyboard/config_state.h"

namespace ai_keyboard {

ConfigParseStatus ConfigState::apply_json(const std::string& json) {
  auto parsed = parse_config_payload(json);
  if (parsed.status != ConfigParseStatus::Ok) {
    return parsed.status;
  }
  const auto effective_platform = parsed.config.target_platform_explicit
                                      ? parsed.config.target_platform
                                      : current_.target_platform;
  current_ = parsed.config;
  // Keep all platform-derived runtime fields behind one invariant boundary.
  // This deliberately re-resolves platform-default PTT hotkeys and active
  // scroll reversal even when target_platform was explicitly present.
  set_target_platform(effective_platform);
  last_applied_json_ = json;
  return ConfigParseStatus::Ok;
}

const Keymap& ConfigState::keymap() const {
  return current_.keymap;
}

const EncoderScrollConfig& ConfigState::encoder_scroll() const {
  return current_.encoder_scroll;
}

const std::string& ConfigState::ptt_hotkey() const {
  return current_.ptt_hotkey;
}

const std::string& ConfigState::edit_ptt_hotkey() const {
  return current_.edit_ptt_hotkey;
}

PttMode ConfigState::ptt_mode() const {
  return current_.ptt_mode;
}

HostPlatform ConfigState::target_platform() const { return current_.target_platform; }

void ConfigState::set_target_platform(HostPlatform platform) {
  current_.target_platform = platform;
  if (current_.ptt_hotkey_source == HotkeySource::PlatformDefault) {
    current_.ptt_hotkey = default_ptt_hotkey(platform);
  }
  if (current_.edit_ptt_hotkey_source == HotkeySource::PlatformDefault) {
    current_.edit_ptt_hotkey = default_edit_ptt_hotkey(platform);
  }
  current_.encoder_scroll.reverse_vertical = platform == HostPlatform::Windows
      ? current_.encoder_scroll.windows_reverse_vertical
      : current_.encoder_scroll.macos_reverse_vertical;
  current_.encoder_scroll.reverse_horizontal = platform == HostPlatform::Windows
      ? current_.encoder_scroll.windows_reverse_horizontal
      : current_.encoder_scroll.macos_reverse_horizontal;
}

bool ConfigState::audio_enabled() const {
  return !current_.wifi_ssid.empty() &&
         !current_.audio_host.empty() &&
         current_.audio_port >= 1024 &&
         current_.audio_port <= 65535;
}

const std::string& ConfigState::wifi_ssid() const {
  return current_.wifi_ssid;
}

const std::string& ConfigState::wifi_password() const {
  return current_.wifi_password;
}

const std::string& ConfigState::audio_host() const {
  return current_.audio_host;
}

int ConfigState::audio_port() const {
  return current_.audio_port;
}

const std::string& ConfigState::speaker_sync_key() const {
  return current_.speaker_sync_key;
}

std::uint16_t ConfigState::speaker_sync_key_epoch() const {
  return current_.speaker_sync_key_epoch;
}

const std::string& ConfigState::last_applied_json() const {
  return last_applied_json_;
}

}  // namespace ai_keyboard
