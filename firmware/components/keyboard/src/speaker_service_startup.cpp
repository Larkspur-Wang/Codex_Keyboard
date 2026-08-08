#include "keyboard/speaker_service_startup.h"

namespace ai_keyboard {

SpeakerServiceStartupAction evaluate_speaker_service_startup(
    const SpeakerServiceStartupInputs& inputs) {
  if (!inputs.local_ready) {
    return SpeakerServiceStartupAction::WaitForLocal;
  }
  if (!inputs.audio_ready) {
    return SpeakerServiceStartupAction::WifiUnavailable;
  }
  if (!inputs.wifi_ready) {
    return SpeakerServiceStartupAction::StartWifi;
  }
  return SpeakerServiceStartupAction::Ready;
}

bool speaker_boot_pipeline_allowed(
    const SpeakerServiceStartupInputs& inputs) {
  const auto action = evaluate_speaker_service_startup(inputs);
  return action == SpeakerServiceStartupAction::Ready ||
         action == SpeakerServiceStartupAction::WifiUnavailable;
}

}  // namespace ai_keyboard
