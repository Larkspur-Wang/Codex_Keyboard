#pragma once

#include <cstdint>

#include "keyboard/hid_report_queue.h"

namespace ai_keyboard {

// Pure state machine for rebuilding a BLE HID owner after a permanent send
// failure. The platform layer owns the actual GAP terminate call; this class
// only decides when that call is needed and when an authoritative adapter
// lifetime proves that recovery is complete.
class BleOwnerRecoveryState {
 public:
  static constexpr std::uint64_t kRetryDelayUs = 250000;
  static constexpr std::uint64_t kTerminateRetryBudgetUs = 1000000;
  static constexpr std::uint64_t kOwnerLossTimeoutUs = 1000000;
  static constexpr std::uint64_t kHostResetRetryUs = 1000000;

  enum class Phase : std::uint8_t {
    Idle,
    Terminating,
    AwaitingOwnerLoss,
    AwaitingHostReset,
  };

  enum class Action : std::uint8_t {
    None,
    RequestTerminate,
    RequestHostReset,
    Completed,
  };

  enum class TerminateResult : std::uint8_t {
    Accepted,
    Retryable,
    NotConnected,
    Failed,
  };

  bool begin(BleOwnerToken target, std::uint64_t now_us);

  // Reconcile against the adapter's lock-protected lifecycle snapshot. A
  // disconnected adapter or any different generation is proof that the
  // failed owner can no longer retain host key state.
  Action observe(bool snapshot_valid,
                 bool adapter_connected,
                 BleOwnerToken adapter_owner,
                 std::uint64_t now_us);

  // TERM_FAILURE means the old connection is still established. Make the
  // next main-task observation retry immediately; never clear recovery here.
  bool note_term_failure(std::uint16_t conn_handle, std::uint64_t now_us);

  void note_terminate_result(TerminateResult result, std::uint64_t now_us);
  void note_host_reset_scheduled(std::uint64_t now_us);
  void reset();

  bool pending() const;
  Phase phase() const;
  BleOwnerToken target() const;

 private:
  Phase phase_ = Phase::Idle;
  BleOwnerToken target_{};
  std::uint64_t action_after_us_ = 0;
  std::uint64_t terminate_deadline_us_ = 0;
};

}  // namespace ai_keyboard
