#include <cassert>

#include "keyboard/encoder.h"

void clockwise_detent_emits_positive_step() {
  ai_keyboard::EncoderDecoder decoder;

  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void counterclockwise_detent_emits_negative_step() {
  ai_keyboard::EncoderDecoder decoder;

  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b00) == -1);
}

void wired_encoder_adjusts_local_speaker_volume() {
  assert(ai_keyboard::adjust_speaker_volume_for_wired_encoder_step(7, -1) == 8);
  assert(ai_keyboard::adjust_speaker_volume_for_wired_encoder_step(7, 1) == 6);
  assert(ai_keyboard::adjust_speaker_volume_for_wired_encoder_step(7, 0) == 7);
}

void local_speaker_volume_is_bounded_and_scales_pcm() {
  assert(ai_keyboard::adjust_speaker_volume_for_wired_encoder_step(10, -4) == 10);
  assert(ai_keyboard::adjust_speaker_volume_for_wired_encoder_step(1, 4) == 1);
  assert(ai_keyboard::speaker_volume_gain_per_mille(1) == 80);
  assert(ai_keyboard::speaker_volume_gain_per_mille(10) == 1000);
  for (std::uint8_t level = 2; level <= 10; ++level) {
    assert(ai_keyboard::speaker_volume_gain_per_mille(level) >
           ai_keyboard::speaker_volume_gain_per_mille(level - 1));
  }
  assert(ai_keyboard::scale_speaker_sample(20'000, 10) == 20'000);
  assert(ai_keyboard::scale_speaker_sample(20'000, 1) == 1'600);
  assert(ai_keyboard::scale_speaker_sample(-20'000, 1) == -1'600);
}

void repeated_same_state_does_not_emit() {
  ai_keyboard::EncoderDecoder decoder;

  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b00) == 0);
}

void reset_to_nonzero_state_does_not_accumulate_a_partial_step() {
  ai_keyboard::EncoderDecoder decoder;

  decoder.reset(0b01);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void pause_mid_detent_resumes_without_losing_the_step() {
  ai_keyboard::EncoderDecoder decoder;

  decoder.reset(0b00);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void invalid_transition_is_counted_and_the_next_detent_recovers() {
  ai_keyboard::EncoderDecoder decoder;

  decoder.reset(0b00);
  assert(decoder.update(0b11) == 0);
  assert(decoder.invalid_transition_count() == 1);
  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void ordered_step_queue_coalesces_runs_but_preserves_reversals() {
  ai_keyboard::EncoderStepQueue queue;
  assert(queue.push(1));
  assert(queue.push(2));
  assert(queue.push(-1));
  assert(queue.push(-3));
  assert(queue.push(1));
  assert(queue.size() == 3);

  int step = 0;
  assert(queue.pop(&step));
  assert(step == 3);
  assert(queue.pop(&step));
  assert(step == -4);
  assert(queue.pop(&step));
  assert(step == 1);
  assert(queue.empty());
}

void rapid_detents_remain_a_single_ordered_run() {
  ai_keyboard::EncoderDecoder decoder;
  ai_keyboard::EncoderStepQueue queue;
  decoder.reset(0b00);

  for (int detent = 0; detent < 12; ++detent) {
    assert(decoder.update(0b01) == 0);
    assert(decoder.update(0b11) == 0);
    assert(decoder.update(0b10) == 0);
    assert(queue.push(decoder.update(0b00)));
  }

  int steps = 0;
  assert(queue.pop(&steps));
  assert(steps == 12);
  assert(queue.empty());
}

int main() {
  clockwise_detent_emits_positive_step();
  counterclockwise_detent_emits_negative_step();
  wired_encoder_adjusts_local_speaker_volume();
  local_speaker_volume_is_bounded_and_scales_pcm();
  repeated_same_state_does_not_emit();
  reset_to_nonzero_state_does_not_accumulate_a_partial_step();
  pause_mid_detent_resumes_without_losing_the_step();
  invalid_transition_is_counted_and_the_next_detent_recovers();
  ordered_step_queue_coalesces_runs_but_preserves_reversals();
  rapid_detents_remain_a_single_ordered_run();
  return 0;
}
