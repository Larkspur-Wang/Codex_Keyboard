#include <cassert>

#include "keyboard/debounce.h"

void immediate_bounce_does_not_emit_a_change() {
  ai_keyboard::DebouncedInput input(20);

  input.reset(false, 1000);
  const auto first = input.update(true, 1001);
  const auto bounced_back = input.update(false, 1002);

  assert(!first.changed);
  assert(!bounced_back.changed);
  assert(bounced_back.filtered_transition);
  assert(!input.stable_state());
}

void stable_candidate_emits_once_after_interval() {
  ai_keyboard::DebouncedInput input(20);

  input.reset(false, 1000);
  assert(!input.update(true, 1005).changed);
  assert(!input.update(true, 1019).changed);

  const auto accepted = input.update(true, 1025);
  assert(accepted.changed);
  assert(accepted.state);
  assert(input.stable_state());

  const auto duplicate = input.update(true, 1050);
  assert(!duplicate.changed);
  assert(duplicate.state);
}

void release_uses_the_same_stable_interval() {
  ai_keyboard::DebouncedInput input(20);

  input.reset(true, 2000);
  assert(!input.update(false, 2001).changed);
  assert(!input.update(false, 2010).changed);

  const auto released = input.update(false, 2021);
  assert(released.changed);
  assert(!released.state);
}

void a_later_opposite_edge_commits_a_stable_pulse() {
  ai_keyboard::DebouncedInput input(8);

  input.reset(false, 4000);
  assert(!input.update(true, 4010).changed);

  const auto pressed = input.update(false, 4060);
  assert(pressed.changed);
  assert(pressed.state);

  assert(!input.update(false, 4067).changed);
  const auto released = input.update(false, 4068);
  assert(released.changed);
  assert(!released.state);
}

void wake_state_is_latched_once_and_release_still_debounces() {
  ai_keyboard::DebouncedInput input(20);

  input.reset(false, 3000);
  const auto pressed = input.latch_wake_state(true, 3010);
  assert(pressed.changed);
  assert(pressed.state);
  assert(input.stable_state());

  const auto duplicate = input.latch_wake_state(true, 3011);
  assert(!duplicate.changed);
  assert(duplicate.state);

  assert(!input.update(false, 3012).changed);
  const auto released = input.update(false, 3032);
  assert(released.changed);
  assert(!released.state);
}

void asymmetric_debounce_preserves_a_fast_release_and_repress() {
  ai_keyboard::DebouncedInput input(5, 3);

  input.reset(true, 5000);
  assert(!input.update(false, 5001).changed);
  const auto released = input.update(false, 5004);
  assert(released.changed);
  assert(!released.state);

  assert(!input.update(true, 5005).changed);
  const auto repressed = input.update(true, 5010);
  assert(repressed.changed);
  assert(repressed.state);
}

void short_release_bounce_is_filtered_without_losing_the_hold() {
  ai_keyboard::DebouncedInput input(5, 3);

  input.reset(true, 6000);
  assert(!input.update(false, 6001).changed);
  const auto bounced = input.update(true, 6002);
  assert(!bounced.changed);
  assert(bounced.filtered_transition);
  assert(input.stable_state());
}

int main() {
  immediate_bounce_does_not_emit_a_change();
  stable_candidate_emits_once_after_interval();
  release_uses_the_same_stable_interval();
  a_later_opposite_edge_commits_a_stable_pulse();
  wake_state_is_latched_once_and_release_still_debounces();
  asymmetric_debounce_preserves_a_fast_release_and_repress();
  short_release_bounce_is_filtered_without_losing_the_hold();
  return 0;
}
