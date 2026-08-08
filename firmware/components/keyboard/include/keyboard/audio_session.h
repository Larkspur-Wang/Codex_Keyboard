#pragma once

#include <cstdint>
#include <string>

namespace ai_keyboard {

enum class AudioSessionPhase {
  Idle,
  Starting,
  Streaming,
  Recovering,
  Stopping,
};

enum class AudioSessionStartResult {
  Started,
  AlreadyActive,
  NeedsStop,
  Rejected,
};

class AudioSessionLifecycle {
 public:
  AudioSessionStartResult request_start(std::uint64_t session_id);
  bool request_stop(std::uint64_t session_id, const char* reason);
  bool mark_streaming(std::uint32_t generation);
  bool mark_recovering(std::uint32_t generation);
  bool finish(std::uint32_t generation, const char* reason);

  bool owns(std::uint32_t generation) const;
  bool should_run(std::uint32_t generation) const;
  bool active() const;
  std::uint64_t session_id() const;
  std::uint32_t generation() const;
  AudioSessionPhase phase() const;
  const std::string& stop_reason() const;

 private:
  AudioSessionPhase phase_ = AudioSessionPhase::Idle;
  std::uint64_t session_id_ = 0;
  std::uint32_t generation_ = 0;
  std::string stop_reason_;
};

const char* audio_session_phase_name(AudioSessionPhase phase);

}  // namespace ai_keyboard
