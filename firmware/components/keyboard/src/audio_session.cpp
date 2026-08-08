#include "keyboard/audio_session.h"

namespace ai_keyboard {

AudioSessionStartResult AudioSessionLifecycle::request_start(std::uint64_t session_id) {
  if (session_id == 0) {
    return AudioSessionStartResult::Rejected;
  }
  if (phase_ != AudioSessionPhase::Idle) {
    if (session_id_ == session_id && phase_ != AudioSessionPhase::Stopping) {
      return AudioSessionStartResult::AlreadyActive;
    }
    return AudioSessionStartResult::NeedsStop;
  }

  generation_ += 1;
  if (generation_ == 0) {
    generation_ = 1;
  }
  session_id_ = session_id;
  phase_ = AudioSessionPhase::Starting;
  stop_reason_.clear();
  return AudioSessionStartResult::Started;
}

bool AudioSessionLifecycle::request_stop(std::uint64_t session_id, const char* reason) {
  if (phase_ == AudioSessionPhase::Idle) {
    return false;
  }
  if (session_id != 0 && session_id_ != session_id) {
    return false;
  }
  phase_ = AudioSessionPhase::Stopping;
  stop_reason_ = reason == nullptr ? "stop_requested" : reason;
  return true;
}

bool AudioSessionLifecycle::mark_streaming(std::uint32_t generation) {
  if (!owns(generation) || phase_ == AudioSessionPhase::Stopping) {
    return false;
  }
  phase_ = AudioSessionPhase::Streaming;
  return true;
}

bool AudioSessionLifecycle::mark_recovering(std::uint32_t generation) {
  if (!owns(generation) || phase_ == AudioSessionPhase::Stopping) {
    return false;
  }
  phase_ = AudioSessionPhase::Recovering;
  return true;
}

bool AudioSessionLifecycle::finish(std::uint32_t generation, const char* reason) {
  if (!owns(generation)) {
    return false;
  }
  if (phase_ != AudioSessionPhase::Stopping || stop_reason_.empty()) {
    stop_reason_ = reason == nullptr ? "stream_finished" : reason;
  }
  phase_ = AudioSessionPhase::Idle;
  session_id_ = 0;
  return true;
}

bool AudioSessionLifecycle::owns(std::uint32_t generation) const {
  return generation != 0 && generation_ == generation && phase_ != AudioSessionPhase::Idle;
}

bool AudioSessionLifecycle::should_run(std::uint32_t generation) const {
  return owns(generation) && phase_ != AudioSessionPhase::Stopping;
}

bool AudioSessionLifecycle::active() const {
  return phase_ != AudioSessionPhase::Idle;
}

std::uint64_t AudioSessionLifecycle::session_id() const {
  return session_id_;
}

std::uint32_t AudioSessionLifecycle::generation() const {
  return generation_;
}

AudioSessionPhase AudioSessionLifecycle::phase() const {
  return phase_;
}

const std::string& AudioSessionLifecycle::stop_reason() const {
  return stop_reason_;
}

const char* audio_session_phase_name(AudioSessionPhase phase) {
  switch (phase) {
    case AudioSessionPhase::Idle:
      return "idle";
    case AudioSessionPhase::Starting:
      return "starting";
    case AudioSessionPhase::Streaming:
      return "streaming";
    case AudioSessionPhase::Recovering:
      return "recovering";
    case AudioSessionPhase::Stopping:
      return "stopping";
  }
  return "unknown";
}

}  // namespace ai_keyboard
