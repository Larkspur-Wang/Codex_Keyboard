#include <cassert>

#include "keyboard/speaker_service_startup.h"

int main() {
  using ai_keyboard::SpeakerServiceStartupAction;
  using ai_keyboard::SpeakerServiceStartupInputs;
  using ai_keyboard::evaluate_speaker_service_startup;
  using ai_keyboard::speaker_boot_pipeline_allowed;

  const SpeakerServiceStartupInputs local_missing{
      false,
      true,
      false,
  };
  assert(evaluate_speaker_service_startup(local_missing) ==
         SpeakerServiceStartupAction::WaitForLocal);
  assert(!speaker_boot_pipeline_allowed(local_missing));

  const SpeakerServiceStartupInputs wifi_pending{
      true,
      true,
      false,
  };
  assert(evaluate_speaker_service_startup(wifi_pending) ==
         SpeakerServiceStartupAction::StartWifi);
  assert(!speaker_boot_pipeline_allowed(wifi_pending));

  const SpeakerServiceStartupInputs wifi_ready{
      true,
      true,
      true,
  };
  assert(evaluate_speaker_service_startup(wifi_ready) ==
         SpeakerServiceStartupAction::Ready);
  assert(speaker_boot_pipeline_allowed(wifi_ready));

  const SpeakerServiceStartupInputs audio_unavailable{
      true,
      false,
      false,
  };
  assert(evaluate_speaker_service_startup(audio_unavailable) ==
         SpeakerServiceStartupAction::WifiUnavailable);
  // A failed microphone/Wi-Fi audio owner must not suppress the independent
  // local boot sound or USB sound management service.
  assert(speaker_boot_pipeline_allowed(audio_unavailable));

  return 0;
}
