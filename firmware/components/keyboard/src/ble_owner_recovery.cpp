#include "keyboard/ble_owner_recovery.h"

namespace ai_keyboard {

bool BleOwnerRecoveryState::begin(BleOwnerToken target,
                                  std::uint64_t now_us) {
  if (!target.valid()) {
    reset();
    return false;
  }
  phase_ = Phase::Terminating;
  target_ = target;
  action_after_us_ = now_us;
  terminate_deadline_us_ = now_us + kTerminateRetryBudgetUs;
  return true;
}

BleOwnerRecoveryState::Action BleOwnerRecoveryState::observe(
    bool snapshot_valid,
    bool adapter_connected,
    BleOwnerToken adapter_owner,
    std::uint64_t now_us) {
  if (!pending()) {
    return Action::None;
  }
  if (snapshot_valid &&
      (!adapter_connected || !adapter_owner.valid() ||
       adapter_owner != target_)) {
    reset();
    return Action::Completed;
  }
  if (phase_ == Phase::Terminating) {
    if (now_us >= terminate_deadline_us_) {
      phase_ = Phase::AwaitingHostReset;
      return Action::RequestHostReset;
    }
    if (now_us < action_after_us_) {
      return Action::None;
    }
    return Action::RequestTerminate;
  }
  if (now_us < action_after_us_) {
    return Action::None;
  }
  if (phase_ == Phase::AwaitingOwnerLoss) {
    phase_ = Phase::AwaitingHostReset;
    return Action::RequestHostReset;
  }
  if (phase_ == Phase::AwaitingHostReset) {
    return Action::RequestHostReset;
  }
  return Action::None;
}

bool BleOwnerRecoveryState::note_term_failure(std::uint16_t conn_handle,
                                              std::uint64_t now_us) {
  if (!pending() || target_.conn_handle != conn_handle) {
    return false;
  }
  phase_ = Phase::AwaitingHostReset;
  action_after_us_ = now_us;
  return true;
}

void BleOwnerRecoveryState::note_terminate_result(TerminateResult result,
                                                  std::uint64_t now_us) {
  if (!pending()) {
    return;
  }
  switch (result) {
    case TerminateResult::Accepted:
      phase_ = Phase::AwaitingOwnerLoss;
      action_after_us_ = now_us + kOwnerLossTimeoutUs;
      return;
    case TerminateResult::Retryable:
      phase_ = Phase::Terminating;
      action_after_us_ = now_us + kRetryDelayUs;
      return;
    case TerminateResult::NotConnected:
      // ENOTCONN is only a result from the terminate request. It does not
      // prove that the adapter has already published an owner-generation
      // boundary. Keep the old reports quarantined until observe() sees the
      // authoritative lifecycle change; if it still sees the failed owner,
      // use the bounded host-reset fallback.
      phase_ = Phase::AwaitingHostReset;
      action_after_us_ = now_us;
      return;
    case TerminateResult::Failed:
      phase_ = Phase::AwaitingHostReset;
      action_after_us_ = now_us;
      return;
  }
}

void BleOwnerRecoveryState::note_host_reset_scheduled(std::uint64_t now_us) {
  if (phase_ != Phase::AwaitingHostReset) {
    return;
  }
  action_after_us_ = now_us + kHostResetRetryUs;
}

void BleOwnerRecoveryState::reset() {
  phase_ = Phase::Idle;
  target_ = {};
  action_after_us_ = 0;
  terminate_deadline_us_ = 0;
}

bool BleOwnerRecoveryState::pending() const {
  return phase_ != Phase::Idle;
}

BleOwnerRecoveryState::Phase BleOwnerRecoveryState::phase() const {
  return phase_;
}

BleOwnerToken BleOwnerRecoveryState::target() const {
  return target_;
}

}  // namespace ai_keyboard
