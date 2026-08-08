#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

#include "keyboard/agent_status.h"

namespace {

void write_u32_le(std::array<std::uint8_t, ai_keyboard::kAgentStatusPayloadLen>* payload,
                  std::size_t offset,
                  std::uint32_t value) {
  (*payload)[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  (*payload)[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  (*payload)[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  (*payload)[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

std::array<std::uint8_t, ai_keyboard::kAgentStatusPayloadLen> valid_payload() {
  std::array<std::uint8_t, ai_keyboard::kAgentStatusPayloadLen> payload{};
  payload[0] = ai_keyboard::kAgentStatusProtocolVersion;
  payload[1] = static_cast<std::uint8_t>(ai_keyboard::AgentStatusState::kRunning);
  payload[2] = 0x5A;
  write_u32_le(&payload, 4, 0x12345678U);
  write_u32_le(&payload, 8, 45'000U);
  write_u32_le(&payload, 12, 0xAABBCCDDU);
  return payload;
}

void test_decodes_fixed_wire_format() {
  const auto payload = valid_payload();
  ai_keyboard::AgentStatusCommand command{};
  assert(ai_keyboard::decode_agent_status(payload.data(), payload.size(), &command));
  assert(command.state == ai_keyboard::AgentStatusState::kRunning);
  assert(command.flags == 0x5A);
  assert(command.sequence == 0x12345678U);
  assert(command.ttl_ms == 45'000U);
  assert(command.source_hash == 0xAABBCCDDU);
}

void test_decodes_payload_with_ignored_trailing_bytes() {
  std::array<std::uint8_t, 63> payload{};
  const auto command_payload = valid_payload();
  std::copy(command_payload.begin(), command_payload.end(), payload.begin());

  ai_keyboard::AgentStatusCommand command{};
  assert(ai_keyboard::decode_agent_status(payload.data(), payload.size(), &command));
  assert(command.state == ai_keyboard::AgentStatusState::kRunning);
  assert(command.sequence == 0x12345678U);
}

void test_rejects_invalid_messages() {
  auto payload = valid_payload();
  ai_keyboard::AgentStatusCommand command{};
  assert(!ai_keyboard::decode_agent_status(payload.data(), payload.size() - 1, &command));

  payload = valid_payload();
  payload[0] = 2;
  assert(!ai_keyboard::decode_agent_status(payload.data(), payload.size(), &command));

  payload = valid_payload();
  payload[1] = 5;
  assert(!ai_keyboard::decode_agent_status(payload.data(), payload.size(), &command));
}

void test_normalizes_ttl() {
  auto payload = valid_payload();
  write_u32_le(&payload, 8, ai_keyboard::kAgentStatusMaxTtlMs + 1U);
  ai_keyboard::AgentStatusCommand command{};
  assert(ai_keyboard::decode_agent_status(payload.data(), payload.size(), &command));
  assert(command.ttl_ms == ai_keyboard::kAgentStatusMaxTtlMs);

  payload[1] = static_cast<std::uint8_t>(ai_keyboard::AgentStatusState::kIdle);
  assert(ai_keyboard::decode_agent_status(payload.data(), payload.size(), &command));
  assert(command.ttl_ms == 0);
}

void test_sequence_comparison_handles_rollover() {
  assert(ai_keyboard::agent_status_sequence_is_newer(11U, 10U));
  assert(!ai_keyboard::agent_status_sequence_is_newer(10U, 10U));
  assert(!ai_keyboard::agent_status_sequence_is_newer(9U, 10U));
  assert(ai_keyboard::agent_status_sequence_is_newer(0U, UINT32_MAX));
}

void test_duplicate_comparison_uses_complete_command() {
  ai_keyboard::AgentStatusCommand baseline{};
  baseline.state = ai_keyboard::AgentStatusState::kRunning;
  baseline.flags = 1;
  baseline.sequence = 7;
  baseline.ttl_ms = 30'000;
  baseline.source_hash = 0x12345678U;

  auto candidate = baseline;
  assert(ai_keyboard::agent_status_commands_equal(candidate, baseline));

  candidate.state = ai_keyboard::AgentStatusState::kFailed;
  assert(!ai_keyboard::agent_status_commands_equal(candidate, baseline));
  candidate = baseline;
  candidate.ttl_ms = 29'000;
  assert(!ai_keyboard::agent_status_commands_equal(candidate, baseline));
  candidate = baseline;
  candidate.sequence = 8;
  assert(!ai_keyboard::agent_status_commands_equal(candidate, baseline));
}

}  // namespace

int main() {
  test_decodes_fixed_wire_format();
  test_decodes_payload_with_ignored_trailing_bytes();
  test_rejects_invalid_messages();
  test_normalizes_ttl();
  test_sequence_comparison_handles_rollover();
  test_duplicate_comparison_uses_complete_command();
  std::cout << "agent_status_tests passed\n";
  return 0;
}
