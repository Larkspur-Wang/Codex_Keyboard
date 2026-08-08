#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "keyboard/keymap.h"

namespace ai_keyboard {

constexpr std::size_t kHidReportQueueCapacity = 32;
constexpr std::size_t kHidReportMaxPayload = 63;
constexpr std::size_t kMouseWheelQueueCapacity = 32;
constexpr std::size_t kKeyboardStateSourceCount =
    static_cast<std::size_t>(InputId::Count);
constexpr std::size_t kKeyboardSnapshotPayloadSize = 8;
constexpr std::int32_t kMouseWheelAccumulationLimit = 32767;
static_assert(
    kHidReportQueueCapacity >= 2 * kKeyboardStateSourceCount,
    "HID queue must reserve one press and one release per keyboard source");

// Identifies one concrete BLE HID owner lifetime. Connection handles may be
// reused, so a handle without its monotonically changing generation is not a
// safe delivery identity. USB queues leave this token invalid.
struct BleOwnerToken {
  static constexpr std::uint16_t kNoConnection = 0xFFFF;

  std::uint16_t conn_handle = kNoConnection;
  std::uint32_t generation = 0;

  bool valid() const {
    return conn_handle != kNoConnection && generation != 0;
  }

  bool operator==(const BleOwnerToken& other) const {
    return conn_handle == other.conn_handle &&
           generation == other.generation;
  }

  bool operator!=(const BleOwnerToken& other) const {
    return !(*this == other);
  }
};

// Keyboard reports are complete state snapshots, not independent key
// commands. Keeping the transition class with the snapshot lets the bounded
// queue reserve enough room for every physical input to be released without
// moving a release ahead of an earlier press.
enum class HidReportClass : std::uint8_t {
  KeyboardPress,
  KeyboardRelease,
  KeyboardAllReleased,
  // Stateless vendor messages may be dropped when the active HID protocol
  // cannot represent them. Stateful app-hotkey transitions must instead keep
  // press/release lifetime semantics so a missing release tears down the host
  // owner rather than leaving the desktop App latched.
  AppCommand,
  AppCommandStatefulPress,
  AppCommandStatefulRelease,
};

enum class HidQueuePushStatus : std::uint8_t {
  Queued,
  Coalesced,
  Full,
  Invalid,
};

struct HidQueuePushResult {
  HidQueuePushStatus status = HidQueuePushStatus::Invalid;
  std::uint32_t sequence = 0;

  bool accepted() const {
    return status == HidQueuePushStatus::Queued ||
           status == HidQueuePushStatus::Coalesced;
  }
};

struct QueuedHidReport {
  std::uint32_t sequence = 0;
  std::uint8_t report_id = 0;
  std::array<std::uint8_t, kHidReportMaxPayload> data{};
  std::size_t len = 0;
  std::uint32_t queued_ms = 0;
  HidReportClass report_class = HidReportClass::AppCommand;
  BleOwnerToken ble_owner{};
  std::uint32_t usb_epoch = 0;
};

class HidReportQueue {
 public:
  HidQueuePushResult push_classified(std::uint8_t report_id,
                                     const std::uint8_t* data,
                                     std::size_t len,
                                     std::uint32_t queued_ms,
                                     HidReportClass report_class,
                                     BleOwnerToken ble_owner = {},
                                     std::uint32_t usb_epoch = 0);

  // Compatibility entry point for existing non-stateful callers. New
  // keyboard-state producers should use push_classified() so the release
  // capacity invariant is enforced.
  bool push(std::uint8_t report_id,
            const std::uint8_t* data,
            std::size_t len,
            std::uint32_t queued_ms,
            std::uint32_t* sequence = nullptr,
            std::size_t reserved_free_slots = 0,
            BleOwnerToken ble_owner = {},
            std::uint32_t usb_epoch = 0);
  bool front(QueuedHidReport* out) const;
  bool pop_if_sequence(std::uint32_t sequence);
  void clear();

  bool empty() const;
  std::size_t size() const;

 private:
  bool tail_matches(std::uint8_t report_id,
                    const std::uint8_t* data,
                    std::size_t len,
                    BleOwnerToken ble_owner,
                    std::uint32_t usb_epoch,
                    std::uint32_t* sequence) const;
  HidQueuePushResult push_with_limit(std::uint8_t report_id,
                                     const std::uint8_t* data,
                                     std::size_t len,
                                     std::uint32_t queued_ms,
                                     HidReportClass report_class,
                                     std::size_t usable_capacity,
                                     bool coalesce_keyboard_snapshot,
                                     BleOwnerToken ble_owner,
                                     std::uint32_t usb_epoch);

  std::array<QueuedHidReport, kHidReportQueueCapacity> reports_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::uint32_t next_sequence_ = 1;
};

struct QueuedMouseWheel {
  std::uint32_t sequence = 0;
  std::int32_t vertical = 0;
  std::int32_t horizontal = 0;
  std::uint32_t queued_ms = 0;
  BleOwnerToken ble_owner{};
  std::uint32_t usb_epoch = 0;
};

// Preserves direction changes while coalescing consecutive movement in the
// same direction. This prevents a quick clockwise/counter-clockwise pair from
// disappearing before the transport has a chance to send either movement.
class MouseWheelQueue {
 public:
  bool push(int vertical,
            int horizontal,
            std::uint32_t queued_ms,
            std::uint32_t* sequence = nullptr,
            bool* coalesced = nullptr,
            bool* saturated = nullptr,
            BleOwnerToken ble_owner = {},
            std::uint32_t usb_epoch = 0);
  bool front(QueuedMouseWheel* out) const;
  bool consume_if_sequence(std::uint32_t sequence, int vertical, int horizontal);
  bool pop_if_sequence(std::uint32_t sequence);
  void clear();

  bool empty() const;
  std::size_t size() const;

 private:
  std::array<QueuedMouseWheel, kMouseWheelQueueCapacity> reports_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::uint32_t next_sequence_ = 1;
};

}  // namespace ai_keyboard
