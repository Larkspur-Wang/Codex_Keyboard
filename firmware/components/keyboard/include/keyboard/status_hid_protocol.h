#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "keyboard/config_status.h"

namespace ai_keyboard {

inline constexpr std::uint8_t kStatusRequestReportId = 0x13;
inline constexpr std::size_t kStatusRequestPayloadLen = 16;
inline constexpr std::uint8_t kStatusHidProtocolVersion = 1;
inline constexpr std::uint8_t kStatusRequestFlagsNone = 0;
inline constexpr std::uint8_t kStatusRequestFlagFresh = 0x01;
inline constexpr std::uint8_t kStatusRequestKnownFlags = kStatusRequestFlagFresh;

inline constexpr std::uint8_t kStatusResponseCommandKind = 0x04;
inline constexpr std::size_t kStatusAppCommandPayloadLen = 63;
inline constexpr std::size_t kStatusAppCommandHeaderLen = 4;
inline constexpr std::size_t kStatusResponseMetadataLen = 9;
inline constexpr std::size_t kStatusResponseJsonPerChunk =
    kStatusAppCommandPayloadLen - kStatusAppCommandHeaderLen -
    kStatusResponseMetadataLen;
inline constexpr std::size_t kStatusResponseMaxJsonLen = kConfigStatusGattSafeLen;

struct StatusHidRequest {
  std::uint32_t request_id = 0;
  std::uint8_t flags = kStatusRequestFlagsNone;
};

// Accepts either the 16-byte Feature payload delivered by TinyUSB or a
// 17-byte host/test buffer that still includes Report ID 0x13.
bool decode_status_hid_request(const std::uint8_t* data,
                               std::size_t len,
                               StatusHidRequest* out);

std::uint8_t status_hid_response_chunk_count(std::size_t json_len);

// Builds one complete 63-byte Report ID 0x11 payload. The caller supplies
// 0x11 separately to the HID stack. Bytes after the declared data length are
// zero padded, matching the existing AppCommand report contract.
bool encode_status_hid_response_chunk(
    std::uint32_t request_id,
    std::string_view json,
    std::uint8_t chunk_index,
    std::array<std::uint8_t, kStatusAppCommandPayloadLen>* out);

// Owns the single USB status response currently being streamed. Replacing a
// pending response is intentional: the App retries with a new request ID and
// must never have that newer request silently consumed behind stale chunks.
class StatusHidResponseStream {
 public:
  bool replace(std::uint32_t request_id, std::string_view json);
  bool encode_next(
      std::array<std::uint8_t, kStatusAppCommandPayloadLen>* out) const;
  // Returns true when this send completed the response and reset the stream.
  bool mark_next_sent();
  void reset();

  bool pending() const;
  std::uint32_t request_id() const;
  std::size_t json_size() const;
  std::uint8_t next_chunk() const;
  std::uint8_t total_chunks() const;

 private:
  std::string json_;
  std::uint32_t request_id_ = 0;
  std::uint8_t next_chunk_ = 0;
  std::uint8_t total_chunks_ = 0;
};

}  // namespace ai_keyboard
