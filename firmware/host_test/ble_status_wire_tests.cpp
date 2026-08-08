#include <cassert>
#include <cstdint>
#include <string>

#include "keyboard/ble_status_wire.h"

namespace {

void appends_required_ble_fields_within_reserved_budget() {
  const std::string base =
      "{\"schema\":\"ai_keyboard.config_status.v1\",\"phase\":\"battery\"}";
  const auto wire = ai_keyboard::append_ble_status_wire_json(
      base,
      {
          true,
          true,
          "deep_idle",
          true,
          UINT16_MAX,
          UINT16_MAX,
          UINT16_MAX,
      });

  assert(wire.size() <= ai_keyboard::kConfigStatusGattSafeLen);
  assert(wire.find(R"("ble":{"connected":1,"idle":1)") != std::string::npos);
  assert(wire.find(R"("profile":"deep_idle")") != std::string::npos);
  assert(wire.find(R"("valid":1)") != std::string::npos);
  assert(wire.find(R"("itvl":65535)") != std::string::npos);
  assert(wire.find(R"("latency":65535)") != std::string::npos);
  assert(wire.find(R"("timeout":65535)") != std::string::npos);
  assert(wire.size() - base.size() <=
         ai_keyboard::kConfigStatusBatteryBleReserveLen);
}

void maximum_reserved_base_still_accepts_worst_case_fragment() {
  const std::string minimum = "{\"x\":\"\"}";
  const auto base_len = ai_keyboard::kConfigStatusGattSafeLen -
                        ai_keyboard::kConfigStatusBatteryBleReserveLen;
  std::string base = "{\"x\":\"";
  base.append(base_len - minimum.size(), 'x');
  base += "\"}";
  assert(base.size() == base_len);

  const auto wire = ai_keyboard::append_ble_status_wire_json(
      base,
      {
          true,
          true,
          "deep_idle",
          true,
          UINT16_MAX,
          UINT16_MAX,
          UINT16_MAX,
      });
  assert(wire.size() > base.size());
  assert(wire.size() <= ai_keyboard::kConfigStatusGattSafeLen);
  assert(wire.find(R"("ble":{)") != std::string::npos);
}

void invalid_or_unbudgeted_payload_is_left_unchanged() {
  const auto invalid =
      ai_keyboard::append_ble_status_wire_json("ready", {});
  assert(invalid == "ready");

  std::string oversized(ai_keyboard::kConfigStatusGattSafeLen, 'x');
  oversized.front() = '{';
  oversized.back() = '}';
  assert(ai_keyboard::append_ble_status_wire_json(oversized, {}) == oversized);
}

}  // namespace

int main() {
  appends_required_ble_fields_within_reserved_budget();
  maximum_reserved_base_still_accepts_worst_case_fragment();
  invalid_or_unbudgeted_payload_is_left_unchanged();
  return 0;
}
