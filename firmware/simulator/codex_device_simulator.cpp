#include "codex_device_simulator.h"

namespace easy_codex {

void CodexDeviceSimulator::connect(std::uint32_t generation) {
  if (connection_generation_ != 0 &&
      connection_generation_ != generation) {
    append(state_.disconnect(connection_generation_));
    active_identity_ = {};
  }
  connection_generation_ = generation;
}

void CodexDeviceSimulator::disconnect() {
  append(state_.disconnect(connection_generation_));
  connection_generation_ = 0;
  active_identity_ = {};
}

void CodexDeviceSimulator::input(ai_keyboard::InputId input,
                                 ai_keyboard::InputPhase phase) {
  append(state_.handle_input(input, phase, connection_generation_));
}

PlaybackBeginResult CodexDeviceSimulator::begin_playback(
    std::uint8_t slot,
    std::uint64_t summary_generation,
    std::uint64_t lease,
    std::uint32_t request_generation,
    std::uint32_t frames,
    std::uint64_t samples) {
  PlaybackBegin begin;
  begin.identity = {slot,
                    summary_generation,
                    lease,
                    connection_generation_};
  begin.request_generation = request_generation;
  begin.total_frames = frames;
  begin.total_samples = samples;
  const auto result = state_.begin_playback(begin);
  if (result == PlaybackBeginResult::Accepted ||
      result == PlaybackBeginResult::Duplicate) {
    active_identity_ = begin.identity;
  }
  return result;
}

bool CodexDeviceSimulator::start_playback() {
  return state_.mark_playback_started(active_identity_);
}

PlaybackFrameResult CodexDeviceSimulator::deliver_frame(
    std::uint32_t sequence,
    bool drop) {
  if (drop) {
    return PlaybackFrameResult::OutOfOrder;
  }
  return state_.accept_playback_frame(active_identity_, sequence);
}

bool CodexDeviceSimulator::end_transfer(std::uint32_t last_sequence) {
  return state_.mark_playback_transfer_complete(active_identity_, last_sequence);
}

bool CodexDeviceSimulator::drain(std::uint64_t played_samples) {
  if (!state_.mark_playback_drained(active_identity_, played_samples)) {
    return false;
  }
  DeviceAction action;
  if (state_.pending_finished(&action)) {
    emitted_.push_back(action);
  }
  return true;
}

bool CodexDeviceSimulator::acknowledge_finished() {
  return acknowledge_finished(active_identity_);
}

bool CodexDeviceSimulator::acknowledge_finished(
    const PlaybackIdentity& identity) {
  const bool acknowledged = state_.acknowledge_finished(identity);
  if (acknowledged) {
    active_identity_ = {};
  }
  return acknowledged;
}

const CodexSlotState& CodexDeviceSimulator::state() const {
  return state_;
}

const std::vector<DeviceAction>& CodexDeviceSimulator::emitted() const {
  return emitted_;
}

void CodexDeviceSimulator::clear_emitted() {
  emitted_.clear();
}

std::uint32_t CodexDeviceSimulator::connection_generation() const {
  return connection_generation_;
}

void CodexDeviceSimulator::append(const DeviceTransition& transition) {
  for (std::size_t index = 0; index < transition.count; ++index) {
    emitted_.push_back(transition.actions[index]);
  }
}

}  // namespace easy_codex
