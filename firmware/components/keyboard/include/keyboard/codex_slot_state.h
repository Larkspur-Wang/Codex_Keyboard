#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "keyboard/keymap.h"

namespace easy_codex {

constexpr std::uint8_t kFirstSlot = 1;
constexpr std::uint8_t kLastSlot = 4;
constexpr std::uint8_t kCaptureSessionMagic = 0xEC;
constexpr std::uint32_t kCaptureSessionMaxConnectionGeneration = 0x000FFFFF;

struct CaptureSessionIdentity {
  std::uint8_t slot = 0;
  std::uint32_t capture_generation = 0;
  std::uint32_t connection_generation = 0;
};

std::uint64_t encode_capture_session_identity(
    const CaptureSessionIdentity& identity);
bool decode_capture_session_identity(
    std::uint64_t session_id,
    CaptureSessionIdentity* identity);

enum class DeviceActionKind : std::uint8_t {
  None,
  PttStarted,
  PttEnded,
  PlayRequested,
  PlaybackPreempted,
  PlaybackFinished,
  RejectedBusy,
};

struct PlaybackIdentity {
  std::uint8_t slot = 0;
  std::uint64_t summary_generation = 0;
  std::uint64_t lease = 0;
  std::uint32_t connection_generation = 0;
};

bool operator==(const PlaybackIdentity& lhs, const PlaybackIdentity& rhs);
bool operator!=(const PlaybackIdentity& lhs, const PlaybackIdentity& rhs);
bool valid_playback_identity(const PlaybackIdentity& identity);

struct DeviceAction {
  DeviceActionKind kind = DeviceActionKind::None;
  std::uint8_t slot = 0;
  std::uint32_t capture_generation = 0;
  std::uint32_t request_generation = 0;
  PlaybackIdentity playback{};
  std::uint32_t connection_generation = 0;
};

struct DeviceTransition {
  std::array<DeviceAction, 2> actions{};
  std::size_t count = 0;

  void push(const DeviceAction& action);
};

enum class PlaybackBeginResult : std::uint8_t {
  Accepted,
  Duplicate,
  Busy,
  Invalid,
};

enum class PlaybackFrameResult : std::uint8_t {
  Accepted,
  Duplicate,
  OutOfOrder,
  Stale,
  Invalid,
};

enum class PlaybackPhase : std::uint8_t {
  Idle,
  Buffering,
  Playing,
  TransferComplete,
  FinishedPendingAck,
};

struct PlaybackBegin {
  PlaybackIdentity identity{};
  std::uint32_t request_generation = 0;
  std::uint32_t total_frames = 0;
  std::uint64_t total_samples = 0;
};

class CodexSlotState {
 public:
  void seed_capture_counter(std::uint32_t seed);
  DeviceTransition handle_input(ai_keyboard::InputId input,
                                ai_keyboard::InputPhase phase,
                                std::uint32_t connection_generation);

  PlaybackBeginResult begin_playback(const PlaybackBegin& begin);
  bool mark_playback_started(const PlaybackIdentity& identity);
  PlaybackFrameResult accept_playback_frame(
      const PlaybackIdentity& identity,
      std::uint32_t frame_sequence);
  bool mark_playback_transfer_complete(
      const PlaybackIdentity& identity,
      std::uint32_t last_frame_sequence);
  bool mark_playback_drained(const PlaybackIdentity& identity,
                             std::uint64_t played_samples);
  bool pending_finished(DeviceAction* action) const;
  bool acknowledge_finished(const PlaybackIdentity& identity);
  bool abort_playback(const PlaybackIdentity& identity);
  DeviceTransition disconnect(std::uint32_t connection_generation);

  bool capture_active() const;
  std::uint8_t capture_slot() const;
  std::uint32_t capture_generation() const;
  std::uint32_t capture_connection_generation() const;
  PlaybackPhase playback_phase() const;
  PlaybackIdentity playback_identity() const;
  std::uint32_t next_playback_frame() const;
  std::uint32_t expected_playback_frames() const;
  std::uint64_t expected_playback_samples() const;
  std::uint8_t pending_play_slot() const;
  std::uint32_t pending_play_request_generation() const;

 private:
  static bool ptt_slot_for_input(ai_keyboard::InputId input,
                                 std::uint8_t* slot);
  static bool play_slot_for_input(ai_keyboard::InputId input,
                                  std::uint8_t* slot);
  static std::uint32_t next_nonzero(std::uint32_t* counter);
  bool playback_matches(const PlaybackIdentity& identity) const;
  DeviceAction preempt_playback();
  void clear_playback();

  std::uint32_t capture_counter_ = 0;
  std::uint32_t request_counter_ = 0;
  std::uint8_t pending_play_slot_ = 0;
  std::uint32_t pending_play_request_generation_ = 0;
  std::uint32_t pending_play_connection_generation_ = 0;
  std::uint32_t active_play_request_generation_ = 0;
  std::uint8_t capture_slot_ = 0;
  std::uint32_t capture_generation_ = 0;
  std::uint32_t capture_connection_generation_ = 0;
  PlaybackPhase playback_phase_ = PlaybackPhase::Idle;
  PlaybackIdentity playback_identity_{};
  std::uint32_t playback_total_frames_ = 0;
  std::uint32_t playback_next_frame_ = 0;
  std::uint64_t playback_total_samples_ = 0;
};

}  // namespace easy_codex
