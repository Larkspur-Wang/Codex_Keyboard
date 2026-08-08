#include "keyboard/ble_input_scheduler.h"

#include <algorithm>
#include <limits>

namespace ai_keyboard {
namespace {

constexpr std::uint64_t kBleConnectionIntervalUnitUs = 1250;

std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return lhs + rhs;
}

bool release_must_keep_order(HidReportClass report_class) {
  return report_class == HidReportClass::KeyboardRelease ||
         report_class == HidReportClass::KeyboardAllReleased ||
         report_class == HidReportClass::AppCommandStatefulRelease;
}

}  // namespace

BleInputScheduler::BleInputScheduler(const BleInputSchedulerConfig& config)
    : config_(config) {
  if (config_.minimum_period_us == 0) {
    config_.minimum_period_us = 1;
  }
  if (config_.maximum_period_us < config_.minimum_period_us) {
    config_.maximum_period_us = config_.minimum_period_us;
  }
  config_.fallback_period_us = std::clamp(config_.fallback_period_us,
                                          config_.minimum_period_us,
                                          config_.maximum_period_us);
  if (config_.maximum_retry_backoff_us < config_.minimum_period_us) {
    config_.maximum_retry_backoff_us = config_.minimum_period_us;
  }
  if (config_.maximum_consecutive_hid == 0) {
    config_.maximum_consecutive_hid = 1;
  }
  config_.maximum_retry_shift =
      std::min<std::uint8_t>(config_.maximum_retry_shift, 20);
  config_.wheel_chunk_limit =
      std::clamp<std::int32_t>(config_.wheel_chunk_limit, 1, 127);
  delivery_period_us_ = config_.fallback_period_us;
}

void BleInputScheduler::set_connection_interval(std::uint16_t interval_units,
                                                bool valid) {
  std::uint64_t period_us = config_.fallback_period_us;
  if (valid && interval_units != 0) {
    period_us = static_cast<std::uint64_t>(interval_units) *
                kBleConnectionIntervalUnitUs;
    period_us = std::clamp(period_us,
                           config_.minimum_period_us,
                           config_.maximum_period_us);
  }
  if (period_us == delivery_period_us_) {
    return;
  }
  delivery_period_us_ = period_us;
  if (has_attempted_) {
    const std::uint64_t rebased_deadline = saturating_add(
        last_attempt_us_,
        retry_streak_ == 0 ? delivery_period_us_ : retry_delay_us());
    next_attempt_us_ =
        retry_streak_ == 0
            ? rebased_deadline
            : std::max(next_attempt_us_, rebased_deadline);
  }
}

std::uint64_t BleInputScheduler::delivery_period_us() const {
  return delivery_period_us_;
}

void BleInputScheduler::reset_delivery_clock() {
  next_attempt_us_ = 0;
  last_attempt_us_ = 0;
  retry_streak_ = 0;
  consecutive_hid_ = 0;
  has_attempted_ = false;
}

void BleInputScheduler::mark_disconnected() {
  state_resync_required_ = true;
  reset_delivery_clock();
}

bool BleInputScheduler::take_state_resync_required() {
  const bool required = state_resync_required_;
  state_resync_required_ = false;
  return required;
}

BleInputPollResult BleInputScheduler::poll(
    std::uint64_t now_us,
    HidReportQueue* hid_reports,
    MouseWheelQueue* wheel_reports,
    BleInputTransmitCallback callback,
    void* callback_context) {
  if (hid_reports == nullptr || wheel_reports == nullptr || callback == nullptr) {
    return BleInputPollResult::Fatal;
  }

  diagnostics_.hid_queue_high_watermark =
      std::max(diagnostics_.hid_queue_high_watermark, hid_reports->size());
  diagnostics_.wheel_queue_high_watermark =
      std::max(diagnostics_.wheel_queue_high_watermark, wheel_reports->size());

  QueuedHidReport hid_report;
  QueuedMouseWheel wheel_report;
  const bool has_hid_report = hid_reports->front(&hid_report);
  const bool has_wheel_report = wheel_reports->front(&wheel_report);
  if (!has_hid_report && !has_wheel_report) {
    return BleInputPollResult::Idle;
  }
  if (has_attempted_ && now_us < next_attempt_us_) {
    return BleInputPollResult::Throttled;
  }

  const bool send_hid =
      has_hid_report && should_send_hid(hid_report, has_wheel_report);
  const BleScheduledReport scheduled =
      send_hid ? scheduled_hid(hid_report) : scheduled_wheel(wheel_report);

  ++diagnostics_.poll_attempts;
  has_attempted_ = true;
  last_attempt_us_ = now_us;
  const auto tx_result = callback(callback_context, scheduled);
  switch (tx_result) {
    case BleInputTxResult::Accepted:
      if (send_hid) {
        if (hid_reports->pop_if_sequence(hid_report.sequence)) {
          ++diagnostics_.accepted;
          ++diagnostics_.hid_accepted;
          if (consecutive_hid_ < std::numeric_limits<std::uint8_t>::max()) {
            ++consecutive_hid_;
          }
        }
      } else {
        if (wheel_reports->consume_if_sequence(
                wheel_report.sequence,
                scheduled.wheel_vertical,
                scheduled.wheel_horizontal)) {
          ++diagnostics_.accepted;
          ++diagnostics_.wheel_accepted;
          consecutive_hid_ = 0;
        }
      }
      retry_streak_ = 0;
      set_next_attempt(now_us, delivery_period_us_);
      return BleInputPollResult::Accepted;

    case BleInputTxResult::DroppedUnsupported:
      if (send_hid) {
        if (hid_reports->pop_if_sequence(hid_report.sequence)) {
          ++diagnostics_.dropped_unsupported;
          ++diagnostics_.hid_dropped_unsupported;
          if (consecutive_hid_ < std::numeric_limits<std::uint8_t>::max()) {
            ++consecutive_hid_;
          }
        }
      } else {
        // Relative movement that the active protocol cannot represent is
        // stale once that protocol changes. Drop the complete direction run
        // rather than retrying every 127-count chunk forever.
        if (wheel_reports->pop_if_sequence(wheel_report.sequence)) {
          ++diagnostics_.dropped_unsupported;
          ++diagnostics_.wheel_dropped_unsupported;
          consecutive_hid_ = 0;
        }
      }
      retry_streak_ = 0;
      set_next_attempt(now_us, delivery_period_us_);
      return BleInputPollResult::DroppedUnsupported;

    case BleInputTxResult::RetryableNoBuffer:
      ++diagnostics_.retryable_no_buffer;
      note_retry();
      set_next_attempt(now_us, retry_delay_us());
      return BleInputPollResult::Retryable;

    case BleInputTxResult::RetryableBusy:
      ++diagnostics_.retryable_busy;
      note_retry();
      set_next_attempt(now_us, retry_delay_us());
      return BleInputPollResult::Retryable;

    case BleInputTxResult::Disconnected:
      ++diagnostics_.disconnected;
      state_resync_required_ = true;
      note_retry();
      set_next_attempt(now_us, retry_delay_us());
      return BleInputPollResult::Disconnected;

    case BleInputTxResult::Fatal:
      ++diagnostics_.fatal;
      note_retry();
      set_next_attempt(now_us, retry_delay_us());
      return BleInputPollResult::Fatal;
  }

  return BleInputPollResult::Fatal;
}

const BleInputSchedulerDiagnostics& BleInputScheduler::diagnostics() const {
  return diagnostics_;
}

bool BleInputScheduler::should_send_hid(
    const QueuedHidReport& hid_report,
    bool has_wheel_report) const {
  if (!has_wheel_report) {
    return true;
  }
  if (release_must_keep_order(hid_report.report_class)) {
    return true;
  }
  return consecutive_hid_ < config_.maximum_consecutive_hid;
}

BleScheduledReport BleInputScheduler::scheduled_hid(
    const QueuedHidReport& report) const {
  BleScheduledReport scheduled;
  scheduled.kind = BleScheduledReportKind::Hid;
  scheduled.sequence = report.sequence;
  scheduled.report_id = report.report_id;
  scheduled.data = report.data;
  scheduled.len = report.len;
  scheduled.report_class = report.report_class;
  scheduled.ble_owner = report.ble_owner;
  return scheduled;
}

BleScheduledReport BleInputScheduler::scheduled_wheel(
    const QueuedMouseWheel& report) const {
  BleScheduledReport scheduled;
  scheduled.kind = BleScheduledReportKind::MouseWheel;
  scheduled.sequence = report.sequence;
  scheduled.wheel_vertical = static_cast<std::int8_t>(
      std::clamp<std::int32_t>(
          report.vertical, -config_.wheel_chunk_limit, config_.wheel_chunk_limit));
  scheduled.wheel_horizontal = static_cast<std::int8_t>(
      std::clamp<std::int32_t>(
          report.horizontal, -config_.wheel_chunk_limit, config_.wheel_chunk_limit));
  scheduled.ble_owner = report.ble_owner;
  return scheduled;
}

std::uint64_t BleInputScheduler::retry_delay_us() const {
  const std::uint32_t shift =
      retry_streak_ == 0
          ? 0
          : std::min<std::uint32_t>(
                retry_streak_ - 1, config_.maximum_retry_shift);
  const std::uint64_t maximum = config_.maximum_retry_backoff_us;
  if (delivery_period_us_ > (maximum >> shift)) {
    return maximum;
  }
  return std::min(delivery_period_us_ << shift, maximum);
}

void BleInputScheduler::set_next_attempt(std::uint64_t now_us,
                                         std::uint64_t delay_us) {
  next_attempt_us_ = saturating_add(now_us, delay_us);
}

void BleInputScheduler::note_retry() {
  if (retry_streak_ < std::numeric_limits<std::uint32_t>::max()) {
    ++retry_streak_;
  }
  diagnostics_.retry_streak_peak =
      std::max(diagnostics_.retry_streak_peak, retry_streak_);
}

}  // namespace ai_keyboard
