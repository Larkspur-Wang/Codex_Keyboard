#include "keyboard/usb_hid_endpoint_arbiter.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>

#include "keyboard/hid_report_queue.h"

namespace {

using Kind = ai_keyboard::UsbHidEndpointReportKind;

constexpr std::size_t index_of(Kind kind) {
  return static_cast<std::size_t>(kind);
}

void persistent_mixed_load_is_round_robin() {
  ai_keyboard::UsbHidEndpointArbiter arbiter;
  const ai_keyboard::UsbHidEndpointPending all_pending{
      true, true, true, true, true};
  std::array<int, index_of(Kind::Count)> accepted{};

  for (int credit = 0; credit < 500; ++credit) {
    const auto selected = arbiter.select(all_pending);
    assert(selected == static_cast<Kind>(credit % accepted.size()));
    ++accepted[index_of(selected)];
    arbiter.mark_accepted(selected);
  }

  assert(accepted[index_of(Kind::Keyboard)] == 100);
  assert(accepted[index_of(Kind::MouseWheel)] == 100);
  assert(accepted[index_of(Kind::AppCommand)] == 100);
  assert(accepted[index_of(Kind::StatusResponse)] == 100);
  assert(accepted[index_of(Kind::SpeakerAssets)] == 100);
}

void busy_endpoint_does_not_rotate_or_consume() {
  ai_keyboard::UsbHidEndpointArbiter arbiter;
  const ai_keyboard::UsbHidEndpointPending all_pending{
      true, true, true, true, true};

  assert(arbiter.select(all_pending) == Kind::Keyboard);
  assert(arbiter.select(all_pending) == Kind::Keyboard);
  assert(arbiter.next_preferred() == Kind::Keyboard);

  arbiter.mark_accepted(Kind::Keyboard);
  assert(arbiter.select(all_pending) == Kind::MouseWheel);
}

void intermittent_queues_use_the_next_available_credit() {
  ai_keyboard::UsbHidEndpointArbiter arbiter;

  assert(arbiter.select({false, true, true, true, true}) == Kind::MouseWheel);
  arbiter.mark_accepted(Kind::MouseWheel);
  assert(arbiter.select({true, false, true, true, true}) == Kind::AppCommand);
  arbiter.mark_accepted(Kind::AppCommand);
  assert(arbiter.select({true, false, false, true, true}) ==
         Kind::StatusResponse);
  arbiter.mark_accepted(Kind::StatusResponse);
  assert(arbiter.select({true, false, false, false, true}) ==
         Kind::SpeakerAssets);
  arbiter.mark_accepted(Kind::SpeakerAssets);
  assert(arbiter.select({true, false, false, false, false}) == Kind::Keyboard);
  arbiter.mark_accepted(Kind::Keyboard);
  assert(arbiter.select({false, false, false, false, false}) == Kind::Count);
}

void finite_mixed_backlogs_all_drain_under_retry_pressure() {
  ai_keyboard::UsbHidEndpointArbiter arbiter;
  std::array<int, index_of(Kind::Count)> remaining{120, 80, 60, 40, 30};
  std::array<int, index_of(Kind::Count)> delivered{};
  int attempts = 0;

  while (remaining[0] + remaining[1] + remaining[2] + remaining[3] +
             remaining[4] >
         0) {
    const ai_keyboard::UsbHidEndpointPending pending{
        remaining[index_of(Kind::Keyboard)] > 0,
        remaining[index_of(Kind::MouseWheel)] > 0,
        remaining[index_of(Kind::AppCommand)] > 0,
        remaining[index_of(Kind::StatusResponse)] > 0,
        remaining[index_of(Kind::SpeakerAssets)] > 0,
    };
    const auto selected = arbiter.select(pending);
    assert(selected != Kind::Count);

    ++attempts;
    if (attempts % 7 == 0) {
      // A rejected TinyUSB attempt leaves both backlog and preference intact.
      assert(arbiter.select(pending) == selected);
      continue;
    }

    --remaining[index_of(selected)];
    ++delivered[index_of(selected)];
    arbiter.mark_accepted(selected);
  }

  assert(delivered[index_of(Kind::Keyboard)] == 120);
  assert(delivered[index_of(Kind::MouseWheel)] == 80);
  assert(delivered[index_of(Kind::AppCommand)] == 60);
  assert(delivered[index_of(Kind::StatusResponse)] == 40);
  assert(delivered[index_of(Kind::SpeakerAssets)] == 30);
}

void synthetic_tap_restores_the_exact_physical_snapshot() {
  ai_keyboard::UsbHidKeyboardSnapshot physical{};
  physical.modifier = 0x02;
  physical.apple_fn = true;
  physical.keycodes = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00};

  ai_keyboard::UsbHidKeyboardSnapshot synthetic{};
  synthetic.modifier = 0x01;
  synthetic.keycodes = {0x05, 0x00, 0x00, 0x00, 0x00, 0x00};

  ai_keyboard::UsbHidSyntheticTapPair pair{};
  assert(ai_keyboard::compose_usb_hid_synthetic_tap(
      physical, synthetic, &pair));
  assert(pair.pressed.modifier == 0x03);
  assert(pair.pressed.apple_fn);
  assert(pair.pressed.keycodes[0] == 0x04);
  assert(pair.pressed.keycodes[1] == 0x05);
  assert(pair.restored == physical);
}

void synthetic_tap_deduplicates_a_key_already_physically_held() {
  ai_keyboard::UsbHidKeyboardSnapshot physical{};
  physical.keycodes = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
  ai_keyboard::UsbHidKeyboardSnapshot synthetic{};
  synthetic.keycodes = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00};

  ai_keyboard::UsbHidSyntheticTapPair pair{};
  assert(ai_keyboard::compose_usb_hid_synthetic_tap(
      physical, synthetic, &pair));
  assert(pair.pressed.keycodes == physical.keycodes);
  assert(pair.restored == physical);
}

void synthetic_tap_waits_instead_of_overwriting_six_key_rollover() {
  ai_keyboard::UsbHidKeyboardSnapshot physical{};
  physical.keycodes = {0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
  ai_keyboard::UsbHidKeyboardSnapshot synthetic{};
  synthetic.keycodes = {0x0A, 0x00, 0x00, 0x00, 0x00, 0x00};

  ai_keyboard::UsbHidSyntheticTapPair sentinel{};
  sentinel.pressed.modifier = 0xAA;
  sentinel.restored.modifier = 0xBB;
  assert(!ai_keyboard::compose_usb_hid_synthetic_tap(
      physical, synthetic, &sentinel));
  assert(sentinel.pressed.modifier == 0xAA);
  assert(sentinel.restored.modifier == 0xBB);
}

void app_command_fifo_retains_the_exact_head_while_endpoint_is_busy() {
  ai_keyboard::HidReportQueue queue;
  std::array<std::uint8_t, ai_keyboard::kHidReportMaxPayload> first{};
  std::array<std::uint8_t, ai_keyboard::kHidReportMaxPayload> second{};
  std::array<std::uint8_t, ai_keyboard::kHidReportMaxPayload> third{};
  first[0] = 0xA1;
  second[0] = 0xB2;
  third[0] = 0xC3;
  assert(queue.push(0x11, first.data(), first.size(), 1));
  assert(queue.push(0x11, second.data(), second.size(), 2));
  assert(queue.push(0x11, third.data(), third.size(), 3));

  ai_keyboard::QueuedHidReport head{};
  assert(queue.front(&head));
  const auto first_sequence = head.sequence;
  assert(head.data[0] == 0xA1);

  // A TinyUSB busy return performs no pop. Every retry observes the exact
  // original FIFO head, including its sequence and payload.
  for (int retry = 0; retry < 20; ++retry) {
    ai_keyboard::QueuedHidReport retry_head{};
    assert(queue.front(&retry_head));
    assert(retry_head.sequence == first_sequence);
    assert(retry_head.data[0] == 0xA1);
  }

  assert(queue.pop_if_sequence(first_sequence));
  assert(queue.front(&head));
  assert(head.data[0] == 0xB2);
  assert(queue.pop_if_sequence(head.sequence));
  assert(queue.front(&head));
  assert(head.data[0] == 0xC3);
}

void synthetic_press_restore_and_later_physical_state_keep_fifo_order() {
  ai_keyboard::UsbHidKeyboardSnapshot physical{};
  physical.keycodes = {0x04, 0, 0, 0, 0, 0};
  ai_keyboard::UsbHidKeyboardSnapshot synthetic{};
  synthetic.keycodes = {0x05, 0, 0, 0, 0, 0};
  ai_keyboard::UsbHidKeyboardSnapshot later_physical{};
  later_physical.keycodes = {0x06, 0, 0, 0, 0, 0};

  ai_keyboard::UsbHidSyntheticTapPair pair{};
  assert(ai_keyboard::compose_usb_hid_synthetic_tap(
      physical, synthetic, &pair));

  const auto encode = [](const ai_keyboard::UsbHidKeyboardSnapshot& snapshot) {
    std::array<std::uint8_t, ai_keyboard::kKeyboardSnapshotPayloadSize> wire{};
    wire[0] = snapshot.modifier;
    wire[1] = snapshot.apple_fn ? 1 : 0;
    std::copy(snapshot.keycodes.begin(),
              snapshot.keycodes.end(),
              wire.begin() + 2);
    return wire;
  };

  ai_keyboard::HidReportQueue queue;
  const auto pressed = encode(pair.pressed);
  const auto restored = encode(pair.restored);
  const auto later = encode(later_physical);
  assert(queue.push_classified(
                  1,
                  pressed.data(),
                  pressed.size(),
                  1,
                  ai_keyboard::HidReportClass::KeyboardPress)
             .accepted());
  assert(queue.push_classified(
                  1,
                  restored.data(),
                  restored.size(),
                  2,
                  ai_keyboard::HidReportClass::KeyboardRelease)
             .accepted());
  assert(queue.push_classified(
                  1,
                  later.data(),
                  later.size(),
                  3,
                  ai_keyboard::HidReportClass::KeyboardPress)
             .accepted());

  ai_keyboard::QueuedHidReport head{};
  assert(queue.front(&head));
  assert(std::equal(pressed.begin(), pressed.end(), head.data.begin()));
  assert(queue.pop_if_sequence(head.sequence));
  assert(queue.front(&head));
  assert(std::equal(restored.begin(), restored.end(), head.data.begin()));
  assert(queue.pop_if_sequence(head.sequence));
  assert(queue.front(&head));
  assert(std::equal(later.begin(), later.end(), head.data.begin()));
}

}  // namespace

int main() {
  persistent_mixed_load_is_round_robin();
  busy_endpoint_does_not_rotate_or_consume();
  intermittent_queues_use_the_next_available_credit();
  finite_mixed_backlogs_all_drain_under_retry_pressure();
  synthetic_tap_restores_the_exact_physical_snapshot();
  synthetic_tap_deduplicates_a_key_already_physically_held();
  synthetic_tap_waits_instead_of_overwriting_six_key_rollover();
  app_command_fifo_retains_the_exact_head_while_endpoint_is_busy();
  synthetic_press_restore_and_later_physical_state_keep_fifo_order();
  return 0;
}
