#include "keyboard/hid_report_queue.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace ai_keyboard {
namespace {

constexpr std::size_t classified_capacity(HidReportClass report_class) {
  switch (report_class) {
    case HidReportClass::KeyboardPress:
      return kHidReportQueueCapacity - kKeyboardStateSourceCount;
    case HidReportClass::AppCommand:
    case HidReportClass::AppCommandStatefulPress:
      // App/config traffic must leave room for both a press from every
      // keyboard-producing input source and the matching releases. Otherwise
      // a large command can make the keyboard unresponsive even though final
      // release safety is intact.
      return kHidReportQueueCapacity - (2 * kKeyboardStateSourceCount);
    case HidReportClass::KeyboardRelease:
    case HidReportClass::AppCommandStatefulRelease:
      return kHidReportQueueCapacity - 1;
    case HidReportClass::KeyboardAllReleased:
      return kHidReportQueueCapacity;
  }
  return 0;
}

bool is_keyboard_snapshot(HidReportClass report_class) {
  return report_class == HidReportClass::KeyboardPress ||
         report_class == HidReportClass::KeyboardRelease ||
         report_class == HidReportClass::KeyboardAllReleased;
}

bool is_app_command(HidReportClass report_class) {
  return report_class == HidReportClass::AppCommand ||
         report_class == HidReportClass::AppCommandStatefulPress ||
         report_class == HidReportClass::AppCommandStatefulRelease;
}

bool is_all_zero(const std::uint8_t* data, std::size_t len) {
  return len > 0 && data != nullptr &&
         std::all_of(data, data + len, [](std::uint8_t value) {
           return value == 0;
         });
}

std::int32_t saturating_wheel_add(std::int32_t current,
                                  std::int32_t incoming,
                                  bool* saturated) {
  const std::int64_t sum =
      static_cast<std::int64_t>(current) + static_cast<std::int64_t>(incoming);
  if (sum > kMouseWheelAccumulationLimit) {
    if (saturated != nullptr) {
      *saturated = true;
    }
    return kMouseWheelAccumulationLimit;
  }
  if (sum < -kMouseWheelAccumulationLimit) {
    if (saturated != nullptr) {
      *saturated = true;
    }
    return -kMouseWheelAccumulationLimit;
  }
  return static_cast<std::int32_t>(sum);
}

bool same_or_empty_direction(std::int32_t current, std::int32_t incoming) {
  return current == 0 || incoming == 0 || (current > 0) == (incoming > 0);
}

bool valid_consumption(std::int32_t pending, std::int32_t consumed) {
  if (consumed == 0) {
    return true;
  }
  return pending != 0 && (pending > 0) == (consumed > 0) &&
         std::abs(consumed) <= std::abs(pending);
}

}  // namespace

HidQueuePushResult HidReportQueue::push_classified(
    std::uint8_t report_id,
    const std::uint8_t* data,
    std::size_t len,
    std::uint32_t queued_ms,
    HidReportClass report_class,
    BleOwnerToken ble_owner,
    std::uint32_t usb_epoch) {
  if (len > kHidReportMaxPayload || (len > 0 && data == nullptr)) {
    return {};
  }
  if (is_keyboard_snapshot(report_class) &&
      len != kKeyboardSnapshotPayloadSize) {
    return {};
  }
  if (report_class == HidReportClass::KeyboardAllReleased &&
      !is_all_zero(data, len)) {
    return {};
  }
  return push_with_limit(report_id,
                         data,
                         len,
                         queued_ms,
                         report_class,
                         classified_capacity(report_class),
                         is_keyboard_snapshot(report_class),
                         ble_owner,
                         usb_epoch);
}

bool HidReportQueue::push(std::uint8_t report_id,
                          const std::uint8_t* data,
                          std::size_t len,
                          std::uint32_t queued_ms,
                          std::uint32_t* sequence,
                          std::size_t reserved_free_slots,
                          BleOwnerToken ble_owner,
                          std::uint32_t usb_epoch) {
  const auto usable_capacity =
      reserved_free_slots >= kHidReportQueueCapacity
          ? 0
          : kHidReportQueueCapacity - reserved_free_slots;
  const auto result = push_with_limit(report_id,
                                      data,
                                      len,
                                      queued_ms,
                                      HidReportClass::AppCommand,
                                      usable_capacity,
                                      false,
                                      ble_owner,
                                      usb_epoch);
  if (sequence != nullptr) {
    *sequence = result.sequence;
  }
  return result.accepted();
}

HidQueuePushResult HidReportQueue::push_with_limit(
    std::uint8_t report_id,
    const std::uint8_t* data,
    std::size_t len,
    std::uint32_t queued_ms,
    HidReportClass report_class,
    std::size_t usable_capacity,
    bool coalesce_keyboard_snapshot,
    BleOwnerToken ble_owner,
    std::uint32_t usb_epoch) {
  if (len > kHidReportMaxPayload || (len > 0 && data == nullptr) ||
      usable_capacity == 0) {
    return {};
  }
  if (coalesce_keyboard_snapshot) {
    std::uint32_t existing_sequence = 0;
    if (tail_matches(report_id,
                     data,
                     len,
                     ble_owner,
                     usb_epoch,
                     &existing_sequence)) {
      return {HidQueuePushStatus::Coalesced, existing_sequence};
    }
  }
  if (size_ >= usable_capacity) {
    return {HidQueuePushStatus::Full, 0};
  }

  const auto tail = (head_ + size_) % reports_.size();
  auto& report = reports_[tail];
  report = {};
  report.sequence = next_sequence_++;
  if (next_sequence_ == 0) {
    next_sequence_ = 1;
  }
  report.report_id = report_id;
  report.len = len;
  report.queued_ms = queued_ms;
  report.report_class = report_class;
  report.ble_owner = ble_owner;
  report.usb_epoch = usb_epoch;
  if (len > 0) {
    std::copy_n(data, len, report.data.begin());
  }
  ++size_;

  return {HidQueuePushStatus::Queued, report.sequence};
}

bool HidReportQueue::tail_matches(std::uint8_t report_id,
                                  const std::uint8_t* data,
                                  std::size_t len,
                                  BleOwnerToken ble_owner,
                                  std::uint32_t usb_epoch,
                                  std::uint32_t* sequence) const {
  if (size_ == 0 || len > kHidReportMaxPayload ||
      (len > 0 && data == nullptr)) {
    return false;
  }
  const auto tail_index = (head_ + size_ - 1) % reports_.size();
  const auto& tail = reports_[tail_index];
  if (is_app_command(tail.report_class) ||
      tail.report_id != report_id || tail.len != len ||
      tail.ble_owner != ble_owner || tail.usb_epoch != usb_epoch) {
    return false;
  }
  if (len > 0 && !std::equal(data, data + len, tail.data.begin())) {
    return false;
  }
  if (sequence != nullptr) {
    *sequence = tail.sequence;
  }
  return true;
}

bool HidReportQueue::front(QueuedHidReport* out) const {
  if (out == nullptr || size_ == 0) {
    return false;
  }
  *out = reports_[head_];
  return true;
}

bool HidReportQueue::pop_if_sequence(std::uint32_t sequence) {
  if (size_ == 0 || reports_[head_].sequence != sequence) {
    return false;
  }
  reports_[head_] = {};
  head_ = (head_ + 1) % reports_.size();
  --size_;
  return true;
}

void HidReportQueue::clear() {
  reports_ = {};
  head_ = 0;
  size_ = 0;
}

bool HidReportQueue::empty() const {
  return size_ == 0;
}

std::size_t HidReportQueue::size() const {
  return size_;
}

bool MouseWheelQueue::push(int vertical,
                           int horizontal,
                           std::uint32_t queued_ms,
                           std::uint32_t* sequence,
                           bool* coalesced,
                           bool* saturated,
                           BleOwnerToken ble_owner,
                           std::uint32_t usb_epoch) {
  if (coalesced != nullptr) {
    *coalesced = false;
  }
  if (saturated != nullptr) {
    *saturated = false;
  }
  if (vertical == 0 && horizontal == 0) {
    return false;
  }

  if (size_ > 0) {
    const auto tail_index = (head_ + size_ - 1) % reports_.size();
    auto& tail = reports_[tail_index];
    if (tail.ble_owner == ble_owner && tail.usb_epoch == usb_epoch &&
        same_or_empty_direction(tail.vertical, vertical) &&
        same_or_empty_direction(tail.horizontal, horizontal)) {
      tail.vertical = saturating_wheel_add(
          tail.vertical, static_cast<std::int32_t>(vertical), saturated);
      tail.horizontal = saturating_wheel_add(
          tail.horizontal, static_cast<std::int32_t>(horizontal), saturated);
      if (sequence != nullptr) {
        *sequence = tail.sequence;
      }
      if (coalesced != nullptr) {
        *coalesced = true;
      }
      return true;
    }
  }

  if (size_ >= reports_.size()) {
    return false;
  }

  const auto tail_index = (head_ + size_) % reports_.size();
  auto& report = reports_[tail_index];
  report = {};
  report.sequence = next_sequence_++;
  if (next_sequence_ == 0) {
    next_sequence_ = 1;
  }
  report.vertical = std::clamp<std::int32_t>(
      vertical, -kMouseWheelAccumulationLimit, kMouseWheelAccumulationLimit);
  report.horizontal = std::clamp<std::int32_t>(
      horizontal, -kMouseWheelAccumulationLimit, kMouseWheelAccumulationLimit);
  report.queued_ms = queued_ms;
  report.ble_owner = ble_owner;
  report.usb_epoch = usb_epoch;
  ++size_;
  if (sequence != nullptr) {
    *sequence = report.sequence;
  }
  return true;
}

bool MouseWheelQueue::front(QueuedMouseWheel* out) const {
  if (out == nullptr || size_ == 0) {
    return false;
  }
  *out = reports_[head_];
  return true;
}

bool MouseWheelQueue::consume_if_sequence(std::uint32_t sequence,
                                          int vertical,
                                          int horizontal) {
  if (size_ == 0 || reports_[head_].sequence != sequence) {
    return false;
  }
  auto& report = reports_[head_];
  if (!valid_consumption(report.vertical, vertical) ||
      !valid_consumption(report.horizontal, horizontal)) {
    return false;
  }
  report.vertical -= vertical;
  report.horizontal -= horizontal;
  if (report.vertical == 0 && report.horizontal == 0) {
    return pop_if_sequence(sequence);
  }
  return true;
}

bool MouseWheelQueue::pop_if_sequence(std::uint32_t sequence) {
  if (size_ == 0 || reports_[head_].sequence != sequence) {
    return false;
  }
  reports_[head_] = {};
  head_ = (head_ + 1) % reports_.size();
  --size_;
  return true;
}

void MouseWheelQueue::clear() {
  reports_ = {};
  head_ = 0;
  size_ = 0;
}

bool MouseWheelQueue::empty() const {
  return size_ == 0;
}

std::size_t MouseWheelQueue::size() const {
  return size_;
}

}  // namespace ai_keyboard
