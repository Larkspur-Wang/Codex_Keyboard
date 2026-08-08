#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ai_keyboard {

inline constexpr std::uint8_t kAudioPacketVersion = 3;
inline constexpr std::size_t kAudioPacketHeaderBytes = 32;
inline constexpr std::size_t kAudioPacketAuthTagBytes = 16;
inline constexpr std::uint8_t kAudioCodecPcmS16Le = 1;
inline constexpr std::uint8_t kAudioChannelsMono = 1;

struct AudioPacketMetadata {
  std::uint64_t session_id = 0;
  std::uint32_t capture_sequence = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t capture_timestamp_ms = 0;
  std::uint16_t frame_samples = 0;
  std::uint16_t payload_bytes = 0;
};

struct AudioEndMetadata {
  std::uint64_t session_id = 0;
  std::uint32_t final_sequence = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t capture_timestamp_ms = 0;
};

inline void write_audio_le16(std::uint8_t* destination, std::uint16_t value) {
  destination[0] = static_cast<std::uint8_t>(value & 0xFF);
  destination[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

inline void write_audio_le32(std::uint8_t* destination, std::uint32_t value) {
  destination[0] = static_cast<std::uint8_t>(value & 0xFF);
  destination[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  destination[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  destination[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

inline void write_audio_le64(std::uint8_t* destination, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    destination[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
  }
}

inline bool encode_audio_packet_header(std::uint8_t* packet,
                                       std::size_t packet_size,
                                       const AudioPacketMetadata& metadata) {
  if (packet == nullptr || packet_size < kAudioPacketHeaderBytes ||
      metadata.session_id == 0 || metadata.sample_rate == 0 ||
      metadata.frame_samples == 0 || metadata.payload_bytes == 0) {
    return false;
  }

  std::memset(packet, 0, kAudioPacketHeaderBytes);
  std::memcpy(packet, "EIAU", 4);
  packet[4] = kAudioPacketVersion;
  packet[5] = static_cast<std::uint8_t>(kAudioPacketHeaderBytes);
  packet[6] = kAudioCodecPcmS16Le;
  packet[7] = kAudioChannelsMono;
  write_audio_le64(packet + 8, metadata.session_id);
  write_audio_le32(packet + 16, metadata.capture_sequence);
  write_audio_le32(packet + 20, metadata.sample_rate);
  write_audio_le32(packet + 24, metadata.capture_timestamp_ms);
  write_audio_le16(packet + 28, metadata.frame_samples);
  write_audio_le16(packet + 30, metadata.payload_bytes);
  return true;
}

inline bool encode_audio_end_header(std::uint8_t* packet,
                                    std::size_t packet_size,
                                    const AudioEndMetadata& metadata) {
  if (packet == nullptr || packet_size < kAudioPacketHeaderBytes ||
      metadata.session_id == 0 || metadata.sample_rate == 0) {
    return false;
  }
  std::memset(packet, 0, kAudioPacketHeaderBytes);
  std::memcpy(packet, "EIAE", 4);
  packet[4] = kAudioPacketVersion;
  packet[5] = static_cast<std::uint8_t>(kAudioPacketHeaderBytes);
  packet[6] = 1U;
  write_audio_le64(packet + 8, metadata.session_id);
  write_audio_le32(packet + 16, metadata.final_sequence);
  write_audio_le32(packet + 20, metadata.sample_rate);
  write_audio_le32(packet + 24, metadata.capture_timestamp_ms);
  return true;
}

}  // namespace ai_keyboard
