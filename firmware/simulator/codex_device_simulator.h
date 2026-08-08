#pragma once

#include <cstdint>
#include <vector>

#include "keyboard/codex_slot_state.h"

namespace easy_codex {

class CodexDeviceSimulator {
 public:
  void connect(std::uint32_t generation);
  void disconnect();
  void input(ai_keyboard::InputId input, ai_keyboard::InputPhase phase);
  PlaybackBeginResult begin_playback(std::uint8_t slot,
                                     std::uint64_t summary_generation,
                                     std::uint64_t lease,
                                     std::uint32_t request_generation,
                                     std::uint32_t frames,
                                     std::uint64_t samples);
  bool start_playback();
  PlaybackFrameResult deliver_frame(std::uint32_t sequence,
                                    bool drop = false);
  bool end_transfer(std::uint32_t last_sequence);
  bool drain(std::uint64_t played_samples);
  bool acknowledge_finished();
  bool acknowledge_finished(const PlaybackIdentity& identity);

  const CodexSlotState& state() const;
  const std::vector<DeviceAction>& emitted() const;
  void clear_emitted();
  std::uint32_t connection_generation() const;

 private:
  void append(const DeviceTransition& transition);

  CodexSlotState state_;
  std::vector<DeviceAction> emitted_;
  std::uint32_t connection_generation_ = 0;
  PlaybackIdentity active_identity_{};
};

}  // namespace easy_codex
