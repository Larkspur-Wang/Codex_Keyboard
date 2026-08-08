#include "eci_protocol.h"

#include <array>
#include <utility>

namespace eci {
namespace {

constexpr std::array<std::pair<std::string_view, MessageKind>, 20> kKinds{{
    {"device.hello", MessageKind::kDeviceHello},
    {"ptt.start", MessageKind::kPttStart},
    {"ptt.audio", MessageKind::kPttAudio},
    {"ptt.end", MessageKind::kPttEnd},
    {"play.request", MessageKind::kPlayRequest},
    {"play.resume", MessageKind::kPlayResume},
    {"play.started", MessageKind::kPlayStarted},
    {"play.finished", MessageKind::kPlayFinished},
    {"play.failed", MessageKind::kPlayFailed},
    {"play.preempted", MessageKind::kPlayPreempted},
    {"binding.projection", MessageKind::kBindingProjection},
    {"prompt.state", MessageKind::kPromptState},
    {"play.begin", MessageKind::kPlayBegin},
    {"play.audio", MessageKind::kPlayAudio},
    {"play.end", MessageKind::kPlayEnd},
    {"play.cancel", MessageKind::kPlayCancel},
    {"bridge.state", MessageKind::kBridgeState},
    {"bridge.frame", MessageKind::kBridgeFrame},
    {"frame.ack", MessageKind::kFrameAck},
    {"protocol.error", MessageKind::kProtocolError},
}};

bool IsIdentifierByte(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.';
}

}  // namespace

bool IsValidIdentifier(std::string_view value) {
  if (value.empty() || value.size() > kMaxIdentifierBytes) {
    return false;
  }
  for (const char byte : value) {
    if (!IsIdentifierByte(byte)) {
      return false;
    }
  }
  return true;
}

bool IsSupportedVersion(std::uint8_t version) {
  return version == kProtocolVersion;
}

bool ParseMessageKind(std::string_view value, MessageKind* output) {
  if (output == nullptr) {
    return false;
  }
  for (const auto& [name, kind] : kKinds) {
    if (value == name) {
      *output = kind;
      return true;
    }
  }
  return false;
}

ReplayResult ReplayWindow::CheckAndRecord(std::uint64_t sequence) {
  if (!initialized_) {
    initialized_ = true;
    highest_ = sequence;
    seen_ = 1;
    return ReplayResult::kAccepted;
  }
  if (sequence > highest_) {
    const std::uint64_t shift = sequence - highest_;
    seen_ = shift >= kReplayWindowBits ? 1 : (seen_ << shift) | 1;
    highest_ = sequence;
    return ReplayResult::kAccepted;
  }
  const std::uint64_t offset = highest_ - sequence;
  if (offset >= kReplayWindowBits) {
    return ReplayResult::kStale;
  }
  const std::uint64_t mask = std::uint64_t{1} << offset;
  if ((seen_ & mask) != 0) {
    return ReplayResult::kDuplicate;
  }
  seen_ |= mask;
  return ReplayResult::kAccepted;
}

void FrameTracker::NoteTransportSend(std::uint32_t frame) {
  in_flight_.insert(frame);
}

bool FrameTracker::ApplyBusinessAck(std::uint32_t frame) {
  if (in_flight_.find(frame) == in_flight_.end()) {
    return false;
  }
  in_flight_.erase(in_flight_.begin(), in_flight_.upper_bound(frame));
  return true;
}

bool FrameTracker::IsPending(std::uint32_t frame) const {
  return in_flight_.find(frame) != in_flight_.end();
}

std::size_t FrameTracker::PendingCount() const {
  return in_flight_.size();
}

}  // namespace eci
