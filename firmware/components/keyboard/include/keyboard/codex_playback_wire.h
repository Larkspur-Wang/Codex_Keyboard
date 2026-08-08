#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace easy_codex {

constexpr std::uint8_t kPlaybackWireVersion = 1U;
constexpr std::size_t kPlaybackAuthTagBytes = 16U;
constexpr std::size_t kPlaybackRequestBytes = 40U;
constexpr std::size_t kPlaybackBeginBytes = 72U;
constexpr std::size_t kPlaybackAckBytes = 52U;
constexpr std::size_t kPlaybackFinishedBytes = 56U;
constexpr std::size_t kPlaybackFinishedAckBytes = 48U;
constexpr std::size_t kPlaybackDataHeaderBytes = 40U;
constexpr std::size_t kPlaybackChunkBytes = 1024U;
constexpr std::size_t kPlaybackMaximumEiadBytes = 4U * 1024U * 1024U;

struct PlaybackWireRequest {
  std::uint8_t slot = 0U;
  std::uint32_t request_generation = 0U;
  std::uint32_t connection_generation = 0U;
  std::uint64_t nonce = 0U;
};

struct PlaybackWireIdentity {
  std::uint8_t slot = 0U;
  std::uint32_t request_generation = 0U;
  std::uint32_t connection_generation = 0U;
  std::uint64_t summary_generation = 0U;
  std::uint64_t lease = 0U;
};

struct PlaybackWireBegin {
  PlaybackWireIdentity identity{};
  std::uint32_t total_bytes = 0U;
  std::uint64_t total_samples = 0U;
  std::uint16_t chunk_bytes = 0U;
  std::uint64_t request_nonce = 0U;
};

struct PlaybackWireData {
  PlaybackWireIdentity identity{};
  std::uint32_t offset = 0U;
  const std::uint8_t* payload = nullptr;
  std::uint16_t payload_length = 0U;
};

struct PlaybackWireAck {
  PlaybackWireIdentity identity{};
  std::uint8_t status = 0U;
  std::uint32_t next_offset = 0U;
};

struct PlaybackWireFinished {
  PlaybackWireIdentity identity{};
  std::uint64_t played_samples = 0U;
};

bool playback_wire_identity_valid(const PlaybackWireIdentity& identity);
bool playback_wire_identity_equal(const PlaybackWireIdentity& first,
                                  const PlaybackWireIdentity& second);
bool playback_data_matches_received_prefix(const PlaybackWireData& data,
                                           const std::uint8_t* received,
                                           std::size_t received_bytes);

bool encode_playback_request(
    const PlaybackWireRequest& request,
    const std::array<std::uint8_t, 32>& key,
    std::uint8_t* output,
    std::size_t output_size);
bool decode_playback_request(
    const std::uint8_t* packet,
    std::size_t packet_size,
    const std::array<std::uint8_t, 32>& key,
    PlaybackWireRequest* request);
bool decode_playback_begin(
    const std::uint8_t* packet,
    std::size_t packet_size,
    const std::array<std::uint8_t, 32>& key,
    PlaybackWireBegin* begin);
bool decode_playback_data(
    const std::uint8_t* packet,
    std::size_t packet_size,
    std::uint64_t request_nonce,
    const std::array<std::uint8_t, 32>& key,
    std::uint8_t* plaintext,
    std::size_t plaintext_size,
    PlaybackWireData* data);
bool encode_playback_ack(
    const PlaybackWireAck& ack,
    const std::array<std::uint8_t, 32>& key,
    std::uint8_t* output,
    std::size_t output_size);
bool encode_playback_finished(
    const PlaybackWireFinished& finished,
    const std::array<std::uint8_t, 32>& key,
    std::uint8_t* output,
    std::size_t output_size);
bool decode_playback_finished_ack(
    const std::uint8_t* packet,
    std::size_t packet_size,
    const std::array<std::uint8_t, 32>& key,
    PlaybackWireIdentity* identity,
    std::uint8_t* status);

}  // namespace easy_codex
