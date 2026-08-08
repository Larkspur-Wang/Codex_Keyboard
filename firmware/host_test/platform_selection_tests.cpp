#include <cassert>

#include "keyboard/platform_selection.h"

using ai_keyboard::InputId;
using ai_keyboard::InputPhase;
using ai_keyboard::PlatformSelectionController;
using ai_keyboard::PlatformSelectionOutcome;

void selects_macos_only_after_the_key_is_released_and_guarded() {
  PlatformSelectionController controller;
  controller.arm(1000, 10000);

  assert(controller.handle_event(InputId::Key1, InputPhase::Pressed, 1100).consumed);
  assert(controller.handle_event(InputId::Key1, InputPhase::Released, 1140).consumed);
  assert(controller.update(1239).outcome == PlatformSelectionOutcome::None);
  assert(controller.update(1240).outcome == PlatformSelectionOutcome::MacOS);
  assert(!controller.active());
}

void selects_windows_and_does_not_leak_the_selection_click() {
  PlatformSelectionController controller;
  controller.arm(0, 10000);

  assert(controller.handle_event(InputId::Key2, InputPhase::Pressed, 20).consumed);
  assert(controller.handle_event(InputId::Key2, InputPhase::Released, 40).consumed);
  assert(controller.update(140).outcome == PlatformSelectionOutcome::Windows);
  const auto next_press =
      controller.handle_event(InputId::Key2, InputPhase::Pressed, 300);
  assert(!next_press.consumed);
  assert(next_press.outcome == PlatformSelectionOutcome::None);
}

void simultaneous_keys_conflict_and_both_releases_are_consumed() {
  PlatformSelectionController controller;
  controller.arm(0, 10000);

  assert(controller.handle_event(InputId::Key1, InputPhase::Pressed, 10).consumed);
  const auto conflict =
      controller.handle_event(InputId::Key2, InputPhase::Pressed, 11);
  assert(conflict.consumed);
  assert(conflict.outcome == PlatformSelectionOutcome::Conflict);
  assert(controller.handle_event(InputId::Key1, InputPhase::Released, 30).consumed);
  assert(controller.handle_event(InputId::Key2, InputPhase::Released, 31).consumed);
  assert(!controller.handle_event(InputId::Key1, InputPhase::Pressed, 100).consumed);
}

void near_simultaneous_second_key_during_guard_conflicts_without_leaking() {
  PlatformSelectionController controller;
  controller.arm(0, 10000);

  assert(controller.handle_event(InputId::Key1, InputPhase::Pressed, 10).consumed);
  assert(controller.handle_event(InputId::Key1, InputPhase::Released, 20).consumed);
  const auto conflict =
      controller.handle_event(InputId::Key2, InputPhase::Pressed, 80);
  assert(conflict.consumed);
  assert(conflict.outcome == PlatformSelectionOutcome::Conflict);
  assert(controller.handle_event(InputId::Key2, InputPhase::Released, 100).consumed);
  assert(controller.update(200).outcome == PlatformSelectionOutcome::None);
}

void later_key_after_guard_finishes_first_choice_before_processing_edge() {
  PlatformSelectionController controller;
  controller.arm(0, 10000);

  assert(controller.handle_event(InputId::Key1, InputPhase::Pressed, 10).consumed);
  assert(controller.handle_event(InputId::Key1, InputPhase::Released, 20).consumed);
  const auto result =
      controller.handle_event(InputId::Key2, InputPhase::Pressed, 121);
  assert(!result.consumed);
  assert(result.outcome == PlatformSelectionOutcome::MacOS);
}

void timeout_cancels_selection_and_drains_a_held_key() {
  PlatformSelectionController controller;
  controller.arm(100, 1000);

  assert(controller.handle_event(InputId::Key1, InputPhase::Pressed, 200).consumed);
  assert(controller.update(1100).outcome == PlatformSelectionOutcome::TimedOut);
  assert(controller.handle_event(InputId::Key1, InputPhase::Released, 1200).consumed);
}

void another_key_cancels_selection_but_passes_through() {
  PlatformSelectionController controller;
  controller.arm(0, 10000);

  assert(controller.handle_event(InputId::Key1, InputPhase::Pressed, 10).consumed);
  const auto cancelled =
      controller.handle_event(InputId::Key3, InputPhase::Pressed, 20);
  assert(!cancelled.consumed);
  assert(cancelled.outcome == PlatformSelectionOutcome::Cancelled);
  assert(controller.handle_event(InputId::Key1, InputPhase::Released, 30).consumed);
}

int main() {
  selects_macos_only_after_the_key_is_released_and_guarded();
  selects_windows_and_does_not_leak_the_selection_click();
  simultaneous_keys_conflict_and_both_releases_are_consumed();
  near_simultaneous_second_key_during_guard_conflicts_without_leaking();
  later_key_after_guard_finishes_first_choice_before_processing_edge();
  timeout_cancels_selection_and_drains_a_held_key();
  another_key_cancels_selection_but_passes_through();
  return 0;
}
