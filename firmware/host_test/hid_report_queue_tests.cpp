#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "keyboard/hid_report_queue.h"

namespace {

void test_preserves_report_order_and_payload() {
  ai_keyboard::HidReportQueue queue;
  const std::array<std::uint8_t, 2> first = {0x01, 0x04};
  const std::array<std::uint8_t, 3> second = {0x02, 0x00, 0x7F};
  std::uint32_t first_sequence = 0;
  std::uint32_t second_sequence = 0;

  assert(queue.push(1, first.data(), first.size(), 10, &first_sequence));
  assert(queue.push(2, second.data(), second.size(), 20, &second_sequence));
  assert(first_sequence != 0);
  assert(second_sequence > first_sequence);
  assert(queue.size() == 2);

  ai_keyboard::QueuedHidReport report;
  assert(queue.front(&report));
  assert(report.sequence == first_sequence);
  assert(report.report_id == 1);
  assert(report.len == first.size());
  assert(report.queued_ms == 10);
  assert(report.data[0] == first[0]);
  assert(report.data[1] == first[1]);
  assert(!queue.pop_if_sequence(second_sequence));
  assert(queue.pop_if_sequence(first_sequence));

  assert(queue.front(&report));
  assert(report.sequence == second_sequence);
  assert(report.report_id == 2);
  assert(report.len == second.size());
  assert(queue.pop_if_sequence(second_sequence));
  assert(queue.empty());
}

void test_rejects_invalid_or_full_reports() {
  ai_keyboard::HidReportQueue queue;
  std::array<std::uint8_t, ai_keyboard::kHidReportMaxPayload + 1> oversized{};
  assert(!queue.push(1, nullptr, 1, 0));
  assert(!queue.push(1, oversized.data(), oversized.size(), 0));

  const std::uint8_t value = 0x01;
  for (std::size_t i = 0; i < ai_keyboard::kHidReportQueueCapacity; ++i) {
    assert(queue.push(1, &value, 1, static_cast<std::uint32_t>(i)));
  }
  assert(!queue.push(1, &value, 1, 99));
}

void test_clear_removes_pending_reports() {
  ai_keyboard::HidReportQueue queue;
  assert(queue.push(1, nullptr, 0, 1));
  assert(!queue.empty());
  queue.clear();
  assert(queue.empty());
  assert(queue.size() == 0);
  ai_keyboard::QueuedHidReport report;
  assert(!queue.front(&report));
}

void test_reserved_slots_keep_keyboard_capacity_available() {
  ai_keyboard::HidReportQueue queue;
  const std::uint8_t value = 0x01;
  constexpr std::size_t reserved = 4;
  for (std::size_t i = 0; i < ai_keyboard::kHidReportQueueCapacity - reserved; ++i) {
    assert(queue.push(0x11, &value, 1, static_cast<std::uint32_t>(i), nullptr, reserved));
  }
  assert(!queue.push(0x11, &value, 1, 99, nullptr, reserved));
  for (std::size_t i = 0; i < reserved; ++i) {
    assert(queue.push(0x01, &value, 1, 100 + static_cast<std::uint32_t>(i)));
  }
  assert(queue.size() == ai_keyboard::kHidReportQueueCapacity);
}

void test_classified_snapshots_reserve_all_release_transitions_in_order() {
  ai_keyboard::HidReportQueue queue;
  std::vector<std::uint32_t> sequences;

  // Ordinary events leave one transition slot for every keyboard-producing
  // source (KEY1-8 plus the three encoder directions) to become released.
  for (std::size_t index = 0;
       index < ai_keyboard::kHidReportQueueCapacity -
                   ai_keyboard::kKeyboardStateSourceCount;
       ++index) {
    std::array<std::uint8_t, 8> report{};
    report[2] = static_cast<std::uint8_t>(index + 1);
    const auto result = queue.push_classified(
        1,
        report.data(),
        report.size(),
        static_cast<std::uint32_t>(index),
        ai_keyboard::HidReportClass::KeyboardPress);
    assert(result.status == ai_keyboard::HidQueuePushStatus::Queued);
    sequences.push_back(result.sequence);
  }

  std::array<std::uint8_t, 8> rejected_press{};
  rejected_press[2] = 0x7F;
  const auto rejected = queue.push_classified(
      1,
      rejected_press.data(),
      rejected_press.size(),
      30,
      ai_keyboard::HidReportClass::KeyboardPress);
  assert(rejected.status == ai_keyboard::HidQueuePushStatus::Full);

  // One partial release per remaining source followed by the final all-zero
  // transition exactly fills the reserved capacity.
  for (std::size_t remaining = ai_keyboard::kKeyboardStateSourceCount - 1;
       remaining > 0;
       --remaining) {
    std::array<std::uint8_t, 8> report{};
    report[2] = static_cast<std::uint8_t>(remaining);
    const auto result = queue.push_classified(
        1,
        report.data(),
        report.size(),
        40 + static_cast<std::uint32_t>(
                 ai_keyboard::kKeyboardStateSourceCount - remaining),
        ai_keyboard::HidReportClass::KeyboardRelease);
    assert(result.status == ai_keyboard::HidQueuePushStatus::Queued);
    sequences.push_back(result.sequence);
  }

  const std::array<std::uint8_t, 8> all_released{};
  const auto final_release = queue.push_classified(
      1,
      all_released.data(),
      all_released.size(),
      50,
      ai_keyboard::HidReportClass::KeyboardAllReleased);
  assert(final_release.status == ai_keyboard::HidQueuePushStatus::Queued);
  sequences.push_back(final_release.sequence);
  assert(queue.size() == ai_keyboard::kHidReportQueueCapacity);

  // Once the final state is already at the tail, another all-zero snapshot is
  // safely coalesced even though the queue is full.
  const auto duplicate_release = queue.push_classified(
      1,
      all_released.data(),
      all_released.size(),
      51,
      ai_keyboard::HidReportClass::KeyboardAllReleased);
  assert(duplicate_release.status == ai_keyboard::HidQueuePushStatus::Coalesced);
  assert(duplicate_release.sequence == final_release.sequence);
  assert(queue.size() == ai_keyboard::kHidReportQueueCapacity);

  for (const auto sequence : sequences) {
    ai_keyboard::QueuedHidReport report;
    assert(queue.front(&report));
    assert(report.sequence == sequence);
    assert(queue.pop_if_sequence(sequence));
  }
  assert(queue.empty());
}

void test_classified_snapshot_coalescing_and_validation() {
  ai_keyboard::HidReportQueue queue;
  std::array<std::uint8_t, 8> pressed{};
  pressed[0] = 0x02;
  pressed[2] = 0x04;

  const auto first = queue.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      10,
      ai_keyboard::HidReportClass::KeyboardPress);
  const auto duplicate = queue.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      11,
      ai_keyboard::HidReportClass::KeyboardRelease);
  assert(first.status == ai_keyboard::HidQueuePushStatus::Queued);
  assert(duplicate.status == ai_keyboard::HidQueuePushStatus::Coalesced);
  assert(duplicate.sequence == first.sequence);
  assert(queue.size() == 1);

  const auto invalid_release = queue.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      12,
      ai_keyboard::HidReportClass::KeyboardAllReleased);
  assert(invalid_release.status == ai_keyboard::HidQueuePushStatus::Invalid);
  assert(queue.size() == 1);

  const std::uint8_t incomplete = 0;
  assert(queue.push_classified(
                  1,
                  &incomplete,
                  1,
                  13,
                  ai_keyboard::HidReportClass::KeyboardRelease)
             .status == ai_keyboard::HidQueuePushStatus::Invalid);
}

void test_app_commands_leave_room_for_physical_presses_and_releases() {
  ai_keyboard::HidReportQueue queue;
  for (std::size_t index = 0;
       index < ai_keyboard::kHidReportQueueCapacity -
                   (2 * ai_keyboard::kKeyboardStateSourceCount);
       ++index) {
    const std::uint8_t value = static_cast<std::uint8_t>(index);
    const auto result = queue.push_classified(
        0x11,
        &value,
        1,
        static_cast<std::uint32_t>(index),
        ai_keyboard::HidReportClass::AppCommand);
    assert(result.status == ai_keyboard::HidQueuePushStatus::Queued);
  }
  const std::uint8_t extra = 0xFF;
  assert(queue.push_classified(0x11,
                               &extra,
                               1,
                               99,
                               ai_keyboard::HidReportClass::AppCommand)
             .status == ai_keyboard::HidQueuePushStatus::Full);

  for (std::size_t index = 0;
       index < ai_keyboard::kKeyboardStateSourceCount;
       ++index) {
    std::array<std::uint8_t, 8> pressed{};
    pressed[2] = static_cast<std::uint8_t>(index + 4);
    assert(queue.push_classified(
                    1,
                    pressed.data(),
                    pressed.size(),
                    100 + static_cast<std::uint32_t>(index),
                    ai_keyboard::HidReportClass::KeyboardPress)
               .accepted());
  }

  for (std::size_t remaining = ai_keyboard::kKeyboardStateSourceCount - 1;
       remaining > 0;
       --remaining) {
    std::array<std::uint8_t, 8> released{};
    released[2] = static_cast<std::uint8_t>(remaining);
    assert(queue.push_classified(
                    1,
                    released.data(),
                    released.size(),
                    200 + static_cast<std::uint32_t>(remaining),
                    ai_keyboard::HidReportClass::KeyboardRelease)
               .accepted());
  }

  const std::array<std::uint8_t, 8> all_released{};
  assert(queue.push_classified(
                  1,
                  all_released.data(),
                  all_released.size(),
                  100,
                  ai_keyboard::HidReportClass::KeyboardAllReleased)
             .accepted());
  assert(queue.size() == ai_keyboard::kHidReportQueueCapacity);
}

void test_mouse_wheel_queue_preserves_reversal_order() {
  ai_keyboard::MouseWheelQueue queue;
  bool coalesced = false;
  assert(queue.push(4, 0, 10, nullptr, &coalesced));
  assert(!coalesced);
  assert(queue.push(6, 0, 11, nullptr, &coalesced));
  assert(coalesced);
  assert(queue.push(-3, 0, 12, nullptr, &coalesced));
  assert(!coalesced);
  assert(queue.size() == 2);

  ai_keyboard::QueuedMouseWheel report;
  assert(queue.front(&report));
  assert(report.vertical == 10);
  assert(queue.consume_if_sequence(report.sequence, 7, 0));
  assert(queue.front(&report));
  assert(report.vertical == 3);
  assert(queue.consume_if_sequence(report.sequence, 3, 0));

  assert(queue.front(&report));
  assert(report.vertical == -3);
  assert(queue.consume_if_sequence(report.sequence, -3, 0));
  assert(queue.empty());
}

void test_mouse_wheel_queue_keeps_axes_and_rejects_invalid_consumption() {
  ai_keyboard::MouseWheelQueue queue;
  assert(queue.push(0, 5, 20));
  assert(queue.push(0, 3, 21));
  assert(queue.size() == 1);

  ai_keyboard::QueuedMouseWheel report;
  assert(queue.front(&report));
  assert(report.horizontal == 8);
  assert(!queue.consume_if_sequence(report.sequence, 0, -1));
  assert(!queue.consume_if_sequence(report.sequence, 0, 9));
  assert(queue.consume_if_sequence(report.sequence, 0, 8));
  assert(queue.empty());
}

void test_mouse_wheel_queue_saturates_without_overflow_and_stays_bounded() {
  ai_keyboard::MouseWheelQueue queue;
  bool coalesced = false;
  bool saturated = false;
  assert(queue.push(32000, 0, 1, nullptr, &coalesced, &saturated));
  assert(!coalesced);
  assert(!saturated);
  assert(queue.push(1000, 0, 2, nullptr, &coalesced, &saturated));
  assert(coalesced);
  assert(saturated);

  ai_keyboard::QueuedMouseWheel report;
  assert(queue.front(&report));
  assert(report.vertical == ai_keyboard::kMouseWheelAccumulationLimit);

  queue.clear();
  for (std::size_t index = 0;
       index < ai_keyboard::kMouseWheelQueueCapacity;
       ++index) {
    const int direction = index % 2 == 0 ? 1 : -1;
    assert(queue.push(direction, 0, static_cast<std::uint32_t>(index)));
  }
  assert(queue.size() == ai_keyboard::kMouseWheelQueueCapacity);
  assert(!queue.push(1, 0, 100));
  assert(queue.size() == ai_keyboard::kMouseWheelQueueCapacity);
}

void test_ble_owner_generation_is_preserved_and_never_coalesced_across_lifetimes() {
  const ai_keyboard::BleOwnerToken first_owner{7, 41};
  const ai_keyboard::BleOwnerToken reused_handle_owner{7, 43};

  ai_keyboard::HidReportQueue hid;
  std::array<std::uint8_t, 8> pressed{};
  pressed[2] = 0x04;
  const auto first = hid.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      1,
      ai_keyboard::HidReportClass::KeyboardPress,
      first_owner);
  const auto second = hid.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      2,
      ai_keyboard::HidReportClass::KeyboardPress,
      reused_handle_owner);
  assert(first.status == ai_keyboard::HidQueuePushStatus::Queued);
  assert(second.status == ai_keyboard::HidQueuePushStatus::Queued);
  assert(hid.size() == 2);

  ai_keyboard::QueuedHidReport hid_report;
  assert(hid.front(&hid_report));
  assert(hid_report.ble_owner == first_owner);
  assert(hid.pop_if_sequence(hid_report.sequence));
  assert(hid.front(&hid_report));
  assert(hid_report.ble_owner == reused_handle_owner);

  ai_keyboard::MouseWheelQueue wheel;
  bool coalesced = false;
  assert(wheel.push(
      3, 0, 1, nullptr, &coalesced, nullptr, first_owner));
  assert(!coalesced);
  assert(wheel.push(
      4, 0, 2, nullptr, &coalesced, nullptr, reused_handle_owner));
  assert(!coalesced);
  assert(wheel.size() == 2);

  ai_keyboard::QueuedMouseWheel wheel_report;
  assert(wheel.front(&wheel_report));
  assert(wheel_report.ble_owner == first_owner);
}

void test_usb_epoch_is_preserved_and_never_coalesced_across_lifetimes() {
  constexpr std::uint32_t first_epoch = 17;
  constexpr std::uint32_t remounted_epoch = 19;

  ai_keyboard::HidReportQueue hid;
  std::array<std::uint8_t, 8> pressed{};
  pressed[2] = 0x04;
  const auto first = hid.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      1,
      ai_keyboard::HidReportClass::KeyboardPress,
      {},
      first_epoch);
  const auto duplicate_same_lifetime = hid.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      2,
      ai_keyboard::HidReportClass::KeyboardPress,
      {},
      first_epoch);
  const auto same_snapshot_after_remount = hid.push_classified(
      1,
      pressed.data(),
      pressed.size(),
      3,
      ai_keyboard::HidReportClass::KeyboardPress,
      {},
      remounted_epoch);
  assert(first.status == ai_keyboard::HidQueuePushStatus::Queued);
  assert(duplicate_same_lifetime.status ==
         ai_keyboard::HidQueuePushStatus::Coalesced);
  assert(same_snapshot_after_remount.status ==
         ai_keyboard::HidQueuePushStatus::Queued);
  assert(hid.size() == 2);

  ai_keyboard::QueuedHidReport hid_report;
  assert(hid.front(&hid_report));
  assert(hid_report.usb_epoch == first_epoch);
  assert(hid.pop_if_sequence(hid_report.sequence));
  assert(hid.front(&hid_report));
  assert(hid_report.usb_epoch == remounted_epoch);

  ai_keyboard::MouseWheelQueue wheel;
  bool coalesced = false;
  assert(wheel.push(
      3, 0, 1, nullptr, &coalesced, nullptr, {}, first_epoch));
  assert(!coalesced);
  assert(wheel.push(
      4, 0, 2, nullptr, &coalesced, nullptr, {}, remounted_epoch));
  assert(!coalesced);
  assert(wheel.size() == 2);

  ai_keyboard::QueuedMouseWheel wheel_report;
  assert(wheel.front(&wheel_report));
  assert(wheel_report.usb_epoch == first_epoch);
}

}  // namespace

int main() {
  test_preserves_report_order_and_payload();
  test_rejects_invalid_or_full_reports();
  test_clear_removes_pending_reports();
  test_reserved_slots_keep_keyboard_capacity_available();
  test_classified_snapshots_reserve_all_release_transitions_in_order();
  test_classified_snapshot_coalescing_and_validation();
  test_app_commands_leave_room_for_physical_presses_and_releases();
  test_mouse_wheel_queue_preserves_reversal_order();
  test_mouse_wheel_queue_keeps_axes_and_rejects_invalid_consumption();
  test_mouse_wheel_queue_saturates_without_overflow_and_stays_bounded();
  test_ble_owner_generation_is_preserved_and_never_coalesced_across_lifetimes();
  test_usb_epoch_is_preserved_and_never_coalesced_across_lifetimes();
  return 0;
}
