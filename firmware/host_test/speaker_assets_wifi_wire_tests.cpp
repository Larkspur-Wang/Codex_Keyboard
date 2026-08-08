#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/speaker_assets_wifi_wire.h"

namespace {

namespace speaker_assets = easy_input::speaker_assets;

using speaker_assets::SoundSha256Digest;
using speaker_assets::SpeakerAssetsWifiClientAuth;
using speaker_assets::SpeakerAssetsWifiDiscovery;
using speaker_assets::SpeakerAssetsWifiReadyStatus;
using speaker_assets::SpeakerAssetsWifiRecord;
using speaker_assets::SpeakerAssetsWifiRecordKind;

constexpr std::array<std::uint8_t, 32> kRootKey{{
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
}};

constexpr std::array<std::uint8_t, 16> kDeviceId{{
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
}};

constexpr std::array<std::uint8_t, 16> kEndpointNonce{{
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
}};

constexpr std::array<std::uint8_t, 16> kClientNonce{{
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
}};

constexpr std::array<std::uint8_t, 80> kDiscoveryGolden{{
    0x45, 0x49, 0x48, 0x42, 0x01, 0x03, 0x00, 0x00,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    0xD4, 0xC3, 0xB2, 0xA1, 0x45, 0x49, 0x53, 0x44,
    0x01, 0x3C, 0x07, 0x00, 0xB6, 0x43, 0x34, 0x12,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0xC3, 0xD1, 0x08, 0x4E, 0x90, 0x6D, 0x3E, 0x20,
    0x9C, 0x0F, 0x4F, 0x05, 0x92, 0xDB, 0x39, 0xE1,
}};

constexpr std::array<std::uint8_t, 40> kClientAuthGolden{{
    0x45, 0x49, 0x53, 0x41, 0x01, 0x28, 0x34, 0x12,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x41, 0x76, 0x27, 0x23, 0xFF, 0x12, 0xFC, 0x28,
    0x73, 0x1A, 0x32, 0xBB, 0xB2, 0x32, 0xA1, 0x29,
}};

constexpr std::array<std::array<std::uint8_t, 32>, 5>
    kServerReadyGolden{{
        {{
            0x45, 0x49, 0x53, 0x52, 0x01, 0x00, 0x00, 0x00,
            0x12, 0x34, 0x56, 0x78, 0x44, 0x33, 0x22, 0x11,
            0xE0, 0x28, 0x8A, 0xCC, 0x15, 0xBD, 0x71, 0x98,
            0xAE, 0xF8, 0x5F, 0x42, 0x29, 0x97, 0x77, 0x8E,
        }},
        {{
            0x45, 0x49, 0x53, 0x52, 0x01, 0x01, 0x00, 0x00,
            0x12, 0x34, 0x56, 0x78, 0x44, 0x33, 0x22, 0x11,
            0xFB, 0x41, 0xF9, 0x45, 0xAB, 0x05, 0x5B, 0x0C,
            0x97, 0xC6, 0x16, 0x9E, 0x4A, 0xC3, 0xF4, 0xA6,
        }},
        {{
            0x45, 0x49, 0x53, 0x52, 0x01, 0x02, 0x00, 0x00,
            0x12, 0x34, 0x56, 0x78, 0x44, 0x33, 0x22, 0x11,
            0x44, 0xB6, 0x2D, 0xB5, 0x61, 0xDF, 0x70, 0x3B,
            0x5A, 0x0C, 0x67, 0x20, 0x07, 0x06, 0x9C, 0xE6,
        }},
        {{
            0x45, 0x49, 0x53, 0x52, 0x01, 0x03, 0x00, 0x00,
            0x12, 0x34, 0x56, 0x78, 0x44, 0x33, 0x22, 0x11,
            0x3F, 0x3A, 0x8F, 0xF6, 0x31, 0xFA, 0xD9, 0x5B,
            0xC7, 0xB4, 0x17, 0xEF, 0xE9, 0xF1, 0xC6, 0x0F,
        }},
        {{
            0x45, 0x49, 0x53, 0x52, 0x01, 0x04, 0x00, 0x00,
            0x12, 0x34, 0x56, 0x78, 0x44, 0x33, 0x22, 0x11,
            0x60, 0xA5, 0x83, 0xF4, 0x3F, 0x2C, 0x02, 0x34,
            0x0B, 0x9B, 0x27, 0x5C, 0xAA, 0x65, 0xA1, 0x9A,
        }},
    }};

constexpr SoundSha256Digest kSessionKeyGolden{{
    0x5B, 0xD9, 0x5C, 0x2A, 0xC6, 0xF8, 0x2F, 0x86,
    0x27, 0x5F, 0x49, 0x48, 0x98, 0x5B, 0xF6, 0xA4,
    0x71, 0x48, 0xAB, 0xED, 0x42, 0x82, 0x7F, 0xB2,
    0x98, 0xCD, 0xFF, 0xC5, 0x5F, 0x11, 0x66, 0x25,
}};

constexpr std::array<std::uint8_t, 59> kRequestRecordGolden{{
    0x45, 0x49, 0x53, 0x46, 0x01, 0x01, 0x1B, 0x00,
    0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00,
    0x05, 0xBA, 0x77, 0xE5, 0x4B, 0xDF, 0x83, 0xA2,
    0x62, 0x8B, 0x9A, 0x55, 0xCF, 0x56, 0x4F, 0xB0,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5A,
}};

constexpr std::array<std::uint8_t, 59> kResponseRecordGolden{{
    0x45, 0x49, 0x53, 0x46, 0x01, 0x02, 0x1B, 0x00,
    0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00,
    0x1F, 0xF5, 0xBC, 0x25, 0xD7, 0x6B, 0xB4, 0x58,
    0x80, 0xF5, 0x82, 0x4B, 0x3C, 0xA4, 0xE8, 0xEA,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5A,
}};

std::array<std::uint8_t, 80> make_base_heartbeat() {
  std::array<std::uint8_t, 80> heartbeat{};
  heartbeat[0] = 'E';
  heartbeat[1] = 'I';
  heartbeat[2] = 'H';
  heartbeat[3] = 'B';
  heartbeat[4] = 1U;
  heartbeat[5] = 3U;
  constexpr std::array<std::uint8_t, 8> kSession{{
      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
  }};
  std::copy(kSession.begin(), kSession.end(), heartbeat.begin() + 8U);
  constexpr std::array<std::uint8_t, 4> kSequence{{
      0xD4, 0xC3, 0xB2, 0xA1,
  }};
  std::copy(
      kSequence.begin(), kSequence.end(), heartbeat.begin() + 16U);
  return heartbeat;
}

SpeakerAssetsWifiDiscovery make_discovery() {
  SpeakerAssetsWifiDiscovery discovery{};
  discovery.flags =
      speaker_assets::kSpeakerAssetsWifiDiscoveryReady |
      speaker_assets::kSpeakerAssetsWifiDiscoveryKeyProvisioned |
      speaker_assets::kSpeakerAssetsWifiDiscoveryAssetsReady;
  discovery.port = 17334U;
  discovery.key_epoch = 0x1234U;
  discovery.device_id = kDeviceId;
  discovery.endpoint_nonce = kEndpointNonce;
  return discovery;
}

SpeakerAssetsWifiRecord make_record(
    SpeakerAssetsWifiRecordKind kind) {
  SpeakerAssetsWifiRecord record{};
  record.kind = kind;
  record.sequence = 0x78563412U;
  record.payload_length = 27U;
  for (std::size_t index = 0U;
       index < record.payload_length;
       ++index) {
    record.payload[index] =
        static_cast<std::uint8_t>(0x40U + index);
  }
  return record;
}

void constants_and_status_values_are_wire_stable() {
  static_assert(speaker_assets::kSpeakerAssetsWifiTcpPort == 17334U);
  static_assert(speaker_assets::kSpeakerAssetsWifiKeyBytes == 32U);
  static_assert(speaker_assets::kSpeakerAssetsWifiIdentityBytes == 16U);
  static_assert(speaker_assets::kSpeakerAssetsWifiTagBytes == 16U);
  static_assert(speaker_assets::kSpeakerAssetsWifiDiscoveryBytes == 80U);
  static_assert(speaker_assets::kSpeakerAssetsWifiClientAuthBytes == 40U);
  static_assert(speaker_assets::kSpeakerAssetsWifiServerReadyBytes == 32U);
  static_assert(speaker_assets::kSpeakerAssetsWifiRecordHeaderBytes == 32U);
  static_assert(speaker_assets::kSpeakerAssetsWifiRecordMaxBytes == 157U);
  static_assert(speaker_assets::kSpeakerAssetsWifiWireVersion == 1U);
  static_assert(speaker_assets::kSpeakerAssetsWifiDiscoveryLength == 60U);
  static_assert(
      static_cast<std::uint8_t>(SpeakerAssetsWifiReadyStatus::Ok) == 0U);
  static_assert(
      static_cast<std::uint8_t>(
          SpeakerAssetsWifiReadyStatus::BadAuth) == 1U);
  static_assert(
      static_cast<std::uint8_t>(
          SpeakerAssetsWifiReadyStatus::Busy) == 2U);
  static_assert(
      static_cast<std::uint8_t>(
          SpeakerAssetsWifiReadyStatus::NotReady) == 3U);
  static_assert(
      static_cast<std::uint8_t>(
          SpeakerAssetsWifiReadyStatus::WrongEpoch) == 4U);
}

void discovery_matches_independent_golden_vector() {
  auto heartbeat = make_base_heartbeat();
  assert(speaker_assets::encode_speaker_assets_wifi_discovery(
      heartbeat.data(),
      heartbeat.size(),
      make_discovery(),
      kRootKey,
      true));
  assert(heartbeat == kDiscoveryGolden);

  auto unsigned_heartbeat = make_base_heartbeat();
  const std::array<std::uint8_t, 32> zero_key{};
  assert(speaker_assets::encode_speaker_assets_wifi_discovery(
      unsigned_heartbeat.data(),
      unsigned_heartbeat.size(),
      make_discovery(),
      zero_key,
      false));
  assert(std::equal(
      unsigned_heartbeat.begin(),
      unsigned_heartbeat.begin() + 64U,
      kDiscoveryGolden.begin()));
  assert(std::all_of(
      unsigned_heartbeat.begin() + 64U,
      unsigned_heartbeat.end(),
      [](std::uint8_t value) { return value == 0U; }));

  assert(!speaker_assets::encode_speaker_assets_wifi_discovery(
      nullptr, heartbeat.size(), make_discovery(), kRootKey, true));
  assert(!speaker_assets::encode_speaker_assets_wifi_discovery(
      heartbeat.data(),
      heartbeat.size() - 1U,
      make_discovery(),
      kRootKey,
      true));
  assert(!speaker_assets::encode_speaker_assets_wifi_discovery(
      heartbeat.data(),
      heartbeat.size(),
      make_discovery(),
      zero_key,
      true));
}

void client_auth_matches_golden_and_rejects_every_bit_flip() {
  SpeakerAssetsWifiClientAuth auth{};
  assert(speaker_assets::parse_speaker_assets_wifi_client_auth(
      kClientAuthGolden.data(),
      kClientAuthGolden.size(),
      kRootKey,
      kDeviceId,
      kEndpointNonce,
      &auth));
  assert(auth.key_epoch == 0x1234U);
  assert(auth.client_nonce == kClientNonce);

  for (std::size_t byte = 0U;
       byte < kClientAuthGolden.size();
       ++byte) {
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      auto corrupted = kClientAuthGolden;
      corrupted[byte] ^=
          static_cast<std::uint8_t>(1U << bit);
      SpeakerAssetsWifiClientAuth unchanged{};
      unchanged.key_epoch = 0xBEEFU;
      unchanged.client_nonce.fill(0xA5U);
      assert(!speaker_assets::parse_speaker_assets_wifi_client_auth(
          corrupted.data(),
          corrupted.size(),
          kRootKey,
          kDeviceId,
          kEndpointNonce,
          &unchanged));
      assert(unchanged.key_epoch == 0xBEEFU);
      assert(std::all_of(
          unchanged.client_nonce.begin(),
          unchanged.client_nonce.end(),
          [](std::uint8_t value) { return value == 0xA5U; }));
    }
  }

  auto wrong_device = kDeviceId;
  wrong_device[0] ^= 1U;
  assert(!speaker_assets::parse_speaker_assets_wifi_client_auth(
      kClientAuthGolden.data(),
      kClientAuthGolden.size(),
      kRootKey,
      wrong_device,
      kEndpointNonce,
      &auth));
  assert(!speaker_assets::parse_speaker_assets_wifi_client_auth(
      kClientAuthGolden.data(),
      kClientAuthGolden.size() - 1U,
      kRootKey,
      kDeviceId,
      kEndpointNonce,
      &auth));
  assert(!speaker_assets::parse_speaker_assets_wifi_client_auth(
      kClientAuthGolden.data(),
      kClientAuthGolden.size(),
      {},
      kDeviceId,
      kEndpointNonce,
      &auth));
}

void server_ready_statuses_match_independent_golden_vectors() {
  constexpr std::array<SpeakerAssetsWifiReadyStatus, 5> kStatuses{{
      SpeakerAssetsWifiReadyStatus::Ok,
      SpeakerAssetsWifiReadyStatus::BadAuth,
      SpeakerAssetsWifiReadyStatus::Busy,
      SpeakerAssetsWifiReadyStatus::NotReady,
      SpeakerAssetsWifiReadyStatus::WrongEpoch,
  }};
  for (std::size_t index = 0U; index < kStatuses.size(); ++index) {
    std::array<std::uint8_t, 32> encoded{};
    assert(speaker_assets::encode_speaker_assets_wifi_server_ready(
        &encoded,
        kStatuses[index],
        0x78563412U,
        0x11223344U,
        kRootKey,
        kDeviceId,
        kEndpointNonce,
        kClientNonce));
    assert(encoded == kServerReadyGolden[index]);
  }

  std::array<std::uint8_t, 32> encoded{};
  assert(!speaker_assets::encode_speaker_assets_wifi_server_ready(
      nullptr,
      SpeakerAssetsWifiReadyStatus::Ok,
      1U,
      1U,
      kRootKey,
      kDeviceId,
      kEndpointNonce,
      kClientNonce));
  assert(!speaker_assets::encode_speaker_assets_wifi_server_ready(
      &encoded,
      SpeakerAssetsWifiReadyStatus::Ok,
      1U,
      1U,
      {},
      kDeviceId,
      kEndpointNonce,
      kClientNonce));
}

void session_key_matches_independent_golden_vector() {
  assert(speaker_assets::derive_speaker_assets_wifi_session_key(
             kRootKey,
             kDeviceId,
             kEndpointNonce,
             kClientNonce) == kSessionKeyGolden);
  assert(speaker_assets::derive_speaker_assets_wifi_session_key(
             {},
             kDeviceId,
             kEndpointNonce,
             kClientNonce) == SoundSha256Digest{});
}

void records_match_golden_round_trip_and_detect_tampering() {
  const std::array expected_records{
      kRequestRecordGolden, kResponseRecordGolden};
  constexpr std::array kinds{
      SpeakerAssetsWifiRecordKind::Request,
      SpeakerAssetsWifiRecordKind::Response};
  for (std::size_t vector_index = 0U;
       vector_index < kinds.size();
       ++vector_index) {
    const auto source = make_record(kinds[vector_index]);
    std::array<
        std::uint8_t,
        speaker_assets::kSpeakerAssetsWifiRecordMaxBytes> encoded{};
    std::size_t encoded_length = 0U;
    assert(speaker_assets::encode_speaker_assets_wifi_record(
        source, kSessionKeyGolden, &encoded, &encoded_length));
    assert(encoded_length == expected_records[vector_index].size());
    assert(std::equal(
        encoded.begin(),
        encoded.begin() + encoded_length,
        expected_records[vector_index].begin()));
    assert(std::all_of(
        encoded.begin() + encoded_length,
        encoded.end(),
        [](std::uint8_t value) { return value == 0U; }));

    SpeakerAssetsWifiRecord decoded{};
    assert(speaker_assets::decode_speaker_assets_wifi_record(
        encoded.data(),
        encoded_length,
        kSessionKeyGolden,
        kinds[vector_index],
        &decoded));
    assert(decoded.kind == kinds[vector_index]);
    assert(decoded.sequence == source.sequence);
    assert(decoded.payload_length == source.payload_length);
    assert(decoded.payload == source.payload);

    const auto other_kind =
        kinds[vector_index] == SpeakerAssetsWifiRecordKind::Request
            ? SpeakerAssetsWifiRecordKind::Response
            : SpeakerAssetsWifiRecordKind::Request;
    assert(!speaker_assets::decode_speaker_assets_wifi_record(
        encoded.data(),
        encoded_length,
        kSessionKeyGolden,
        other_kind,
        &decoded));

    for (std::size_t byte = 0U;
         byte < encoded_length;
         ++byte) {
      for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
        auto corrupted = encoded;
        corrupted[byte] ^=
            static_cast<std::uint8_t>(1U << bit);
        assert(!speaker_assets::decode_speaker_assets_wifi_record(
            corrupted.data(),
            encoded_length,
            kSessionKeyGolden,
            kinds[vector_index],
            &decoded));
      }
    }
  }
}

void record_boundaries_and_invalid_arguments_fail_closed() {
  std::array<
      std::uint8_t,
      speaker_assets::kSpeakerAssetsWifiRecordMaxBytes> encoded{};
  std::size_t encoded_length = 0U;
  auto record = make_record(SpeakerAssetsWifiRecordKind::Request);

  record.sequence = 0U;
  assert(!speaker_assets::encode_speaker_assets_wifi_record(
      record, kSessionKeyGolden, &encoded, &encoded_length));
  record.sequence = 1U;
  record.payload_length =
      speaker_assets::kSpeakerAssetsFrameHeaderBytes - 1U;
  assert(!speaker_assets::encode_speaker_assets_wifi_record(
      record, kSessionKeyGolden, &encoded, &encoded_length));
  record.payload_length =
      speaker_assets::kSpeakerAssetsWifiFrameMaxBytes + 1U;
  assert(!speaker_assets::encode_speaker_assets_wifi_record(
      record, kSessionKeyGolden, &encoded, &encoded_length));
  record.payload_length =
      speaker_assets::kSpeakerAssetsFrameHeaderBytes;
  record.kind = static_cast<SpeakerAssetsWifiRecordKind>(0x7FU);
  assert(!speaker_assets::encode_speaker_assets_wifi_record(
      record, kSessionKeyGolden, &encoded, &encoded_length));

  record.kind = SpeakerAssetsWifiRecordKind::Request;
  record.payload_length =
      speaker_assets::kSpeakerAssetsWifiFrameMaxBytes;
  record.sequence = 0xFFFFFFFFU;
  for (std::size_t index = 0U;
       index < record.payload_length;
       ++index) {
    record.payload[index] =
        static_cast<std::uint8_t>(index);
  }
  assert(speaker_assets::encode_speaker_assets_wifi_record(
      record, kSessionKeyGolden, &encoded, &encoded_length));
  assert(encoded_length ==
         speaker_assets::kSpeakerAssetsWifiRecordMaxBytes);

  SpeakerAssetsWifiRecord decoded{};
  assert(speaker_assets::decode_speaker_assets_wifi_record(
      encoded.data(),
      encoded_length,
      kSessionKeyGolden,
      SpeakerAssetsWifiRecordKind::Request,
      &decoded));
  assert(decoded.sequence == 0xFFFFFFFFU);
  assert(decoded.payload_length ==
         speaker_assets::kSpeakerAssetsWifiFrameMaxBytes);

  assert(!speaker_assets::decode_speaker_assets_wifi_record(
      encoded.data(),
      encoded_length - 1U,
      kSessionKeyGolden,
      SpeakerAssetsWifiRecordKind::Request,
      &decoded));
  assert(!speaker_assets::decode_speaker_assets_wifi_record(
      encoded.data(),
      encoded_length,
      {},
      SpeakerAssetsWifiRecordKind::Request,
      &decoded));
  assert(!speaker_assets::decode_speaker_assets_wifi_record(
      encoded.data(),
      encoded_length,
      kSessionKeyGolden,
      static_cast<SpeakerAssetsWifiRecordKind>(0x7FU),
      &decoded));
}

}  // namespace

int main() {
  constants_and_status_values_are_wire_stable();
  discovery_matches_independent_golden_vector();
  client_auth_matches_golden_and_rejects_every_bit_flip();
  server_ready_statuses_match_independent_golden_vectors();
  session_key_matches_independent_golden_vector();
  records_match_golden_round_trip_and_detect_tampering();
  record_boundaries_and_invalid_arguments_fail_closed();
  return 0;
}
