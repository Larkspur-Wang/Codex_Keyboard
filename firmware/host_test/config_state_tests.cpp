#include <cassert>
#include <string>

#include "keyboard/config_state.h"

using ai_keyboard::ActionKind;
using ai_keyboard::ConfigParseStatus;
using ai_keyboard::InputId;
using ai_keyboard::PttMode;

namespace {

std::string valid_payload(const std::string& key6_hotkey,
                          const std::string& ptt_hotkey,
                          const std::string& edit_ptt_hotkey) {
  return R"({
    "schema":"ai_keyboard.v1",
    "wifi_ssid":"Office WiFi",
    "wifi_password":"demo-pass",
    "audio_host":"192.168.1.55",
    "audio_port":17333,
    "microphone_source":"keyboard",
    "audio_source":"wifi_udp",
    "audio_enabled":true,
    "ptt_hotkey":")" + ptt_hotkey + R"(",
    "edit_ptt_hotkey":")" + edit_ptt_hotkey + R"(",
    "hotkey_mode":"toggle",
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":"voice_ptt_hold"},
        "KEY2":{"press":{"hotkey":"Return"}},
        "KEY3":{"press":"paste_last"},
        "KEY4":{"press":"history"},
        "KEY5":{"press":"toggle_profile"},
        "KEY6":{"press":{"hotkey":")" + key6_hotkey + R"("}},
        "KEY7":{"press":{"hotkey":"Escape"}},
        "KEY8":{"press":"disabled"}
      },
      "encoder":{
        "left":"previous_profile",
        "right":"next_profile",
        "press":"settings"
      }
    }]
  })";
}

std::string legacy_factory_payload(const std::string& top_level_fields) {
  return R"({"schema":"ai_keyboard.v1")" + top_level_fields + R"(,
    "profiles":[{"id":"default","keys":{
      "KEY1":{"press":"voice_ptt_hold"},
      "KEY2":{"press":{"hotkey":"Return"}},
      "KEY3":{"press":"edit_ptt_hold"},
      "KEY4":{"press":{"hotkey":"Backspace"}},
      "KEY5":{"press":{"hotkey":"Meta+A"}},
      "KEY6":{"press":{"hotkey":"Meta+C"}},
      "KEY7":{"press":{"hotkey":"Meta+V"}},
      "KEY8":{"press":{"hotkey":"Ctrl+Tab"}}
    },"encoder":{"left":"disabled","right":"disabled","press":"disabled"}}]
  })";
}

}  // namespace

void starts_with_default_keymap() {
  const ai_keyboard::ConfigState state;

  assert(state.ptt_hotkey() == "RightMeta");
  assert(state.edit_ptt_hotkey() == "RightOption");
  assert(state.ptt_mode() == PttMode::Toggle);
  assert(state.keymap().action_for(InputId::Key2).kind == ActionKind::Hotkey);
  assert(state.keymap().action_for(InputId::Key2).hotkey == "Return");
  assert(state.keymap().action_for(InputId::Key3).kind == ActionKind::EditPttHold);
  assert(state.keymap().action_for(InputId::Key4).kind == ActionKind::Hotkey);
  assert(state.keymap().action_for(InputId::Key4).hotkey == "Backspace");
  assert(state.keymap().action_for(InputId::Key5).kind == ActionKind::SelectAll);
  assert(state.keymap().action_for(InputId::Key6).kind == ActionKind::Copy);
  assert(state.keymap().action_for(InputId::Key7).kind == ActionKind::Paste);
}

void applies_valid_payload() {
  ai_keyboard::ConfigState state;
  const auto payload = valid_payload("Ctrl+Alt+Space", "F13", "F14");

  const auto status = state.apply_json(payload);

  assert(status == ConfigParseStatus::Ok);
  assert(state.ptt_hotkey() == "F13");
  assert(state.edit_ptt_hotkey() == "F14");
  assert(state.ptt_mode() == PttMode::Toggle);
  assert(state.audio_enabled());
  assert(state.wifi_ssid() == "Office WiFi");
  assert(state.wifi_password() == "demo-pass");
  assert(state.audio_host() == "192.168.1.55");
  assert(state.audio_port() == 17333);
  assert(state.keymap().action_for(InputId::Key6).kind == ActionKind::Hotkey);
  assert(state.keymap().action_for(InputId::Key6).hotkey == "Ctrl+Alt+Space");
  assert(state.last_applied_json() == payload);
}

void preserves_explicit_hold_mode() {
  ai_keyboard::ConfigState state;
  auto payload = valid_payload("Ctrl+Alt+Space", "F13", "F14");
  const auto toggle_mode = payload.find("\"hotkey_mode\":\"toggle\"");
  assert(toggle_mode != std::string::npos);
  payload.replace(toggle_mode,
                  std::string("\"hotkey_mode\":\"toggle\"").size(),
                  "\"hotkey_mode\":\"hold\"");

  assert(state.apply_json(payload) == ConfigParseStatus::Ok);
  assert(state.ptt_mode() == PttMode::Hold);
}

void missing_target_platform_preserves_current_windows_mode() {
  ai_keyboard::ConfigState state;
  state.set_target_platform(ai_keyboard::HostPlatform::Windows);

  const auto status = state.apply_json(legacy_factory_payload(
      R"(,"ptt_hotkey":"RightMeta","edit_ptt_hotkey":"RightOption")"));

  assert(status == ConfigParseStatus::Ok);
  assert(state.target_platform() == ai_keyboard::HostPlatform::Windows);
  assert(state.ptt_hotkey() == "Ctrl+Shift+Space");
  assert(state.edit_ptt_hotkey() == "Ctrl+Shift+E");
  assert(state.keymap().action_for(InputId::Key5).kind == ActionKind::SelectAll);
}

void explicit_windows_reapplies_platform_defaults_and_scroll_preset() {
  const std::string payload = R"({
    "schema":"ai_keyboard.v1",
    "target_platform":"windows",
    "ptt_hotkey":"RightMeta",
    "ptt_hotkey_source":"platform_default",
    "edit_ptt_hotkey":"RightOption",
    "edit_ptt_hotkey_source":"platform_default",
    "profiles":[{"id":"default","keys":{
      "KEY1":{"press":"voice_ptt_hold"},
      "KEY2":{"press":{"hotkey":"Return"}},
      "KEY3":{"press":"edit_ptt_hold"},
      "KEY4":{"press":{"hotkey":"Backspace"}},
      "KEY5":{"press":"select_all"},
      "KEY6":{"press":"copy"},
      "KEY7":{"press":"paste"},
      "KEY8":{"press":"undo"}
    },"encoder":{
      "left":"disabled","right":"disabled","press":"disabled",
      "scroll":{
        "enabled":true,"mode":"scroll","axis":"vertical","speed":3,
        "macos_reverse_vertical":false,
        "macos_reverse_horizontal":true,
        "windows_reverse_vertical":true,
        "windows_reverse_horizontal":false
      }
    }}]
  })";

  ai_keyboard::ConfigState state;
  assert(state.apply_json(payload) == ConfigParseStatus::Ok);
  assert(state.target_platform() == ai_keyboard::HostPlatform::Windows);
  assert(state.ptt_hotkey() == "Ctrl+Shift+Space");
  assert(state.edit_ptt_hotkey() == "Ctrl+Shift+E");
  assert(state.encoder_scroll().reverse_vertical);
  assert(!state.encoder_scroll().reverse_horizontal);
}

void explicit_custom_ptt_and_raw_hotkeys_do_not_change_with_platform() {
  ai_keyboard::ConfigState state;
  const auto status = state.apply_json(legacy_factory_payload(
      R"(,"target_platform":"macos","ptt_hotkey":"RightMeta",
      "ptt_hotkey_source":"custom","edit_ptt_hotkey":"RightOption",
      "edit_ptt_hotkey_source":"custom")"));
  assert(status == ConfigParseStatus::Ok);

  state.set_target_platform(ai_keyboard::HostPlatform::Windows);
  assert(state.ptt_hotkey() == "RightMeta");
  assert(state.edit_ptt_hotkey() == "RightOption");

  assert(state.apply_json(valid_payload("Meta+Shift+K", "F13", "F14")) ==
         ConfigParseStatus::Ok);
  state.set_target_platform(ai_keyboard::HostPlatform::MacOS);
  assert(state.ptt_hotkey() == "F13");
  assert(state.edit_ptt_hotkey() == "F14");
  assert(state.keymap().action_for(InputId::Key6).kind == ActionKind::Hotkey);
  assert(state.keymap().action_for(InputId::Key6).hotkey == "Meta+Shift+K");
  state.set_target_platform(ai_keyboard::HostPlatform::Windows);
  assert(state.keymap().action_for(InputId::Key6).hotkey == "Meta+Shift+K");
}

void derives_audio_capability_from_complete_endpoint() {
  ai_keyboard::ConfigState state;
  auto payload = valid_payload("Ctrl+Alt+Space", "F13", "F14");
  const auto enabled_field = payload.find("\"audio_enabled\":true");
  assert(enabled_field != std::string::npos);
  payload.replace(enabled_field,
                  std::string("\"audio_enabled\":true").size(),
                  "\"audio_enabled\":false");
  const auto source_field = payload.find("\"audio_source\":\"wifi_udp\"");
  assert(source_field != std::string::npos);
  payload.replace(source_field,
                  std::string("\"audio_source\":\"wifi_udp\"").size(),
                  "\"audio_source\":\"unavailable\"");
  const auto microphone_field = payload.find("\"microphone_source\":\"keyboard\"");
  assert(microphone_field != std::string::npos);
  payload.replace(microphone_field,
                  std::string("\"microphone_source\":\"keyboard\"").size(),
                  "\"microphone_source\":\"computer\"");

  assert(state.apply_json(payload) == ConfigParseStatus::Ok);
  assert(state.audio_enabled());
}

void rejects_incomplete_endpoint_as_audio_capability() {
  ai_keyboard::ConfigState state;
  auto payload = valid_payload("Ctrl+Alt+Space", "F13", "F14");
  const auto host_field = payload.find("\"audio_host\":\"192.168.1.55\"");
  assert(host_field != std::string::npos);
  payload.replace(host_field,
                  std::string("\"audio_host\":\"192.168.1.55\"").size(),
                  "\"audio_host\":\"\"");

  assert(state.apply_json(payload) == ConfigParseStatus::Ok);
  assert(!state.audio_enabled());
}

void rejects_invalid_payload_without_overwriting_current_config() {
  ai_keyboard::ConfigState state;
  assert(state.apply_json(valid_payload("F14", "F13", "F12")) == ConfigParseStatus::Ok);

  const auto status = state.apply_json(R"({
    "schema":"ai_keyboard.v1",
    "audio_enabled":false,
    "ptt_hotkey":"F15",
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":"not_real"},
        "KEY2":{"press":{"hotkey":"Return"}},
        "KEY3":{"press":"paste_last"},
        "KEY4":{"press":"history"},
        "KEY5":{"press":"toggle_profile"},
        "KEY6":{"press":{"hotkey":"F16"}},
        "KEY7":{"press":{"hotkey":"Escape"}},
        "KEY8":{"press":"disabled"}
      },
      "encoder":{
        "left":"previous_profile",
        "right":"next_profile",
        "press":"settings"
      }
    }]
  })");

  assert(status == ConfigParseStatus::UnknownAction);
  assert(state.ptt_hotkey() == "F13");
  assert(state.edit_ptt_hotkey() == "F12");
  assert(state.keymap().action_for(InputId::Key6).hotkey == "F14");
  assert(state.last_applied_json() == valid_payload("F14", "F13", "F12"));
}

int main() {
  starts_with_default_keymap();
  applies_valid_payload();
  preserves_explicit_hold_mode();
  missing_target_platform_preserves_current_windows_mode();
  explicit_windows_reapplies_platform_defaults_and_scroll_preset();
  explicit_custom_ptt_and_raw_hotkeys_do_not_change_with_platform();
  derives_audio_capability_from_complete_endpoint();
  rejects_incomplete_endpoint_as_audio_capability();
  rejects_invalid_payload_without_overwriting_current_config();
  return 0;
}
