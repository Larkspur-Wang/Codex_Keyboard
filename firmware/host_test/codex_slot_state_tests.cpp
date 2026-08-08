#include "keyboard/codex_slot_state.h"

#include <array>
#include <cassert>
#include <cstdint>

namespace {

using ai_keyboard::InputId;
using ai_keyboard::InputPhase;
using easy_codex::CodexSlotState;
using easy_codex::CaptureSessionIdentity;
using easy_codex::DeviceActionKind;
using easy_codex::PlaybackBegin;
using easy_codex::PlaybackBeginResult;
using easy_codex::PlaybackFrameResult;
using easy_codex::PlaybackIdentity;
using easy_codex::PlaybackPhase;

PlaybackIdentity identity(std::uint8_t slot,
                          std::uint64_t generation,
                          std::uint64_t lease,
                          std::uint32_t connection) {
  return {slot, generation, lease, connection};
}

PlaybackBegin request_playback(CodexSlotState* state,
                               InputId key,
                               const PlaybackIdentity& playback,
                               std::uint32_t frames,
                               std::uint64_t samples) {
  const auto request = state->handle_input(
      key, InputPhase::Pressed, playback.connection_generation);
  assert(request.count == 1);
  assert(request.actions[0].kind == DeviceActionKind::PlayRequested);
  return {playback,
          request.actions[0].request_generation,
          frames,
          samples};
}

void capture_session_identity_round_trips_and_rejects_aliases() {
  for (std::uint8_t slot = 1; slot <= 4; ++slot) {
    const CaptureSessionIdentity expected{
        slot, 0xA1B2C3D4U, 0x000ABCDEU};
    const auto encoded =
        easy_codex::encode_capture_session_identity(expected);
    assert(encoded != 0);
    CaptureSessionIdentity decoded;
    assert(easy_codex::decode_capture_session_identity(
        encoded, &decoded));
    assert(decoded.slot == expected.slot);
    assert(decoded.capture_generation ==
           expected.capture_generation);
    assert(decoded.connection_generation ==
           expected.connection_generation);
  }

  assert(easy_codex::encode_capture_session_identity(
             {0, 1, 1}) == 0);
  assert(easy_codex::encode_capture_session_identity(
             {5, 1, 1}) == 0);
  assert(easy_codex::encode_capture_session_identity(
             {1, 0, 1}) == 0);
  assert(easy_codex::encode_capture_session_identity(
             {1, 1, 0}) == 0);
  assert(easy_codex::encode_capture_session_identity(
             {1, 1,
              easy_codex::kCaptureSessionMaxConnectionGeneration + 1U}) == 0);
  CaptureSessionIdentity ignored;
  assert(!easy_codex::decode_capture_session_identity(0, &ignored));
  assert(!easy_codex::decode_capture_session_identity(
      0xEC50000100000001ULL, &ignored));
  assert(!easy_codex::decode_capture_session_identity(
      0xEC10000100000000ULL, &ignored));
  assert(!easy_codex::decode_capture_session_identity(
      0xEC10000000000001ULL, &ignored));
}

void all_eight_keys_map_to_four_slots() {
  constexpr std::array<InputId, 4> ptt{
      InputId::Key1, InputId::Key2, InputId::Key3, InputId::Key4};
  constexpr std::array<InputId, 4> play{
      InputId::Key5, InputId::Key6, InputId::Key7, InputId::Key8};

  for (std::size_t index = 0; index < ptt.size(); ++index) {
    CodexSlotState state;
    const auto down = state.handle_input(
        ptt[index], InputPhase::Pressed, 1);
    assert(down.count == 1);
    assert(down.actions[0].kind == DeviceActionKind::PttStarted);
    assert(down.actions[0].slot == index + 1U);
    assert(down.actions[0].capture_generation != 0);
    assert(down.actions[0].connection_generation == 1);
    const auto up = state.handle_input(
        ptt[index], InputPhase::Released, 1);
    assert(up.count == 1);
    assert(up.actions[0].kind == DeviceActionKind::PttEnded);
    assert(up.actions[0].slot == index + 1U);
    assert(up.actions[0].capture_generation ==
           down.actions[0].capture_generation);
    assert(up.actions[0].connection_generation == 1);

    const auto request = state.handle_input(
        play[index], InputPhase::Pressed, 1);
    assert(request.count == 1);
    assert(request.actions[0].kind == DeviceActionKind::PlayRequested);
    assert(request.actions[0].slot == index + 1U);
    assert(request.actions[0].request_generation != 0);
    assert(request.actions[0].connection_generation == 1);
    assert(state.handle_input(
               play[index], InputPhase::Released, 1).count == 0);
  }
}

void boot_seed_changes_the_first_capture_identity_once() {
  CodexSlotState state;
  state.seed_capture_counter(0x12345678U);
  state.seed_capture_counter(0x87654321U);
  const auto started = state.handle_input(
      InputId::Key1, InputPhase::Pressed, 0xABCDEU);
  assert(started.count == 1);
  assert(started.actions[0].capture_generation == 0x12345679U);
  const auto session = easy_codex::encode_capture_session_identity({
      started.actions[0].slot,
      started.actions[0].capture_generation,
      started.actions[0].connection_generation,
  });
  assert(session == 0xEC1ABCDE12345679ULL);
}

void ptt_has_absolute_priority_without_cross_slot_release() {
  CodexSlotState state;
  const auto begin = request_playback(
      &state, InputId::Key7, identity(3, 44, 91, 7), 2, 640);
  assert(state.begin_playback(begin) == PlaybackBeginResult::Accepted);

  const auto ptt = state.handle_input(
      InputId::Key2, InputPhase::Pressed, 7);
  assert(ptt.count == 2);
  assert(ptt.actions[0].kind == DeviceActionKind::PlaybackPreempted);
  assert(ptt.actions[0].playback == begin.identity);
  assert(ptt.actions[1].kind == DeviceActionKind::PttStarted);
  assert(ptt.actions[1].slot == 2);
  assert(state.playback_phase() == PlaybackPhase::Idle);

  const auto other_down =
      state.handle_input(InputId::Key1, InputPhase::Pressed, 7);
  assert(other_down.count == 1);
  assert(other_down.actions[0].kind == DeviceActionKind::RejectedBusy);
  assert(state.capture_slot() == 2);
  assert(state.handle_input(
             InputId::Key1, InputPhase::Released, 7).count == 0);
  assert(state.capture_active());

  const auto play_while_talking =
      state.handle_input(InputId::Key6, InputPhase::Pressed, 7);
  assert(play_while_talking.count == 1);
  assert(play_while_talking.actions[0].kind ==
         DeviceActionKind::RejectedBusy);
  assert(state.capture_active());

  const auto ptt_up = state.handle_input(
      InputId::Key2, InputPhase::Released, 7);
  assert(ptt_up.count == 1);
  assert(ptt_up.actions[0].capture_generation ==
         ptt.actions[1].capture_generation);
  assert(ptt_up.actions[0].connection_generation == 7);
  assert(!state.capture_active());
}

void playback_requires_exact_generation_frames_samples_and_ack() {
  CodexSlotState state;
  const auto begin = request_playback(
      &state, InputId::Key5, identity(1, 12, 23, 34), 3, 960);
  auto unsolicited = begin;
  ++unsolicited.request_generation;
  assert(state.begin_playback(unsolicited) == PlaybackBeginResult::Invalid);
  assert(state.begin_playback(begin) == PlaybackBeginResult::Accepted);
  assert(state.begin_playback(begin) == PlaybackBeginResult::Duplicate);

  auto forged = begin.identity;
  forged.summary_generation = 13;
  assert(!state.mark_playback_started(forged));
  assert(state.mark_playback_started(begin.identity));
  assert(state.accept_playback_frame(begin.identity, 1) ==
         PlaybackFrameResult::OutOfOrder);
  assert(state.accept_playback_frame(begin.identity, 0) ==
         PlaybackFrameResult::Accepted);
  assert(state.accept_playback_frame(begin.identity, 0) ==
         PlaybackFrameResult::Duplicate);
  assert(!state.mark_playback_transfer_complete(begin.identity, 2));
  assert(state.accept_playback_frame(begin.identity, 1) ==
         PlaybackFrameResult::Accepted);
  assert(state.accept_playback_frame(begin.identity, 2) ==
         PlaybackFrameResult::Accepted);
  assert(state.mark_playback_transfer_complete(begin.identity, 2));
  assert(!state.mark_playback_drained(begin.identity, 959));
  assert(state.mark_playback_drained(begin.identity, 960));

  easy_codex::DeviceAction finished;
  assert(state.pending_finished(&finished));
  assert(finished.kind == DeviceActionKind::PlaybackFinished);
  assert(finished.playback == begin.identity);
  easy_codex::DeviceAction retry;
  assert(state.pending_finished(&retry));
  assert(retry.playback == finished.playback);
  assert(!state.acknowledge_finished(forged));
  assert(state.playback_phase() == PlaybackPhase::FinishedPendingAck);
  assert(state.acknowledge_finished(begin.identity));
  assert(state.playback_phase() == PlaybackPhase::Idle);
  assert(state.begin_playback(begin) == PlaybackBeginResult::Invalid);
}

void playback_abort_requires_the_exact_active_identity() {
  CodexSlotState state;
  const auto begin = request_playback(
      &state, InputId::Key6, identity(2, 21, 22, 23), 1, 320);
  assert(state.begin_playback(begin) == PlaybackBeginResult::Accepted);
  auto wrong = begin.identity;
  ++wrong.lease;
  assert(!state.abort_playback(wrong));
  assert(state.playback_phase() == PlaybackPhase::Buffering);
  assert(state.abort_playback(begin.identity));
  assert(state.playback_phase() == PlaybackPhase::Idle);
}

void disconnect_invalidates_only_the_matching_transport_generation() {
  CodexSlotState state;
  const auto begin = request_playback(
      &state, InputId::Key8, identity(4, 2, 9, 77), 1, 320);
  assert(state.begin_playback(begin) == PlaybackBeginResult::Accepted);
  assert(state.disconnect(76).count == 0);
  assert(state.playback_phase() == PlaybackPhase::Buffering);
  const auto disconnected = state.disconnect(77);
  assert(disconnected.count == 1);
  assert(disconnected.actions[0].kind == DeviceActionKind::PlaybackPreempted);
  assert(disconnected.actions[0].playback == begin.identity);
  assert(state.playback_phase() == PlaybackPhase::Idle);

  const auto down = state.handle_input(
      InputId::Key4, InputPhase::Pressed, 88);
  assert(down.count == 1);
  assert(state.disconnect(87).count == 0);
  assert(state.capture_active());
  const auto capture_disconnect = state.disconnect(88);
  assert(capture_disconnect.count == 1);
  assert(capture_disconnect.actions[0].kind == DeviceActionKind::PttEnded);
  assert(!state.capture_active());

  const auto request = state.handle_input(
      InputId::Key5, InputPhase::Pressed, 90);
  assert(request.count == 1);
  const auto request_generation = request.actions[0].request_generation;
  assert(state.disconnect(89).count == 0);
  assert(state.pending_play_request_generation() == request_generation);
  assert(state.disconnect(90).count == 0);
  assert(state.pending_play_request_generation() == 0);
}

void offline_ptt_still_preempts_playback_without_starting_capture() {
  CodexSlotState state;
  const auto begin = request_playback(
      &state, InputId::Key6, identity(2, 81, 82, 83), 1, 320);
  assert(state.begin_playback(begin) == PlaybackBeginResult::Accepted);

  const auto offline_ptt = state.handle_input(
      InputId::Key1, InputPhase::Pressed, 0);
  assert(offline_ptt.count == 2);
  assert(offline_ptt.actions[0].kind ==
         DeviceActionKind::PlaybackPreempted);
  assert(offline_ptt.actions[0].playback == begin.identity);
  assert(offline_ptt.actions[1].kind == DeviceActionKind::RejectedBusy);
  assert(state.playback_phase() == PlaybackPhase::Idle);
  assert(!state.capture_active());
}

std::uint32_t xorshift32(std::uint32_t* value) {
  *value ^= *value << 13U;
  *value ^= *value >> 17U;
  *value ^= *value << 5U;
  return *value;
}

void one_thousand_seeded_traces_preserve_slot_invariants() {
  for (std::uint32_t trace = 0; trace < 1000; ++trace) {
    CodexSlotState state;
    std::uint32_t random =
        0xEC1A110U + trace * 0x9E3779B9U;
    std::uint64_t summary_generation =
        static_cast<std::uint64_t>(trace) * 1000U + 1U;
    std::uint64_t lease = summary_generation + 1000000U;
    std::uint32_t connection = trace + 1U;

    for (std::size_t operation = 0; operation < 100; ++operation) {
      const auto value = xorshift32(&random);
      const auto key = static_cast<InputId>(
          static_cast<std::size_t>(InputId::Key1) + (value % 8U));
      const auto phase = (value & 0x100U) == 0
                             ? InputPhase::Pressed
                             : InputPhase::Released;
      const auto transition = state.handle_input(key, phase, connection);
      for (std::size_t index = 0; index < transition.count; ++index) {
        const auto& action = transition.actions[index];
        assert(action.slot >= 1 && action.slot <= 4);
        if (action.kind == DeviceActionKind::PttStarted ||
            action.kind == DeviceActionKind::PttEnded) {
          assert(action.capture_generation != 0);
        }
        if (action.kind == DeviceActionKind::PlayRequested) {
          assert(action.request_generation != 0);
        }
      }

      if ((value % 17U) == 0 && !state.capture_active() &&
          state.playback_phase() == PlaybackPhase::Idle) {
        const auto slot = static_cast<std::uint8_t>((value % 4U) + 1U);
        const auto play_key = static_cast<InputId>(
            static_cast<std::size_t>(InputId::Key5) + slot - 1U);
        auto begin = request_playback(&state,
                                      play_key,
                                      identity(slot,
                                               summary_generation++,
                                               lease++,
                                               connection),
                                      1,
                                      320);
        assert(state.begin_playback(begin) == PlaybackBeginResult::Accepted);
        assert(state.accept_playback_frame(begin.identity, 0) ==
               PlaybackFrameResult::Accepted);
        assert(state.mark_playback_transfer_complete(begin.identity, 0));
        if ((value & 1U) == 0) {
          assert(state.mark_playback_drained(begin.identity, 320));
          assert(!state.acknowledge_finished(
              identity(slot, begin.identity.summary_generation + 1U,
                       begin.identity.lease, connection)));
          assert(state.acknowledge_finished(begin.identity));
        } else {
          const auto stopped = state.disconnect(connection);
          assert(stopped.count == 1);
          ++connection;
        }
      }

      assert(!state.capture_active() ||
             (state.capture_slot() >= 1 && state.capture_slot() <= 4));
      assert(state.playback_phase() == PlaybackPhase::Idle ||
             easy_codex::valid_playback_identity(state.playback_identity()));
      assert(!(state.capture_active() &&
               state.playback_phase() != PlaybackPhase::Idle));
    }
  }
}

}  // namespace

int main() {
  capture_session_identity_round_trips_and_rejects_aliases();
  all_eight_keys_map_to_four_slots();
  boot_seed_changes_the_first_capture_identity_once();
  ptt_has_absolute_priority_without_cross_slot_release();
  playback_requires_exact_generation_frames_samples_and_ack();
  playback_abort_requires_the_exact_active_identity();
  disconnect_invalidates_only_the_matching_transport_generation();
  offline_ptt_still_preempts_playback_without_starting_capture();
  one_thousand_seeded_traces_preserve_slot_invariants();
  return 0;
}
