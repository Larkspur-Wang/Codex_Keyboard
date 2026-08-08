#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>

#include "keyboard/speaker_probe_status.h"

namespace {

using ai_keyboard::SpeakerProbeError;
using ai_keyboard::SpeakerProbeResult;
using ai_keyboard::SpeakerProbeSnapshot;
using ai_keyboard::SpeakerProbeStage;

void wire_enums_are_frozen() {
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::None) == 0);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::BootPending) == 1);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Begin) == 2);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::I2sNew) == 3);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::I2sConfig) == 4);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::OpusInit) == 5);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::TaskAlloc) == 6);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Ready) == 7);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Request) == 8);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::RequestReject) == 9);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Dma) == 10);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::DecodeReset) == 11);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::DecodeFirst) == 12);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Clock) == 13);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::PowerWait) == 14);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::PowerReady) == 15);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::FirstSubmit) == 16);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::DecodeStream) == 17);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Write) == 18);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Cleanup) == 19);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Cancel) == 20);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeStage::Done) == 21);

  static_assert(static_cast<std::uint8_t>(SpeakerProbeResult::None) == 0);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeResult::Pending) == 1);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeResult::Running) == 2);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeResult::Ok) == 3);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeResult::Rejected) == 4);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeResult::Failed) == 5);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeResult::Cancelled) == 6);

  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::None) == 0);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::NotReady) == 1);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::InvalidArgument) == 2);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::Unsupported) == 3);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::I2sNew) == 4);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::I2sConfig) == 5);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::OpusInit) == 6);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::TaskAlloc) == 7);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::PlaybackBusy) == 8);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::MicrophoneBusy) == 9);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::Dma) == 10);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::DecodeReset) == 11);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::DecodeFirst) == 12);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::DecodeStream) == 13);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::Clock) == 14);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::PowerTimeout) == 15);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::Write) == 16);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::Cleanup) == 17);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::Cancelled) == 18);
  static_assert(static_cast<std::uint8_t>(SpeakerProbeError::Unknown) == 19);
}

void snapshot_is_allocation_free_pod() {
  static_assert(std::is_standard_layout_v<SpeakerProbeSnapshot>);
  static_assert(std::is_trivially_copyable_v<SpeakerProbeSnapshot>);
  SpeakerProbeSnapshot snapshot;
  assert(!snapshot.present);
  assert(snapshot.version == ai_keyboard::kSpeakerProbeStatusVersion);
}

void every_wire_value_has_a_stable_diagnostic_name() {
  for (std::uint8_t value = 0; value <= 21; ++value) {
    assert(std::string(ai_keyboard::speaker_probe_stage_name(
               static_cast<SpeakerProbeStage>(value))) != "unknown");
  }
  for (std::uint8_t value = 0; value <= 6; ++value) {
    assert(std::string(ai_keyboard::speaker_probe_result_name(
               static_cast<SpeakerProbeResult>(value))) != "unknown");
  }
  for (std::uint8_t value = 0; value <= 19; ++value) {
    assert(std::string(ai_keyboard::speaker_probe_error_name(
               static_cast<SpeakerProbeError>(value))) != "unknown" ||
           value == static_cast<std::uint8_t>(SpeakerProbeError::Unknown));
  }
}

SpeakerProbeSnapshot running_snapshot(std::uint32_t generation) {
  SpeakerProbeSnapshot snapshot;
  snapshot.present = true;
  snapshot.generation = generation;
  snapshot.stage = SpeakerProbeStage::DecodeFirst;
  snapshot.result = SpeakerProbeResult::Running;
  return snapshot;
}

void first_real_failure_survives_concurrent_cancel_and_cleanup() {
  auto snapshot = running_snapshot(7);
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      7,
      SpeakerProbeStage::DecodeFirst,
      SpeakerProbeResult::Running,
      SpeakerProbeError::DecodeFirst,
      -23));
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      7,
      SpeakerProbeStage::Cancel,
      SpeakerProbeResult::Running,
      SpeakerProbeError::Cancelled,
      0));
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      7,
      SpeakerProbeStage::Cleanup,
      SpeakerProbeResult::Running,
      SpeakerProbeError::Cleanup,
      -99));
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      7,
      SpeakerProbeStage::Cancel,
      SpeakerProbeResult::Cancelled,
      SpeakerProbeError::Cancelled,
      0));

  assert(snapshot.stage == SpeakerProbeStage::DecodeFirst);
  assert(snapshot.result == SpeakerProbeResult::Failed);
  assert(snapshot.error == SpeakerProbeError::DecodeFirst);
  assert(snapshot.raw_error == -23);
}

void cancellation_can_be_upgraded_to_a_real_failure() {
  auto snapshot = running_snapshot(9);
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      9,
      SpeakerProbeStage::Cancel,
      SpeakerProbeResult::Running,
      SpeakerProbeError::Cancelled,
      0));
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      9,
      SpeakerProbeStage::DecodeStream,
      SpeakerProbeResult::Running,
      SpeakerProbeError::DecodeStream,
      -41));
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      9,
      SpeakerProbeStage::DecodeStream,
      SpeakerProbeResult::Failed,
      SpeakerProbeError::DecodeStream,
      -41));

  assert(snapshot.stage == SpeakerProbeStage::DecodeStream);
  assert(snapshot.result == SpeakerProbeResult::Failed);
  assert(snapshot.error == SpeakerProbeError::DecodeStream);
  assert(snapshot.raw_error == -41);
}

void terminal_cancellation_can_be_upgraded_to_cleanup_failure() {
  auto snapshot = running_snapshot(10);
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      10,
      SpeakerProbeStage::Cancel,
      SpeakerProbeResult::Cancelled,
      SpeakerProbeError::Cancelled,
      0));
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      10,
      SpeakerProbeStage::Cleanup,
      SpeakerProbeResult::Running,
      SpeakerProbeError::Cleanup,
      -55));

  assert(snapshot.stage == SpeakerProbeStage::Cleanup);
  assert(snapshot.result == SpeakerProbeResult::Failed);
  assert(snapshot.error == SpeakerProbeError::Cleanup);
  assert(snapshot.raw_error == -55);
}

void successful_terminal_state_rejects_late_failure() {
  auto snapshot = running_snapshot(11);
  assert(ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      11,
      SpeakerProbeStage::Done,
      SpeakerProbeResult::Ok,
      SpeakerProbeError::None,
      0));
  assert(!ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      11,
      SpeakerProbeStage::Cleanup,
      SpeakerProbeResult::Running,
      SpeakerProbeError::Cleanup,
      -77));

  assert(snapshot.stage == SpeakerProbeStage::Done);
  assert(snapshot.result == SpeakerProbeResult::Ok);
  assert(snapshot.error == SpeakerProbeError::None);
  assert(snapshot.raw_error == 0);
}

void failed_terminal_state_never_carries_none_or_cancelled_error() {
  auto no_error = running_snapshot(14);
  assert(ai_keyboard::apply_speaker_probe_state(
      &no_error,
      14,
      SpeakerProbeStage::Cleanup,
      SpeakerProbeResult::Failed,
      SpeakerProbeError::None,
      0));
  assert(no_error.result == SpeakerProbeResult::Failed);
  assert(no_error.error == SpeakerProbeError::Unknown);
  assert(no_error.raw_error == -1);

  auto cancelled = running_snapshot(15);
  cancelled.stage = SpeakerProbeStage::Cancel;
  cancelled.error = SpeakerProbeError::Cancelled;
  assert(ai_keyboard::apply_speaker_probe_state(
      &cancelled,
      15,
      SpeakerProbeStage::Cancel,
      SpeakerProbeResult::Failed,
      SpeakerProbeError::Cancelled,
      0));
  assert(cancelled.result == SpeakerProbeResult::Failed);
  assert(cancelled.error == SpeakerProbeError::Unknown);
  assert(cancelled.raw_error == -1);
}

void cleanup_outcome_keeps_the_first_real_cleanup_error() {
  ai_keyboard::SpeakerProbeCleanupOutcome cleanup;
  cleanup = ai_keyboard::observe_speaker_probe_cleanup(cleanup, 0);
  assert(!cleanup.failed);
  cleanup = ai_keyboard::observe_speaker_probe_cleanup(cleanup, -31);
  cleanup = ai_keyboard::observe_speaker_probe_cleanup(cleanup, -63);
  assert(cleanup.failed);
  assert(cleanup.first_raw_error == -31);
}

void terminal_resolution_enforces_failure_cancel_success_priority() {
  const auto success = ai_keyboard::resolve_speaker_probe_terminal(
      running_snapshot(13), SpeakerProbeResult::Ok, -1);
  assert(success.stage == SpeakerProbeStage::Done);
  assert(success.result == SpeakerProbeResult::Ok);
  assert(success.error == SpeakerProbeError::None);
  assert(success.raw_error == 0);

  auto cancelled = running_snapshot(13);
  cancelled.stage = SpeakerProbeStage::Cancel;
  cancelled.error = SpeakerProbeError::Cancelled;
  const auto cancelled_success = ai_keyboard::resolve_speaker_probe_terminal(
      cancelled, SpeakerProbeResult::Ok, -1);
  assert(cancelled_success.stage == SpeakerProbeStage::Cancel);
  assert(cancelled_success.result == SpeakerProbeResult::Cancelled);
  assert(cancelled_success.error == SpeakerProbeError::Cancelled);

  const auto failed_after_cancel =
      ai_keyboard::resolve_speaker_probe_terminal(
          cancelled, SpeakerProbeResult::Failed, -1);
  assert(failed_after_cancel.result == SpeakerProbeResult::Failed);
  assert(failed_after_cancel.error == SpeakerProbeError::Unknown);
  assert(failed_after_cancel.raw_error == -1);

  auto decode_failure = running_snapshot(13);
  decode_failure.stage = SpeakerProbeStage::DecodeFirst;
  decode_failure.error = SpeakerProbeError::DecodeFirst;
  decode_failure.raw_error = -44;
  const auto failure_beats_cancel =
      ai_keyboard::resolve_speaker_probe_terminal(
          decode_failure, SpeakerProbeResult::Cancelled, -1);
  assert(failure_beats_cancel.stage == SpeakerProbeStage::DecodeFirst);
  assert(failure_beats_cancel.result == SpeakerProbeResult::Failed);
  assert(failure_beats_cancel.error == SpeakerProbeError::DecodeFirst);
  assert(failure_beats_cancel.raw_error == -44);

  const auto unclassified_failure =
      ai_keyboard::resolve_speaker_probe_terminal(
          running_snapshot(13), SpeakerProbeResult::Failed, -9);
  assert(unclassified_failure.result == SpeakerProbeResult::Failed);
  assert(unclassified_failure.error == SpeakerProbeError::Unknown);
  assert(unclassified_failure.raw_error == -9);

  auto malformed_failed = running_snapshot(13);
  malformed_failed.result = SpeakerProbeResult::Failed;
  const auto malformed_cannot_become_success =
      ai_keyboard::resolve_speaker_probe_terminal(
          malformed_failed, SpeakerProbeResult::Ok, -11);
  assert(malformed_cannot_become_success.result ==
         SpeakerProbeResult::Failed);
  assert(malformed_cannot_become_success.error ==
         SpeakerProbeError::Unknown);
  assert(malformed_cannot_become_success.raw_error == -11);
}

void stale_generation_cannot_corrupt_the_current_probe() {
  auto snapshot = running_snapshot(12);
  assert(!ai_keyboard::apply_speaker_probe_state(
      &snapshot,
      11,
      SpeakerProbeStage::Cancel,
      SpeakerProbeResult::Cancelled,
      SpeakerProbeError::Cancelled,
      0));
  assert(snapshot.generation == 12);
  assert(snapshot.stage == SpeakerProbeStage::DecodeFirst);
  assert(snapshot.result == SpeakerProbeResult::Running);
  assert(snapshot.error == SpeakerProbeError::None);
}

}  // namespace

int main() {
  wire_enums_are_frozen();
  snapshot_is_allocation_free_pod();
  every_wire_value_has_a_stable_diagnostic_name();
  first_real_failure_survives_concurrent_cancel_and_cleanup();
  cancellation_can_be_upgraded_to_a_real_failure();
  terminal_cancellation_can_be_upgraded_to_cleanup_failure();
  successful_terminal_state_rejects_late_failure();
  failed_terminal_state_never_carries_none_or_cancelled_error();
  cleanup_outcome_keeps_the_first_real_cleanup_error();
  terminal_resolution_enforces_failure_cancel_success_priority();
  stale_generation_cannot_corrupt_the_current_probe();
  return 0;
}
