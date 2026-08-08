#include "keyboard/audio_packet_wire.h"

#include <array>
#include <cassert>
#include <cstdint>

namespace {

std::uint16_t read_le16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_le64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

}  // namespace

int main() {
  std::array<std::uint8_t, ai_keyboard::kAudioPacketHeaderBytes> packet{};
  const ai_keyboard::AudioPacketMetadata metadata{
      .session_id = 0x0102030405060708ULL,
      .capture_sequence = 17,
      .sample_rate = 16000,
      .capture_timestamp_ms = 123456,
      .frame_samples = 320,
      .payload_bytes = 640,
  };

  assert(ai_keyboard::encode_audio_packet_header(packet.data(), packet.size(), metadata));
  assert(packet[0] == 'E' && packet[1] == 'I' && packet[2] == 'A' && packet[3] == 'U');
  assert(packet[4] == 3);
  assert(ai_keyboard::kAudioPacketAuthTagBytes == 16);
  assert(packet[5] == ai_keyboard::kAudioPacketHeaderBytes);
  assert(packet[6] == ai_keyboard::kAudioCodecPcmS16Le);
  assert(packet[7] == ai_keyboard::kAudioChannelsMono);
  assert(read_le64(packet.data() + 8) == metadata.session_id);
  assert(read_le32(packet.data() + 16) == metadata.capture_sequence);
  assert(read_le32(packet.data() + 20) == metadata.sample_rate);
  assert(read_le32(packet.data() + 24) == metadata.capture_timestamp_ms);
  assert(read_le16(packet.data() + 28) == metadata.frame_samples);
  assert(read_le16(packet.data() + 30) == metadata.payload_bytes);

  ai_keyboard::AudioPacketMetadata invalid{};
  assert(!ai_keyboard::encode_audio_packet_header(packet.data(), packet.size(), invalid));
  assert(!ai_keyboard::encode_audio_packet_header(nullptr, packet.size(), metadata));

  const ai_keyboard::AudioEndMetadata end{
      .session_id = metadata.session_id,
      .final_sequence = 18,
      .sample_rate = 16000,
      .capture_timestamp_ms = 123876,
  };
  assert(ai_keyboard::encode_audio_end_header(packet.data(), packet.size(), end));
  assert(packet[0] == 'E' && packet[1] == 'I' && packet[2] == 'A' && packet[3] == 'E');
  assert(packet[4] == 3);
  assert(packet[5] == ai_keyboard::kAudioPacketHeaderBytes);
  assert(packet[6] == 1 && packet[7] == 0);
  assert(read_le64(packet.data() + 8) == end.session_id);
  assert(read_le32(packet.data() + 16) == end.final_sequence);
  assert(read_le32(packet.data() + 20) == end.sample_rate);
  assert(read_le32(packet.data() + 24) == end.capture_timestamp_ms);
  assert(read_le16(packet.data() + 28) == 0);
  assert(read_le16(packet.data() + 30) == 0);
  return 0;
}
