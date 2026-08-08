#include <cassert>

#include "keyboard/audio_io_arbiter.h"

namespace {

using ai_keyboard::AudioIoArbiter;

void speaker_requires_an_idle_microphone_and_one_active_generation() {
  AudioIoArbiter arbiter;
  assert(arbiter.try_begin_speaker(10));
  assert(arbiter.speaker_active());
  assert(arbiter.speaker_generation() == 10);
  assert(!arbiter.try_begin_speaker(11));
  assert(!arbiter.finish_speaker(11));
  assert(arbiter.finish_speaker(10));
  assert(!arbiter.speaker_active());

  assert(arbiter.request_microphone(20));
  assert(!arbiter.try_begin_speaker(12));
  assert(arbiter.finish_microphone(20));
  assert(arbiter.try_begin_speaker(12));
}

void microphone_waits_for_power_and_speaker_drain() {
  AudioIoArbiter arbiter;
  assert(arbiter.try_begin_speaker(30));
  assert(arbiter.request_microphone(40));
  assert(arbiter.microphone_requested());
  assert(!arbiter.microphone_hardware_ready(40));

  assert(arbiter.mark_microphone_power_ready(40));
  assert(!arbiter.microphone_hardware_ready(40));
  assert(arbiter.finish_speaker(30));
  assert(arbiter.microphone_hardware_ready(40));

  assert(arbiter.finish_microphone(40));
  assert(!arbiter.microphone_requested());
  assert(!arbiter.microphone_hardware_ready(40));
}

void stale_completions_cannot_release_a_new_generation() {
  AudioIoArbiter arbiter;
  assert(arbiter.request_microphone(50));
  assert(arbiter.mark_microphone_power_ready(50));
  assert(arbiter.request_microphone(51));
  assert(!arbiter.finish_microphone(50));
  assert(arbiter.microphone_generation() == 51);
  assert(!arbiter.microphone_hardware_ready(51));
  assert(arbiter.mark_microphone_power_ready(51));
  assert(arbiter.microphone_hardware_ready(51));
  assert(arbiter.finish_microphone(51));

  assert(arbiter.try_begin_speaker(60));
  assert(!arbiter.finish_speaker(59));
  assert(arbiter.speaker_generation() == 60);
  assert(arbiter.finish_speaker(60));
}

void zero_generations_fail_closed() {
  AudioIoArbiter arbiter;
  assert(!arbiter.try_begin_speaker(0));
  assert(!arbiter.finish_speaker(0));
  assert(!arbiter.request_microphone(0));
  assert(!arbiter.mark_microphone_power_ready(0));
  assert(!arbiter.finish_microphone(0));
  assert(!arbiter.microphone_hardware_ready(0));
}

void deep_sleep_quiesce_rejects_new_audio_owners_until_cancelled() {
  AudioIoArbiter arbiter;

  assert(arbiter.try_begin_deep_sleep_quiesce());
  assert(arbiter.deep_sleep_quiescing());
  assert(!arbiter.request_microphone(41));
  assert(!arbiter.try_begin_speaker(42));
  assert(arbiter.microphone_generation() == 0);
  assert(arbiter.speaker_generation() == 0);

  assert(arbiter.cancel_deep_sleep_quiesce());
  assert(!arbiter.deep_sleep_quiescing());
  assert(arbiter.request_microphone(43));
  assert(arbiter.finish_microphone(43));
}

void active_audio_prevents_deep_sleep_quiesce_without_sticking_gate() {
  AudioIoArbiter arbiter;

  assert(arbiter.request_microphone(51));
  assert(!arbiter.try_begin_deep_sleep_quiesce());
  assert(!arbiter.deep_sleep_quiescing());
  assert(arbiter.finish_microphone(51));

  assert(arbiter.try_begin_speaker(52));
  assert(!arbiter.try_begin_deep_sleep_quiesce());
  assert(!arbiter.deep_sleep_quiescing());
  assert(arbiter.finish_speaker(52));

  assert(arbiter.try_begin_deep_sleep_quiesce());
  assert(arbiter.cancel_deep_sleep_quiesce());
}

}  // namespace

int main() {
  speaker_requires_an_idle_microphone_and_one_active_generation();
  microphone_waits_for_power_and_speaker_drain();
  stale_completions_cannot_release_a_new_generation();
  zero_generations_fail_closed();
  deep_sleep_quiesce_rejects_new_audio_owners_until_cancelled();
  active_audio_prevents_deep_sleep_quiesce_without_sticking_gate();
  return 0;
}
