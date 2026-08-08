#pragma once

#include <cstdint>

namespace ai_keyboard {

// Wi-Fi sound synchronization is a required transport service, while startup
// playback is an optional consumer of the same Store and internal heap. Keep
// the service admission decision independent from the playback state machine
// so an incomplete or failed boot sound can never suppress Wi-Fi discovery.
enum class SpeakerServiceStartupAction : std::uint8_t {
  WaitForLocal,
  StartWifi,
  WifiUnavailable,
  Ready,
};

struct SpeakerServiceStartupInputs {
  bool local_ready = false;
  bool audio_ready = false;
  bool wifi_ready = false;
};

SpeakerServiceStartupAction evaluate_speaker_service_startup(
    const SpeakerServiceStartupInputs& inputs);

bool speaker_boot_pipeline_allowed(
    const SpeakerServiceStartupInputs& inputs);

}  // namespace ai_keyboard
