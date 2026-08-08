#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "keyboard/hid_report_queue.h"

namespace ai_keyboard {

enum class BleInputTxResult : std::uint8_t {
  Accepted,
  DroppedUnsupported,
  RetryableNoBuffer,
  RetryableBusy,
  Disconnected,
  Fatal,
};

enum class BleInputPollResult : std::uint8_t {
  Idle,
  Throttled,
  Accepted,
  DroppedUnsupported,
  Retryable,
  Disconnected,
  Fatal,
};

enum class BleScheduledReportKind : std::uint8_t {
  Hid,
  MouseWheel,
};

struct BleScheduledReport {
  BleScheduledReportKind kind = BleScheduledReportKind::Hid;
  std::uint32_t sequence = 0;
  std::uint8_t report_id = 0;
  std::array<std::uint8_t, kHidReportMaxPayload> data{};
  std::size_t len = 0;
  HidReportClass report_class = HidReportClass::AppCommand;
  std::int8_t wheel_vertical = 0;
  std::int8_t wheel_horizontal = 0;
  BleOwnerToken ble_owner{};
};

struct BleInputSchedulerConfig {
  // BLE connection intervals are expressed in 1.25 ms units. The scheduler
  // uses a conservative single notification credit per interval.
  std::uint64_t fallback_period_us = 30000;
  std::uint64_t minimum_period_us = 7500;
  std::uint64_t maximum_period_us = 200000;
  std::uint64_t maximum_retry_backoff_us = 200000;
  std::uint8_t maximum_retry_shift = 2;
  std::uint8_t maximum_consecutive_hid = 3;
  std::int32_t wheel_chunk_limit = 127;
};

struct BleInputSchedulerDiagnostics {
  std::uint32_t poll_attempts = 0;
  std::uint32_t accepted = 0;
  std::uint32_t dropped_unsupported = 0;
  std::uint32_t hid_dropped_unsupported = 0;
  std::uint32_t wheel_dropped_unsupported = 0;
  std::uint32_t retryable_no_buffer = 0;
  std::uint32_t retryable_busy = 0;
  std::uint32_t disconnected = 0;
  std::uint32_t fatal = 0;
  std::uint32_t hid_accepted = 0;
  std::uint32_t wheel_accepted = 0;
  std::uint32_t retry_streak_peak = 0;
  std::size_t hid_queue_high_watermark = 0;
  std::size_t wheel_queue_high_watermark = 0;
};

using BleInputTransmitCallback =
    BleInputTxResult (*)(void* context, const BleScheduledReport& report);

// Platform-independent BLE input producer/consumer scheduler.
//
// The caller owns synchronization around the two queues and must not mutate
// their head entries while poll() invokes the callback. The callback must be
// non-blocking. A report is removed after Accepted or when the active HID
// protocol cannot represent an optional report. Retryable results retain the
// exact head transition for the next scheduled attempt. Fatal standard-input
// failures remain at the head until the caller tears down that owner lifetime.
class BleInputScheduler {
 public:
  explicit BleInputScheduler(
      const BleInputSchedulerConfig& config = BleInputSchedulerConfig{});

  void set_connection_interval(std::uint16_t interval_units, bool valid);
  std::uint64_t delivery_period_us() const;

  void reset_delivery_clock();
  void mark_disconnected();
  bool take_state_resync_required();

  BleInputPollResult poll(std::uint64_t now_us,
                          HidReportQueue* hid_reports,
                          MouseWheelQueue* wheel_reports,
                          BleInputTransmitCallback callback,
                          void* callback_context);

  const BleInputSchedulerDiagnostics& diagnostics() const;

 private:
  bool should_send_hid(const QueuedHidReport& hid_report,
                       bool has_wheel_report) const;
  BleScheduledReport scheduled_hid(const QueuedHidReport& report) const;
  BleScheduledReport scheduled_wheel(const QueuedMouseWheel& report) const;
  std::uint64_t retry_delay_us() const;
  void set_next_attempt(std::uint64_t now_us, std::uint64_t delay_us);
  void note_retry();

  BleInputSchedulerConfig config_;
  BleInputSchedulerDiagnostics diagnostics_;
  std::uint64_t delivery_period_us_ = 0;
  std::uint64_t next_attempt_us_ = 0;
  std::uint64_t last_attempt_us_ = 0;
  std::uint32_t retry_streak_ = 0;
  std::uint8_t consecutive_hid_ = 0;
  bool has_attempted_ = false;
  bool state_resync_required_ = false;
};

}  // namespace ai_keyboard
