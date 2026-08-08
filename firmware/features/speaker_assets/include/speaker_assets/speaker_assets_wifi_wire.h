#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/sound_asset_crypto.h"
#include "speaker_assets/speaker_assets_protocol.h"

namespace easy_input::speaker_assets {

// Wi-Fi speaker sync is a low-rate, authenticated stop-and-wait carrier for
// the existing EIA logical frames. It deliberately does not replace the
// microphone UDP control/audio path and never carries Flash work itself.
inline constexpr std::uint16_t kSpeakerAssetsWifiTcpPort = 17334U;
inline constexpr std::size_t kSpeakerAssetsWifiKeyBytes = 32U;
inline constexpr std::size_t kSpeakerAssetsWifiIdentityBytes = 16U;
inline constexpr std::size_t kSpeakerAssetsWifiTagBytes = 16U;
inline constexpr std::size_t kSpeakerAssetsWifiDiscoveryBytes = 80U;
inline constexpr std::size_t kSpeakerAssetsWifiClientAuthBytes = 40U;
inline constexpr std::size_t kSpeakerAssetsWifiServerReadyBytes = 32U;
inline constexpr std::size_t kSpeakerAssetsWifiRecordHeaderBytes = 32U;
inline constexpr std::size_t kSpeakerAssetsWifiRecordMaxBytes =
    kSpeakerAssetsWifiRecordHeaderBytes +
    kSpeakerAssetsWifiFrameMaxBytes;

inline constexpr std::uint8_t kSpeakerAssetsWifiWireVersion = 1U;
inline constexpr std::uint8_t kSpeakerAssetsWifiDiscoveryLength = 60U;

inline constexpr std::uint16_t kSpeakerAssetsWifiDiscoveryReady = 0x0001U;
inline constexpr std::uint16_t
    kSpeakerAssetsWifiDiscoveryKeyProvisioned = 0x0002U;
inline constexpr std::uint16_t
    kSpeakerAssetsWifiDiscoveryAssetsReady = 0x0004U;

enum class SpeakerAssetsWifiReadyStatus : std::uint8_t {
  Ok = 0U,
  BadAuth = 1U,
  Busy = 2U,
  NotReady = 3U,
  WrongEpoch = 4U,
};

enum class SpeakerAssetsWifiRecordKind : std::uint8_t {
  Request = 1U,
  Response = 2U,
};

struct SpeakerAssetsWifiDiscovery {
  std::uint16_t flags = 0U;
  std::uint16_t port = kSpeakerAssetsWifiTcpPort;
  std::uint16_t key_epoch = 0U;
  std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>
      device_id{};
  std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>
      endpoint_nonce{};
};

struct SpeakerAssetsWifiClientAuth {
  std::uint16_t key_epoch = 0U;
  std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>
      client_nonce{};
};

struct SpeakerAssetsWifiRecord {
  SpeakerAssetsWifiRecordKind kind =
      SpeakerAssetsWifiRecordKind::Request;
  std::uint32_t sequence = 0U;
  std::uint16_t payload_length = 0U;
  std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes>
      payload{};
};

// The first 20 bytes must already contain the ordinary EIHB v1 heartbeat.
// This function fills bytes 20..79 and authenticates bytes 0..63.
bool encode_speaker_assets_wifi_discovery(
    std::uint8_t* heartbeat,
    std::size_t heartbeat_capacity,
    const SpeakerAssetsWifiDiscovery& discovery,
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>& key,
    bool key_valid);

bool parse_speaker_assets_wifi_client_auth(
    const std::uint8_t* encoded,
    std::size_t length,
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>& key,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        device_id,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        endpoint_nonce,
    SpeakerAssetsWifiClientAuth* auth);

bool encode_speaker_assets_wifi_server_ready(
    std::array<std::uint8_t, kSpeakerAssetsWifiServerReadyBytes>*
        encoded,
    SpeakerAssetsWifiReadyStatus status,
    std::uint32_t route_id,
    std::uint32_t generation,
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>& key,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        device_id,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        endpoint_nonce,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        client_nonce);

SoundSha256Digest derive_speaker_assets_wifi_session_key(
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>& key,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        device_id,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        endpoint_nonce,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        client_nonce);

bool encode_speaker_assets_wifi_record(
    const SpeakerAssetsWifiRecord& record,
    const SoundSha256Digest& session_key,
    std::array<std::uint8_t, kSpeakerAssetsWifiRecordMaxBytes>*
        encoded,
    std::size_t* encoded_length);

bool decode_speaker_assets_wifi_record(
    const std::uint8_t* encoded,
    std::size_t length,
    const SoundSha256Digest& session_key,
    SpeakerAssetsWifiRecordKind expected_kind,
    SpeakerAssetsWifiRecord* record);

}  // namespace easy_input::speaker_assets
