#pragma once

#include <cstdint>

namespace ai_keyboard {

struct BleConnectionParameters {
  std::uint16_t interval = 0;
  std::uint16_t latency = 0;
  std::uint16_t supervision_timeout = 0;
};

struct BleConnectionProfileBounds {
  std::uint16_t minimum_interval = 0;
  std::uint16_t maximum_interval = 0;
  std::uint16_t maximum_latency = 0;
  std::uint16_t minimum_supervision_timeout = 0;
};

enum class BleConnectionUpdateDisposition : std::uint8_t {
  Settled,
  RetryImmediately,
  RetryWithBackoff,
};

constexpr bool ble_connection_parameters_match(
    const BleConnectionParameters& actual,
    const BleConnectionProfileBounds& expected) {
  return actual.interval >= expected.minimum_interval &&
         actual.interval <= expected.maximum_interval &&
         actual.latency <= expected.maximum_latency &&
         actual.supervision_timeout >= expected.minimum_supervision_timeout;
}

constexpr BleConnectionUpdateDisposition classify_ble_connection_update(
    bool update_succeeded,
    bool desired_profile_changed_while_in_flight,
    bool actual_parameters_match) {
  if (desired_profile_changed_while_in_flight) {
    return BleConnectionUpdateDisposition::RetryImmediately;
  }
  if (update_succeeded && actual_parameters_match) {
    return BleConnectionUpdateDisposition::Settled;
  }
  return BleConnectionUpdateDisposition::RetryWithBackoff;
}

constexpr std::int64_t ble_connection_update_retry_delay_us(
    std::uint8_t retry_attempt) {
  constexpr std::int64_t kInitialDelayUs = 500LL * 1000;
  constexpr std::uint8_t kMaximumShift = 4;
  const auto shift =
      retry_attempt < kMaximumShift ? retry_attempt : kMaximumShift;
  return kInitialDelayUs << shift;
}

// The owner loop polls BLE delivery even when every input queue is empty.
// Merely polling must not promote an idle/deep-idle connection back to the
// active profile; doing so makes the power-policy reconciliation request the
// low-power profile again in the same loop and creates an endless connection
// parameter update cycle. A promotion is justified only by real queued work.
constexpr bool ble_input_delivery_requires_active_profile(
    bool connected,
    bool delivery_pending) {
  return connected && delivery_pending;
}

}  // namespace ai_keyboard
