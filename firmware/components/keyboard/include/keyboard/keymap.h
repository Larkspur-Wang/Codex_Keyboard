#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace ai_keyboard {

enum class InputId : std::size_t {
  Key1 = 0,
  Key2,
  Key3,
  Key4,
  Key5,
  Key6,
  Key7,
  Key8,
  EncoderLeft,
  EncoderRight,
  EncoderPress,
  Count,
};

enum class InputPhase {
  Pressed,
  Released,
};

enum class ActionKind {
  Disabled,
  VoicePttHold,
  EditPttHold,
  PasteLast,
  OpenHistory,
  ToggleProfile,
  Hotkey,
  FixedText,
  PreviousProfile,
  NextProfile,
  Settings,
  ScrollAxisToggle,
  SelectAll,
  Copy,
  Paste,
  Undo,
};

enum class HostPlatform {
  MacOS,
  Windows,
};

struct Action {
  Action() = default;
  Action(ActionKind next_kind, std::string next_hotkey)
      : kind(next_kind), hotkey(std::move(next_hotkey)) {}
  Action(ActionKind next_kind, std::string next_hotkey, std::string next_text)
      : kind(next_kind), hotkey(std::move(next_hotkey)), text(std::move(next_text)) {}

  ActionKind kind = ActionKind::Disabled;
  std::string hotkey;
  std::string text;
};

class Keymap {
 public:
  explicit Keymap(std::array<Action, static_cast<std::size_t>(InputId::Count)> actions);

  const Action& action_for(InputId input) const;

 private:
  std::array<Action, static_cast<std::size_t>(InputId::Count)> actions_;
};

enum class FirmwareEventKind {
  None,
  HidKeyDown,
  HidKeyUp,
  HidTap,
  FixedText,
  AppCommand,
};

struct FirmwareEvent {
  FirmwareEvent() = default;
  FirmwareEvent(FirmwareEventKind next_kind,
                std::string next_value,
                bool next_bridge_app_hotkey = false)
      : kind(next_kind),
        value(std::move(next_value)),
        bridge_app_hotkey(next_bridge_app_hotkey) {}

  FirmwareEventKind kind = FirmwareEventKind::None;
  std::string value;
  bool bridge_app_hotkey = false;
};

enum class AudioTransport {
  Unavailable,
  WifiUdp,
};

struct HardwareCapabilities {
  bool has_microphone = false;
  AudioTransport audio_transport = AudioTransport::Unavailable;
};

Keymap DefaultKeymap();
FirmwareEvent event_for_action(const Action& action,
                               InputPhase phase,
                               const std::string& ptt_hotkey,
                               const std::string& edit_ptt_hotkey,
                               HostPlatform platform = HostPlatform::MacOS);
const char* host_platform_name(HostPlatform platform);
std::string semantic_hotkey(ActionKind action, HostPlatform platform);

}  // namespace ai_keyboard
