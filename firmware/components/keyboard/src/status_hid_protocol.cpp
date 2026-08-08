#include "keyboard/status_hid_protocol.h"

#include <algorithm>

#include "keyboard/config_receiver.h"

namespace ai_keyboard {
namespace {

constexpr std::uint8_t kStatusRequestMagic0 = 'S';
constexpr std::uint8_t kStatusRequestMagic1 = '3';
constexpr std::uint8_t kStatusRequestMagic2 = 'R';

std::uint32_t read_u32_le(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

void write_u16_le(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFF);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void write_u32_le(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFF);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

}  // namespace

bool decode_status_hid_request(const std::uint8_t* data,
                               std::size_t len,
                               StatusHidRequest* out) {
  if (data == nullptr || out == nullptr) {
    return false;
  }

  if (len == kStatusRequestPayloadLen + 1 && data[0] == kStatusRequestReportId) {
    ++data;
    --len;
  }
  if (len != kStatusRequestPayloadLen ||
      data[0] != kStatusRequestMagic0 ||
      data[1] != kStatusRequestMagic1 ||
      data[2] != kStatusRequestMagic2 ||
      data[3] != kStatusHidProtocolVersion) {
    return false;
  }

  const auto request_id = read_u32_le(data + 4);
  const auto flags = data[8];
  if (request_id == 0 ||
      (flags & static_cast<std::uint8_t>(~kStatusRequestKnownFlags)) != 0) {
    return false;
  }
  for (std::size_t index = 9; index < kStatusRequestPayloadLen; ++index) {
    if (data[index] != 0) {
      return false;
    }
  }

  out->request_id = request_id;
  out->flags = flags;
  return true;
}

std::uint8_t status_hid_response_chunk_count(std::size_t json_len) {
  if (json_len == 0 || json_len > kStatusResponseMaxJsonLen) {
    return 0;
  }
  const auto chunks =
      (json_len + kStatusResponseJsonPerChunk - 1) / kStatusResponseJsonPerChunk;
  if (chunks == 0 || chunks > 255) {
    return 0;
  }
  return static_cast<std::uint8_t>(chunks);
}

bool encode_status_hid_response_chunk(
    std::uint32_t request_id,
    std::string_view json,
    std::uint8_t chunk_index,
    std::array<std::uint8_t, kStatusAppCommandPayloadLen>* out) {
  if (request_id == 0 || out == nullptr) {
    return false;
  }
  const auto total_chunks = status_hid_response_chunk_count(json.size());
  if (total_chunks == 0 || chunk_index >= total_chunks) {
    return false;
  }

  out->fill(0);
  (*out)[0] = kStatusResponseCommandKind;
  (*out)[1] = chunk_index;
  (*out)[2] = total_chunks;

  const auto offset = static_cast<std::size_t>(chunk_index) *
                      kStatusResponseJsonPerChunk;
  const auto chunk_len =
      std::min(kStatusResponseJsonPerChunk, json.size() - offset);
  (*out)[3] = static_cast<std::uint8_t>(kStatusResponseMetadataLen + chunk_len);
  (*out)[4] = kStatusHidProtocolVersion;
  write_u32_le(out->data() + 5, request_id);
  write_u16_le(out->data() + 9, static_cast<std::uint16_t>(json.size()));
  const auto crc = crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
  write_u16_le(out->data() + 11, crc);
  std::copy_n(reinterpret_cast<const std::uint8_t*>(json.data()) + offset,
              chunk_len,
              out->data() + kStatusAppCommandHeaderLen +
                  kStatusResponseMetadataLen);
  return true;
}

bool StatusHidResponseStream::replace(std::uint32_t request_id,
                                      std::string_view json) {
  const auto total_chunks = status_hid_response_chunk_count(json.size());
  if (request_id == 0 || total_chunks == 0) {
    return false;
  }
  json_.assign(json.data(), json.size());
  request_id_ = request_id;
  next_chunk_ = 0;
  total_chunks_ = total_chunks;
  return true;
}

bool StatusHidResponseStream::encode_next(
    std::array<std::uint8_t, kStatusAppCommandPayloadLen>* out) const {
  if (!pending()) {
    return false;
  }
  return encode_status_hid_response_chunk(request_id_, json_, next_chunk_, out);
}

bool StatusHidResponseStream::mark_next_sent() {
  if (!pending()) {
    return false;
  }
  ++next_chunk_;
  if (next_chunk_ < total_chunks_) {
    return false;
  }
  reset();
  return true;
}

void StatusHidResponseStream::reset() {
  json_.clear();
  request_id_ = 0;
  next_chunk_ = 0;
  total_chunks_ = 0;
}

bool StatusHidResponseStream::pending() const { return total_chunks_ != 0; }

std::uint32_t StatusHidResponseStream::request_id() const { return request_id_; }

std::size_t StatusHidResponseStream::json_size() const { return json_.size(); }

std::uint8_t StatusHidResponseStream::next_chunk() const { return next_chunk_; }

std::uint8_t StatusHidResponseStream::total_chunks() const { return total_chunks_; }

}  // namespace ai_keyboard
