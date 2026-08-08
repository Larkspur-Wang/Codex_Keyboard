#include <array>
#include <cassert>
#include <cstring>

#include "keyboard/audio_control_wire.h"

using ai_keyboard::AudioControlAckStatus;
using ai_keyboard::AudioControlAction;
using ai_keyboard::HeartbeatFlags;

void heartbeat_encodes_magic_flags_session_and_sequence() {
  std::array<std::uint8_t, ai_keyboard::kHeartbeatPacketBytes> buffer{};
  HeartbeatFlags flags;
  flags.streaming = true;
  flags.audio_ready = true;
  const auto written =
      ai_keyboard::encode_heartbeat(buffer.data(), flags, 0x1122334455667788ULL, 0xA1B2C3D4U);
  assert(written == ai_keyboard::kHeartbeatPacketBytes);
  assert(std::memcmp(buffer.data(), "EIHB", 4) == 0);
  assert(buffer[4] == ai_keyboard::kAudioControlWireVersion);
  assert(buffer[5] == 0x03);
  assert(buffer[6] == 0 && buffer[7] == 0);
  assert(buffer[8] == 0x88 && buffer[15] == 0x11);
  assert(buffer[16] == 0xD4 && buffer[19] == 0xA1);
}

void heartbeat_idle_flags_are_zero() {
  std::array<std::uint8_t, ai_keyboard::kHeartbeatPacketBytes> buffer{};
  ai_keyboard::encode_heartbeat(buffer.data(), HeartbeatFlags{}, 0, 7);
  assert(buffer[5] == 0x00);
}

void control_roundtrip_via_manual_encoding() {
  std::array<std::uint8_t, ai_keyboard::kControlPacketBytes> buffer{};
  std::memcpy(buffer.data(), "EICC", 4);
  buffer[4] = ai_keyboard::kAudioControlWireVersion;
  buffer[5] = static_cast<std::uint8_t>(AudioControlAction::Start);
  buffer[8] = 0x2A;  // session_id = 42
  buffer[16] = 0x05;  // sequence = 5
  buffer[20] = 0xEE;  // token 首字节
  const auto command = ai_keyboard::parse_audio_control(buffer.data(), buffer.size());
  assert(command.has_value());
  assert(command->action == AudioControlAction::Start);
  assert(command->session_id == 42);
  assert(command->sequence == 5);
  assert(command->token[0] == 0xEE);
  assert(command->token[1] == 0x00);
}

void control_rejects_bad_magic_version_action_and_length() {
  std::array<std::uint8_t, ai_keyboard::kControlPacketBytes> buffer{};
  std::memcpy(buffer.data(), "EICC", 4);
  buffer[4] = ai_keyboard::kAudioControlWireVersion;
  buffer[5] = static_cast<std::uint8_t>(AudioControlAction::Stop);

  assert(ai_keyboard::parse_audio_control(buffer.data(), buffer.size() - 1) == std::nullopt);

  auto bad_magic = buffer;
  bad_magic[0] = 'X';
  assert(ai_keyboard::parse_audio_control(bad_magic.data(), bad_magic.size()) == std::nullopt);

  auto bad_version = buffer;
  bad_version[4] = 9;
  assert(ai_keyboard::parse_audio_control(bad_version.data(), bad_version.size()) == std::nullopt);

  auto bad_action = buffer;
  bad_action[5] = 0x77;
  assert(ai_keyboard::parse_audio_control(bad_action.data(), bad_action.size()) == std::nullopt);

  assert(ai_keyboard::parse_audio_control(nullptr, buffer.size()) == std::nullopt);
}

void control_accepts_trailing_bytes_from_newer_clients() {
  std::array<std::uint8_t, ai_keyboard::kControlPacketBytes + 8> buffer{};
  std::memcpy(buffer.data(), "EICC", 4);
  buffer[4] = ai_keyboard::kAudioControlWireVersion;
  buffer[5] = static_cast<std::uint8_t>(AudioControlAction::Keepalive);
  const auto command = ai_keyboard::parse_audio_control(buffer.data(), buffer.size());
  assert(command.has_value());
  assert(command->action == AudioControlAction::Keepalive);
}

void ack_encodes_action_status_session_and_sequence() {
  std::array<std::uint8_t, ai_keyboard::kControlAckPacketBytes> buffer{};
  const auto written = ai_keyboard::encode_control_ack(buffer.data(),
                                                       AudioControlAction::Stop,
                                                       AudioControlAckStatus::Unavailable,
                                                       42,
                                                       9);
  assert(written == ai_keyboard::kControlAckPacketBytes);
  assert(std::memcmp(buffer.data(), "EICA", 4) == 0);
  assert(buffer[4] == ai_keyboard::kAudioControlWireVersion);
  assert(buffer[5] == static_cast<std::uint8_t>(AudioControlAction::Stop));
  assert(buffer[6] == static_cast<std::uint8_t>(AudioControlAckStatus::Unavailable));
  assert(buffer[8] == 42);
  assert(buffer[16] == 9);
}

int main() {
  heartbeat_encodes_magic_flags_session_and_sequence();
  heartbeat_idle_flags_are_zero();
  control_roundtrip_via_manual_encoding();
  control_rejects_bad_magic_version_action_and_length();
  control_accepts_trailing_bytes_from_newer_clients();
  ack_encodes_action_status_session_and_sequence();
  return 0;
}
