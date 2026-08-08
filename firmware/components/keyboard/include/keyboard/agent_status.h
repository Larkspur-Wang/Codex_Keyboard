#pragma once

#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

constexpr std::uint8_t kAgentStatusProtocolVersion = 1;
constexpr std::uint8_t kAgentStatusReportId = 0x12;
constexpr std::size_t kAgentStatusPayloadLen = 16;
constexpr std::uint32_t kAgentStatusMaxTtlMs = 12U * 60U * 60U * 1000U;

enum class AgentStatusState : std::uint8_t {
  kIdle = 0,
  kRunning = 1,
  kWaitingUser = 2,
  kCompletedUnread = 3,
  kFailed = 4,
};

struct AgentStatusCommand {
  AgentStatusState state = AgentStatusState::kIdle;
  std::uint8_t flags = 0;
  std::uint32_t sequence = 0;
  std::uint32_t ttl_ms = 0;
  std::uint32_t source_hash = 0;
};

inline bool agent_status_commands_equal(const AgentStatusCommand& left,
                                        const AgentStatusCommand& right) {
  return left.state == right.state && left.flags == right.flags &&
         left.sequence == right.sequence && left.ttl_ms == right.ttl_ms &&
         left.source_hash == right.source_hash;
}

inline std::uint32_t read_agent_status_u32_le(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

inline bool decode_agent_status(const std::uint8_t* data,
                                std::size_t len,
                                AgentStatusCommand* out) {
  if (data == nullptr || out == nullptr || len < kAgentStatusPayloadLen ||
      data[0] != kAgentStatusProtocolVersion ||
      data[1] > static_cast<std::uint8_t>(AgentStatusState::kFailed)) {
    return false;
  }

  AgentStatusCommand command{};
  command.state = static_cast<AgentStatusState>(data[1]);
  command.flags = data[2];
  command.sequence = read_agent_status_u32_le(data + 4);
  command.ttl_ms = read_agent_status_u32_le(data + 8);
  command.source_hash = read_agent_status_u32_le(data + 12);
  if (command.ttl_ms > kAgentStatusMaxTtlMs) {
    command.ttl_ms = kAgentStatusMaxTtlMs;
  }
  if (command.state == AgentStatusState::kIdle) {
    command.ttl_ms = 0;
  }
  *out = command;
  return true;
}

inline bool agent_status_sequence_is_newer(std::uint32_t candidate,
                                           std::uint32_t baseline) {
  return candidate != baseline &&
         static_cast<std::int32_t>(candidate - baseline) > 0;
}

}  // namespace ai_keyboard
