#include <cassert>
#include <cstdint>

#include "keyboard/ble_owner_recovery.h"

namespace {

using ai_keyboard::BleOwnerRecoveryState;
using ai_keyboard::BleOwnerToken;

constexpr BleOwnerToken kOwnerA{7, 41};
constexpr BleOwnerToken kOwnerSameHandleNextGeneration{7, 42};

void test_invalid_target_does_not_enter_recovery() {
  BleOwnerRecoveryState state;
  assert(!state.begin({}, 100));
  assert(!state.pending());
}

void test_accepted_terminate_retries_until_lifetime_ends() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  assert(state.observe(true, true, kOwnerA, 100) ==
         BleOwnerRecoveryState::Action::RequestTerminate);
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::Accepted, 100);
  assert(state.observe(true, true, kOwnerA, 100) ==
         BleOwnerRecoveryState::Action::None);
  assert(state.observe(true,
                       true,
                       kOwnerA,
                       100 + BleOwnerRecoveryState::kOwnerLossTimeoutUs) ==
         BleOwnerRecoveryState::Action::RequestHostReset);
}

void test_term_failure_requests_immediate_retry_without_unlocking_input() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::Accepted, 100);
  assert(state.note_term_failure(kOwnerA.conn_handle, 150));
  assert(state.pending());
  assert(state.phase() == BleOwnerRecoveryState::Phase::AwaitingHostReset);
  assert(state.observe(true, true, kOwnerA, 150) ==
         BleOwnerRecoveryState::Action::RequestHostReset);
}

void test_unrelated_term_failure_is_ignored() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::Accepted, 100);
  assert(!state.note_term_failure(8, 150));
  assert(state.observe(true, true, kOwnerA, 150) ==
         BleOwnerRecoveryState::Action::None);
}

void test_adapter_disconnect_completes_without_hidd_event() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  assert(state.observe(true, false, {}, 110) ==
         BleOwnerRecoveryState::Action::Completed);
  assert(!state.pending());
}

void test_host_reset_generation_change_completes_without_start_event() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  assert(state.observe(true, true, kOwnerSameHandleNextGeneration, 110) ==
         BleOwnerRecoveryState::Action::Completed);
  assert(!state.pending());
}

void test_not_connected_waits_for_authoritative_lifetime_change() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::NotConnected, 110);
  assert(state.pending());
  assert(state.observe(true, true, kOwnerA, 110) ==
         BleOwnerRecoveryState::Action::RequestHostReset);
  assert(state.observe(true, false, {}, 111) ==
         BleOwnerRecoveryState::Action::Completed);
  assert(!state.pending());
}

void test_unknown_snapshot_never_discards_owner_state() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::Accepted, 100);
  assert(state.observe(false, false, {}, 110) ==
         BleOwnerRecoveryState::Action::None);
  assert(state.pending());
}

void test_retryable_terminate_error_uses_short_backoff() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::Retryable, 100);
  assert(state.observe(true, true, kOwnerA, 100) ==
         BleOwnerRecoveryState::Action::None);
  assert(state.observe(true,
                       true,
                       kOwnerA,
                       100 + BleOwnerRecoveryState::kRetryDelayUs) ==
         BleOwnerRecoveryState::Action::RequestTerminate);
}

void test_retryable_terminate_errors_have_a_total_reset_deadline() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::Retryable, 100);
  assert(state.observe(true,
                       true,
                       kOwnerA,
                       100 + BleOwnerRecoveryState::kRetryDelayUs) ==
         BleOwnerRecoveryState::Action::RequestTerminate);
  state.note_terminate_result(
      BleOwnerRecoveryState::TerminateResult::Retryable,
      100 + BleOwnerRecoveryState::kRetryDelayUs);
  assert(state.observe(true,
                       true,
                       kOwnerA,
                       100 + BleOwnerRecoveryState::kTerminateRetryBudgetUs) ==
         BleOwnerRecoveryState::Action::RequestHostReset);
  assert(state.pending());
}

void test_host_reset_request_is_rate_limited() {
  BleOwnerRecoveryState state;
  assert(state.begin(kOwnerA, 100));
  state.note_term_failure(kOwnerA.conn_handle, 110);
  assert(state.observe(true, true, kOwnerA, 110) ==
         BleOwnerRecoveryState::Action::RequestHostReset);
  state.note_host_reset_scheduled(110);
  assert(state.observe(true, true, kOwnerA, 111) ==
         BleOwnerRecoveryState::Action::None);
}

}  // namespace

int main() {
  test_invalid_target_does_not_enter_recovery();
  test_accepted_terminate_retries_until_lifetime_ends();
  test_term_failure_requests_immediate_retry_without_unlocking_input();
  test_unrelated_term_failure_is_ignored();
  test_adapter_disconnect_completes_without_hidd_event();
  test_host_reset_generation_change_completes_without_start_event();
  test_not_connected_waits_for_authoritative_lifetime_change();
  test_unknown_snapshot_never_discards_owner_state();
  test_retryable_terminate_error_uses_short_backoff();
  test_retryable_terminate_errors_have_a_total_reset_deadline();
  test_host_reset_request_is_rate_limited();
  return 0;
}
