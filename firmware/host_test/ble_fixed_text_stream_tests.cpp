#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "keyboard/ble_fixed_text_stream.h"

namespace {

using ai_keyboard::BleFixedTextStartStatus;
using ai_keyboard::BleFixedTextStream;
using ai_keyboard::BleOwnerToken;
using ai_keyboard::HidReportClass;
using ai_keyboard::HidReportQueue;
using ai_keyboard::QueuedHidReport;

constexpr BleOwnerToken kOwnerA{7, 41};
constexpr BleOwnerToken kOwnerANextGeneration{7, 42};

std::string patterned_text(std::size_t size) {
  std::string text(size, '\0');
  for (std::size_t index = 0; index < text.size(); ++index) {
    text[index] = static_cast<char>('a' + (index % 26));
  }
  return text;
}

void exact_limit_streams_in_order_through_app_command_backpressure() {
  static_assert(ai_keyboard::kFixedTextMaxUtf8Bytes == 960);
  static_assert(
      ai_keyboard::kHidReportQueueCapacity -
              (2 * ai_keyboard::kKeyboardStateSourceCount) ==
          10);

  const auto text = patterned_text(ai_keyboard::kFixedTextMaxUtf8Bytes);
  BleFixedTextStream stream;
  HidReportQueue queue;
  assert(stream.start(text, kOwnerA) == BleFixedTextStartStatus::Started);

  const auto initial = stream.pump(kOwnerA, &queue, 10);
  assert(initial.queued_chunks == ai_keyboard::kBleFixedTextQueuedWindow);
  assert(initial.blocked);
  assert(!initial.completed);
  assert(stream.pending());
  assert(stream.next_chunk() == ai_keyboard::kBleFixedTextQueuedWindow);
  assert(queue.size() == ai_keyboard::kBleFixedTextQueuedWindow);

  // Only two text chunks may be ahead of a newly produced physical report.
  std::array<std::uint8_t, ai_keyboard::kKeyboardSnapshotPayloadSize> pressed{};
  pressed[2] = 0x04;
  assert(queue.push_classified(0x01,
                               pressed.data(),
                               pressed.size(),
                               11,
                               HidReportClass::KeyboardPress,
                               kOwnerA)
             .accepted());
  const std::array<std::uint8_t,
                   ai_keyboard::kKeyboardSnapshotPayloadSize>
      released{};
  assert(queue.push_classified(0x01,
                               released.data(),
                               released.size(),
                               12,
                               HidReportClass::KeyboardAllReleased,
                               kOwnerA)
             .accepted());
  assert(queue.size() == ai_keyboard::kBleFixedTextQueuedWindow + 2);

  std::string reconstructed;
  std::vector<std::uint8_t> chunk_indexes;
  std::vector<HidReportClass> delivered_classes;
  std::size_t guard = 0;
  while ((!queue.empty() || stream.pending()) && guard++ < 100) {
    QueuedHidReport report;
    if (queue.front(&report)) {
      delivered_classes.push_back(report.report_class);
      if (report.report_id == ai_keyboard::kFixedTextAppCommandReportId) {
        assert(report.report_class == HidReportClass::AppCommand);
        assert(report.ble_owner == kOwnerA);
        assert(report.len == ai_keyboard::kFixedTextAppCommandPayloadLen);
        assert(report.data[0] ==
               ai_keyboard::kFixedTextAppCommandKind);
        assert(report.data[2] ==
               ai_keyboard::fixed_text_chunk_count(text.size()));
        const auto chunk_len = report.data[3];
        assert(chunk_len <= ai_keyboard::kFixedTextAppCommandChunkDataLen);
        chunk_indexes.push_back(report.data[1]);
        reconstructed.append(
            reinterpret_cast<const char*>(
                report.data.data() +
                ai_keyboard::kFixedTextAppCommandHeaderLen),
            chunk_len);
      }
      assert(queue.pop_if_sequence(report.sequence));
    }
    stream.pump(kOwnerA, &queue, static_cast<std::uint32_t>(20 + guard));
    if (delivered_classes.size() >= 4) {
      assert(queue.size() <= ai_keyboard::kBleFixedTextQueuedWindow);
    }
  }

  assert(guard < 100);
  assert(!stream.pending());
  assert(queue.empty());
  assert(reconstructed == text);
  assert(chunk_indexes.size() ==
         ai_keyboard::fixed_text_chunk_count(text.size()));
  for (std::size_t index = 0; index < chunk_indexes.size(); ++index) {
    assert(chunk_indexes[index] == index);
  }
  assert(delivered_classes.size() >= 4);
  assert(delivered_classes[0] == HidReportClass::AppCommand);
  assert(delivered_classes[1] == HidReportClass::AppCommand);
  assert(delivered_classes[2] == HidReportClass::KeyboardPress);
  assert(delivered_classes[3] == HidReportClass::KeyboardAllReleased);
}

void saturated_app_command_capacity_retains_the_unsent_tail() {
  BleFixedTextStream stream;
  HidReportQueue queue;
  const std::uint8_t filler = 0xA5;
  constexpr std::size_t kAppCommandCapacity =
      ai_keyboard::kHidReportQueueCapacity -
      (2 * ai_keyboard::kKeyboardStateSourceCount);
  for (std::size_t index = 0; index < kAppCommandCapacity; ++index) {
    assert(queue.push_classified(
                    0x11,
                    &filler,
                    1,
                    static_cast<std::uint32_t>(index),
                    HidReportClass::AppCommand,
                    kOwnerA)
               .accepted());
  }

  assert(stream.start(patterned_text(120), kOwnerA) ==
         BleFixedTextStartStatus::Started);
  const auto saturated = stream.pump(kOwnerA, &queue, 20);
  assert(saturated.queued_chunks == 0);
  assert(saturated.blocked);
  assert(stream.pending());
  assert(stream.next_chunk() == 0);

  QueuedHidReport report;
  while (queue.size() >= ai_keyboard::kBleFixedTextQueuedWindow) {
    assert(queue.front(&report));
    assert(queue.pop_if_sequence(report.sequence));
  }
  const auto resumed = stream.pump(kOwnerA, &queue, 21);
  assert(resumed.queued_chunks == 1);
  assert(resumed.blocked);
  assert(stream.pending());
  assert(stream.next_chunk() == 1);
  assert(queue.size() == ai_keyboard::kBleFixedTextQueuedWindow);
}

void disconnect_or_owner_generation_change_cancels_unsent_tail() {
  const auto text = patterned_text(ai_keyboard::kFixedTextMaxUtf8Bytes);
  BleFixedTextStream stream;
  HidReportQueue queue;
  assert(stream.start(text, kOwnerA) == BleFixedTextStartStatus::Started);
  assert(stream.pump(kOwnerA, &queue, 1).blocked);
  assert(stream.pending());

  const auto disconnected = stream.pump({}, &queue, 2);
  assert(disconnected.owner_changed);
  assert(!stream.pending());

  queue.clear();
  assert(stream.start(text, kOwnerA) == BleFixedTextStartStatus::Started);
  assert(stream.pump(kOwnerA, &queue, 3).blocked);
  const auto generation_changed =
      stream.pump(kOwnerANextGeneration, &queue, 4);
  assert(generation_changed.owner_changed);
  assert(!stream.pending());

  QueuedHidReport report;
  while (queue.front(&report)) {
    assert(report.ble_owner == kOwnerA);
    assert(queue.pop_if_sequence(report.sequence));
  }
}

void rejects_oversize_or_competing_stream_without_replacing_the_owner() {
  BleFixedTextStream stream;
  assert(stream.start(
             patterned_text(ai_keyboard::kFixedTextMaxUtf8Bytes + 1),
             kOwnerA) == BleFixedTextStartStatus::TooLarge);
  assert(!stream.pending());

  assert(stream.start(patterned_text(120), kOwnerA) ==
         BleFixedTextStartStatus::Started);
  assert(stream.start("replacement", kOwnerANextGeneration) ==
         BleFixedTextStartStatus::Busy);
  assert(stream.owner() == kOwnerA);
}

}  // namespace

int main() {
  exact_limit_streams_in_order_through_app_command_backpressure();
  saturated_app_command_capacity_retains_the_unsent_tail();
  disconnect_or_owner_generation_change_cancels_unsent_tail();
  rejects_oversize_or_competing_stream_without_replacing_the_owner();
  return 0;
}
