#include <cassert>

#include "keyboard/hid_report_queue.h"
#include "keyboard/keyboard_snapshot_delivery.h"
#include "keyboard/transport_routing.h"

using ai_keyboard::BridgedHotkeyDelivery;
using ai_keyboard::FirmwareEvent;
using ai_keyboard::FirmwareEventKind;
using ai_keyboard::FirmwareTransportRoute;
using ai_keyboard::HidQueuePushStatus;
using ai_keyboard::HidReportClass;
using ai_keyboard::HidReportQueue;
using ai_keyboard::KeyboardTransportLatch;
using ai_keyboard::KeyboardTransportOwner;
using ai_keyboard::KeyboardSnapshotDelivery;
using ai_keyboard::UsbEndpointLifetime;
using ai_keyboard::UsbPhysicalPresenceMonitor;

constexpr std::uint32_t kUsbEpoch = 11;
constexpr std::uint32_t kBleEpoch = 21;

void hid_events_prefer_usb_even_when_ble_is_connected() {
  assert(ai_keyboard::route_for_firmware_event(
             FirmwareEvent{FirmwareEventKind::HidTap, "Return"},
             true) == FirmwareTransportRoute::UsbFirst);
  assert(ai_keyboard::route_for_firmware_event(
             FirmwareEvent{FirmwareEventKind::HidKeyDown, "RightMeta"},
             true) == FirmwareTransportRoute::UsbFirst);
}

void fixed_text_prefers_usb_even_when_ble_is_connected() {
  assert(ai_keyboard::route_for_firmware_event(
             FirmwareEvent{FirmwareEventKind::FixedText, "hello"},
             true) == FirmwareTransportRoute::UsbFirst);
}

void fixed_text_prefers_usb_when_ble_is_not_connected() {
  assert(ai_keyboard::route_for_firmware_event(
             FirmwareEvent{FirmwareEventKind::FixedText, "hello"},
             false) == FirmwareTransportRoute::UsbFirst);
}

void app_commands_prefer_usb_even_when_ble_is_connected() {
  assert(ai_keyboard::route_for_firmware_event(
             FirmwareEvent{FirmwareEventKind::AppCommand, "history"},
             true) == FirmwareTransportRoute::UsbFirst);
}

void usb_endpoint_lifetime_treats_every_mount_callback_as_a_new_endpoint() {
  UsbEndpointLifetime lifetime;
  assert(!lifetime.mounted());
  assert(lifetime.epoch() == 0);

  assert(lifetime.on_mount());
  assert(lifetime.mounted());
  assert(lifetime.epoch() == 1);
  // TinyUSB does not emit umount for every BUS RESET. A subsequent mount
  // callback therefore has to invalidate reports from the old endpoint even
  // while the logical mounted bit was still true.
  assert(lifetime.on_mount());
  assert(lifetime.epoch() == 2);

  assert(lifetime.on_unmount());
  assert(!lifetime.mounted());
  assert(lifetime.epoch() == 3);
  assert(!lifetime.on_unmount());
  assert(lifetime.epoch() == 3);
}

void usb_unmount_remount_is_visible_when_intermediate_state_is_not_sampled() {
  UsbEndpointLifetime lifetime;
  assert(lifetime.on_mount());
  const auto first_endpoint_epoch = lifetime.epoch();

  // A main-loop consumer does not sample between these two callbacks.
  assert(lifetime.on_unmount());
  assert(lifetime.on_mount());

  assert(lifetime.mounted());
  assert(lifetime.epoch() != 0);
  assert(lifetime.epoch() != first_endpoint_epoch);
}

void usb_endpoint_epoch_wraps_without_exposing_zero() {
  UsbEndpointLifetime lifetime(UINT32_MAX - 1);

  assert(lifetime.on_mount());
  assert(lifetime.epoch() == UINT32_MAX);
  assert(lifetime.on_unmount());
  assert(lifetime.epoch() == 1);
  assert(lifetime.on_mount());
  assert(lifetime.epoch() == 2);
}

void usb_physical_presence_filters_brief_disconnect_glitches() {
  UsbPhysicalPresenceMonitor presence(25);
  presence.reset(true, 100);
  assert(presence.present());
  assert(!presence.disconnect_pending());

  assert(!presence.update(false, 100));
  assert(presence.present());
  assert(presence.disconnect_pending());
  assert(!presence.update(false, 124));
  assert(presence.present());

  // VBUS recovered before the confirmation boundary. The mounted endpoint
  // must stay valid and the candidate is cancelled immediately.
  assert(!presence.update(true, 125));
  assert(presence.present());
  assert(!presence.disconnect_pending());

  assert(!presence.update(false, 200));
  assert(!presence.update(false, 224));
  assert(presence.update(false, 225));
  assert(!presence.present());
  assert(!presence.disconnect_pending());

  // Re-attach is intentionally immediate; routability still waits for a real
  // TinyUSB mount callback in UsbEndpointLifetime.
  assert(presence.update(true, 226));
  assert(presence.present());
}

void usb_physical_disconnect_confirmation_survives_clock_wrap() {
  UsbPhysicalPresenceMonitor presence(25);
  presence.reset(true, UINT32_MAX - 10);
  assert(!presence.update(false, UINT32_MAX - 10));
  assert(!presence.update(false, 5));
  assert(presence.present());
  assert(presence.disconnect_pending());
  assert(presence.update(false, 15));
  assert(!presence.present());
}

void usb_physical_disconnect_invalidates_endpoint_without_forging_epoch() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(lifetime.on_mount());
  const auto mounted_epoch = lifetime.epoch();
  assert(lifetime.mounted());

  assert(lifetime.observe_physical_presence(false));
  assert(!lifetime.mounted());
  // SEN_VIN revokes routability, but only TinyUSB mount/unmount callbacks own
  // the epoch counter.
  assert(lifetime.epoch() == mounted_epoch);
  assert(!lifetime.observe_physical_presence(false));
  assert(lifetime.epoch() == mounted_epoch);

  // A later real unmount remains visible exactly once.
  assert(lifetime.on_unmount());
  assert(lifetime.epoch() != mounted_epoch);
  const auto unmounted_epoch = lifetime.epoch();
  assert(!lifetime.on_unmount());
  assert(lifetime.epoch() == unmounted_epoch);
}

void usb_late_mount_is_ignored_while_physical_link_is_absent() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(lifetime.on_mount());
  assert(lifetime.observe_physical_presence(false));
  const auto disconnected_epoch = lifetime.epoch();

  assert(!lifetime.on_mount());
  assert(!lifetime.mounted());
  assert(lifetime.epoch() == disconnected_epoch);
}

void usb_mount_callback_rejects_raw_vbus_absent_before_debounce() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(lifetime.on_mount_with_physical_presence(true));
  const auto mounted_epoch = lifetime.epoch();

  // Model a late TinyUSB callback in the 25 ms confirmation window: the
  // debounced state still says present, but the callback's raw SEN_VIN sample
  // already says VBUS is absent. It must not create a new epoch or resurrect
  // an endpoint.
  assert(!lifetime.on_mount_with_physical_presence(false));
  assert(lifetime.mounted());
  assert(lifetime.epoch() == mounted_epoch);

  assert(lifetime.observe_physical_presence(false));
  assert(!lifetime.mounted());
  assert(lifetime.epoch() == mounted_epoch);
}

void usb_physical_replug_waits_for_real_mount_callback() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(lifetime.on_mount());
  assert(lifetime.observe_physical_presence(false));
  const auto disconnected_epoch = lifetime.epoch();

  assert(!lifetime.observe_physical_presence(true));
  assert(!lifetime.mounted());
  assert(lifetime.epoch() == disconnected_epoch);

  assert(lifetime.on_mount());
  assert(lifetime.mounted());
  assert(lifetime.epoch() != disconnected_epoch);
}

void usb_present_or_suspended_path_keeps_existing_mount_semantics() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(!lifetime.mounted());
  assert(!lifetime.observe_physical_presence(true));
  assert(lifetime.epoch() == 0);

  assert(lifetime.on_mount());
  const auto mounted_epoch = lifetime.epoch();
  // VBUS remains present during ordinary endpoint busy/suspend. No physical
  // update occurs and routing must not depend on transient tud_hid_ready().
  assert(!lifetime.observe_physical_presence(true));
  assert(lifetime.mounted());
  assert(lifetime.epoch() == mounted_epoch);

  // A real SET_CONFIGURATION callback after BUS RESET is still a new epoch.
  assert(lifetime.on_mount());
  assert(lifetime.mounted());
  assert(lifetime.epoch() != mounted_epoch);
}

void usb_presence_tracking_disabled_preserves_legacy_lifecycle() {
  UsbEndpointLifetime lifetime;
  assert(lifetime.on_mount());
  const auto mounted_epoch = lifetime.epoch();
  assert(!lifetime.observe_physical_presence(false));
  assert(lifetime.mounted());
  assert(lifetime.epoch() == mounted_epoch);
  assert(lifetime.on_unmount());
  assert(!lifetime.mounted());
}

void physical_usb_disconnect_routes_next_chord_to_ble() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(lifetime.on_mount());

  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(
             false, lifetime.mounted(), lifetime.epoch(), true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
  assert(latch.select_for_snapshot(
             true, lifetime.mounted(), lifetime.epoch(), true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
  latch.commit_snapshot(true);

  assert(lifetime.observe_physical_presence(false));
  assert(latch.select_for_snapshot(
             false, lifetime.mounted(), 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(latch.owner_epoch() == kBleEpoch);
}

void physical_usb_disconnect_while_held_suppresses_until_release() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(lifetime.on_mount());

  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(
             false, true, lifetime.epoch(), true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
  assert(lifetime.observe_physical_presence(false));
  assert(latch.observe_transport_state(false, 0, true, kBleEpoch));
  assert(latch.owner() == KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(true, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::None);
  latch.commit_snapshot(true);
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
}

void usb_replug_does_not_steal_an_active_ble_chord() {
  UsbEndpointLifetime lifetime;
  lifetime.enable_physical_presence_monitor(true);
  assert(lifetime.on_mount());
  assert(lifetime.observe_physical_presence(false));

  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(!lifetime.observe_physical_presence(true));
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);

  assert(lifetime.on_mount());
  assert(latch.select_for_snapshot(
             false, true, lifetime.epoch(), true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(latch.select_for_snapshot(
             true, true, lifetime.epoch(), true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  latch.commit_snapshot(true);
  assert(latch.select_for_snapshot(
             false, true, lifetime.epoch(), true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
}

void keyboard_chord_keeps_ble_owner_when_usb_appears() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(latch.select_for_snapshot(
             true, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(latch.owner() == KeyboardTransportOwner::Ble);
  assert(latch.owner_epoch() == kBleEpoch);
  latch.commit_snapshot(true);
  assert(latch.owner() == KeyboardTransportOwner::None);
  assert(latch.owner_epoch() == 0);
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
}

void keyboard_chord_does_not_migrate_after_owner_disconnects() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
  assert(latch.select_for_snapshot(
             false, false, kUsbEpoch + 1, true, kBleEpoch) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             false, false, kUsbEpoch + 1, true, kBleEpoch) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             true, false, kUsbEpoch + 1, true, kBleEpoch) ==
         KeyboardTransportOwner::None);
  assert(latch.owner() == KeyboardTransportOwner::Suppressed);
  latch.commit_snapshot(true);
  assert(latch.owner() == KeyboardTransportOwner::None);
  assert(latch.select_for_snapshot(
             false, false, kUsbEpoch + 1, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
}

void keyboard_chord_started_offline_stays_suppressed_until_release() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(false, false, 0, false, 0) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             true, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::None);
  latch.commit_snapshot(true);
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
  latch.reset();
  assert(latch.owner() == KeyboardTransportOwner::None);
}

void connected_endpoint_without_identity_epoch_is_not_routable() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(false, false, 0, true, 0) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.owner_epoch() == 0);
}

void rejected_release_keeps_owner_for_retry() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);

  // Merely selecting the release does not mean its transport queue accepted
  // it. A later retry must still resolve to the original owner.
  assert(latch.select_for_snapshot(
             true, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(latch.owner() == KeyboardTransportOwner::Ble);
  assert(latch.select_for_snapshot(
             true, true, kUsbEpoch, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  latch.commit_snapshot(true);
  assert(latch.owner() == KeyboardTransportOwner::None);
}

void ble_same_handle_reuse_is_a_fresh_endpoint_lifetime() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);

  // The numeric NimBLE connection handle may be identical after reconnect;
  // the adapter generation is what distinguishes the endpoint lifetime.
  assert(latch.observe_transport_state(
      false, 0, true, kBleEpoch + 1));
  assert(latch.owner() == KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             false, false, 0, true, kBleEpoch + 1) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             true, false, 0, true, kBleEpoch + 1) ==
         KeyboardTransportOwner::None);
  latch.commit_snapshot(true);
  assert(latch.select_for_snapshot(
             false, false, 0, true, kBleEpoch + 1) ==
         KeyboardTransportOwner::Ble);
  assert(latch.owner_epoch() == kBleEpoch + 1);
}

void usb_remount_is_a_fresh_endpoint_lifetime() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch, false, 0) ==
         KeyboardTransportOwner::Usb);

  assert(latch.observe_transport_state(
      true, kUsbEpoch + 2, false, 0));
  assert(latch.owner() == KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch + 2, true, kBleEpoch) ==
         KeyboardTransportOwner::Suppressed);
  assert(latch.select_for_snapshot(
             true, true, kUsbEpoch + 2, true, kBleEpoch) ==
         KeyboardTransportOwner::None);
  latch.commit_snapshot(true);
  assert(latch.select_for_snapshot(
             false, true, kUsbEpoch + 2, true, kBleEpoch) ==
         KeyboardTransportOwner::Usb);
}

void unchanged_epoch_does_not_invalidate_active_chord() {
  KeyboardTransportLatch latch;
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(!latch.observe_transport_state(
      false, 0, true, kBleEpoch));
  assert(latch.owner() == KeyboardTransportOwner::Ble);
}

void invalidated_endpoint_refreshes_delivery_baseline_until_release() {
  KeyboardTransportLatch latch;
  KeyboardSnapshotDelivery delivery;
  ai_keyboard::HidKeyboardSnapshot held;
  held.keycodes[0] = 0x04;

  delivery.set_desired(held);
  const auto pressed = delivery.pending_snapshot();
  assert(pressed.valid());
  assert(delivery.mark_accepted(pressed.generation));
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);

  ai_keyboard::HidKeyboardSnapshot current = held;
  current.keycodes[1] = 0x05;
  delivery.set_desired(current);
  const auto stale_pending = delivery.pending_snapshot();
  assert(stale_pending.valid());

  assert(latch.observe_transport_state(
      false, 0, true, kBleEpoch + 1));
  delivery.reset(current);
  assert(!delivery.pending());
  assert(!delivery.mark_accepted(stale_pending.generation));
  assert(delivery.accepted() == current);

  delivery.set_desired({});
  const auto release = delivery.pending_snapshot();
  assert(release.valid());
  assert(release.report_class ==
         ai_keyboard::HidReportClass::KeyboardAllReleased);
  assert(latch.select_for_snapshot(
             true, false, 0, true, kBleEpoch + 1) ==
         KeyboardTransportOwner::None);
  assert(delivery.mark_accepted(release.generation));
  latch.commit_snapshot(true);

  assert(latch.select_for_snapshot(
             false, false, 0, true, kBleEpoch + 1) ==
         KeyboardTransportOwner::Ble);
}

void overlapping_bridged_sources_release_independently() {
  KeyboardTransportLatch voice;
  KeyboardTransportLatch edit;
  assert(voice.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  assert(edit.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);

  assert(voice.select_for_snapshot(true, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  voice.commit_snapshot(true);
  assert(voice.owner() == KeyboardTransportOwner::None);
  assert(edit.owner() == KeyboardTransportOwner::Ble);

  assert(edit.select_for_snapshot(true, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);
  edit.commit_snapshot(true);
  assert(edit.owner() == KeyboardTransportOwner::None);
}

void rejected_bridged_press_and_release_before_admission_coalesces() {
  BridgedHotkeyDelivery delivery;
  assert(delivery.set_desired(true, "RightMeta"));
  const auto rejected_press = delivery.pending_transition();
  assert(rejected_press.valid);
  assert(rejected_press.pressed);
  assert(rejected_press.hotkey == "RightMeta");

  // The transport never accepted the press. Once the physical key is
  // released, neither edge should be replayed later.
  assert(delivery.set_desired(false, "ignored"));
  assert(!delivery.pending());
  assert(!delivery.mark_accepted(rejected_press));
  assert(!delivery.accepted_pressed());
}

void accepted_bridged_press_retains_release_until_admission() {
  BridgedHotkeyDelivery delivery;
  assert(delivery.set_desired(true, "Ctrl+Shift+Space"));
  const auto press = delivery.pending_transition();
  assert(delivery.mark_accepted(press));
  assert(delivery.accepted_pressed());

  assert(delivery.set_desired(false, "ignored"));
  const auto first_release_attempt = delivery.pending_transition();
  assert(first_release_attempt.valid);
  assert(!first_release_attempt.pressed);
  assert(first_release_attempt.hotkey == "Ctrl+Shift+Space");

  // A queue-full response does not call mark_accepted(). The exact release
  // remains available to the next main-loop retry.
  const auto retry = delivery.pending_transition();
  assert(retry.valid);
  assert(retry.desired_generation ==
         first_release_attempt.desired_generation);
  assert(retry.hotkey == first_release_attempt.hotkey);
  assert(delivery.mark_accepted(retry));
  assert(!delivery.pending());
  assert(!delivery.accepted_pressed());
}

void changed_bridged_hotkey_releases_old_value_before_pressing_new_value() {
  BridgedHotkeyDelivery delivery;
  assert(delivery.set_desired(true, "RightMeta"));
  assert(delivery.mark_accepted(delivery.pending_transition()));

  assert(delivery.set_desired(true, "Ctrl+Shift+Space"));
  const auto release_old = delivery.pending_transition();
  assert(release_old.valid);
  assert(!release_old.pressed);
  assert(release_old.hotkey == "RightMeta");
  assert(delivery.mark_accepted(release_old));

  const auto press_new = delivery.pending_transition();
  assert(press_new.valid);
  assert(press_new.pressed);
  assert(press_new.hotkey == "Ctrl+Shift+Space");
  assert(delivery.mark_accepted(press_new));
  assert(!delivery.pending());
}

void bridged_hotkey_endpoint_change_suppresses_replay_until_release() {
  KeyboardTransportLatch latch;
  BridgedHotkeyDelivery delivery;
  assert(delivery.set_desired(true, "RightMeta"));
  assert(delivery.mark_accepted(delivery.pending_transition()));
  assert(latch.select_for_snapshot(false, false, 0, true, kBleEpoch) ==
         KeyboardTransportOwner::Ble);

  assert(latch.observe_transport_state(
      false, 0, true, kBleEpoch + 1));
  delivery.reset_to_desired();
  assert(!delivery.pending());
  assert(latch.owner() == KeyboardTransportOwner::Suppressed);

  assert(delivery.set_desired(false, {}));
  const auto suppressed_release = delivery.pending_transition();
  assert(suppressed_release.valid);
  assert(!suppressed_release.pressed);
  assert(latch.select_for_snapshot(
             true, false, 0, true, kBleEpoch + 1) ==
         KeyboardTransportOwner::None);
  assert(delivery.mark_accepted(suppressed_release));
  latch.commit_snapshot(true);
  assert(!delivery.pending());

  assert(delivery.set_desired(true, "RightMeta"));
  assert(latch.select_for_snapshot(
             false, false, 0, true, kBleEpoch + 1) ==
         KeyboardTransportOwner::Ble);
}

void pop_front(HidReportQueue* queue) {
  ai_keyboard::QueuedHidReport front;
  assert(queue->front(&front));
  assert(queue->pop_if_sequence(front.sequence));
}

void ble_full_queue_retries_two_overlapping_bridged_releases() {
  HidReportQueue queue;
  constexpr std::size_t app_command_capacity =
      ai_keyboard::kHidReportQueueCapacity -
      (2 * ai_keyboard::kKeyboardStateSourceCount);
  static_assert(app_command_capacity >= 2);
  const std::uint8_t filler = 0xA5;
  for (std::size_t index = 0; index < app_command_capacity - 2; ++index) {
    assert(queue.push_classified(
                    0x11,
                    &filler,
                    1,
                    static_cast<std::uint32_t>(index),
                    HidReportClass::AppCommand)
               .accepted());
  }

  BridgedHotkeyDelivery voice;
  BridgedHotkeyDelivery edit;
  assert(voice.set_desired(true, "RightMeta"));
  assert(edit.set_desired(true, "RightOption"));
  for (auto* delivery : {&voice, &edit}) {
    const auto transition = delivery->pending_transition();
    const std::uint8_t state = transition.pressed ? 1 : 0;
    assert(queue.push_classified(
                    0x11, &state, 1, 100, HidReportClass::AppCommand)
               .accepted());
    assert(delivery->mark_accepted(transition));
  }
  assert(queue.size() == app_command_capacity);

  assert(voice.set_desired(false, {}));
  assert(edit.set_desired(false, {}));
  for (auto* delivery : {&voice, &edit}) {
    const auto transition = delivery->pending_transition();
    const std::uint8_t state = 0;
    const auto result = queue.push_classified(
        0x11, &state, 1, 101, HidReportClass::AppCommand);
    assert(result.status == HidQueuePushStatus::Full);
    assert(delivery->pending());
  }

  // Each release survives its first rejection and is accepted as queue space
  // becomes available. No physical edge has to occur again.
  for (auto* delivery : {&voice, &edit}) {
    pop_front(&queue);
    const auto transition = delivery->pending_transition();
    const std::uint8_t state = 0;
    assert(queue.push_classified(
                    0x11, &state, 1, 102, HidReportClass::AppCommand)
               .accepted());
    assert(delivery->mark_accepted(transition));
    assert(!delivery->pending());
  }
}

void usb_full_queue_retries_last_overlapping_bridged_release() {
  HidReportQueue queue;
  const std::uint8_t filler = 0x5A;
  for (std::size_t index = 0;
       index < ai_keyboard::kHidReportQueueCapacity - 3;
       ++index) {
    assert(queue.push(0x11, &filler, 1, 0));
  }

  BridgedHotkeyDelivery voice;
  BridgedHotkeyDelivery edit;
  assert(voice.set_desired(true, "RightMeta"));
  assert(edit.set_desired(true, "RightOption"));
  for (auto* delivery : {&voice, &edit}) {
    const auto transition = delivery->pending_transition();
    const std::uint8_t state = 1;
    assert(queue.push(0x11, &state, 1, 1, nullptr, 1));
    assert(delivery->mark_accepted(transition));
  }

  assert(voice.set_desired(false, {}));
  auto voice_release = voice.pending_transition();
  const std::uint8_t released = 0;
  assert(queue.push(0x11, &released, 1, 2));
  assert(voice.mark_accepted(voice_release));
  assert(queue.size() == ai_keyboard::kHidReportQueueCapacity);

  assert(edit.set_desired(false, {}));
  const auto rejected_edit_release = edit.pending_transition();
  assert(!queue.push(0x11, &released, 1, 3));
  assert(edit.pending());

  pop_front(&queue);
  const auto retry = edit.pending_transition();
  assert(retry.hotkey == rejected_edit_release.hotkey);
  assert(queue.push(0x11, &released, 1, 4));
  assert(edit.mark_accepted(retry));
  assert(!edit.pending());
}

int main() {
  hid_events_prefer_usb_even_when_ble_is_connected();
  fixed_text_prefers_usb_even_when_ble_is_connected();
  fixed_text_prefers_usb_when_ble_is_not_connected();
  app_commands_prefer_usb_even_when_ble_is_connected();
  usb_endpoint_lifetime_treats_every_mount_callback_as_a_new_endpoint();
  usb_unmount_remount_is_visible_when_intermediate_state_is_not_sampled();
  usb_endpoint_epoch_wraps_without_exposing_zero();
  usb_physical_presence_filters_brief_disconnect_glitches();
  usb_physical_disconnect_confirmation_survives_clock_wrap();
  usb_physical_disconnect_invalidates_endpoint_without_forging_epoch();
  usb_late_mount_is_ignored_while_physical_link_is_absent();
  usb_mount_callback_rejects_raw_vbus_absent_before_debounce();
  usb_physical_replug_waits_for_real_mount_callback();
  usb_present_or_suspended_path_keeps_existing_mount_semantics();
  usb_presence_tracking_disabled_preserves_legacy_lifecycle();
  physical_usb_disconnect_routes_next_chord_to_ble();
  physical_usb_disconnect_while_held_suppresses_until_release();
  usb_replug_does_not_steal_an_active_ble_chord();
  keyboard_chord_keeps_ble_owner_when_usb_appears();
  keyboard_chord_does_not_migrate_after_owner_disconnects();
  keyboard_chord_started_offline_stays_suppressed_until_release();
  connected_endpoint_without_identity_epoch_is_not_routable();
  rejected_release_keeps_owner_for_retry();
  ble_same_handle_reuse_is_a_fresh_endpoint_lifetime();
  usb_remount_is_a_fresh_endpoint_lifetime();
  unchanged_epoch_does_not_invalidate_active_chord();
  invalidated_endpoint_refreshes_delivery_baseline_until_release();
  overlapping_bridged_sources_release_independently();
  rejected_bridged_press_and_release_before_admission_coalesces();
  accepted_bridged_press_retains_release_until_admission();
  changed_bridged_hotkey_releases_old_value_before_pressing_new_value();
  bridged_hotkey_endpoint_change_suppresses_replay_until_release();
  ble_full_queue_retries_two_overlapping_bridged_releases();
  usb_full_queue_retries_last_overlapping_bridged_release();
  return 0;
}
