#include "speaker_assets/speaker_assets_wifi_wire.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace easy_input::speaker_assets {
namespace {

constexpr std::array<std::uint8_t, 4> kDiscoveryMagic{{
    'E', 'I', 'S', 'D'}};
constexpr std::array<std::uint8_t, 4> kClientAuthMagic{{
    'E', 'I', 'S', 'A'}};
constexpr std::array<std::uint8_t, 4> kServerReadyMagic{{
    'E', 'I', 'S', 'R'}};
constexpr std::array<std::uint8_t, 4> kRecordMagic{{
    'E', 'I', 'S', 'F'}};
constexpr std::array<std::uint8_t, 16> kDiscoveryContext{{
    'E', 'a', 's', 'y', 'I', 'n', 'p', 'u',
    't', '/', 'E', 'I', 'S', 'D', '/', 'v'}};
constexpr std::uint8_t kDiscoveryContextSuffix = '1';
constexpr std::array<std::uint8_t, 16> kClientAuthContext{{
    'E', 'a', 's', 'y', 'I', 'n', 'p', 'u',
    't', '/', 'E', 'I', 'S', 'A', '/', 'v'}};
constexpr std::uint8_t kClientAuthContextSuffix = '1';
constexpr std::array<std::uint8_t, 16> kServerReadyContext{{
    'E', 'a', 's', 'y', 'I', 'n', 'p', 'u',
    't', '/', 'E', 'I', 'S', 'R', '/', 'v'}};
constexpr std::uint8_t kServerReadyContextSuffix = '1';
constexpr std::array<std::uint8_t, 16> kSessionKeyContext{{
    'E', 'a', 's', 'y', 'I', 'n', 'p', 'u',
    't', '/', 'E', 'I', 'S', 'K', '/', 'v'}};
constexpr std::uint8_t kSessionKeyContextSuffix = '1';

void write_le16(std::uint8_t* destination, std::uint16_t value) {
  destination[0] = static_cast<std::uint8_t>(value);
  destination[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_le32(std::uint8_t* destination, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    destination[index] = static_cast<std::uint8_t>(
        value >> (index * 8U));
  }
}

std::uint16_t read_le16(const std::uint8_t* source) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(source[0]) |
      (static_cast<std::uint16_t>(source[1]) << 8U));
}

std::uint32_t read_le32(const std::uint8_t* source) {
  return static_cast<std::uint32_t>(source[0]) |
         (static_cast<std::uint32_t>(source[1]) << 8U) |
         (static_cast<std::uint32_t>(source[2]) << 16U) |
         (static_cast<std::uint32_t>(source[3]) << 24U);
}

bool constant_time_equal(const std::uint8_t* first,
                         const std::uint8_t* second,
                         std::size_t length) {
  if (first == nullptr || second == nullptr) {
    return false;
  }
  std::uint8_t difference = 0U;
  for (std::size_t index = 0U; index < length; ++index) {
    difference = static_cast<std::uint8_t>(
        difference | (first[index] ^ second[index]));
  }
  return difference == 0U;
}

class SoundHmacSha256 {
 public:
  explicit SoundHmacSha256(
      const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>&
          key) {
    std::array<std::uint8_t, 64> inner_pad{};
    std::array<std::uint8_t, 64> outer_pad{};
    inner_pad.fill(0x36U);
    outer_pad.fill(0x5CU);
    for (std::size_t index = 0U; index < key.size(); ++index) {
      inner_pad[index] ^= key[index];
      outer_pad[index] ^= key[index];
    }
    inner_.update(inner_pad.data(), inner_pad.size());
    outer_.update(outer_pad.data(), outer_pad.size());
  }

  bool update(const std::uint8_t* data, std::size_t length) {
    return inner_.update(data, length);
  }

  SoundSha256Digest finish() {
    const auto inner_digest = inner_.finish();
    if (!outer_.update(inner_digest.data(), inner_digest.size())) {
      return {};
    }
    return outer_.finish();
  }

 private:
  SoundSha256 inner_;
  SoundSha256 outer_;
};

template <std::size_t ContextBytes>
bool update_context(SoundHmacSha256* hmac,
                    const std::array<std::uint8_t, ContextBytes>&
                        context,
                    std::uint8_t suffix) {
  return hmac != nullptr &&
         hmac->update(context.data(), context.size()) &&
         hmac->update(&suffix, 1U);
}

bool key_is_valid(
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>&
        key) {
  return std::any_of(
      key.begin(), key.end(),
      [](std::uint8_t value) { return value != 0U; });
}

bool record_kind_is_valid(SpeakerAssetsWifiRecordKind kind) {
  return kind == SpeakerAssetsWifiRecordKind::Request ||
         kind == SpeakerAssetsWifiRecordKind::Response;
}

}  // namespace

bool encode_speaker_assets_wifi_discovery(
    std::uint8_t* heartbeat,
    std::size_t heartbeat_capacity,
    const SpeakerAssetsWifiDiscovery& discovery,
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>& key,
    bool key_valid) {
  if (heartbeat == nullptr ||
      heartbeat_capacity < kSpeakerAssetsWifiDiscoveryBytes ||
      (key_valid && !key_is_valid(key))) {
    return false;
  }
  std::copy(
      kDiscoveryMagic.begin(), kDiscoveryMagic.end(),
      heartbeat + 20U);
  heartbeat[24] = kSpeakerAssetsWifiWireVersion;
  heartbeat[25] = kSpeakerAssetsWifiDiscoveryLength;
  write_le16(heartbeat + 26U, discovery.flags);
  write_le16(heartbeat + 28U, discovery.port);
  write_le16(heartbeat + 30U, discovery.key_epoch);
  std::copy(
      discovery.device_id.begin(), discovery.device_id.end(),
      heartbeat + 32U);
  std::copy(
      discovery.endpoint_nonce.begin(),
      discovery.endpoint_nonce.end(),
      heartbeat + 48U);
  std::fill_n(
      heartbeat + 64U, kSpeakerAssetsWifiTagBytes, 0U);
  if (!key_valid) {
    return true;
  }
  SoundHmacSha256 hmac(key);
  if (!update_context(
          &hmac, kDiscoveryContext,
          kDiscoveryContextSuffix) ||
      !hmac.update(heartbeat, 64U)) {
    return false;
  }
  const auto digest = hmac.finish();
  std::copy_n(
      digest.begin(), kSpeakerAssetsWifiTagBytes,
      heartbeat + 64U);
  return true;
}

bool parse_speaker_assets_wifi_client_auth(
    const std::uint8_t* encoded,
    std::size_t length,
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>& key,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        device_id,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        endpoint_nonce,
    SpeakerAssetsWifiClientAuth* auth) {
  if (encoded == nullptr || auth == nullptr ||
      length != kSpeakerAssetsWifiClientAuthBytes ||
      !key_is_valid(key) ||
      !constant_time_equal(
          encoded, kClientAuthMagic.data(),
          kClientAuthMagic.size()) ||
      encoded[4] != kSpeakerAssetsWifiWireVersion ||
      encoded[5] != kSpeakerAssetsWifiClientAuthBytes) {
    return false;
  }
  SoundHmacSha256 hmac(key);
  if (!update_context(
          &hmac, kClientAuthContext,
          kClientAuthContextSuffix) ||
      !hmac.update(device_id.data(), device_id.size()) ||
      !hmac.update(
          endpoint_nonce.data(), endpoint_nonce.size()) ||
      !hmac.update(encoded, 24U)) {
    return false;
  }
  const auto digest = hmac.finish();
  if (!constant_time_equal(
          digest.data(), encoded + 24U,
          kSpeakerAssetsWifiTagBytes)) {
    return false;
  }
  SpeakerAssetsWifiClientAuth decoded{};
  decoded.key_epoch = read_le16(encoded + 6U);
  std::copy_n(
      encoded + 8U, decoded.client_nonce.size(),
      decoded.client_nonce.begin());
  *auth = decoded;
  return true;
}

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
        client_nonce) {
  if (encoded == nullptr || !key_is_valid(key)) {
    return false;
  }
  encoded->fill(0U);
  std::copy(
      kServerReadyMagic.begin(), kServerReadyMagic.end(),
      encoded->begin());
  (*encoded)[4] = kSpeakerAssetsWifiWireVersion;
  (*encoded)[5] = static_cast<std::uint8_t>(status);
  write_le32(encoded->data() + 8U, route_id);
  write_le32(encoded->data() + 12U, generation);
  SoundHmacSha256 hmac(key);
  if (!update_context(
          &hmac, kServerReadyContext,
          kServerReadyContextSuffix) ||
      !hmac.update(device_id.data(), device_id.size()) ||
      !hmac.update(
          endpoint_nonce.data(), endpoint_nonce.size()) ||
      !hmac.update(
          client_nonce.data(), client_nonce.size()) ||
      !hmac.update(encoded->data(), 16U)) {
    return false;
  }
  const auto digest = hmac.finish();
  std::copy_n(
      digest.begin(), kSpeakerAssetsWifiTagBytes,
      encoded->begin() + 16U);
  return true;
}

SoundSha256Digest derive_speaker_assets_wifi_session_key(
    const std::array<std::uint8_t, kSpeakerAssetsWifiKeyBytes>& key,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        device_id,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        endpoint_nonce,
    const std::array<std::uint8_t, kSpeakerAssetsWifiIdentityBytes>&
        client_nonce) {
  if (!key_is_valid(key)) {
    return {};
  }
  SoundHmacSha256 hmac(key);
  if (!update_context(
          &hmac, kSessionKeyContext,
          kSessionKeyContextSuffix) ||
      !hmac.update(device_id.data(), device_id.size()) ||
      !hmac.update(
          endpoint_nonce.data(), endpoint_nonce.size()) ||
      !hmac.update(
          client_nonce.data(), client_nonce.size())) {
    return {};
  }
  return hmac.finish();
}

bool encode_speaker_assets_wifi_record(
    const SpeakerAssetsWifiRecord& record,
    const SoundSha256Digest& session_key,
    std::array<std::uint8_t, kSpeakerAssetsWifiRecordMaxBytes>*
        encoded,
    std::size_t* encoded_length) {
  if (encoded == nullptr || encoded_length == nullptr ||
      !record_kind_is_valid(record.kind) ||
      record.sequence == 0U ||
      record.payload_length < kSpeakerAssetsFrameHeaderBytes ||
      record.payload_length > kSpeakerAssetsWifiFrameMaxBytes ||
      !key_is_valid(session_key)) {
    return false;
  }
  encoded->fill(0U);
  std::copy(
      kRecordMagic.begin(), kRecordMagic.end(),
      encoded->begin());
  (*encoded)[4] = kSpeakerAssetsWifiWireVersion;
  (*encoded)[5] = static_cast<std::uint8_t>(record.kind);
  write_le16(encoded->data() + 6U, record.payload_length);
  write_le32(encoded->data() + 8U, record.sequence);
  std::copy_n(
      record.payload.begin(), record.payload_length,
      encoded->begin() + kSpeakerAssetsWifiRecordHeaderBytes);
  SoundHmacSha256 hmac(session_key);
  if (!hmac.update(encoded->data(), 16U) ||
      !hmac.update(
          encoded->data() + kSpeakerAssetsWifiRecordHeaderBytes,
          record.payload_length)) {
    return false;
  }
  const auto digest = hmac.finish();
  std::copy_n(
      digest.begin(), kSpeakerAssetsWifiTagBytes,
      encoded->begin() + 16U);
  *encoded_length =
      kSpeakerAssetsWifiRecordHeaderBytes +
      record.payload_length;
  return true;
}

bool decode_speaker_assets_wifi_record(
    const std::uint8_t* encoded,
    std::size_t length,
    const SoundSha256Digest& session_key,
    SpeakerAssetsWifiRecordKind expected_kind,
    SpeakerAssetsWifiRecord* record) {
  if (encoded == nullptr || record == nullptr ||
      length < kSpeakerAssetsWifiRecordHeaderBytes ||
      length > kSpeakerAssetsWifiRecordMaxBytes ||
      !record_kind_is_valid(expected_kind) ||
      !key_is_valid(session_key) ||
      !constant_time_equal(
          encoded, kRecordMagic.data(), kRecordMagic.size()) ||
      encoded[4] != kSpeakerAssetsWifiWireVersion ||
      encoded[5] != static_cast<std::uint8_t>(expected_kind) ||
      encoded[12] != 0U || encoded[13] != 0U ||
      encoded[14] != 0U || encoded[15] != 0U) {
    return false;
  }
  const auto payload_length = read_le16(encoded + 6U);
  const auto sequence = read_le32(encoded + 8U);
  if (sequence == 0U ||
      payload_length < kSpeakerAssetsFrameHeaderBytes ||
      payload_length > kSpeakerAssetsWifiFrameMaxBytes ||
      length !=
          kSpeakerAssetsWifiRecordHeaderBytes +
              static_cast<std::size_t>(payload_length)) {
    return false;
  }
  SoundHmacSha256 hmac(session_key);
  if (!hmac.update(encoded, 16U) ||
      !hmac.update(
          encoded + kSpeakerAssetsWifiRecordHeaderBytes,
          payload_length)) {
    return false;
  }
  const auto digest = hmac.finish();
  if (!constant_time_equal(
          digest.data(), encoded + 16U,
          kSpeakerAssetsWifiTagBytes)) {
    return false;
  }
  SpeakerAssetsWifiRecord decoded{};
  decoded.kind = expected_kind;
  decoded.sequence = sequence;
  decoded.payload_length = payload_length;
  std::copy_n(
      encoded + kSpeakerAssetsWifiRecordHeaderBytes,
      payload_length, decoded.payload.begin());
  *record = decoded;
  return true;
}

}  // namespace easy_input::speaker_assets
