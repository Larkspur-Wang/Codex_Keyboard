#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace ai_keyboard {

constexpr std::uint8_t kConfigReportId = 0x10;
constexpr std::size_t kConfigHeaderLen = 11;
constexpr std::size_t kConfigMaxChunkData = 52;
constexpr std::size_t kConfigMaxJsonLen = 2048;

enum class ConfigReceiveStatus {
  Pending,
  Complete,
  InvalidReport,
  MalformedChunk,
  OutOfOrder,
  CrcMismatch,
};

struct ConfigReceiveResult {
  ConfigReceiveResult() = default;
  ConfigReceiveResult(ConfigReceiveStatus next_status, std::string next_json)
      : status(next_status), json(std::move(next_json)) {}

  ConfigReceiveStatus status = ConfigReceiveStatus::Pending;
  std::string json;
};

class ConfigReceiver {
 public:
  ConfigReceiveResult receive(const std::uint8_t* data, std::size_t len);
  void reset();

 private:
  std::array<std::uint8_t, kConfigMaxJsonLen + 1> buffer_{};
  std::uint16_t total_len_ = 0;
  std::uint16_t expected_crc_ = 0;
  std::uint8_t total_chunks_ = 0;
  std::uint8_t next_chunk_ = 0;
  std::uint16_t received_len_ = 0;
};

// Prevents a chunked message from being assembled across two physical
// endpoint lifetimes. The transport supplies its callback-owned epoch for
// every received chunk.
class EndpointBoundConfigReceiver {
 public:
  ConfigReceiveResult receive(std::uint32_t endpoint_epoch,
                              const std::uint8_t* data,
                              std::size_t len);
  void reset();
  std::uint32_t endpoint_epoch() const;

 private:
  ConfigReceiver receiver_;
  std::uint32_t endpoint_epoch_ = 0;
};

std::uint16_t crc16_ccitt(const std::uint8_t* data, std::size_t len);

}  // namespace ai_keyboard
