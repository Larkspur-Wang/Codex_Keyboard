#include "keyboard/config_receiver.h"

#include <algorithm>
#include <cstring>

namespace ai_keyboard {
namespace {

constexpr std::uint8_t kConfigMagic0 = 'S';
constexpr std::uint8_t kConfigMagic1 = '3';
constexpr std::uint8_t kConfigMagic2 = 'C';
constexpr std::uint8_t kConfigVersion = 1;

}  // namespace

std::uint16_t crc16_ccitt(const std::uint8_t* data, std::size_t len) {
  std::uint16_t crc = 0xFFFF;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= static_cast<std::uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 0x8000) != 0) {
        crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<std::uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

ConfigReceiveResult ConfigReceiver::receive(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) {
    return {ConfigReceiveStatus::InvalidReport, ""};
  }

  if (data[0] == kConfigReportId && len > 1) {
    ++data;
    --len;
  }

  if (len < kConfigHeaderLen ||
      data[0] != kConfigMagic0 ||
      data[1] != kConfigMagic1 ||
      data[2] != kConfigMagic2 ||
      data[3] != kConfigVersion) {
    return {ConfigReceiveStatus::InvalidReport, ""};
  }

  const auto chunk_index = data[4];
  const auto total_chunks = data[5];
  const auto total_len = static_cast<std::uint16_t>(data[6]) |
                         (static_cast<std::uint16_t>(data[7]) << 8);
  const auto chunk_len = data[8];
  const auto expected_crc = static_cast<std::uint16_t>(data[9]) |
                            (static_cast<std::uint16_t>(data[10]) << 8);

  if (total_chunks == 0 ||
      chunk_index >= total_chunks ||
      total_len == 0 ||
      static_cast<std::size_t>(total_len) > kConfigMaxJsonLen ||
      chunk_len > kConfigMaxChunkData ||
      kConfigHeaderLen + chunk_len > len) {
    return {ConfigReceiveStatus::MalformedChunk, ""};
  }

  if (chunk_index == 0) {
    total_len_ = total_len;
    expected_crc_ = expected_crc;
    total_chunks_ = total_chunks;
    next_chunk_ = 0;
    received_len_ = 0;
    std::fill(buffer_.begin(), buffer_.end(), 0);
  }

  if (chunk_index != next_chunk_ ||
      total_len != total_len_ ||
      total_chunks != total_chunks_ ||
      expected_crc != expected_crc_ ||
      received_len_ + chunk_len > total_len_) {
    return {ConfigReceiveStatus::OutOfOrder, ""};
  }

  std::memcpy(buffer_.data() + received_len_, data + kConfigHeaderLen, chunk_len);
  received_len_ = static_cast<std::uint16_t>(received_len_ + chunk_len);
  ++next_chunk_;

  if (next_chunk_ < total_chunks_) {
    return {ConfigReceiveStatus::Pending, ""};
  }

  if (received_len_ != total_len_ ||
      crc16_ccitt(buffer_.data(), received_len_) != expected_crc_) {
    return {ConfigReceiveStatus::CrcMismatch, ""};
  }

  buffer_[received_len_] = 0;
  return {
      ConfigReceiveStatus::Complete,
      std::string(reinterpret_cast<const char*>(buffer_.data()), received_len_),
  };
}

void ConfigReceiver::reset() {
  std::fill(buffer_.begin(), buffer_.end(), 0);
  total_len_ = 0;
  expected_crc_ = 0;
  total_chunks_ = 0;
  next_chunk_ = 0;
  received_len_ = 0;
}

ConfigReceiveResult EndpointBoundConfigReceiver::receive(
    std::uint32_t endpoint_epoch,
    const std::uint8_t* data,
    std::size_t len) {
  if (endpoint_epoch == 0) {
    return {ConfigReceiveStatus::InvalidReport, ""};
  }
  if (endpoint_epoch_ != endpoint_epoch) {
    receiver_.reset();
    endpoint_epoch_ = endpoint_epoch;
  }
  return receiver_.receive(data, len);
}

void EndpointBoundConfigReceiver::reset() {
  receiver_.reset();
  endpoint_epoch_ = 0;
}

std::uint32_t EndpointBoundConfigReceiver::endpoint_epoch() const {
  return endpoint_epoch_;
}

}  // namespace ai_keyboard
