#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace eci {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kNonceBytes = 12;
inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kMaxIdentifierBytes = 64;
inline constexpr std::size_t kMaxCiphertextBytes = 64 * 1024;
inline constexpr std::uint32_t kReplayWindowBits = 64;

enum class MessageKind : std::uint8_t {
  kDeviceHello,
  kPttStart,
  kPttAudio,
  kPttEnd,
  kPlayRequest,
  kPlayResume,
  kPlayStarted,
  kPlayFinished,
  kPlayFailed,
  kPlayPreempted,
  kBindingProjection,
  kPromptState,
  kPlayBegin,
  kPlayAudio,
  kPlayEnd,
  kPlayCancel,
  kBridgeState,
  kBridgeFrame,
  kFrameAck,
  kProtocolError,
};

enum class ReplayResult : std::uint8_t {
  kAccepted,
  kDuplicate,
  kStale,
};

class ReplayWindow {
 public:
  ReplayResult CheckAndRecord(std::uint64_t sequence);

 private:
  bool initialized_ = false;
  std::uint64_t highest_ = 0;
  std::uint64_t seen_ = 0;
};

class FrameTracker {
 public:
  void NoteTransportSend(std::uint32_t frame);
  bool ApplyBusinessAck(std::uint32_t frame);
  bool IsPending(std::uint32_t frame) const;
  std::size_t PendingCount() const;

 private:
  std::set<std::uint32_t> in_flight_;
};

bool IsValidIdentifier(std::string_view value);
bool IsSupportedVersion(std::uint8_t version);
bool ParseMessageKind(std::string_view value, MessageKind* output);

}  // namespace eci
