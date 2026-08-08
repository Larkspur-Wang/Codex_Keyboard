#pragma once

#include <string>

#include "keyboard/config_payload.h"
#include "keyboard/keymap.h"

namespace ai_keyboard {

class ConfigState {
 public:
  ConfigState() = default;

  ConfigParseStatus apply_json(const std::string& json);

  const Keymap& keymap() const;
  const EncoderScrollConfig& encoder_scroll() const;
  const std::string& ptt_hotkey() const;
  const std::string& edit_ptt_hotkey() const;
  PttMode ptt_mode() const;
  HostPlatform target_platform() const;
  // 键盘麦克风能力只由 SSID、上位机和合法端口组成的完整端点决定。
  // 旧 audio_enabled/audio_source/microphone_source 只做协议校验，不进入运行态。
  bool audio_enabled() const;
  const std::string& wifi_ssid() const;
  const std::string& wifi_password() const;
  const std::string& audio_host() const;
  int audio_port() const;
  const std::string& speaker_sync_key() const;
  std::uint16_t speaker_sync_key_epoch() const;
  void set_target_platform(HostPlatform platform);
  const std::string& last_applied_json() const;

 private:
  ParsedKeyboardConfig current_{};
  std::string last_applied_json_;
};

}  // namespace ai_keyboard
