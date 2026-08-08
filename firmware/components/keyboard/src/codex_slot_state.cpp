#include "keyboard/codex_slot_state.h"

#include <limits>

namespace easy_codex {

std::uint64_t encode_capture_session_identity(
    const CaptureSessionIdentity& identity) {
  if (identity.slot < kFirstSlot || identity.slot > kLastSlot ||
      identity.capture_generation == 0 ||
      identity.connection_generation == 0 ||
      identity.connection_generation >
          kCaptureSessionMaxConnectionGeneration) {
    return 0;
  }
  return (static_cast<std::uint64_t>(kCaptureSessionMagic) << 56U) |
         (static_cast<std::uint64_t>(identity.slot) << 52U) |
         (static_cast<std::uint64_t>(identity.connection_generation) << 32U) |
         identity.capture_generation;
}

bool decode_capture_session_identity(
    std::uint64_t session_id,
    CaptureSessionIdentity* identity) {
  if (identity == nullptr ||
      static_cast<std::uint8_t>(session_id >> 56U) !=
          kCaptureSessionMagic ||
      ((session_id >> 52U) & 0x0FU) < kFirstSlot ||
      ((session_id >> 52U) & 0x0FU) > kLastSlot) {
    return false;
  }
  CaptureSessionIdentity decoded{
      static_cast<std::uint8_t>((session_id >> 52U) & 0x0FU),
      static_cast<std::uint32_t>(session_id & 0xFFFFFFFFU),
      static_cast<std::uint32_t>((session_id >> 32U) &
                                 kCaptureSessionMaxConnectionGeneration),
  };
  if (decoded.capture_generation == 0 ||
      decoded.connection_generation == 0) {
    return false;
  }
  *identity = decoded;
  return true;
}

bool operator==(const PlaybackIdentity& lhs, const PlaybackIdentity& rhs) {
  return lhs.slot == rhs.slot &&
         lhs.summary_generation == rhs.summary_generation &&
         lhs.lease == rhs.lease &&
         lhs.connection_generation == rhs.connection_generation;
}

bool operator!=(const PlaybackIdentity& lhs, const PlaybackIdentity& rhs) {
  return !(lhs == rhs);
}

bool valid_playback_identity(const PlaybackIdentity& identity) {
  return identity.slot >= kFirstSlot && identity.slot <= kLastSlot &&
         identity.summary_generation != 0 && identity.lease != 0 &&
         identity.connection_generation != 0;
}

void CodexSlotState::seed_capture_counter(std::uint32_t seed) {
  if (!capture_active() && capture_counter_ == 0U) {
    capture_counter_ = seed;
  }
}

void DeviceTransition::push(const DeviceAction& action) {
  if (action.kind == DeviceActionKind::None || count >= actions.size()) {
    return;
  }
  actions[count++] = action;
}

DeviceTransition CodexSlotState::handle_input(ai_keyboard::InputId input,
                                              ai_keyboard::InputPhase phase,
                                              std::uint32_t connection_generation) {
  DeviceTransition transition;
  std::uint8_t slot = 0;
  const bool ptt_input = ptt_slot_for_input(input, &slot);
  const auto input_index = static_cast<std::size_t>(input);
  if (connection_generation == 0 &&
      input_index >= static_cast<std::size_t>(ai_keyboard::InputId::Key1) &&
      input_index <= static_cast<std::size_t>(ai_keyboard::InputId::Key8)) {
    if (phase == ai_keyboard::InputPhase::Released) {
      return transition;
    }
    if (ptt_input && playback_phase_ != PlaybackPhase::Idle) {
      transition.push(preempt_playback());
    }
    const auto key_index = input_index -
                           static_cast<std::size_t>(
                               ai_keyboard::InputId::Key1);
    transition.push({DeviceActionKind::RejectedBusy,
                     static_cast<std::uint8_t>((key_index % 4U) + 1U)});
    return transition;
  }
  if (ptt_input) {
    if (phase == ai_keyboard::InputPhase::Pressed) {
      if (capture_active()) {
        if (capture_slot_ != slot) {
          transition.push({DeviceActionKind::RejectedBusy, slot});
        }
        return transition;
      }
      if (playback_phase_ != PlaybackPhase::Idle) {
        transition.push(preempt_playback());
      }
      pending_play_slot_ = 0;
      pending_play_request_generation_ = 0;
      pending_play_connection_generation_ = 0;
      capture_slot_ = slot;
      capture_generation_ = next_nonzero(&capture_counter_);
      capture_connection_generation_ = connection_generation;
      DeviceAction action{DeviceActionKind::PttStarted,
                          slot,
                          capture_generation_};
      action.connection_generation = connection_generation;
      transition.push(action);
      return transition;
    }

    if (capture_active() && capture_slot_ == slot) {
      DeviceAction action{DeviceActionKind::PttEnded,
                          slot,
                          capture_generation_};
      action.connection_generation = capture_connection_generation_;
      transition.push(action);
      capture_slot_ = 0;
      capture_generation_ = 0;
      capture_connection_generation_ = 0;
    }
    return transition;
  }

  if (!play_slot_for_input(input, &slot) ||
      phase == ai_keyboard::InputPhase::Released) {
    return transition;
  }

  if (capture_active()) {
    transition.push({DeviceActionKind::RejectedBusy, slot});
    return transition;
  }

  if (playback_phase_ != PlaybackPhase::Idle) {
    const bool toggles_current = playback_identity_.slot == slot;
    transition.push(preempt_playback());
    if (toggles_current) {
      return transition;
    }
  }

  pending_play_slot_ = slot;
  pending_play_request_generation_ = next_nonzero(&request_counter_);
  pending_play_connection_generation_ = connection_generation;
  DeviceAction action{DeviceActionKind::PlayRequested,
                      slot,
                      0,
                      pending_play_request_generation_};
  action.connection_generation = connection_generation;
  transition.push(action);
  return transition;
}

PlaybackBeginResult CodexSlotState::begin_playback(
    const PlaybackBegin& begin) {
  if (!valid_playback_identity(begin.identity) || begin.request_generation == 0 ||
      begin.total_frames == 0 || begin.total_samples == 0) {
    return PlaybackBeginResult::Invalid;
  }
  if (capture_active()) {
    return PlaybackBeginResult::Busy;
  }
  if (playback_phase_ != PlaybackPhase::Idle) {
    if (playback_identity_ == begin.identity &&
        active_play_request_generation_ == begin.request_generation &&
        playback_total_frames_ == begin.total_frames &&
        playback_total_samples_ == begin.total_samples) {
      return PlaybackBeginResult::Duplicate;
    }
    return PlaybackBeginResult::Busy;
  }
  if (pending_play_slot_ != begin.identity.slot ||
      pending_play_request_generation_ != begin.request_generation ||
      pending_play_connection_generation_ !=
          begin.identity.connection_generation) {
    return PlaybackBeginResult::Invalid;
  }

  playback_identity_ = begin.identity;
  active_play_request_generation_ = begin.request_generation;
  pending_play_slot_ = 0;
  pending_play_request_generation_ = 0;
  pending_play_connection_generation_ = 0;
  playback_total_frames_ = begin.total_frames;
  playback_total_samples_ = begin.total_samples;
  playback_next_frame_ = 0;
  playback_phase_ = PlaybackPhase::Buffering;
  return PlaybackBeginResult::Accepted;
}

bool CodexSlotState::mark_playback_started(
    const PlaybackIdentity& identity) {
  if (!playback_matches(identity) ||
      playback_phase_ != PlaybackPhase::Buffering) {
    return false;
  }
  playback_phase_ = PlaybackPhase::Playing;
  return true;
}

PlaybackFrameResult CodexSlotState::accept_playback_frame(
    const PlaybackIdentity& identity,
    std::uint32_t frame_sequence) {
  if (!valid_playback_identity(identity)) {
    return PlaybackFrameResult::Invalid;
  }
  if (!playback_matches(identity) ||
      (playback_phase_ != PlaybackPhase::Buffering &&
       playback_phase_ != PlaybackPhase::Playing)) {
    return PlaybackFrameResult::Stale;
  }
  if (frame_sequence >= playback_total_frames_) {
    return PlaybackFrameResult::Invalid;
  }
  if (frame_sequence < playback_next_frame_) {
    return PlaybackFrameResult::Duplicate;
  }
  if (frame_sequence > playback_next_frame_) {
    return PlaybackFrameResult::OutOfOrder;
  }
  ++playback_next_frame_;
  return PlaybackFrameResult::Accepted;
}

bool CodexSlotState::mark_playback_transfer_complete(
    const PlaybackIdentity& identity,
    std::uint32_t last_frame_sequence) {
  if (!playback_matches(identity) ||
      (playback_phase_ != PlaybackPhase::Buffering &&
       playback_phase_ != PlaybackPhase::Playing) ||
      playback_next_frame_ != playback_total_frames_ ||
      last_frame_sequence != playback_total_frames_ - 1U) {
    return false;
  }
  playback_phase_ = PlaybackPhase::TransferComplete;
  return true;
}

bool CodexSlotState::mark_playback_drained(
    const PlaybackIdentity& identity,
    std::uint64_t played_samples) {
  if (!playback_matches(identity) ||
      playback_phase_ != PlaybackPhase::TransferComplete ||
      played_samples != playback_total_samples_) {
    return false;
  }
  playback_phase_ = PlaybackPhase::FinishedPendingAck;
  return true;
}

bool CodexSlotState::pending_finished(DeviceAction* action) const {
  if (action == nullptr ||
      playback_phase_ != PlaybackPhase::FinishedPendingAck) {
    return false;
  }
  *action = {DeviceActionKind::PlaybackFinished,
             playback_identity_.slot};
  action->playback = playback_identity_;
  return true;
}

bool CodexSlotState::acknowledge_finished(
    const PlaybackIdentity& identity) {
  if (!playback_matches(identity) ||
      playback_phase_ != PlaybackPhase::FinishedPendingAck) {
    return false;
  }
  clear_playback();
  return true;
}

bool CodexSlotState::abort_playback(
    const PlaybackIdentity& identity) {
  if (!playback_matches(identity)) {
    return false;
  }
  clear_playback();
  return true;
}

DeviceTransition CodexSlotState::disconnect(
    std::uint32_t connection_generation) {
  DeviceTransition transition;
  if (capture_active() &&
      (connection_generation == 0 ||
       capture_connection_generation_ == connection_generation)) {
    DeviceAction action{DeviceActionKind::PttEnded,
                        capture_slot_,
                        capture_generation_};
    action.connection_generation = capture_connection_generation_;
    transition.push(action);
    capture_slot_ = 0;
    capture_generation_ = 0;
    capture_connection_generation_ = 0;
  }
  if (connection_generation == 0 ||
      pending_play_connection_generation_ == connection_generation) {
    pending_play_slot_ = 0;
    pending_play_request_generation_ = 0;
    pending_play_connection_generation_ = 0;
  }
  if (playback_phase_ != PlaybackPhase::Idle &&
      (connection_generation == 0 ||
       playback_identity_.connection_generation == connection_generation)) {
    transition.push(preempt_playback());
  }
  return transition;
}

bool CodexSlotState::capture_active() const {
  return capture_generation_ != 0;
}

std::uint8_t CodexSlotState::capture_slot() const {
  return capture_slot_;
}

std::uint32_t CodexSlotState::capture_generation() const {
  return capture_generation_;
}

std::uint32_t CodexSlotState::capture_connection_generation() const {
  return capture_connection_generation_;
}

PlaybackPhase CodexSlotState::playback_phase() const {
  return playback_phase_;
}

PlaybackIdentity CodexSlotState::playback_identity() const {
  return playback_identity_;
}

std::uint32_t CodexSlotState::next_playback_frame() const {
  return playback_next_frame_;
}

std::uint32_t CodexSlotState::expected_playback_frames() const {
  return playback_total_frames_;
}

std::uint64_t CodexSlotState::expected_playback_samples() const {
  return playback_total_samples_;
}

std::uint8_t CodexSlotState::pending_play_slot() const {
  return pending_play_slot_;
}

std::uint32_t CodexSlotState::pending_play_request_generation() const {
  return pending_play_request_generation_;
}

bool CodexSlotState::ptt_slot_for_input(ai_keyboard::InputId input,
                                        std::uint8_t* slot) {
  const auto index = static_cast<std::size_t>(input);
  const auto first = static_cast<std::size_t>(ai_keyboard::InputId::Key1);
  const auto last = static_cast<std::size_t>(ai_keyboard::InputId::Key4);
  if (slot == nullptr || index < first || index > last) {
    return false;
  }
  *slot = static_cast<std::uint8_t>(index - first + 1U);
  return true;
}

bool CodexSlotState::play_slot_for_input(ai_keyboard::InputId input,
                                         std::uint8_t* slot) {
  const auto index = static_cast<std::size_t>(input);
  const auto first = static_cast<std::size_t>(ai_keyboard::InputId::Key5);
  const auto last = static_cast<std::size_t>(ai_keyboard::InputId::Key8);
  if (slot == nullptr || index < first || index > last) {
    return false;
  }
  *slot = static_cast<std::uint8_t>(index - first + 1U);
  return true;
}

std::uint32_t CodexSlotState::next_nonzero(std::uint32_t* counter) {
  if (counter == nullptr) {
    return 0;
  }
  if (*counter == std::numeric_limits<std::uint32_t>::max()) {
    *counter = 1;
  } else {
    ++(*counter);
  }
  return *counter;
}

bool CodexSlotState::playback_matches(
    const PlaybackIdentity& identity) const {
  return playback_phase_ != PlaybackPhase::Idle &&
         valid_playback_identity(identity) &&
         playback_identity_ == identity;
}

DeviceAction CodexSlotState::preempt_playback() {
  DeviceAction action;
  action.kind = DeviceActionKind::PlaybackPreempted;
  action.slot = playback_identity_.slot;
  action.playback = playback_identity_;
  clear_playback();
  return action;
}

void CodexSlotState::clear_playback() {
  playback_phase_ = PlaybackPhase::Idle;
  playback_identity_ = {};
  playback_total_frames_ = 0;
  playback_next_frame_ = 0;
  playback_total_samples_ = 0;
  active_play_request_generation_ = 0;
}

}  // namespace easy_codex
