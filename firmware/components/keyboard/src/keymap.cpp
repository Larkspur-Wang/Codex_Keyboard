#include "keyboard/keymap.h"

#include <utility>

namespace ai_keyboard {
namespace {

constexpr std::size_t index_of(InputId input) {
  return static_cast<std::size_t>(input);
}

FirmwareEvent command_event(const char* value, InputPhase phase) {
  if (phase == InputPhase::Released) {
    return {};
  }
  return {FirmwareEventKind::AppCommand, value};
}

}  // namespace

Keymap::Keymap(std::array<Action, static_cast<std::size_t>(InputId::Count)> actions)
    : actions_(std::move(actions)) {}

const Action& Keymap::action_for(InputId input) const {
  return actions_.at(index_of(input));
}

Keymap DefaultKeymap() {
  std::array<Action, static_cast<std::size_t>(InputId::Count)> actions{};
  actions[index_of(InputId::Key1)] = {ActionKind::VoicePttHold, ""};
  actions[index_of(InputId::Key2)] = {ActionKind::Hotkey, "Return"};
  actions[index_of(InputId::Key3)] = {ActionKind::EditPttHold, ""};
  actions[index_of(InputId::Key4)] = {ActionKind::Hotkey, "Backspace"};
  actions[index_of(InputId::Key5)] = {ActionKind::SelectAll, ""};
  actions[index_of(InputId::Key6)] = {ActionKind::Copy, ""};
  actions[index_of(InputId::Key7)] = {ActionKind::Paste, ""};
  actions[index_of(InputId::Key8)] = {ActionKind::Undo, ""};
  actions[index_of(InputId::EncoderLeft)] = {ActionKind::Disabled, ""};
  actions[index_of(InputId::EncoderRight)] = {ActionKind::Disabled, ""};
  actions[index_of(InputId::EncoderPress)] = {ActionKind::ScrollAxisToggle, ""};
  return Keymap(actions);
}

FirmwareEvent event_for_action(const Action& action,
                               InputPhase phase,
                               const std::string& ptt_hotkey,
                               const std::string& edit_ptt_hotkey,
                               HostPlatform platform) {
  switch (action.kind) {
    case ActionKind::VoicePttHold:
      return {
          phase == InputPhase::Pressed ? FirmwareEventKind::HidKeyDown : FirmwareEventKind::HidKeyUp,
          ptt_hotkey,
          true,
      };
    case ActionKind::EditPttHold:
      return {
          phase == InputPhase::Pressed ? FirmwareEventKind::HidKeyDown : FirmwareEventKind::HidKeyUp,
          edit_ptt_hotkey,
          true,
      };
    case ActionKind::Hotkey:
      return {
          phase == InputPhase::Pressed ? FirmwareEventKind::HidKeyDown : FirmwareEventKind::HidKeyUp,
          action.hotkey,
      };
    case ActionKind::SelectAll:
    case ActionKind::Copy:
    case ActionKind::Paste:
    case ActionKind::Undo:
      return {
          phase == InputPhase::Pressed ? FirmwareEventKind::HidKeyDown : FirmwareEventKind::HidKeyUp,
          semantic_hotkey(action.kind, platform),
      };
    case ActionKind::FixedText:
      if (phase == InputPhase::Released) {
        return {};
      }
      return {FirmwareEventKind::FixedText, action.text};
    case ActionKind::PasteLast:
      return command_event("paste_last", phase);
    case ActionKind::OpenHistory:
      return command_event("history", phase);
    case ActionKind::ToggleProfile:
      return command_event("toggle_profile", phase);
    case ActionKind::PreviousProfile:
      return command_event("previous_profile", phase);
    case ActionKind::NextProfile:
      return command_event("next_profile", phase);
    case ActionKind::Settings:
      return command_event("settings", phase);
    case ActionKind::ScrollAxisToggle:
      return {};
    case ActionKind::Disabled:
      return {};
  }
  return {};
}

const char* host_platform_name(HostPlatform platform) {
  return platform == HostPlatform::Windows ? "windows" : "macos";
}

std::string semantic_hotkey(ActionKind action, HostPlatform platform) {
  const char* modifier = platform == HostPlatform::Windows ? "Ctrl" : "Meta";
  switch (action) {
    case ActionKind::SelectAll: return std::string(modifier) + "+A";
    case ActionKind::Copy: return std::string(modifier) + "+C";
    case ActionKind::Paste: return std::string(modifier) + "+V";
    case ActionKind::Undo: return std::string(modifier) + "+Z";
    default: return {};
  }
}

}  // namespace ai_keyboard
