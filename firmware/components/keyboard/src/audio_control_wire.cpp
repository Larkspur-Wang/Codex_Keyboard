#include "keyboard/audio_control_wire.h"

#include <cstring>

namespace ai_keyboard {
namespace {

constexpr std::array<std::uint8_t, 4> kHeartbeatMagic{{'E', 'I', 'H', 'B'}};
constexpr std::array<std::uint8_t, 4> kControlMagic{{'E', 'I', 'C', 'C'}};
constexpr std::array<std::uint8_t, 4> kControlAckMagic{{'E', 'I', 'C', 'A'}};

void write_le32(std::uint8_t* destination, std::uint32_t value) {
  destination[0] = static_cast<std::uint8_t>(value & 0xFF);
  destination[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  destination[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  destination[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

void write_le64(std::uint8_t* destination, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    destination[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
  }
}

std::uint32_t read_le32(const std::uint8_t* source) {
  return static_cast<std::uint32_t>(source[0]) |
         (static_cast<std::uint32_t>(source[1]) << 8) |
         (static_cast<std::uint32_t>(source[2]) << 16) |
         (static_cast<std::uint32_t>(source[3]) << 24);
}

std::uint64_t read_le64(const std::uint8_t* source) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(source[index]) << (index * 8);
  }
  return value;
}

}  // namespace

std::size_t encode_heartbeat(std::uint8_t* buffer,
                             const HeartbeatFlags& flags,
                             std::uint64_t session_id,
                             std::uint32_t sequence) {
  std::memcpy(buffer, kHeartbeatMagic.data(), kHeartbeatMagic.size());
  buffer[4] = kAudioControlWireVersion;
  std::uint8_t flag_bits = 0;
  if (flags.streaming) {
    flag_bits |= 0x01;
  }
  if (flags.audio_ready) {
    flag_bits |= 0x02;
  }
  buffer[5] = flag_bits;
  buffer[6] = 0;
  buffer[7] = 0;
  write_le64(buffer + 8, session_id);
  write_le32(buffer + 16, sequence);
  return kHeartbeatPacketBytes;
}

std::optional<AudioControlCommand> parse_audio_control(const std::uint8_t* buffer,
                                                       std::size_t length) {
  if (buffer == nullptr || length < kControlPacketBytes) {
    return std::nullopt;
  }
  if (std::memcmp(buffer, kControlMagic.data(), kControlMagic.size()) != 0) {
    return std::nullopt;
  }
  if (buffer[4] != kAudioControlWireVersion) {
    return std::nullopt;
  }
  const auto action = buffer[5];
  if (action != static_cast<std::uint8_t>(AudioControlAction::Start) &&
      action != static_cast<std::uint8_t>(AudioControlAction::Stop) &&
      action != static_cast<std::uint8_t>(AudioControlAction::Keepalive)) {
    return std::nullopt;
  }

  AudioControlCommand command;
  command.action = static_cast<AudioControlAction>(action);
  command.session_id = read_le64(buffer + 8);
  command.sequence = read_le32(buffer + 16);
  std::memcpy(command.token.data(), buffer + 20, kControlTokenBytes);
  return command;
}

std::size_t encode_control_ack(std::uint8_t* buffer,
                               AudioControlAction action,
                               AudioControlAckStatus status,
                               std::uint64_t session_id,
                               std::uint32_t sequence) {
  std::memcpy(buffer, kControlAckMagic.data(), kControlAckMagic.size());
  buffer[4] = kAudioControlWireVersion;
  buffer[5] = static_cast<std::uint8_t>(action);
  buffer[6] = static_cast<std::uint8_t>(status);
  buffer[7] = 0;
  write_le64(buffer + 8, session_id);
  write_le32(buffer + 16, sequence);
  return kControlAckPacketBytes;
}

}  // namespace ai_keyboard
