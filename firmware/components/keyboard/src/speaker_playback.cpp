#include "keyboard/speaker_playback.h"

#include <limits>

namespace ai_keyboard {

SpeakerPlaybackTicket SpeakerPlayback::request() {
  if (phase_ != SpeakerPlaybackPhase::Idle) {
    return {};
  }

  active_generation_ = next_generation();
  pending_result_ = SpeakerPlaybackResult::None;
  phase_ = SpeakerPlaybackPhase::Starting;
  return {true, active_generation_};
}

bool SpeakerPlayback::mark_started(std::uint32_t generation) {
  if (!matches(generation) || phase_ != SpeakerPlaybackPhase::Starting) {
    return false;
  }
  phase_ = SpeakerPlaybackPhase::Playing;
  return true;
}

bool SpeakerPlayback::finish(std::uint32_t generation) {
  return begin_drain(generation, SpeakerPlaybackResult::Succeeded);
}

bool SpeakerPlayback::cancel(std::uint32_t generation) {
  return begin_drain(generation, SpeakerPlaybackResult::Cancelled);
}

bool SpeakerPlayback::fail(std::uint32_t generation) {
  return begin_drain(generation, SpeakerPlaybackResult::Failed);
}

bool SpeakerPlayback::mark_drained(std::uint32_t generation) {
  if (!matches(generation) || phase_ != SpeakerPlaybackPhase::Draining) {
    return false;
  }

  last_result_ = pending_result_;
  last_completed_generation_ = active_generation_;
  active_generation_ = 0;
  pending_result_ = SpeakerPlaybackResult::None;
  phase_ = SpeakerPlaybackPhase::Idle;
  return true;
}

SpeakerPlaybackPhase SpeakerPlayback::phase() const {
  return phase_;
}

std::uint32_t SpeakerPlayback::active_generation() const {
  return active_generation_;
}

bool SpeakerPlayback::active() const {
  return phase_ != SpeakerPlaybackPhase::Idle;
}

bool SpeakerPlayback::sleep_blocked() const {
  return active();
}

bool SpeakerPlayback::power_required() const {
  return active();
}

SpeakerPlaybackResult SpeakerPlayback::pending_result() const {
  return pending_result_;
}

SpeakerPlaybackResult SpeakerPlayback::last_result() const {
  return last_result_;
}

std::uint32_t SpeakerPlayback::last_completed_generation() const {
  return last_completed_generation_;
}

bool SpeakerPlayback::begin_drain(std::uint32_t generation,
                                  SpeakerPlaybackResult result) {
  if (!matches(generation)) {
    return false;
  }

  if (phase_ == SpeakerPlaybackPhase::Draining) {
    if (result == SpeakerPlaybackResult::Failed) {
      pending_result_ = SpeakerPlaybackResult::Failed;
    }
    return true;
  }

  if (phase_ != SpeakerPlaybackPhase::Starting &&
      phase_ != SpeakerPlaybackPhase::Playing) {
    return false;
  }

  pending_result_ = result;
  phase_ = SpeakerPlaybackPhase::Draining;
  return true;
}

bool SpeakerPlayback::matches(std::uint32_t generation) const {
  return generation != 0 && phase_ != SpeakerPlaybackPhase::Idle &&
         generation == active_generation_;
}

std::uint32_t SpeakerPlayback::next_generation() {
  if (generation_counter_ == std::numeric_limits<std::uint32_t>::max()) {
    generation_counter_ = 1;
  } else {
    ++generation_counter_;
  }
  return generation_counter_;
}

}  // namespace ai_keyboard
