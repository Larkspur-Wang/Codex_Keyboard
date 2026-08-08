#include <cassert>
#include <string>

#include "keyboard/config_state.h"
#include "keyboard/hid_keycode.h"
#include "keyboard/keymap.h"

using ai_keyboard::ActionKind;
using ai_keyboard::FirmwareEventKind;
using ai_keyboard::InputId;

void default_mapping_covers_all_keys_and_encoder() {
  const auto keymap = ai_keyboard::DefaultKeymap();

  assert(keymap.action_for(InputId::Key1).kind == ActionKind::VoicePttHold);
  assert(keymap.action_for(InputId::Key2).kind == ActionKind::Hotkey);
  assert(keymap.action_for(InputId::Key2).hotkey == "Return");
  assert(keymap.action_for(InputId::Key3).kind == ActionKind::EditPttHold);
  assert(keymap.action_for(InputId::Key4).kind == ActionKind::Hotkey);
  assert(keymap.action_for(InputId::Key4).hotkey == "Backspace");
  assert(keymap.action_for(InputId::Key5).kind == ActionKind::SelectAll);
  assert(keymap.action_for(InputId::Key6).kind == ActionKind::Copy);
  assert(keymap.action_for(InputId::Key7).kind == ActionKind::Paste);
  assert(keymap.action_for(InputId::Key8).kind == ActionKind::Undo);
  assert(keymap.action_for(InputId::EncoderLeft).kind == ActionKind::Disabled);
  assert(keymap.action_for(InputId::EncoderRight).kind == ActionKind::Disabled);
  assert(keymap.action_for(InputId::EncoderPress).kind == ActionKind::ScrollAxisToggle);
}

void voice_hold_actions_emit_press_and_release_events() {
  const auto keymap = ai_keyboard::DefaultKeymap();

  const auto down = ai_keyboard::event_for_action(
      keymap.action_for(InputId::Key1),
      ai_keyboard::InputPhase::Pressed,
      "RightMeta",
      "AltGr");
  const auto up = ai_keyboard::event_for_action(
      keymap.action_for(InputId::Key1),
      ai_keyboard::InputPhase::Released,
      "RightMeta",
      "AltGr");

  assert(down.kind == FirmwareEventKind::HidKeyDown);
  assert(down.value == "RightMeta");
  assert(down.bridge_app_hotkey);
  assert(up.kind == FirmwareEventKind::HidKeyUp);
  assert(up.value == "RightMeta");
  assert(up.bridge_app_hotkey);
}

void edit_hold_actions_emit_edit_hotkey_events() {
  const auto keymap = ai_keyboard::DefaultKeymap();

  const auto down = ai_keyboard::event_for_action(
      keymap.action_for(InputId::Key3),
      ai_keyboard::InputPhase::Pressed,
      "RightMeta",
      "AltGr");
  const auto up = ai_keyboard::event_for_action(
      keymap.action_for(InputId::Key3),
      ai_keyboard::InputPhase::Released,
      "RightMeta",
      "AltGr");

  assert(down.kind == FirmwareEventKind::HidKeyDown);
  assert(down.value == "AltGr");
  assert(down.bridge_app_hotkey);
  assert(up.kind == FirmwareEventKind::HidKeyUp);
  assert(up.value == "AltGr");
  assert(up.bridge_app_hotkey);
}

void disabled_actions_emit_no_event() {
  const ai_keyboard::Action action{ActionKind::Disabled, ""};

  const auto event = ai_keyboard::event_for_action(
      action,
      ai_keyboard::InputPhase::Pressed,
      "F12",
      "F13");

  assert(event.kind == FirmwareEventKind::None);
  assert(event.value.empty());
}

void fixed_text_actions_emit_text_event_on_press_only() {
  const ai_keyboard::Action action{ActionKind::FixedText, "", "给我说中文"};

  const auto down = ai_keyboard::event_for_action(
      action,
      ai_keyboard::InputPhase::Pressed,
      "RightMeta",
      "AltGr");
  const auto up = ai_keyboard::event_for_action(
      action,
      ai_keyboard::InputPhase::Released,
      "RightMeta",
      "AltGr");

  assert(down.kind == FirmwareEventKind::FixedText);
  assert(down.value == "给我说中文");
  assert(up.kind == FirmwareEventKind::None);
}

void custom_hotkey_actions_emit_press_and_release_events() {
  const ai_keyboard::Action action{ActionKind::Hotkey, "Ctrl+Alt+Space"};

  const auto down = ai_keyboard::event_for_action(
      action,
      ai_keyboard::InputPhase::Pressed,
      "RightMeta",
      "AltGr");
  const auto up = ai_keyboard::event_for_action(
      action,
      ai_keyboard::InputPhase::Released,
      "RightMeta",
      "AltGr");

  assert(down.kind == FirmwareEventKind::HidKeyDown);
  assert(down.value == "Ctrl+Alt+Space");
  assert(!down.bridge_app_hotkey);
  assert(up.kind == FirmwareEventKind::HidKeyUp);
  assert(up.value == "Ctrl+Alt+Space");
  assert(!up.bridge_app_hotkey);
}

void semantic_actions_resolve_for_each_target_platform() {
  const auto keymap = ai_keyboard::DefaultKeymap();
  const std::array<std::pair<InputId, const char*>, 4> mac{{
      {InputId::Key5, "Meta+A"}, {InputId::Key6, "Meta+C"},
      {InputId::Key7, "Meta+V"}, {InputId::Key8, "Meta+Z"},
  }};
  const std::array<std::pair<InputId, const char*>, 4> windows{{
      {InputId::Key5, "Ctrl+A"}, {InputId::Key6, "Ctrl+C"},
      {InputId::Key7, "Ctrl+V"}, {InputId::Key8, "Ctrl+Z"},
  }};
  for (const auto& [input, expected] : mac) {
    const auto event = ai_keyboard::event_for_action(keymap.action_for(input),
        ai_keyboard::InputPhase::Pressed, "RightMeta", "AltGr",
        ai_keyboard::HostPlatform::MacOS);
    assert(event.value == expected);
  }
  for (const auto& [input, expected] : windows) {
    const auto event = ai_keyboard::event_for_action(keymap.action_for(input),
        ai_keyboard::InputPhase::Pressed, "Ctrl+Shift+Space", "Ctrl+Shift+E",
        ai_keyboard::HostPlatform::Windows);
    assert(event.value == expected);
  }
  const ai_keyboard::Action custom{ActionKind::Hotkey, "Meta+Shift+K"};
  for (const auto platform : {ai_keyboard::HostPlatform::MacOS,
                              ai_keyboard::HostPlatform::Windows}) {
    assert(ai_keyboard::event_for_action(custom, ai_keyboard::InputPhase::Pressed,
        "", "", platform).value == "Meta+Shift+K");
  }
}

void default_eight_keys_emit_exact_hid_for_both_platforms() {
  struct ExpectedReport {
    InputId input;
    std::uint8_t mac_modifier;
    std::uint8_t mac_keycode;
    std::uint8_t windows_modifier;
    std::uint8_t windows_keycode;
  };
  const std::array<ExpectedReport, 8> expected{{
      {InputId::Key1, 0x80, 0x00, 0x03, 0x2C},
      {InputId::Key2, 0x00, 0x28, 0x00, 0x28},
      {InputId::Key3, 0x40, 0x00, 0x03, 0x08},
      {InputId::Key4, 0x00, 0x2A, 0x00, 0x2A},
      {InputId::Key5, 0x08, 0x04, 0x01, 0x04},
      {InputId::Key6, 0x08, 0x06, 0x01, 0x06},
      {InputId::Key7, 0x08, 0x19, 0x01, 0x19},
      {InputId::Key8, 0x08, 0x1D, 0x01, 0x1D},
  }};

  ai_keyboard::ConfigState state;
  for (const auto platform : {ai_keyboard::HostPlatform::MacOS,
                              ai_keyboard::HostPlatform::Windows}) {
    state.set_target_platform(platform);
    for (const auto& item : expected) {
      const auto event = ai_keyboard::event_for_action(
          state.keymap().action_for(item.input),
          ai_keyboard::InputPhase::Pressed,
          state.ptt_hotkey(),
          state.edit_ptt_hotkey(),
          state.target_platform());
      const auto report = ai_keyboard::hid_report_for_hotkey(event.value);
      assert(report.valid);
      assert(report.modifier ==
             (platform == ai_keyboard::HostPlatform::Windows
                  ? item.windows_modifier
                  : item.mac_modifier));
      assert(report.keycode ==
             (platform == ai_keyboard::HostPlatform::Windows
                  ? item.windows_keycode
                  : item.mac_keycode));
    }
  }
}

void this_hardware_revision_reports_audio_unavailable() {
  const auto capabilities = ai_keyboard::HardwareCapabilities();

  assert(!capabilities.has_microphone);
  assert(capabilities.audio_transport == ai_keyboard::AudioTransport::Unavailable);
}

int main() {
  default_mapping_covers_all_keys_and_encoder();
  voice_hold_actions_emit_press_and_release_events();
  edit_hold_actions_emit_edit_hotkey_events();
  disabled_actions_emit_no_event();
  fixed_text_actions_emit_text_event_on_press_only();
  custom_hotkey_actions_emit_press_and_release_events();
  semantic_actions_resolve_for_each_target_platform();
  default_eight_keys_emit_exact_hid_for_both_platforms();
  this_hardware_revision_reports_audio_unavailable();
  return 0;
}
