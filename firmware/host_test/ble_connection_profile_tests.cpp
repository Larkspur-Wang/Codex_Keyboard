#include <cassert>
#include <cstdint>
#include <limits>

#include "keyboard/ble_connection_profile.h"

namespace {

constexpr ai_keyboard::BleConnectionProfileBounds kActive{
    0,
    36,
    0,
    400,
};
constexpr ai_keyboard::BleConnectionProfileBounds kIdle{
    48,
    96,
    4,
    400,
};
constexpr ai_keyboard::BleConnectionProfileBounds kDeepIdle{
    96,
    std::numeric_limits<std::uint16_t>::max(),
    8,
    400,
};

void active_profile_rejects_short_supervision_timeout() {
  assert(ai_keyboard::ble_connection_parameters_match({12, 0, 400}, kActive));
  assert(ai_keyboard::ble_connection_parameters_match({36, 0, 800}, kActive));
  assert(!ai_keyboard::ble_connection_parameters_match({12, 0, 72}, kActive));
  assert(!ai_keyboard::ble_connection_parameters_match({37, 0, 400}, kActive));
  assert(!ai_keyboard::ble_connection_parameters_match({12, 1, 400}, kActive));
}

void idle_profiles_require_the_safe_timeout_floor() {
  assert(ai_keyboard::ble_connection_parameters_match({48, 4, 400}, kIdle));
  assert(!ai_keyboard::ble_connection_parameters_match({48, 4, 72}, kIdle));
  assert(!ai_keyboard::ble_connection_parameters_match({47, 4, 600}, kIdle));
  assert(!ai_keyboard::ble_connection_parameters_match({96, 5, 600}, kIdle));

  assert(ai_keyboard::ble_connection_parameters_match({160, 8, 400}, kDeepIdle));
  assert(!ai_keyboard::ble_connection_parameters_match({160, 8, 72}, kDeepIdle));
  assert(!ai_keyboard::ble_connection_parameters_match({95, 8, 800}, kDeepIdle));
}

void connection_update_outcomes_do_not_spin_on_peer_rejection() {
  using ai_keyboard::BleConnectionUpdateDisposition;
  assert(ai_keyboard::classify_ble_connection_update(true, false, true) ==
         BleConnectionUpdateDisposition::Settled);
  assert(ai_keyboard::classify_ble_connection_update(true, false, false) ==
         BleConnectionUpdateDisposition::RetryWithBackoff);
  assert(ai_keyboard::classify_ble_connection_update(false, false, false) ==
         BleConnectionUpdateDisposition::RetryWithBackoff);
  assert(ai_keyboard::classify_ble_connection_update(false, true, false) ==
         BleConnectionUpdateDisposition::RetryImmediately);
  assert(ai_keyboard::classify_ble_connection_update(true, true, false) ==
         BleConnectionUpdateDisposition::RetryImmediately);
}

void connection_update_backoff_is_bounded() {
  assert(ai_keyboard::ble_connection_update_retry_delay_us(0) == 500000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(1) == 1000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(2) == 2000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(3) == 4000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(4) == 8000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(5) == 8000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(UINT8_MAX) ==
         8000000);
}

void idle_polling_does_not_promote_without_delivery_work() {
  assert(!ai_keyboard::ble_input_delivery_requires_active_profile(false, false));
  assert(!ai_keyboard::ble_input_delivery_requires_active_profile(false, true));
  assert(!ai_keyboard::ble_input_delivery_requires_active_profile(true, false));
  assert(ai_keyboard::ble_input_delivery_requires_active_profile(true, true));
}

}  // namespace

int main() {
  active_profile_rejects_short_supervision_timeout();
  idle_profiles_require_the_safe_timeout_floor();
  connection_update_outcomes_do_not_spin_on_peer_rejection();
  connection_update_backoff_is_bounded();
  idle_polling_does_not_promote_without_delivery_work();
  return 0;
}
