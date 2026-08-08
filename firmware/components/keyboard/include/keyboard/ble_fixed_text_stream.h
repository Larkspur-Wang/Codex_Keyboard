#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "keyboard/fixed_text_protocol.h"
#include "keyboard/hid_report_queue.h"

namespace ai_keyboard {

inline constexpr std::size_t kBleFixedTextQueuedWindow = 2;

enum class BleFixedTextStartStatus : std::uint8_t {
  Started,
  Empty,
  InvalidOwner,
  TooLarge,
  Busy,
};

struct BleFixedTextPumpResult {
  std::size_t queued_chunks = 0;
  bool blocked = false;
  bool completed = false;
  bool owner_changed = false;
};

// Owns the not-yet-queued tail of one BLE fixed-text AppCommand. It advances
// only after HidReportQueue accepts the next chunk, so classified queue
// backpressure cannot silently discard the remainder. The exact owner token
// binds all chunks to one connection lifetime.
class BleFixedTextStream {
 public:
  BleFixedTextStartStatus start(std::string_view text, BleOwnerToken owner);
  BleFixedTextPumpResult pump(BleOwnerToken current_owner,
                              HidReportQueue* queue,
                              std::uint32_t queued_ms);
  void reset();

  bool pending() const;
  BleOwnerToken owner() const;
  std::uint8_t next_chunk() const;
  std::uint8_t total_chunks() const;
  std::size_t remaining_bytes() const;

 private:
  std::string text_;
  BleOwnerToken owner_{};
  std::uint8_t next_chunk_ = 0;
  std::uint8_t total_chunks_ = 0;
};

}  // namespace ai_keyboard
