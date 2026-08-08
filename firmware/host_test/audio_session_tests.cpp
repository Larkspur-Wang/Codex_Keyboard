#include <cassert>
#include <string>

#include "keyboard/audio_session.h"

int main() {
  ai_keyboard::AudioSessionLifecycle lifecycle;
  assert(!lifecycle.active());
  assert(lifecycle.request_start(0) == ai_keyboard::AudioSessionStartResult::Rejected);

  assert(lifecycle.request_start(42) == ai_keyboard::AudioSessionStartResult::Started);
  const auto first_generation = lifecycle.generation();
  assert(first_generation != 0);
  assert(lifecycle.should_run(first_generation));
  assert(lifecycle.mark_streaming(first_generation));
  assert(lifecycle.phase() == ai_keyboard::AudioSessionPhase::Streaming);
  assert(lifecycle.request_start(42) == ai_keyboard::AudioSessionStartResult::AlreadyActive);
  assert(lifecycle.request_start(43) == ai_keyboard::AudioSessionStartResult::NeedsStop);

  assert(!lifecycle.request_stop(43, "stale_stop"));
  assert(lifecycle.request_stop(42, "client_stop"));
  assert(!lifecycle.should_run(first_generation));
  assert(lifecycle.finish(first_generation, "stream_stop"));
  assert(!lifecycle.active());
  assert(lifecycle.stop_reason() == "client_stop");

  assert(lifecycle.request_start(43) == ai_keyboard::AudioSessionStartResult::Started);
  const auto second_generation = lifecycle.generation();
  assert(second_generation != first_generation);
  assert(!lifecycle.mark_recovering(first_generation));
  assert(!lifecycle.finish(first_generation, "stale_cleanup"));
  assert(lifecycle.session_id() == 43);
  assert(lifecycle.mark_recovering(second_generation));
  assert(lifecycle.phase() == ai_keyboard::AudioSessionPhase::Recovering);
  assert(lifecycle.mark_streaming(second_generation));
  assert(lifecycle.finish(second_generation, "udp_recovery_exhausted"));
  assert(lifecycle.stop_reason() == "udp_recovery_exhausted");

  return 0;
}
