#include "keyboard/audio_io_arbiter.h"

namespace ai_keyboard {

bool AudioIoArbiter::try_begin_speaker(std::uint32_t generation) {
  if (generation == 0 || !try_enter_runtime_transition()) {
    return false;
  }

  bool accepted = false;
  if (microphone_generation_.load(std::memory_order_acquire) == 0) {
    std::uint32_t expected = 0;
    accepted = speaker_generation_.compare_exchange_strong(
        expected,
        generation,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
  }

  // Close the race where a microphone request arrives between the first
  // microphone check and the speaker reservation.
  if (accepted &&
      microphone_generation_.load(std::memory_order_acquire) != 0) {
    std::uint32_t expected = generation;
    speaker_generation_.compare_exchange_strong(
        expected,
        0,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    accepted = false;
  }
  leave_runtime_transition();
  return accepted;
}

bool AudioIoArbiter::finish_speaker(std::uint32_t generation) {
  if (generation == 0) {
    return false;
  }
  std::uint32_t expected = generation;
  return speaker_generation_.compare_exchange_strong(
      expected,
      0,
      std::memory_order_acq_rel,
      std::memory_order_acquire);
}

bool AudioIoArbiter::request_microphone(std::uint32_t generation) {
  if (generation == 0 || !try_enter_runtime_transition()) {
    return false;
  }
  microphone_power_ready_generation_.store(0, std::memory_order_release);
  microphone_generation_.store(generation, std::memory_order_release);
  leave_runtime_transition();
  return true;
}

bool AudioIoArbiter::mark_microphone_power_ready(std::uint32_t generation) {
  if (generation == 0 ||
      microphone_generation_.load(std::memory_order_acquire) != generation) {
    return false;
  }
  microphone_power_ready_generation_.store(generation,
                                           std::memory_order_release);
  return true;
}

bool AudioIoArbiter::finish_microphone(std::uint32_t generation) {
  if (generation == 0) {
    return false;
  }
  std::uint32_t expected = generation;
  if (!microphone_generation_.compare_exchange_strong(
          expected,
          0,
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  expected = generation;
  microphone_power_ready_generation_.compare_exchange_strong(
      expected,
      0,
      std::memory_order_acq_rel,
      std::memory_order_acquire);
  return true;
}

bool AudioIoArbiter::speaker_active() const {
  return speaker_generation() != 0;
}

std::uint32_t AudioIoArbiter::speaker_generation() const {
  return speaker_generation_.load(std::memory_order_acquire);
}

bool AudioIoArbiter::microphone_requested() const {
  return microphone_generation() != 0;
}

std::uint32_t AudioIoArbiter::microphone_generation() const {
  return microphone_generation_.load(std::memory_order_acquire);
}

bool AudioIoArbiter::microphone_hardware_ready(
    std::uint32_t generation) const {
  return generation != 0 &&
         microphone_generation_.load(std::memory_order_acquire) == generation &&
         microphone_power_ready_generation_.load(std::memory_order_acquire) ==
             generation &&
         speaker_generation_.load(std::memory_order_acquire) == 0;
}

bool AudioIoArbiter::try_begin_deep_sleep_quiesce() {
  std::uint32_t expected = 0;
  if (!runtime_transition_gate_.compare_exchange_strong(
          expected,
          kDeepSleepQuiescingBit,
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }

  if (speaker_generation_.load(std::memory_order_acquire) != 0 ||
      microphone_generation_.load(std::memory_order_acquire) != 0) {
    runtime_transition_gate_.store(0, std::memory_order_release);
    return false;
  }
  return true;
}

bool AudioIoArbiter::cancel_deep_sleep_quiesce() {
  std::uint32_t expected = kDeepSleepQuiescingBit;
  return runtime_transition_gate_.compare_exchange_strong(
      expected,
      0,
      std::memory_order_acq_rel,
      std::memory_order_acquire);
}

bool AudioIoArbiter::deep_sleep_quiescing() const {
  return (runtime_transition_gate_.load(std::memory_order_acquire) &
          kDeepSleepQuiescingBit) != 0;
}

bool AudioIoArbiter::try_enter_runtime_transition() {
  auto state = runtime_transition_gate_.load(std::memory_order_acquire);
  while ((state & kDeepSleepQuiescingBit) == 0) {
    if ((state & kRuntimeTransitionCountMask) ==
        kRuntimeTransitionCountMask) {
      return false;
    }
    if (runtime_transition_gate_.compare_exchange_weak(
            state,
            state + 1,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void AudioIoArbiter::leave_runtime_transition() {
  runtime_transition_gate_.fetch_sub(1, std::memory_order_release);
}

}  // namespace ai_keyboard
