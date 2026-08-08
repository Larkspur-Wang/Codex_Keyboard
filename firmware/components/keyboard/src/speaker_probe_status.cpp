#include "keyboard/speaker_probe_status.h"

namespace ai_keyboard {
namespace {

bool is_terminal_result(SpeakerProbeResult result) {
  return result == SpeakerProbeResult::Ok ||
         result == SpeakerProbeResult::Rejected ||
         result == SpeakerProbeResult::Failed ||
         result == SpeakerProbeResult::Cancelled;
}

bool is_real_failure(SpeakerProbeError error) {
  return error != SpeakerProbeError::None &&
         error != SpeakerProbeError::Cancelled;
}

}  // namespace

const char* speaker_probe_stage_name(SpeakerProbeStage stage) {
  switch (stage) {
    case SpeakerProbeStage::None:
      return "none";
    case SpeakerProbeStage::BootPending:
      return "boot_pending";
    case SpeakerProbeStage::Begin:
      return "begin";
    case SpeakerProbeStage::I2sNew:
      return "i2s_new";
    case SpeakerProbeStage::I2sConfig:
      return "i2s_config";
    case SpeakerProbeStage::OpusInit:
      return "opus_init";
    case SpeakerProbeStage::TaskAlloc:
      return "task_alloc";
    case SpeakerProbeStage::Ready:
      return "ready";
    case SpeakerProbeStage::Request:
      return "request";
    case SpeakerProbeStage::RequestReject:
      return "request_reject";
    case SpeakerProbeStage::Dma:
      return "dma";
    case SpeakerProbeStage::DecodeReset:
      return "decode_reset";
    case SpeakerProbeStage::DecodeFirst:
      return "decode_first";
    case SpeakerProbeStage::Clock:
      return "clock";
    case SpeakerProbeStage::PowerWait:
      return "power_wait";
    case SpeakerProbeStage::PowerReady:
      return "power_ready";
    case SpeakerProbeStage::FirstSubmit:
      return "first_submit";
    case SpeakerProbeStage::DecodeStream:
      return "decode_stream";
    case SpeakerProbeStage::Write:
      return "write";
    case SpeakerProbeStage::Cleanup:
      return "cleanup";
    case SpeakerProbeStage::Cancel:
      return "cancel";
    case SpeakerProbeStage::Done:
      return "done";
  }
  return "unknown";
}

const char* speaker_probe_result_name(SpeakerProbeResult result) {
  switch (result) {
    case SpeakerProbeResult::None:
      return "none";
    case SpeakerProbeResult::Pending:
      return "pending";
    case SpeakerProbeResult::Running:
      return "running";
    case SpeakerProbeResult::Ok:
      return "ok";
    case SpeakerProbeResult::Rejected:
      return "rejected";
    case SpeakerProbeResult::Failed:
      return "failed";
    case SpeakerProbeResult::Cancelled:
      return "cancelled";
  }
  return "unknown";
}

const char* speaker_probe_error_name(SpeakerProbeError error) {
  switch (error) {
    case SpeakerProbeError::None:
      return "none";
    case SpeakerProbeError::NotReady:
      return "not_ready";
    case SpeakerProbeError::InvalidArgument:
      return "invalid_arg";
    case SpeakerProbeError::Unsupported:
      return "unsupported";
    case SpeakerProbeError::I2sNew:
      return "i2s_new";
    case SpeakerProbeError::I2sConfig:
      return "i2s_config";
    case SpeakerProbeError::OpusInit:
      return "opus_init";
    case SpeakerProbeError::TaskAlloc:
      return "task_alloc";
    case SpeakerProbeError::PlaybackBusy:
      return "playback_busy";
    case SpeakerProbeError::MicrophoneBusy:
      return "mic_busy";
    case SpeakerProbeError::Dma:
      return "dma";
    case SpeakerProbeError::DecodeReset:
      return "decode_reset";
    case SpeakerProbeError::DecodeFirst:
      return "decode_first";
    case SpeakerProbeError::DecodeStream:
      return "decode_stream";
    case SpeakerProbeError::Clock:
      return "clock";
    case SpeakerProbeError::PowerTimeout:
      return "power_timeout";
    case SpeakerProbeError::Write:
      return "write";
    case SpeakerProbeError::Cleanup:
      return "cleanup";
    case SpeakerProbeError::Cancelled:
      return "cancelled";
    case SpeakerProbeError::Unknown:
      return "unknown";
  }
  return "unknown";
}

bool apply_speaker_probe_state(SpeakerProbeSnapshot* snapshot,
                               std::uint32_t expected_generation,
                               SpeakerProbeStage stage,
                               SpeakerProbeResult result,
                               SpeakerProbeError error,
                               std::int32_t raw_error) {
  if (snapshot == nullptr || !snapshot->present ||
      snapshot->generation != expected_generation) {
    return false;
  }

  // Failed is never allowed to carry None/Cancelled. Normalize malformed or
  // defensive terminal calls at the state-machine boundary so they cannot
  // become immutable contradictory observations.
  if (result == SpeakerProbeResult::Failed && !is_real_failure(error)) {
    error = SpeakerProbeError::Unknown;
    if (raw_error == 0) {
      raw_error = -1;
    }
  }

  const bool existing_failure = is_real_failure(snapshot->error);
  const bool incoming_failure = is_real_failure(error);
  const bool existing_cancel =
      snapshot->error == SpeakerProbeError::Cancelled;

  if (is_terminal_result(snapshot->result)) {
    // A published terminal observation is immutable except for one deliberate
    // causality correction: cancellation may be upgraded when the same worker
    // later observes a concrete failure while unwinding.
    if (snapshot->result == SpeakerProbeResult::Cancelled &&
        existing_cancel && incoming_failure) {
      snapshot->stage = stage;
      snapshot->result = SpeakerProbeResult::Failed;
      snapshot->error = error;
      snapshot->raw_error = raw_error;
      return true;
    }
    return false;
  }

  if (existing_failure) {
    // The first real failure is the causal observation. Terminalize it as a
    // failure even if the worker concurrently reports cancellation.
    if (is_terminal_result(result)) {
      snapshot->result = SpeakerProbeResult::Failed;
    }
    return true;
  }

  if (existing_cancel && !incoming_failure) {
    // Once cancellation is observed, ordinary progress cannot make it
    // disappear. A later real failure is handled below and takes precedence.
    if (is_terminal_result(result)) {
      snapshot->result = SpeakerProbeResult::Cancelled;
    }
    return true;
  }

  snapshot->stage = stage;
  snapshot->result = result;
  snapshot->error = error;
  snapshot->raw_error = raw_error;
  return true;
}

SpeakerProbeCleanupOutcome observe_speaker_probe_cleanup(
    SpeakerProbeCleanupOutcome current,
    std::int32_t raw_error) {
  if (!current.failed && raw_error != 0) {
    current.failed = true;
    current.first_raw_error = raw_error;
  }
  return current;
}

SpeakerProbeTerminalTransition resolve_speaker_probe_terminal(
    const SpeakerProbeSnapshot& current,
    SpeakerProbeResult worker_result,
    std::int32_t fallback_raw_error) {
  if (current.result == SpeakerProbeResult::Failed &&
      !is_real_failure(current.error)) {
    return {
        current.stage,
        SpeakerProbeResult::Failed,
        SpeakerProbeError::Unknown,
        fallback_raw_error,
    };
  }

  if (is_real_failure(current.error)) {
    return {
        current.stage,
        SpeakerProbeResult::Failed,
        current.error,
        current.raw_error == 0 ? fallback_raw_error : current.raw_error,
    };
  }

  if (current.error == SpeakerProbeError::Cancelled) {
    if (worker_result == SpeakerProbeResult::Failed) {
      return {
          current.stage,
          SpeakerProbeResult::Failed,
          SpeakerProbeError::Unknown,
          fallback_raw_error,
      };
    }
    return {
        SpeakerProbeStage::Cancel,
        SpeakerProbeResult::Cancelled,
        SpeakerProbeError::Cancelled,
        current.raw_error,
    };
  }

  switch (worker_result) {
    case SpeakerProbeResult::Ok:
      return {
          SpeakerProbeStage::Done,
          SpeakerProbeResult::Ok,
          SpeakerProbeError::None,
          0,
      };
    case SpeakerProbeResult::Cancelled:
      return {
          SpeakerProbeStage::Cancel,
          SpeakerProbeResult::Cancelled,
          SpeakerProbeError::Cancelled,
          0,
      };
    case SpeakerProbeResult::None:
    case SpeakerProbeResult::Pending:
    case SpeakerProbeResult::Running:
    case SpeakerProbeResult::Rejected:
    case SpeakerProbeResult::Failed:
      return {
          current.stage,
          SpeakerProbeResult::Failed,
          SpeakerProbeError::Unknown,
          fallback_raw_error,
      };
  }

  return {
      current.stage,
      SpeakerProbeResult::Failed,
      SpeakerProbeError::Unknown,
      fallback_raw_error,
  };
}

}  // namespace ai_keyboard
