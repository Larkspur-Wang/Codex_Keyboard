#include "speaker_assets/sound_asset_crypto.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace easy_input::speaker_assets {
namespace {

constexpr std::array<std::uint32_t, 8> kSha256InitialState = {
    0x6A09E667U,
    0xBB67AE85U,
    0x3C6EF372U,
    0xA54FF53AU,
    0x510E527FU,
    0x9B05688CU,
    0x1F83D9ABU,
    0x5BE0CD19U,
};

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

constexpr std::uint32_t rotate_right(std::uint32_t value,
                                     std::uint32_t bits) {
  return (value >> bits) | (value << (32U - bits));
}

std::uint32_t load_be32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

void store_be32(std::uint32_t value, std::uint8_t* output) {
  output[0] = static_cast<std::uint8_t>(value >> 24U);
  output[1] = static_cast<std::uint8_t>(value >> 16U);
  output[2] = static_cast<std::uint8_t>(value >> 8U);
  output[3] = static_cast<std::uint8_t>(value);
}

}  // namespace

SoundSha256::SoundSha256() {
  reset();
}

void SoundSha256::reset() {
  state_ = kSha256InitialState;
  buffer_.fill(0U);
  digest_.fill(0U);
  total_bytes_ = 0U;
  buffered_bytes_ = 0U;
  finished_ = false;
}

bool SoundSha256::update(const std::uint8_t* data, std::size_t length) {
  if (finished_ || (data == nullptr && length != 0U)) {
    return false;
  }
  if (length == 0U) {
    return true;
  }

  constexpr auto kMaxMessageBytes =
      std::numeric_limits<std::uint64_t>::max() / 8U;
  if (length > kMaxMessageBytes - total_bytes_) {
    return false;
  }
  total_bytes_ += static_cast<std::uint64_t>(length);

  if (buffered_bytes_ != 0U) {
    const auto copy_length =
        std::min(length, buffer_.size() - buffered_bytes_);
    std::memcpy(buffer_.data() + buffered_bytes_, data, copy_length);
    buffered_bytes_ += copy_length;
    data += copy_length;
    length -= copy_length;

    if (buffered_bytes_ == buffer_.size()) {
      transform(buffer_.data());
      buffered_bytes_ = 0U;
    }
  }

  while (length >= buffer_.size()) {
    transform(data);
    data += buffer_.size();
    length -= buffer_.size();
  }

  if (length != 0U) {
    std::memcpy(buffer_.data(), data, length);
    buffered_bytes_ = length;
  }
  return true;
}

SoundSha256Digest SoundSha256::finish() {
  if (finished_) {
    return digest_;
  }

  const std::uint64_t message_bits = total_bytes_ * 8U;
  buffer_[buffered_bytes_++] = 0x80U;

  if (buffered_bytes_ > 56U) {
    std::fill(buffer_.begin() + buffered_bytes_, buffer_.end(), 0U);
    transform(buffer_.data());
    buffered_bytes_ = 0U;
  }

  std::fill(buffer_.begin() + buffered_bytes_,
            buffer_.begin() + 56U,
            0U);
  for (std::size_t index = 0; index < 8U; ++index) {
    buffer_[56U + index] = static_cast<std::uint8_t>(
        message_bits >> (56U - index * 8U));
  }
  transform(buffer_.data());
  buffered_bytes_ = 0U;

  for (std::size_t index = 0; index < state_.size(); ++index) {
    store_be32(state_[index], digest_.data() + index * 4U);
  }
  finished_ = true;
  return digest_;
}

void SoundSha256::transform(const std::uint8_t* block) {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16U; ++index) {
    schedule[index] = load_be32(block + index * 4U);
  }
  for (std::size_t index = 16U; index < schedule.size(); ++index) {
    const std::uint32_t sigma0 =
        rotate_right(schedule[index - 15U], 7U) ^
        rotate_right(schedule[index - 15U], 18U) ^
        (schedule[index - 15U] >> 3U);
    const std::uint32_t sigma1 =
        rotate_right(schedule[index - 2U], 17U) ^
        rotate_right(schedule[index - 2U], 19U) ^
        (schedule[index - 2U] >> 10U);
    schedule[index] = schedule[index - 16U] + sigma0 +
                      schedule[index - 7U] + sigma1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const std::uint32_t sum1 =
        rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
        rotate_right(e, 25U);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temporary1 =
        h + sum1 + choose + kSha256RoundConstants[index] + schedule[index];
    const std::uint32_t sum0 =
        rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
        rotate_right(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

std::uint32_t sound_crc32_iso_hdlc(const std::uint8_t* data,
                                   std::size_t length) {
  if (data == nullptr && length != 0U) {
    return 0U;
  }

  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (std::uint32_t bit = 0; bit < 8U; ++bit) {
      const std::uint32_t reflected_polynomial =
          (crc & 1U) != 0U ? 0xEDB88320U : 0U;
      crc = (crc >> 1U) ^ reflected_polynomial;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

bool sound_digest_equal(const SoundSha256Digest& first,
                        const SoundSha256Digest& second) {
  std::uint32_t difference = 0U;
  for (std::size_t index = 0; index < first.size(); ++index) {
    difference |=
        static_cast<std::uint32_t>(first[index] ^ second[index]);
  }
  return difference == 0U;
}

}  // namespace easy_input::speaker_assets
