#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "keyboard/ble_input_scheduler.h"

namespace {

struct FakeTransmitter {
  std::vector<ai_keyboard::BleScheduledReport> attempts;
  std::vector<ai_keyboard::BleInputTxResult> results;
  std::size_t next_result = 0;
};

ai_keyboard::BleInputTxResult transmit_fake(
    void* context,
    const ai_keyboard::BleScheduledReport& report) {
  auto* fake = static_cast<FakeTransmitter*>(context);
  assert(fake != nullptr);
  fake->attempts.push_back(report);
  if (fake->next_result < fake->results.size()) {
    return fake->results[fake->next_result++];
  }
  return ai_keyboard::BleInputTxResult::Accepted;
}

ai_keyboard::HidQueuePushResult push_key(
    ai_keyboard::HidReportQueue* queue,
    std::uint8_t value,
    ai_keyboard::HidReportClass report_class,
    ai_keyboard::BleOwnerToken ble_owner = {}) {
  std::array<std::uint8_t, 8> report{};
  if (report_class != ai_keyboard::HidReportClass::KeyboardAllReleased) {
    report[2] = value;
  }
  return queue->push_classified(
      1, report.data(), report.size(), value, report_class, ble_owner);
}

void test_first_report_is_immediate_and_interval_is_negotiated() {
  ai_keyboard::BleInputScheduler scheduler;
  scheduler.set_connection_interval(12, true);
  assert(scheduler.delivery_period_us() == 15000);

  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  assert(push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 5, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  FakeTransmitter fake;

  assert(scheduler.poll(100, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts.size() == 1);
  assert(hid.size() == 1);
  assert(scheduler.poll(15099, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Throttled);
  assert(fake.attempts.size() == 1);
  assert(scheduler.poll(15100, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts.size() == 2);
  assert(hid.empty());

  scheduler.reset_delivery_clock();
  scheduler.set_connection_interval(36, true);
  assert(scheduler.delivery_period_us() == 45000);
  assert(push_key(&hid, 6, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 7, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(scheduler.poll(200000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(scheduler.poll(244999, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Throttled);
  assert(scheduler.poll(245000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);

  scheduler.reset_delivery_clock();
  scheduler.set_connection_interval(0, false);
  assert(scheduler.delivery_period_us() == 30000);
}

void test_retryable_result_never_pops_or_reorders_release() {
  ai_keyboard::BleInputScheduler scheduler;
  scheduler.set_connection_interval(12, true);
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  const auto press =
      push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress);
  const auto release =
      push_key(&hid, 0, ai_keyboard::HidReportClass::KeyboardAllReleased);
  assert(press.accepted());
  assert(release.accepted());

  FakeTransmitter fake;
  fake.results = {
      ai_keyboard::BleInputTxResult::RetryableNoBuffer,
      ai_keyboard::BleInputTxResult::Accepted,
      ai_keyboard::BleInputTxResult::Accepted,
  };

  assert(scheduler.poll(0, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Retryable);
  assert(fake.attempts.size() == 1);
  assert(fake.attempts[0].sequence == press.sequence);
  assert(hid.size() == 2);

  assert(scheduler.poll(14999, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Throttled);
  assert(fake.attempts.size() == 1);
  assert(scheduler.poll(15000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts.size() == 2);
  assert(fake.attempts[1].sequence == press.sequence);
  assert(hid.size() == 1);

  assert(scheduler.poll(30000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts.back().sequence == release.sequence);
  assert(fake.attempts.back().report_class ==
         ai_keyboard::HidReportClass::KeyboardAllReleased);
  assert(hid.empty());

  const auto& diagnostics = scheduler.diagnostics();
  assert(diagnostics.retryable_no_buffer == 1);
  assert(diagnostics.retry_streak_peak == 1);
  assert(diagnostics.accepted == 2);
}

void test_retry_backoff_is_bounded_and_one_attempt_per_poll() {
  ai_keyboard::BleInputSchedulerConfig config;
  config.maximum_retry_shift = 2;
  config.maximum_retry_backoff_us = 50000;
  ai_keyboard::BleInputScheduler scheduler(config);
  scheduler.set_connection_interval(12, true);
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  assert(push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress).accepted());

  FakeTransmitter fake;
  fake.results = {
      ai_keyboard::BleInputTxResult::RetryableBusy,
      ai_keyboard::BleInputTxResult::RetryableBusy,
      ai_keyboard::BleInputTxResult::RetryableBusy,
      ai_keyboard::BleInputTxResult::Accepted,
  };

  assert(scheduler.poll(0, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Retryable);
  assert(fake.attempts.size() == 1);
  assert(scheduler.poll(15000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Retryable);
  assert(fake.attempts.size() == 2);
  // Production publishes the same negotiated interval before every poll.
  // Repeating an unchanged value must not collapse the retry backoff.
  scheduler.set_connection_interval(12, true);
  assert(scheduler.poll(44999, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Throttled);

  // A real interval change during retry must not shorten an already-issued
  // backoff deadline.
  scheduler.set_connection_interval(6, true);
  assert(scheduler.delivery_period_us() == 7500);
  assert(scheduler.poll(44999, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Throttled);
  assert(scheduler.poll(45000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Retryable);
  assert(fake.attempts.size() == 3);

  // Increasing the interval while retrying extends the deadline using the
  // retry backoff (capped at 50 ms), rather than collapsing it to one period.
  scheduler.set_connection_interval(36, true);
  assert(scheduler.poll(94999, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Throttled);
  assert(scheduler.poll(95000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts.size() == 4);
  assert(hid.empty());
}

void test_wheel_fairness_and_release_priority() {
  ai_keyboard::BleInputScheduler scheduler;
  scheduler.set_connection_interval(12, true);
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  assert(push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 5, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 6, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 7, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(wheel.push(300, 0, 1));
  FakeTransmitter fake;

  assert(scheduler.poll(0, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(scheduler.poll(15000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(scheduler.poll(30000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(scheduler.poll(45000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts[0].kind == ai_keyboard::BleScheduledReportKind::Hid);
  assert(fake.attempts[1].kind == ai_keyboard::BleScheduledReportKind::Hid);
  assert(fake.attempts[2].kind == ai_keyboard::BleScheduledReportKind::Hid);
  assert(fake.attempts[3].kind ==
         ai_keyboard::BleScheduledReportKind::MouseWheel);
  assert(fake.attempts[3].wheel_vertical == 127);

  ai_keyboard::QueuedMouseWheel remaining;
  assert(wheel.front(&remaining));
  assert(remaining.vertical == 173);

  // A release at the HID head is never moved behind a wheel report, even
  // after the fairness budget has been consumed.
  scheduler.reset_delivery_clock();
  hid.clear();
  wheel.clear();
  assert(push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 5, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 6, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  const std::array<std::uint8_t, 2> app_hotkey_release{2, 0};
  assert(hid.push_classified(
                0x11,
                app_hotkey_release.data(),
                app_hotkey_release.size(),
                0,
                ai_keyboard::HidReportClass::AppCommandStatefulRelease)
             .accepted());
  assert(wheel.push(1, 0, 1));

  const std::size_t attempt_offset = fake.attempts.size();
  assert(scheduler.poll(100000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(scheduler.poll(115000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(scheduler.poll(130000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(scheduler.poll(145000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts[attempt_offset + 3].kind ==
         ai_keyboard::BleScheduledReportKind::Hid);
  assert(fake.attempts[attempt_offset + 3].report_class ==
         ai_keyboard::HidReportClass::AppCommandStatefulRelease);
}

void test_interval_update_rebases_deadline_and_disconnect_requests_resync() {
  ai_keyboard::BleInputScheduler scheduler;
  scheduler.set_connection_interval(36, true);
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  assert(push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  assert(push_key(&hid, 5, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  FakeTransmitter fake;

  assert(scheduler.poll(0, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  scheduler.set_connection_interval(12, true);
  assert(scheduler.poll(14999, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Throttled);
  assert(scheduler.poll(15000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);

  assert(push_key(&hid, 6, ai_keyboard::HidReportClass::KeyboardPress).accepted());
  fake.results.push_back(ai_keyboard::BleInputTxResult::Disconnected);
  // Consume the result appended after prior implicit Accepted calls.
  fake.next_result = fake.results.size() - 1;
  assert(scheduler.poll(30000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Disconnected);
  assert(hid.size() == 1);
  assert(scheduler.take_state_resync_required());
  assert(!scheduler.take_state_resync_required());

  scheduler.mark_disconnected();
  assert(scheduler.take_state_resync_required());
}

void test_rapid_taps_and_wheel_bursts_drain_without_loss_or_reordering() {
  ai_keyboard::BleInputScheduler scheduler;
  scheduler.set_connection_interval(12, true);  // 15 ms negotiated interval.
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  FakeTransmitter fake;

  constexpr std::uint64_t kInputEdgePeriodUs = 20000;
  constexpr std::size_t kInputEdgeCount = 200;  // 100 complete taps.
  constexpr std::uint64_t kWheelPeriodUs = 100000;
  constexpr std::size_t kWheelBurstCount = 40;
  std::size_t next_edge = 0;
  std::size_t next_wheel = 0;

  // A 25-tap/s burst plus wheel traffic remains below one notification per
  // negotiated interval, but is fast enough to exercise buffering and HID /
  // wheel fairness. Polling more frequently must not consume extra credits.
  for (std::uint64_t now_us = 0; now_us <= 6000000; now_us += 1000) {
    while (next_edge < kInputEdgeCount &&
           now_us >= next_edge * kInputEdgePeriodUs) {
      const bool pressed = (next_edge % 2) == 0;
      const auto result = push_key(
          &hid,
          pressed ? 4 : 0,
          pressed ? ai_keyboard::HidReportClass::KeyboardPress
                  : ai_keyboard::HidReportClass::KeyboardAllReleased);
      assert(result.accepted());
      ++next_edge;
    }
    while (next_wheel < kWheelBurstCount &&
           now_us >= next_wheel * kWheelPeriodUs) {
      assert(wheel.push(3, 0, static_cast<std::uint32_t>(now_us / 1000)));
      ++next_wheel;
    }
    scheduler.poll(now_us, &hid, &wheel, transmit_fake, &fake);
  }

  assert(next_edge == kInputEdgeCount);
  assert(next_wheel == kWheelBurstCount);
  assert(hid.empty());
  assert(wheel.empty());

  std::size_t delivered_edges = 0;
  int delivered_wheel = 0;
  for (const auto& attempt : fake.attempts) {
    if (attempt.kind == ai_keyboard::BleScheduledReportKind::MouseWheel) {
      delivered_wheel += attempt.wheel_vertical;
      continue;
    }
    const bool expected_pressed = (delivered_edges % 2) == 0;
    assert(attempt.report_class ==
           (expected_pressed ? ai_keyboard::HidReportClass::KeyboardPress
                             : ai_keyboard::HidReportClass::KeyboardAllReleased));
    assert(attempt.data[2] == (expected_pressed ? 4 : 0));
    ++delivered_edges;
  }
  assert(delivered_edges == kInputEdgeCount);
  assert(delivered_wheel == static_cast<int>(kWheelBurstCount * 3));

  const auto& diagnostics = scheduler.diagnostics();
  assert(diagnostics.accepted == fake.attempts.size());
  assert(diagnostics.retryable_no_buffer == 0);
  assert(diagnostics.retryable_busy == 0);
  assert(diagnostics.disconnected == 0);
  assert(diagnostics.fatal == 0);
  assert(diagnostics.hid_queue_high_watermark <
         ai_keyboard::kHidReportQueueCapacity);
}

void test_exact_ble_owner_token_reaches_transmitter_and_stale_send_is_retained() {
  const ai_keyboard::BleOwnerToken owner{9, 101};
  ai_keyboard::BleInputScheduler scheduler;
  scheduler.set_connection_interval(12, true);
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  assert(push_key(
             &hid,
             4,
             ai_keyboard::HidReportClass::KeyboardPress,
             owner)
             .accepted());

  FakeTransmitter fake;
  fake.results = {ai_keyboard::BleInputTxResult::Disconnected};
  assert(scheduler.poll(0, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Disconnected);
  assert(fake.attempts.size() == 1);
  assert(fake.attempts[0].ble_owner == owner);
  assert(hid.size() == 1);

  scheduler.reset_delivery_clock();
  fake.results = {ai_keyboard::BleInputTxResult::Accepted};
  fake.next_result = 0;
  assert(scheduler.poll(1, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(fake.attempts.back().ble_owner == owner);
  assert(hid.empty());
}

void test_unsupported_optional_report_does_not_poison_queue_head() {
  ai_keyboard::BleInputScheduler scheduler;
  scheduler.set_connection_interval(12, true);
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;

  std::array<std::uint8_t, 4> app_command{1, 2, 3, 4};
  const auto optional = hid.push_classified(
      0x11,
      app_command.data(),
      app_command.size(),
      0,
      ai_keyboard::HidReportClass::AppCommand);
  const auto key =
      push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress);
  assert(optional.accepted());
  assert(key.accepted());

  FakeTransmitter fake;
  fake.results = {
      ai_keyboard::BleInputTxResult::DroppedUnsupported,
      ai_keyboard::BleInputTxResult::Accepted,
  };
  assert(scheduler.poll(0, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::DroppedUnsupported);
  assert(hid.size() == 1);
  assert(scheduler.poll(15000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Accepted);
  assert(hid.empty());
  assert(fake.attempts.size() == 2);
  assert(fake.attempts[1].sequence == key.sequence);

  assert(wheel.push(300, 0, 0));
  fake.results = {ai_keyboard::BleInputTxResult::DroppedUnsupported};
  fake.next_result = 0;
  assert(scheduler.poll(30000, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::DroppedUnsupported);
  assert(wheel.empty());

  const auto& diagnostics = scheduler.diagnostics();
  assert(diagnostics.dropped_unsupported == 2);
  assert(diagnostics.hid_dropped_unsupported == 1);
  assert(diagnostics.wheel_dropped_unsupported == 1);
}

void test_fatal_standard_input_is_retained_for_owner_recovery() {
  ai_keyboard::BleInputScheduler scheduler;
  ai_keyboard::HidReportQueue hid;
  ai_keyboard::MouseWheelQueue wheel;
  const auto key =
      push_key(&hid, 4, ai_keyboard::HidReportClass::KeyboardPress);
  assert(key.accepted());

  FakeTransmitter fake;
  fake.results = {ai_keyboard::BleInputTxResult::Fatal};
  assert(scheduler.poll(0, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Fatal);
  assert(hid.size() == 1);
  ai_keyboard::QueuedHidReport retained;
  assert(hid.front(&retained));
  assert(retained.sequence == key.sequence);

  hid.clear();
  scheduler.reset_delivery_clock();
  const std::array<std::uint8_t, 2> stateful_release{2, 0};
  const auto app_release = hid.push_classified(
      0x11,
      stateful_release.data(),
      stateful_release.size(),
      1,
      ai_keyboard::HidReportClass::AppCommandStatefulRelease);
  assert(app_release.accepted());
  fake.results = {ai_keyboard::BleInputTxResult::Fatal};
  fake.next_result = 0;
  assert(scheduler.poll(1, &hid, &wheel, transmit_fake, &fake) ==
         ai_keyboard::BleInputPollResult::Fatal);
  assert(hid.front(&retained));
  assert(retained.sequence == app_release.sequence);
  assert(retained.report_class ==
         ai_keyboard::HidReportClass::AppCommandStatefulRelease);
}

}  // namespace

int main() {
  test_first_report_is_immediate_and_interval_is_negotiated();
  test_retryable_result_never_pops_or_reorders_release();
  test_retry_backoff_is_bounded_and_one_attempt_per_poll();
  test_wheel_fairness_and_release_priority();
  test_interval_update_rebases_deadline_and_disconnect_requests_resync();
  test_rapid_taps_and_wheel_bursts_drain_without_loss_or_reordering();
  test_exact_ble_owner_token_reaches_transmitter_and_stale_send_is_retained();
  test_unsupported_optional_report_does_not_poison_queue_head();
  test_fatal_standard_input_is_retained_for_owner_recovery();
  return 0;
}
