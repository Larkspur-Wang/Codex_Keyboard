#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "keyboard/config_status.h"

namespace ai_keyboard {

struct BleStatusWireSnapshot {
  bool connected = false;
  bool idle = false;
  const char* profile = "unknown";
  bool parameters_valid = false;
  std::uint16_t interval = 0;
  std::uint16_t latency = 0;
  std::uint16_t supervision_timeout = 0;
};

inline std::string append_ble_status_wire_json(
    std::string status_json,
    const BleStatusWireSnapshot& snapshot) {
  if (status_json.size() < 2 || status_json.front() != '{' ||
      status_json.back() != '}') {
    return status_json;
  }

  std::array<char, kConfigStatusBatteryBleReserveLen> fragment{};
  const int fragment_len = std::snprintf(
      fragment.data(),
      fragment.size(),
      ",\"ble\":{\"connected\":%u,\"idle\":%u,\"profile\":\"%s\",\"valid\":%u,\"itvl\":%u,\"latency\":%u,\"timeout\":%u}",
      snapshot.connected ? 1U : 0U,
      snapshot.idle ? 1U : 0U,
      snapshot.profile == nullptr ? "unknown" : snapshot.profile,
      snapshot.parameters_valid ? 1U : 0U,
      snapshot.parameters_valid ? static_cast<unsigned>(snapshot.interval) : 0U,
      snapshot.parameters_valid ? static_cast<unsigned>(snapshot.latency) : 0U,
      snapshot.parameters_valid
          ? static_cast<unsigned>(snapshot.supervision_timeout)
          : 0U);
  if (fragment_len <= 0 ||
      static_cast<std::size_t>(fragment_len) >= fragment.size() ||
      status_json.size() + static_cast<std::size_t>(fragment_len) >
          kConfigStatusGattSafeLen) {
    return status_json;
  }

  status_json.insert(status_json.size() - 1,
                     fragment.data(),
                     static_cast<std::size_t>(fragment_len));
  return status_json;
}

}  // namespace ai_keyboard
