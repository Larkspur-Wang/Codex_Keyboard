#pragma once

namespace ai_keyboard {

constexpr bool kAudioHardwareAvailable = true;
constexpr const char* kAudioHardwareProfile = "v2_i2s";
constexpr const char* kAudioTransport = "wifi_udp";
constexpr const char* kAudioCaptureStatus = "mic_ready";
constexpr const char* kAudioMicrophoneChannel = "right";
constexpr const char* kAudioSpeakerChannel = "left";
constexpr const char* kAudioUnavailableReason =
    "V2 MIC I2S right-channel capture streams PCM16 over Wi-Fi UDP; speaker output is pending";

}  // namespace ai_keyboard
