#include <cassert>
#include <string>
#include <vector>

#include "keyboard/config_payload.h"
#include "keyboard/fixed_text_protocol.h"

using ai_keyboard::ActionKind;
using ai_keyboard::ConfigParseStatus;
using ai_keyboard::EncoderRotationMode;
using ai_keyboard::EncoderScrollAxis;
using ai_keyboard::InputId;
using ai_keyboard::PttMode;
using ai_keyboard::HotkeySource;

namespace {

std::string payload_with_fixed_text(const std::string& text) {
  return std::string(R"({
    "schema":"ai_keyboard.v1",
    "audio_enabled":false,
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":{"text":")") + text + R"("}},
        "KEY2":{"press":"disabled"},
        "KEY3":{"press":"disabled"},
        "KEY4":{"press":"disabled"},
        "KEY5":{"press":"disabled"},
        "KEY6":{"press":"disabled"},
        "KEY7":{"press":"disabled"},
        "KEY8":{"press":"disabled"}
      },
      "encoder":{
        "left":"disabled",
        "right":"disabled",
        "press":"disabled"
      }
    }]
  })";
}

std::string payload_with_extra_top_fields(
    const std::string& fields) {
  auto payload = payload_with_fixed_text("test");
  const auto schema = payload.find("\"schema\"");
  assert(schema != std::string::npos);
  payload.insert(schema, fields + ",");
  return payload;
}

}  // namespace

void migrates_exact_legacy_factory_keymap_to_platform_semantics() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "ptt_hotkey":"RightMeta",
    "edit_ptt_hotkey":"RightOption",
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

  const auto result = ai_keyboard::parse_config_payload(json);
  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.keymap.action_for(InputId::Key5).kind == ActionKind::SelectAll);
  assert(result.config.keymap.action_for(InputId::Key6).kind == ActionKind::Copy);
  assert(result.config.keymap.action_for(InputId::Key7).kind == ActionKind::Paste);
  assert(result.config.keymap.action_for(InputId::Key8).kind == ActionKind::Undo);
  assert(result.config.ptt_hotkey_source == HotkeySource::PlatformDefault);
  assert(result.config.edit_ptt_hotkey_source == HotkeySource::PlatformDefault);
  assert(!result.config.target_platform_explicit);
  assert(!result.config.ptt_hotkey_source_explicit);
  assert(!result.config.edit_ptt_hotkey_source_explicit);
}

void explicit_custom_sources_survive_exact_legacy_keymap_migration() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "target_platform":"windows",
    "ptt_hotkey":"RightMeta",
    "ptt_hotkey_source":"custom",
    "edit_ptt_hotkey":"RightOption",
    "edit_ptt_hotkey_source":"custom",
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

  const auto result = ai_keyboard::parse_config_payload(json);
  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.target_platform_explicit);
  assert(result.config.ptt_hotkey_source_explicit);
  assert(result.config.edit_ptt_hotkey_source_explicit);
  assert(result.config.ptt_hotkey_source == HotkeySource::Custom);
  assert(result.config.edit_ptt_hotkey_source == HotkeySource::Custom);
  assert(result.config.ptt_hotkey == "RightMeta");
  assert(result.config.edit_ptt_hotkey == "RightOption");
  assert(result.config.keymap.action_for(InputId::Key5).kind == ActionKind::SelectAll);
  assert(result.config.keymap.action_for(InputId::Key8).kind == ActionKind::Undo);
}

void missing_sources_migrate_factory_hotkeys_after_semantic_keymap_migration() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "target_platform":"windows",
    "ptt_hotkey":"RightMeta",
    "edit_ptt_hotkey":"RightOption",
    "profiles":[{"id":"default","keys":{
      "KEY1":{"press":"voice_ptt_hold"},
      "KEY2":{"press":{"hotkey":"Return"}},
      "KEY3":{"press":"edit_ptt_hold"},
      "KEY4":{"press":{"hotkey":"Backspace"}},
      "KEY5":{"press":"select_all"},
      "KEY6":{"press":"copy"},
      "KEY7":{"press":"paste"},
      "KEY8":{"press":"undo"}
    },"encoder":{"left":"disabled","right":"disabled","press":"disabled"}}]
  })";

  const auto result = ai_keyboard::parse_config_payload(json);
  assert(result.status == ConfigParseStatus::Ok);
  assert(!result.config.ptt_hotkey_source_explicit);
  assert(!result.config.edit_ptt_hotkey_source_explicit);
  assert(result.config.ptt_hotkey_source == HotkeySource::PlatformDefault);
  assert(result.config.edit_ptt_hotkey_source == HotkeySource::PlatformDefault);
  assert(result.config.ptt_hotkey == "Ctrl+Shift+Space");
  assert(result.config.edit_ptt_hotkey == "Ctrl+Shift+E");
}

void migrates_v1_expanded_factory_actions_for_both_platforms() {
  const auto payload_for = [](const char* modifier) {
    return std::string(R"({
      "schema":"ai_keyboard.v1",
      "profiles":[{"id":"default","keys":{
        "KEY1":{"press":"voice_ptt_hold"},
        "KEY2":{"press":{"hotkey":"Return"}},
        "KEY3":{"press":"edit_ptt_hold"},
        "KEY4":{"press":{"hotkey":"Backspace"}},
        "KEY5":{"press":{"hotkey":")") + modifier + R"(+A"}},
        "KEY6":{"press":{"hotkey":")" + modifier + R"(+C"}},
        "KEY7":{"press":{"hotkey":")" + modifier + R"(+V"}},
        "KEY8":{"press":{"hotkey":")" + modifier + R"(+Z"}}
      },"encoder":{"left":"disabled","right":"disabled","press":"disabled"}}]
    })";
  };

  for (const auto* modifier : {"Meta", "Ctrl"}) {
    const auto result = ai_keyboard::parse_config_payload(payload_for(modifier));
    assert(result.status == ConfigParseStatus::Ok);
    assert(result.config.keymap.action_for(InputId::Key5).kind == ActionKind::SelectAll);
    assert(result.config.keymap.action_for(InputId::Key6).kind == ActionKind::Copy);
    assert(result.config.keymap.action_for(InputId::Key7).kind == ActionKind::Paste);
    assert(result.config.keymap.action_for(InputId::Key8).kind == ActionKind::Undo);
  }
}

void does_not_migrate_near_match_custom_expanded_layout() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "profiles":[{"id":"default","keys":{
      "KEY1":{"press":"voice_ptt_hold"},
      "KEY2":{"press":{"hotkey":"Return"}},
      "KEY3":{"press":"edit_ptt_hold"},
      "KEY4":{"press":{"hotkey":"Backspace"}},
      "KEY5":{"press":{"hotkey":"Meta+A"}},
      "KEY6":{"press":{"hotkey":"Meta+C"}},
      "KEY7":{"press":{"hotkey":"Meta+V"}},
      "KEY8":{"press":{"hotkey":"Meta+Shift+Z"}}
    },"encoder":{"left":"disabled","right":"disabled","press":"disabled"}}]
  })";

  const auto result = ai_keyboard::parse_config_payload(json);
  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.keymap.action_for(InputId::Key5).kind == ActionKind::Hotkey);
  assert(result.config.keymap.action_for(InputId::Key6).kind == ActionKind::Hotkey);
  assert(result.config.keymap.action_for(InputId::Key7).kind == ActionKind::Hotkey);
  assert(result.config.keymap.action_for(InputId::Key8).kind == ActionKind::Hotkey);
  assert(result.config.keymap.action_for(InputId::Key8).hotkey == "Meta+Shift+Z");
}

void preserves_non_factory_raw_hotkeys_as_custom() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "ptt_hotkey":"F13",
    "edit_ptt_hotkey":"F14",
    "profiles":[{"id":"default","keys":{
      "KEY1":{"press":"voice_ptt_hold"},
      "KEY2":{"press":{"hotkey":"Return"}},
      "KEY3":{"press":"edit_ptt_hold"},
      "KEY4":{"press":{"hotkey":"Backspace"}},
      "KEY5":{"press":{"hotkey":"Meta+A"}},
      "KEY6":{"press":{"hotkey":"Meta+C"}},
      "KEY7":{"press":{"hotkey":"Meta+V"}},
      "KEY8":{"press":{"hotkey":"F12"}}
    },"encoder":{"left":"disabled","right":"disabled","press":"disabled"}}]
  })";

  const auto result = ai_keyboard::parse_config_payload(json);
  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.keymap.action_for(InputId::Key5).kind == ActionKind::Hotkey);
  assert(result.config.keymap.action_for(InputId::Key8).hotkey == "F12");
  assert(result.config.ptt_hotkey_source == HotkeySource::Custom);
  assert(result.config.edit_ptt_hotkey_source == HotkeySource::Custom);
}

void parses_easy_input_v1_payload_into_keymap() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "device_name":"AIOTWAN AI Keyboard",
    "audio_source":"unavailable",
    "audio_enabled":false,
    "ptt_hotkey":"F13",
    "edit_ptt_hotkey":"F14",
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":"voice_ptt_hold"},
        "KEY2":{"press":"edit_ptt_hold"},
        "KEY3":{"press":"disabled"},
        "KEY4":{"press":"disabled"},
        "KEY5":{"press":"disabled"},
        "KEY6":{"press":{"text":"给我说中文"}},
        "KEY7":{"press":"disabled"},
        "KEY8":{"press":"disabled"}
      },
      "encoder":{
        "left":"disabled",
        "right":"disabled",
        "press":"disabled",
        "scroll":{
          "enabled":true,
          "mode":"cursor",
          "axis":"horizontal",
          "speed":5,
          "reverse_vertical":true,
          "reverse_horizontal":false
        }
      }
    }]
  })";

  const auto result = ai_keyboard::parse_config_payload(json);

  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.ptt_hotkey == "F13");
  assert(result.config.edit_ptt_hotkey == "F14");
  assert(result.config.ptt_mode == PttMode::Hold);
  assert(result.config.keymap.action_for(InputId::Key1).kind == ActionKind::VoicePttHold);
  assert(result.config.keymap.action_for(InputId::Key2).kind == ActionKind::EditPttHold);
  assert(result.config.keymap.action_for(InputId::Key6).kind == ActionKind::FixedText);
  assert(result.config.keymap.action_for(InputId::Key6).text == "给我说中文");
  assert(result.config.keymap.action_for(InputId::Key8).kind == ActionKind::Disabled);
  assert(result.config.keymap.action_for(InputId::EncoderLeft).kind == ActionKind::Disabled);
  assert(result.config.keymap.action_for(InputId::EncoderRight).kind == ActionKind::Disabled);
  assert(result.config.keymap.action_for(InputId::EncoderPress).kind == ActionKind::Disabled);
  assert(result.config.encoder_scroll.enabled);
  assert(result.config.encoder_scroll.mode == EncoderRotationMode::Cursor);
  assert(result.config.encoder_scroll.axis == EncoderScrollAxis::Horizontal);
  assert(result.config.encoder_scroll.speed == 5);
  assert(result.config.encoder_scroll.reverse_vertical);
  assert(!result.config.encoder_scroll.reverse_horizontal);
}

void keeps_legacy_encoder_scroll_defaults_when_scroll_config_is_missing() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "audio_enabled":false,
    "ptt_hotkey":"F13",
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":"voice_ptt_hold"},
        "KEY2":{"press":{"hotkey":"Return"}},
        "KEY3":{"press":"paste_last"},
        "KEY4":{"press":"history"},
        "KEY5":{"press":"toggle_profile"},
        "KEY6":{"press":"disabled"},
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

  const auto result = ai_keyboard::parse_config_payload(json);

  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.keymap.action_for(InputId::EncoderPress).kind == ActionKind::Settings);
  assert(result.config.edit_ptt_hotkey == "RightOption");
  assert(result.config.encoder_scroll.enabled);
  assert(result.config.encoder_scroll.mode == EncoderRotationMode::Scroll);
  assert(result.config.encoder_scroll.axis == EncoderScrollAxis::Vertical);
  assert(result.config.encoder_scroll.speed == 3);
}

void parses_v2_keyboard_microphone_settings() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "wifi_ssid":"Office WiFi",
    "wifi_password":"demo-pass",
    "audio_host":"192.168.1.55",
    "audio_port":17333,
    "microphone_source":"keyboard",
    "audio_source":"wifi_udp",
    "audio_enabled":true,
    "ptt_hotkey":"F13",
    "edit_ptt_hotkey":"F14",
    "hotkey_mode":"toggle",
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":"voice_ptt_hold"},
        "KEY2":{"press":"edit_ptt_hold"},
        "KEY3":{"press":"disabled"},
        "KEY4":{"press":"disabled"},
        "KEY5":{"press":"disabled"},
        "KEY6":{"press":"disabled"},
        "KEY7":{"press":"disabled"},
        "KEY8":{"press":"disabled"}
      },
      "encoder":{
        "left":"disabled",
        "right":"disabled",
        "press":"disabled"
      }
    }]
  })";

  const auto result = ai_keyboard::parse_config_payload(json);

  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.wifi_ssid == "Office WiFi");
  assert(result.config.wifi_password == "demo-pass");
  assert(result.config.audio_host == "192.168.1.55");
  assert(result.config.audio_port == 17333);
  assert(result.config.ptt_mode == PttMode::Toggle);
}

void accepts_legacy_audio_enabled_without_wifi_endpoint() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "audio_source":"wifi_udp",
    "audio_enabled":true,
    "ptt_hotkey":"F13",
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":"voice_ptt_hold"},
        "KEY2":{"press":"edit_ptt_hold"},
        "KEY3":{"press":"disabled"},
        "KEY4":{"press":"disabled"},
        "KEY5":{"press":"disabled"},
        "KEY6":{"press":"disabled"},
        "KEY7":{"press":"disabled"},
        "KEY8":{"press":"disabled"}
      },
      "encoder":{
        "left":"disabled",
        "right":"disabled",
        "press":"disabled"
      }
    }]
  })";

  const auto result = ai_keyboard::parse_config_payload(json);

  assert(result.status == ConfigParseStatus::Ok);
}

void parses_paired_speaker_sync_credentials() {
  const std::string key(
      "00112233445566778899aabbccddeeff"
      "102132435465768798a9bacbdcedfe0f");
  const auto result = ai_keyboard::parse_config_payload(
      payload_with_extra_top_fields(
          std::string(R"("speaker_sync_key":")") +
          key +
          R"(","speaker_sync_key_epoch":4660)"));
  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.speaker_sync_key == key);
  assert(result.config.speaker_sync_key_epoch == 4660U);
}

void rejects_unpaired_or_invalid_speaker_sync_credentials() {
  const std::vector<std::string> invalid{
      R"({"schema":"ai_keyboard.v1","speaker_sync_key":"1111111111111111111111111111111111111111111111111111111111111111"})",
      R"({"schema":"ai_keyboard.v1","speaker_sync_key_epoch":1})",
      R"({"schema":"ai_keyboard.v1","speaker_sync_key":"0000000000000000000000000000000000000000000000000000000000000000","speaker_sync_key_epoch":1})",
      R"({"schema":"ai_keyboard.v1","speaker_sync_key":"xyz","speaker_sync_key_epoch":1})",
      R"({"schema":"ai_keyboard.v1","speaker_sync_key":"1111111111111111111111111111111111111111111111111111111111111111","speaker_sync_key_epoch":0})",
      R"({"schema":"ai_keyboard.v1","speaker_sync_key":"1111111111111111111111111111111111111111111111111111111111111111","speaker_sync_key_epoch":65536})",
  };
  for (const auto& fields : invalid) {
    const auto result =
        ai_keyboard::parse_config_payload(
            payload_with_extra_top_fields(
                fields.substr(1U, fields.size() - 2U)));
    assert(result.status == ConfigParseStatus::InvalidJson);
  }
}

void rejects_unknown_named_action() {
  const std::string json = R"({
    "schema":"ai_keyboard.v1",
    "audio_enabled":false,
    "ptt_hotkey":"F12",
    "profiles":[{
      "id":"default",
      "keys":{
        "KEY1":{"press":"not_real"},
        "KEY2":{"press":{"hotkey":"Return"}},
        "KEY3":{"press":"paste_last"},
        "KEY4":{"press":"history"},
        "KEY5":{"press":"toggle_profile"},
        "KEY6":{"press":{"hotkey":"Return"}},
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

  const auto result = ai_keyboard::parse_config_payload(json);

  assert(result.status == ConfigParseStatus::UnknownAction);
}

void accepts_fixed_text_at_the_960_utf8_byte_limit() {
  static_assert(ai_keyboard::kFixedTextMaxUtf8Bytes == 960);
  std::string text;
  for (int index = 0; index < 320; ++index) {
    text += "中";
  }
  assert(text.size() == 960);

  const auto result =
      ai_keyboard::parse_config_payload(payload_with_fixed_text(text));

  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.keymap.action_for(InputId::Key1).kind ==
         ActionKind::FixedText);
  assert(result.config.keymap.action_for(InputId::Key1).text == text);
}

void rejects_fixed_text_above_the_960_utf8_byte_limit() {
  const std::string text(ai_keyboard::kFixedTextMaxUtf8Bytes + 1, 'x');

  const auto result =
      ai_keyboard::parse_config_payload(payload_with_fixed_text(text));

  assert(result.status == ConfigParseStatus::FixedTextTooLarge);
}

void decodes_escaped_bmp_and_surrogate_pair_as_utf8() {
  const auto result = ai_keyboard::parse_config_payload(
      payload_with_fixed_text("\\u4F60\\u597D \\uD83D\\uDE00"));

  assert(result.status == ConfigParseStatus::Ok);
  assert(result.config.keymap.action_for(InputId::Key1).text ==
         "你好 😀");
}

void decodes_escaped_json_control_characters() {
  const auto result = ai_keyboard::parse_config_payload(
      payload_with_fixed_text(
          "\\u0000\\u001F\\b\\f\\n\\r\\t"));

  assert(result.status == ConfigParseStatus::Ok);
  const std::string expected{
      '\0', '\x1F', '\b', '\f', '\n', '\r', '\t'};
  assert(result.config.keymap.action_for(InputId::Key1).text == expected);
}

void rejects_isolated_or_invalid_utf16_surrogates() {
  const std::vector<std::string> invalid{
      "\\uD83D",
      "\\uDE00",
      "\\uD83D\\u0041",
      "\\uDE00\\uD83D",
      "\\uD83X",
  };
  for (const auto& text : invalid) {
    const auto result =
        ai_keyboard::parse_config_payload(payload_with_fixed_text(text));
    assert(result.status != ConfigParseStatus::Ok);
  }
}

void rejects_invalid_literal_utf8_sequences() {
  const std::vector<std::string> invalid{
      std::string(1, '\x1F'),
      std::string("\xC0\xAF", 2),
      std::string("\xED\xA0\x80", 3),
      std::string("\xF4\x90\x80\x80", 4),
      std::string("\xE4\xB8", 2),
  };
  for (const auto& text : invalid) {
    const auto result =
        ai_keyboard::parse_config_payload(payload_with_fixed_text(text));
    assert(result.status != ConfigParseStatus::Ok);
  }
}

int main() {
  migrates_exact_legacy_factory_keymap_to_platform_semantics();
  explicit_custom_sources_survive_exact_legacy_keymap_migration();
  missing_sources_migrate_factory_hotkeys_after_semantic_keymap_migration();
  migrates_v1_expanded_factory_actions_for_both_platforms();
  does_not_migrate_near_match_custom_expanded_layout();
  preserves_non_factory_raw_hotkeys_as_custom();
  parses_easy_input_v1_payload_into_keymap();
  keeps_legacy_encoder_scroll_defaults_when_scroll_config_is_missing();
  parses_v2_keyboard_microphone_settings();
  accepts_legacy_audio_enabled_without_wifi_endpoint();
  parses_paired_speaker_sync_credentials();
  rejects_unpaired_or_invalid_speaker_sync_credentials();
  rejects_unknown_named_action();
  accepts_fixed_text_at_the_960_utf8_byte_limit();
  rejects_fixed_text_above_the_960_utf8_byte_limit();
  decodes_escaped_bmp_and_surrogate_pair_as_utf8();
  decodes_escaped_json_control_characters();
  rejects_isolated_or_invalid_utf16_surrogates();
  rejects_invalid_literal_utf8_sequences();
  return 0;
}
