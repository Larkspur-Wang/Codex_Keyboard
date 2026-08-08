#pragma once

#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

inline constexpr std::size_t kFixedTextMaxUtf8Bytes = 960;
inline constexpr std::uint8_t kFixedTextAppCommandReportId = 0x11;
inline constexpr std::uint8_t kFixedTextAppCommandKind = 0x01;
inline constexpr std::size_t kFixedTextAppCommandPayloadLen = 63;
inline constexpr std::size_t kFixedTextAppCommandHeaderLen = 4;
inline constexpr std::size_t kFixedTextAppCommandChunkDataLen =
    kFixedTextAppCommandPayloadLen - kFixedTextAppCommandHeaderLen;

inline constexpr std::uint8_t fixed_text_chunk_count(
    std::size_t text_size) {
  if (text_size == 0 || text_size > kFixedTextMaxUtf8Bytes) {
    return 0;
  }
  return static_cast<std::uint8_t>(
      (text_size + kFixedTextAppCommandChunkDataLen - 1) /
      kFixedTextAppCommandChunkDataLen);
}

static_assert(
    (kFixedTextMaxUtf8Bytes + kFixedTextAppCommandChunkDataLen - 1) /
            kFixedTextAppCommandChunkDataLen <=
        255,
    "fixed-text chunk count must fit the AppCommand wire field");

}  // namespace ai_keyboard
