#include "keyboard/codex_playback_wire.h"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(EASY_CODEX_HOST_TEST)
#include <openssl/evp.h>
#include <openssl/hmac.h>
#else
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#endif

namespace easy_codex {
namespace {

void put_u32(std::uint8_t* output, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4U; ++index) {
    output[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void put_u64(std::uint8_t* output, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8U; ++index) {
    output[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint16_t get_u16(const std::uint8_t* input, std::size_t offset) {
  return static_cast<std::uint16_t>(input[offset]) |
         (static_cast<std::uint16_t>(input[offset + 1U]) << 8U);
}

std::uint32_t get_u32(const std::uint8_t* input, std::size_t offset) {
  std::uint32_t value = 0U;
  for (std::size_t index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(input[offset + index]) << (index * 8U);
  }
  return value;
}

std::uint64_t get_u64(const std::uint8_t* input, std::size_t offset) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8U);
  }
  return value;
}

bool magic_matches(const std::uint8_t* packet, const char* magic) {
  return packet != nullptr && std::memcmp(packet, magic, 4U) == 0;
}

bool hmac(const std::uint8_t* bytes,
          std::size_t length,
          const std::array<std::uint8_t, 32>& key,
          std::array<std::uint8_t, 32>* digest) {
  if (bytes == nullptr || digest == nullptr) {
    return false;
  }
#if defined(EASY_CODEX_HOST_TEST)
  unsigned int digest_length = 0U;
  return HMAC(EVP_sha256(),
              key.data(),
              static_cast<int>(key.size()),
              bytes,
              length,
              digest->data(),
              &digest_length) != nullptr &&
         digest_length == digest->size();
#else
  const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr &&
         mbedtls_md_hmac(info,
                         key.data(),
                         key.size(),
                         bytes,
                         length,
                         digest->data()) == 0;
#endif
}

bool sign(std::uint8_t* packet,
          std::size_t packet_size,
          const std::array<std::uint8_t, 32>& key) {
  if (packet == nullptr || packet_size < kPlaybackAuthTagBytes) {
    return false;
  }
  const auto body_size = packet_size - kPlaybackAuthTagBytes;
  std::array<std::uint8_t, 32> digest{};
  if (!hmac(packet, body_size, key, &digest)) {
    return false;
  }
  std::copy_n(digest.begin(), kPlaybackAuthTagBytes, packet + body_size);
  return true;
}

bool authenticate(const std::uint8_t* packet,
                  std::size_t packet_size,
                  const std::array<std::uint8_t, 32>& key) {
  if (packet == nullptr || packet_size < kPlaybackAuthTagBytes) {
    return false;
  }
  const auto body_size = packet_size - kPlaybackAuthTagBytes;
  std::array<std::uint8_t, 32> digest{};
  if (!hmac(packet, body_size, key, &digest)) {
    return false;
  }
  std::uint8_t difference = 0U;
  for (std::size_t index = 0; index < kPlaybackAuthTagBytes; ++index) {
    difference |= digest[index] ^ packet[body_size + index];
  }
  return difference == 0U;
}

bool derive_data_key(const PlaybackWireIdentity& identity,
                     std::uint64_t request_nonce,
                     const std::array<std::uint8_t, 32>& root_key,
                     std::array<std::uint8_t, 32>* data_key) {
  if (request_nonce == 0U || data_key == nullptr) {
    return false;
  }
  std::array<std::uint8_t, 44> context{};
  std::memcpy(context.data(), "EIPDKEY1", 8U);
  context[8U] = identity.slot;
  put_u32(context.data(), 12U, identity.request_generation);
  put_u32(context.data(), 16U, identity.connection_generation);
  put_u64(context.data(), 20U, identity.summary_generation);
  put_u64(context.data(), 28U, identity.lease);
  put_u64(context.data(), 36U, request_nonce);
  return hmac(context.data(), context.size(), root_key, data_key);
}

std::array<std::uint8_t, 12> data_nonce(std::uint32_t offset) {
  std::array<std::uint8_t, 12> nonce{};
  put_u32(nonce.data(), 0U, offset);
  return nonce;
}

bool decrypt_data_payload(const std::uint8_t* packet,
                          std::size_t payload_length,
                          const std::array<std::uint8_t, 32>& data_key,
                          std::uint8_t* plaintext) {
  if (packet == nullptr || plaintext == nullptr) {
    return false;
  }
  const auto nonce = data_nonce(get_u32(packet, 32U));
  const auto* ciphertext = packet + kPlaybackDataHeaderBytes;
  const auto* tag = ciphertext + payload_length;
#if defined(EASY_CODEX_HOST_TEST)
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (context == nullptr) {
    return false;
  }
  int written = 0;
  int final_written = 0;
  const bool ok =
      EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) == 1 &&
      EVP_DecryptInit_ex(context, nullptr, nullptr, data_key.data(), nonce.data()) == 1 &&
      EVP_DecryptUpdate(context, nullptr, &written, packet,
                        static_cast<int>(kPlaybackDataHeaderBytes)) == 1 &&
      EVP_DecryptUpdate(context, plaintext, &written, ciphertext,
                        static_cast<int>(payload_length)) == 1 &&
      EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(kPlaybackAuthTagBytes),
                          const_cast<std::uint8_t*>(tag)) == 1 &&
      EVP_DecryptFinal_ex(context, plaintext + written, &final_written) == 1 &&
      static_cast<std::size_t>(written + final_written) == payload_length;
  EVP_CIPHER_CTX_free(context);
  return ok;
#else
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const int key_result = mbedtls_gcm_setkey(
      &context, MBEDTLS_CIPHER_ID_AES, data_key.data(), data_key.size() * 8U);
  const int result =
      key_result == 0
          ? mbedtls_gcm_auth_decrypt(&context,
                                     payload_length,
                                     nonce.data(),
                                     nonce.size(),
                                     packet,
                                     kPlaybackDataHeaderBytes,
                                     tag,
                                     kPlaybackAuthTagBytes,
                                     ciphertext,
                                     plaintext)
          : key_result;
  mbedtls_gcm_free(&context);
  return result == 0;
#endif
}

void encode_identity(std::uint8_t* packet, const PlaybackWireIdentity& identity) {
  put_u32(packet, 8U, identity.request_generation);
  put_u32(packet, 12U, identity.connection_generation);
  put_u64(packet, 16U, identity.summary_generation);
  put_u64(packet, 24U, identity.lease);
}

PlaybackWireIdentity decode_identity(const std::uint8_t* packet) {
  return {
      packet[5U],
      get_u32(packet, 8U),
      get_u32(packet, 12U),
      get_u64(packet, 16U),
      get_u64(packet, 24U),
  };
}

bool reserved_zero(const std::uint8_t* packet, std::size_t first, std::size_t last) {
  for (std::size_t index = first; index < last; ++index) {
    if (packet[index] != 0U) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool playback_wire_identity_valid(const PlaybackWireIdentity& identity) {
  return identity.slot >= 1U && identity.slot <= 4U &&
         identity.request_generation != 0U &&
         identity.connection_generation != 0U &&
         identity.summary_generation != 0U && identity.lease != 0U;
}

bool playback_wire_identity_equal(const PlaybackWireIdentity& first,
                                  const PlaybackWireIdentity& second) {
  return first.slot == second.slot &&
         first.request_generation == second.request_generation &&
         first.connection_generation == second.connection_generation &&
         first.summary_generation == second.summary_generation &&
         first.lease == second.lease;
}

bool playback_data_matches_received_prefix(const PlaybackWireData& data,
                                           const std::uint8_t* received,
                                           std::size_t received_bytes) {
  if (data.payload == nullptr || received == nullptr ||
      data.payload_length == 0U || data.offset >= received_bytes) {
    return false;
  }
  const auto offset = static_cast<std::size_t>(data.offset);
  const auto payload_length = static_cast<std::size_t>(data.payload_length);
  return payload_length <= received_bytes - offset &&
         std::memcmp(received + offset, data.payload, payload_length) == 0;
}

bool encode_playback_request(const PlaybackWireRequest& request,
                             const std::array<std::uint8_t, 32>& key,
                             std::uint8_t* output,
                             std::size_t output_size) {
  if (output == nullptr || output_size != kPlaybackRequestBytes ||
      request.slot < 1U || request.slot > 4U ||
      request.request_generation == 0U ||
      request.connection_generation == 0U || request.nonce == 0U) {
    return false;
  }
  std::memset(output, 0, output_size);
  std::memcpy(output, "EIPR", 4U);
  output[4U] = kPlaybackWireVersion;
  output[5U] = request.slot;
  put_u32(output, 8U, request.request_generation);
  put_u32(output, 12U, request.connection_generation);
  put_u64(output, 16U, request.nonce);
  return sign(output, output_size, key);
}

bool decode_playback_request(const std::uint8_t* packet,
                             std::size_t packet_size,
                             const std::array<std::uint8_t, 32>& key,
                             PlaybackWireRequest* request) {
  if (request == nullptr || packet_size != kPlaybackRequestBytes ||
      !magic_matches(packet, "EIPR") || packet[4U] != kPlaybackWireVersion ||
      !authenticate(packet, packet_size, key) || !reserved_zero(packet, 6U, 8U)) {
    return false;
  }
  *request = {packet[5U], get_u32(packet, 8U), get_u32(packet, 12U),
              get_u64(packet, 16U)};
  return request->slot >= 1U && request->slot <= 4U &&
         request->request_generation != 0U &&
         request->connection_generation != 0U && request->nonce != 0U;
}

bool decode_playback_begin(const std::uint8_t* packet,
                           std::size_t packet_size,
                           const std::array<std::uint8_t, 32>& key,
                           PlaybackWireBegin* begin) {
  if (begin == nullptr || packet_size != kPlaybackBeginBytes ||
      !magic_matches(packet, "EIPB") || packet[4U] != kPlaybackWireVersion ||
      !authenticate(packet, packet_size, key) || !reserved_zero(packet, 6U, 8U) ||
      !reserved_zero(packet, 46U, 48U)) {
    return false;
  }
  *begin = {decode_identity(packet), get_u32(packet, 32U), get_u64(packet, 36U),
            get_u16(packet, 44U), get_u64(packet, 48U)};
  return playback_wire_identity_valid(begin->identity) && begin->total_bytes != 0U &&
         begin->total_bytes <= kPlaybackMaximumEiadBytes && begin->total_samples != 0U &&
         begin->total_samples <= kPlaybackMaximumSamples &&
         begin->chunk_bytes != 0U && begin->chunk_bytes <= kPlaybackChunkBytes &&
         begin->request_nonce != 0U;
}

bool decode_playback_data(const std::uint8_t* packet,
                          std::size_t packet_size,
                          std::uint64_t request_nonce,
                          const std::array<std::uint8_t, 32>& key,
                          std::uint8_t* plaintext,
                          std::size_t plaintext_size,
                          PlaybackWireData* data) {
  if (data == nullptr || plaintext == nullptr || request_nonce == 0U ||
      packet_size < kPlaybackDataHeaderBytes + 1U + kPlaybackAuthTagBytes ||
      !magic_matches(packet, "EIPD") || packet[4U] != kPlaybackWireVersion ||
      !reserved_zero(packet, 6U, 8U) || !reserved_zero(packet, 38U, 40U)) {
    return false;
  }
  const auto payload_length = get_u16(packet, 36U);
  const auto identity = decode_identity(packet);
  if (!playback_wire_identity_valid(identity) || payload_length == 0U ||
      payload_length > kPlaybackChunkBytes || plaintext_size < payload_length ||
      packet_size !=
          kPlaybackDataHeaderBytes + payload_length + kPlaybackAuthTagBytes) {
    return false;
  }
  std::array<std::uint8_t, 32> data_key{};
  if (!derive_data_key(identity, request_nonce, key, &data_key) ||
      !decrypt_data_payload(packet, payload_length, data_key, plaintext)) {
    return false;
  }
  *data = {identity, get_u32(packet, 32U), plaintext, payload_length};
  return true;
}

bool encode_playback_ack(const PlaybackWireAck& ack,
                         const std::array<std::uint8_t, 32>& key,
                         std::uint8_t* output,
                         std::size_t output_size) {
  if (output == nullptr || output_size != kPlaybackAckBytes ||
      !playback_wire_identity_valid(ack.identity) || ack.status > 3U) {
    return false;
  }
  std::memset(output, 0, output_size);
  std::memcpy(output, "EIPA", 4U);
  output[4U] = kPlaybackWireVersion;
  output[5U] = ack.identity.slot;
  output[6U] = ack.status;
  encode_identity(output, ack.identity);
  put_u32(output, 32U, ack.next_offset);
  return sign(output, output_size, key);
}

bool encode_playback_finished(const PlaybackWireFinished& finished,
                              const std::array<std::uint8_t, 32>& key,
                              std::uint8_t* output,
                              std::size_t output_size) {
  if (output == nullptr || output_size != kPlaybackFinishedBytes ||
      !playback_wire_identity_valid(finished.identity) || finished.played_samples == 0U) {
    return false;
  }
  std::memset(output, 0, output_size);
  std::memcpy(output, "EIPF", 4U);
  output[4U] = kPlaybackWireVersion;
  output[5U] = finished.identity.slot;
  encode_identity(output, finished.identity);
  put_u64(output, 32U, finished.played_samples);
  return sign(output, output_size, key);
}

bool decode_playback_finished_ack(const std::uint8_t* packet,
                                  std::size_t packet_size,
                                  const std::array<std::uint8_t, 32>& key,
                                  PlaybackWireIdentity* identity,
                                  std::uint8_t* status) {
  if (identity == nullptr || status == nullptr || packet_size != kPlaybackFinishedAckBytes ||
      !magic_matches(packet, "EIPK") || packet[4U] != kPlaybackWireVersion ||
      !authenticate(packet, packet_size, key) || packet[7U] != 0U) {
    return false;
  }
  *identity = decode_identity(packet);
  *status = packet[6U];
  return playback_wire_identity_valid(*identity) && *status <= 1U;
}

bool decode_mailbox_status(const std::uint8_t* packet,
                           std::size_t packet_size,
                           const std::array<std::uint8_t, 32>& key,
                           MailboxWireStatus* status) {
  if (status == nullptr || packet_size != kMailboxStatusBytes ||
      !magic_matches(packet, "EIMB") || packet[4U] != kMailboxStatusVersion ||
      !authenticate(packet, packet_size, key) ||
      packet[7U] != 0U) {
    return false;
  }
  status->unread_slots = packet[5U];
  status->running_tasks = packet[6U];
  status->heartbeat_sequence = get_u32(packet, 8U);
  std::copy_n(packet + 12U, status->coverage_by_slot.size(),
              status->coverage_by_slot.begin());
  if ((status->unread_slots & 0xF0U) != 0U || status->running_tasks > 4U) {
    return false;
  }
  for (std::size_t index = 0U; index < status->coverage_by_slot.size(); ++index) {
    const bool unread = (status->unread_slots & (1U << index)) != 0U;
    if (unread != (status->coverage_by_slot[index] != 0U)) {
      return false;
    }
  }
  return true;
}

}  // namespace easy_codex
