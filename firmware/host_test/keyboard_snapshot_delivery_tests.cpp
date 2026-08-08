#include <array>
#include <cassert>
#include <cstdint>

#include "keyboard/keyboard_snapshot_delivery.h"

namespace {

using ai_keyboard::HidKeyboardSnapshot;
using ai_keyboard::HidQueuePushStatus;
using ai_keyboard::HidReportClass;
using ai_keyboard::HidReportQueue;
using ai_keyboard::KeyboardSnapshotDelivery;
using ai_keyboard::QueuedHidReport;

HidKeyboardSnapshot snapshot(std::uint8_t modifier,
                             std::uint8_t first_key,
                             std::uint8_t second_key = 0) {
  HidKeyboardSnapshot result;
  result.modifier = modifier;
  result.keycodes[0] = first_key;
  result.keycodes[1] = second_key;
  return result;
}

std::array<std::uint8_t, ai_keyboard::kKeyboardSnapshotPayloadSize> payload(
    const HidKeyboardSnapshot& value) {
  std::array<std::uint8_t, ai_keyboard::kKeyboardSnapshotPayloadSize> result{};
  result[0] = value.modifier;
  result[1] = value.apple_fn ? 1 : 0;
  for (std::size_t index = 0; index < value.keycodes.size(); ++index) {
    result[index + 2] = value.keycodes[index];
  }
  return result;
}

void rejected_snapshot_keeps_latest_desired_state() {
  KeyboardSnapshotDelivery delivery;
  const auto key_a = snapshot(0, 0x04);
  const auto key_a_b = snapshot(0, 0x04, 0x05);

  delivery.set_desired(key_a);
  const auto first = delivery.pending_snapshot();
  assert(first.valid());
  assert(first.report_class == HidReportClass::KeyboardPress);

  // No mark_accepted(): model a full transport queue.
  delivery.set_desired(key_a_b);
  const auto latest = delivery.pending_snapshot();
  assert(latest.valid());
  assert(latest.generation != first.generation);
  assert(latest.snapshot == key_a_b);
  assert(!delivery.mark_accepted(first.generation));
  assert(delivery.pending());
  assert(delivery.mark_accepted(latest.generation));
  assert(!delivery.pending());
  assert(delivery.accepted() == key_a_b);
}

void release_is_classified_from_last_accepted_snapshot() {
  KeyboardSnapshotDelivery delivery;
  const auto key_a_b = snapshot(0x02, 0x04, 0x05);
  delivery.set_desired(key_a_b);
  assert(delivery.mark_accepted(
      delivery.pending_snapshot().generation));

  const auto key_b = snapshot(0, 0x05);
  delivery.set_desired(key_b);
  const auto release = delivery.pending_snapshot();
  assert(release.valid());
  assert(release.report_class == HidReportClass::KeyboardRelease);
  assert(release.snapshot == key_b);

  delivery.set_desired({});
  const auto all_released = delivery.pending_snapshot();
  assert(all_released.valid());
  assert(all_released.report_class ==
         HidReportClass::KeyboardAllReleased);
  assert(all_released.snapshot.empty());
}

void returning_to_accepted_state_cancels_obsolete_attempt() {
  KeyboardSnapshotDelivery delivery;
  const auto key_a = snapshot(0, 0x04);
  delivery.set_desired(key_a);
  assert(delivery.mark_accepted(
      delivery.pending_snapshot().generation));

  delivery.set_desired({});
  const auto stale_release = delivery.pending_snapshot();
  assert(stale_release.valid());
  delivery.set_desired(key_a);

  assert(!delivery.pending());
  assert(!delivery.pending_snapshot().valid());
  assert(!delivery.mark_accepted(stale_release.generation));
  assert(delivery.accepted() == key_a);

  const auto key_b = snapshot(0, 0x05);
  delivery.set_desired(key_b);
  const auto new_attempt = delivery.pending_snapshot();
  assert(new_attempt.generation != stale_release.generation);
  assert(!delivery.mark_accepted(stale_release.generation));
  assert(delivery.mark_accepted(new_attempt.generation));
}

void all_release_retries_after_a_bounded_queue_frees_space() {
  KeyboardSnapshotDelivery delivery;
  const auto key_a = snapshot(0, 0x04);
  delivery.set_desired(key_a);
  assert(delivery.mark_accepted(
      delivery.pending_snapshot().generation));
  delivery.set_desired({});
  const auto release = delivery.pending_snapshot();
  assert(release.report_class ==
         HidReportClass::KeyboardAllReleased);

  HidReportQueue queue;
  const std::array<std::uint8_t, 1> command{{0xA5}};
  for (std::size_t index = 0;
       index < ai_keyboard::kHidReportQueueCapacity;
       ++index) {
    assert(queue.push(
        0x10, command.data(), command.size(),
        static_cast<std::uint32_t>(index)));
  }

  const auto release_payload = payload(release.snapshot);
  const auto full = queue.push_classified(
      0x01,
      release_payload.data(),
      release_payload.size(),
      100,
      release.report_class);
  assert(full.status == HidQueuePushStatus::Full);
  assert(delivery.pending());

  QueuedHidReport front;
  assert(queue.front(&front));
  assert(queue.pop_if_sequence(front.sequence));
  const auto retried = queue.push_classified(
      0x01,
      release_payload.data(),
      release_payload.size(),
      101,
      release.report_class);
  assert(retried.accepted());
  assert(delivery.mark_accepted(release.generation));
  assert(!delivery.pending());

  QueuedHidReport queued;
  while (queue.front(&queued) &&
         queued.sequence != retried.sequence) {
    assert(queue.pop_if_sequence(queued.sequence));
  }
  assert(queue.front(&queued));
  assert(queued.report_class ==
         HidReportClass::KeyboardAllReleased);
  for (std::size_t index = 0; index < queued.len; ++index) {
    assert(queued.data[index] == 0);
  }
}

void reset_establishes_a_new_known_baseline() {
  KeyboardSnapshotDelivery delivery;
  delivery.set_desired(snapshot(0, 0x04));
  const auto before_reset = delivery.pending_snapshot();
  assert(delivery.pending());
  const auto baseline = snapshot(0x01, 0x05);
  delivery.reset(baseline);
  assert(!delivery.pending());
  assert(delivery.desired() == baseline);
  assert(delivery.accepted() == baseline);

  delivery.set_desired(snapshot(0, 0x06));
  const auto after_reset = delivery.pending_snapshot();
  assert(after_reset.generation != before_reset.generation);
  assert(!delivery.mark_accepted(before_reset.generation));
  assert(delivery.mark_accepted(after_reset.generation));
}

}  // namespace

int main() {
  rejected_snapshot_keeps_latest_desired_state();
  release_is_classified_from_last_accepted_snapshot();
  returning_to_accepted_state_cancels_obsolete_attempt();
  all_release_retries_after_a_bounded_queue_frees_space();
  reset_establishes_a_new_known_baseline();
  return 0;
}
