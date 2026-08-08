#include "codex_device_simulator.h"

#include <cassert>

namespace {

using ai_keyboard::InputId;
using ai_keyboard::InputPhase;
using easy_codex::CodexDeviceSimulator;
using easy_codex::DeviceActionKind;
using easy_codex::PlaybackBeginResult;
using easy_codex::PlaybackFrameResult;
using easy_codex::PlaybackPhase;

void ack_loss_is_idempotent_and_never_consumes_a_new_generation() {
  CodexDeviceSimulator device;
  device.connect(10);
  device.input(InputId::Key5, InputPhase::Pressed);
  assert(device.emitted().size() == 1);
  assert(device.emitted().back().kind == DeviceActionKind::PlayRequested);
  const auto first_request = device.emitted().back().request_generation;
  assert(device.begin_playback(1, 41, 90, first_request, 2, 640) ==
         PlaybackBeginResult::Accepted);
  assert(device.start_playback());
  assert(device.deliver_frame(0) == PlaybackFrameResult::Accepted);
  assert(device.deliver_frame(1) == PlaybackFrameResult::Accepted);
  assert(device.end_transfer(1));
  assert(device.drain(640));
  assert(device.emitted().back().kind == DeviceActionKind::PlaybackFinished);
  const auto first_finished = device.emitted().back().playback;

  easy_codex::DeviceAction retry;
  assert(device.state().pending_finished(&retry));
  assert(retry.playback == first_finished);
  assert(device.begin_playback(1, 42, 91, first_request, 1, 320) ==
         PlaybackBeginResult::Busy);
  assert(device.acknowledge_finished());
  device.input(InputId::Key5, InputPhase::Pressed);
  const auto second_request = device.emitted().back().request_generation;
  assert(second_request != first_request);
  assert(device.begin_playback(1, 42, 91, second_request, 1, 320) ==
         PlaybackBeginResult::Accepted);
  assert(!device.acknowledge_finished(first_finished));
  assert(device.state().playback_phase() == PlaybackPhase::Buffering);
  assert(device.state().playback_identity().summary_generation == 42);
}

void dropped_and_out_of_order_frames_resume_from_the_exact_sequence() {
  CodexDeviceSimulator device;
  device.connect(21);
  device.input(InputId::Key7, InputPhase::Pressed);
  const auto request = device.emitted().back().request_generation;
  assert(device.begin_playback(3, 8, 55, request, 3, 960) ==
         PlaybackBeginResult::Accepted);
  assert(device.deliver_frame(0) == PlaybackFrameResult::Accepted);
  assert(device.deliver_frame(1, true) == PlaybackFrameResult::OutOfOrder);
  assert(device.deliver_frame(2) == PlaybackFrameResult::OutOfOrder);
  assert(device.state().next_playback_frame() == 1);
  assert(device.deliver_frame(1) == PlaybackFrameResult::Accepted);
  assert(device.deliver_frame(2) == PlaybackFrameResult::Accepted);
  assert(device.end_transfer(2));
  assert(device.drain(960));
  assert(device.acknowledge_finished());
}

void ptt_preempts_playback_and_disconnect_never_reports_finished() {
  CodexDeviceSimulator device;
  device.connect(31);
  device.input(InputId::Key6, InputPhase::Pressed);
  device.clear_emitted();
  const auto request = device.state().pending_play_request_generation();
  assert(device.begin_playback(2, 70, 71, request, 4, 1280) ==
         PlaybackBeginResult::Accepted);
  assert(device.deliver_frame(0) == PlaybackFrameResult::Accepted);
  device.input(InputId::Key4, InputPhase::Pressed);
  assert(device.emitted().size() == 2);
  assert(device.emitted()[0].kind == DeviceActionKind::PlaybackPreempted);
  assert(device.emitted()[0].playback.slot == 2);
  assert(device.emitted()[1].kind == DeviceActionKind::PttStarted);
  assert(device.emitted()[1].slot == 4);
  assert(device.state().playback_phase() == PlaybackPhase::Idle);

  device.disconnect();
  assert(device.emitted().back().kind == DeviceActionKind::PttEnded);
  for (const auto& action : device.emitted()) {
    assert(action.kind != DeviceActionKind::PlaybackFinished);
  }
}

void disconnected_input_fails_closed_without_creating_a_generation() {
  CodexDeviceSimulator device;
  device.input(InputId::Key1, InputPhase::Pressed);
  assert(device.emitted().size() == 1);
  assert(device.emitted().back().kind == DeviceActionKind::RejectedBusy);
  assert(!device.state().capture_active());
  assert(device.state().pending_play_request_generation() == 0);

  device.clear_emitted();
  device.input(InputId::Key5, InputPhase::Pressed);
  assert(device.emitted().size() == 1);
  assert(device.emitted().back().kind == DeviceActionKind::RejectedBusy);
  assert(device.state().pending_play_request_generation() == 0);
}

void reconnect_invalidates_the_old_connection_generation() {
  CodexDeviceSimulator device;
  device.connect(40);
  device.input(InputId::Key8, InputPhase::Pressed);
  const auto old_request = device.emitted().back().request_generation;
  device.connect(41);
  assert(device.state().pending_play_request_generation() == 0);
  assert(device.begin_playback(4, 1, 2, old_request, 1, 320) ==
         PlaybackBeginResult::Invalid);

  device.input(InputId::Key1, InputPhase::Pressed);
  assert(device.state().capture_active());
  assert(device.state().capture_connection_generation() == 41);
  device.connect(42);
  assert(!device.state().capture_active());
  assert(device.emitted().back().kind == DeviceActionKind::PttEnded);
  assert(device.emitted().back().connection_generation == 41);
}

}  // namespace

int main() {
  ack_loss_is_idempotent_and_never_consumes_a_new_generation();
  dropped_and_out_of_order_frames_resume_from_the_exact_sequence();
  ptt_preempts_playback_and_disconnect_never_reports_finished();
  disconnected_input_fails_closed_without_creating_a_generation();
  reconnect_invalidates_the_old_connection_generation();
  return 0;
}
