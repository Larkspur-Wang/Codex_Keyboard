#include <cassert>
#include <cstdint>

#include "keyboard/ble_advertising_state.h"

namespace {

using ai_keyboard::BleAdvertisingMode;
using ai_keyboard::BleAdvertisingState;

void test_synced_inactive_host_starts_without_start_event() {
  BleAdvertisingState state;
  const auto observation = state.observe_host(1, true);
  assert(observation.generation_initialized);
  assert(!observation.generation_changed);
  assert(observation.sync_changed);
  assert(state.next_action(false, BleAdvertisingMode::HidFast) ==
         BleAdvertisingState::Action::Start);
}

void test_first_generation_baseline_does_not_close_live_resource_route() {
  BleAdvertisingState state;
  bool resource_route_open = true;

  // GAP may connect and publish the resource route before the platform task
  // performs its first adapter-level reconciliation.
  const auto observation = state.observe_host(42, true);
  if (observation.generation_changed) {
    resource_route_open = false;
  }

  assert(observation.generation_initialized);
  assert(!observation.generation_changed);
  assert(resource_route_open);
}

void test_matching_profile_is_left_running() {
  BleAdvertisingState state;
  state.observe_host(3, true);
  state.note_started(BleAdvertisingMode::ControlSlow);
  assert(state.next_action(true, BleAdvertisingMode::ControlSlow) ==
         BleAdvertisingState::Action::None);
}

void test_active_wrong_profile_is_stopped_before_restart() {
  BleAdvertisingState state;
  state.observe_host(3, true);
  state.note_started(BleAdvertisingMode::ControlSlow);
  assert(state.next_action(true, BleAdvertisingMode::HidFast) ==
         BleAdvertisingState::Action::Stop);
  state.note_stopped();
  assert(state.next_action(false, BleAdvertisingMode::HidFast) ==
         BleAdvertisingState::Action::Start);
}

void test_same_mode_from_old_host_generation_is_not_trusted() {
  BleAdvertisingState state;
  state.observe_host(7, true);
  state.note_started(BleAdvertisingMode::HidSlow);
  const auto observation = state.observe_host(8, true);
  assert(!observation.generation_initialized);
  assert(observation.generation_changed);
  assert(state.next_action(true, BleAdvertisingMode::HidSlow) ==
         BleAdvertisingState::Action::Stop);
}

void test_reset_and_resync_between_polls_still_restarts() {
  BleAdvertisingState state;
  state.observe_host(10, true);
  state.note_started(BleAdvertisingMode::HidFast);
  // The application never observed the unsynced edge, only the new reset
  // generation and the final synced level.
  const auto observation = state.observe_host(11, true);
  assert(!observation.generation_initialized);
  assert(observation.generation_changed);
  assert(state.next_action(false, BleAdvertisingMode::HidFast) ==
         BleAdvertisingState::Action::Start);
}

void test_unsynced_host_never_starts() {
  BleAdvertisingState state;
  state.observe_host(2, false);
  assert(state.next_action(false, BleAdvertisingMode::HidFast) ==
         BleAdvertisingState::Action::None);
  state.observe_host(2, true);
  assert(state.next_action(false, BleAdvertisingMode::HidFast) ==
         BleAdvertisingState::Action::Start);
}

void test_force_restart_extends_same_config_profile() {
  BleAdvertisingState state;
  state.observe_host(4, true);
  state.note_started(BleAdvertisingMode::ControlConfig);
  assert(state.next_action(true,
                           BleAdvertisingMode::ControlConfig,
                           true) == BleAdvertisingState::Action::Stop);
}

void test_stopped_is_stable_when_no_advertising_is_desired() {
  BleAdvertisingState state;
  state.observe_host(5, true);
  assert(state.next_action(false, BleAdvertisingMode::Stopped) ==
         BleAdvertisingState::Action::None);
}

}  // namespace

int main() {
  test_synced_inactive_host_starts_without_start_event();
  test_first_generation_baseline_does_not_close_live_resource_route();
  test_matching_profile_is_left_running();
  test_active_wrong_profile_is_stopped_before_restart();
  test_same_mode_from_old_host_generation_is_not_trusted();
  test_reset_and_resync_between_polls_still_restarts();
  test_unsynced_host_never_starts();
  test_force_restart_extends_same_config_profile();
  test_stopped_is_stable_when_no_advertising_is_desired();
  return 0;
}
