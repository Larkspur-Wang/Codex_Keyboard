#pragma once

#include <cstdint>
#include <type_traits>

namespace ai_keyboard {

inline constexpr std::uint8_t kSpeakerProbeStatusVersion = 1;

// Stable wire values for ConfigStatusSnapshot::speaker. These numeric values
// are versioned by `spk.v`; append new values rather than reordering them.
enum class SpeakerProbeStage : std::uint8_t {
  None = 0,
  BootPending = 1,
  Begin = 2,
  I2sNew = 3,
  I2sConfig = 4,
  OpusInit = 5,
  TaskAlloc = 6,
  Ready = 7,
  Request = 8,
  RequestReject = 9,
  Dma = 10,
  DecodeReset = 11,
  DecodeFirst = 12,
  Clock = 13,
  PowerWait = 14,
  PowerReady = 15,
  FirstSubmit = 16,
  DecodeStream = 17,
  Write = 18,
  Cleanup = 19,
  Cancel = 20,
  Done = 21,
};

enum class SpeakerProbeResult : std::uint8_t {
  None = 0,
  Pending = 1,
  Running = 2,
  Ok = 3,
  Rejected = 4,
  Failed = 5,
  Cancelled = 6,
};

enum class SpeakerProbeError : std::uint8_t {
  None = 0,
  NotReady = 1,
  InvalidArgument = 2,
  Unsupported = 3,
  I2sNew = 4,
  I2sConfig = 5,
  OpusInit = 6,
  TaskAlloc = 7,
  PlaybackBusy = 8,
  MicrophoneBusy = 9,
  Dma = 10,
  DecodeReset = 11,
  DecodeFirst = 12,
  DecodeStream = 13,
  Clock = 14,
  PowerTimeout = 15,
  Write = 16,
  Cleanup = 17,
  Cancelled = 18,
  Unknown = 19,
};

// Fixed, allocation-free snapshot latched for the current boot. SpeakerOutput
// copies it under one critical section, so generation, terminal state and
// metrics always describe the same probe observation.
struct SpeakerProbeSnapshot {
  bool present = false;
  std::uint8_t version = kSpeakerProbeStatusVersion;
  SpeakerProbeStage stage = SpeakerProbeStage::None;
  SpeakerProbeResult result = SpeakerProbeResult::None;
  SpeakerProbeError error = SpeakerProbeError::None;
  std::uint32_t generation = 0;
  std::int32_t raw_error = 0;
  std::uint32_t microphone_generation = 0;
  std::uint32_t first_submit_us = 0;
  std::uint32_t decode_total_us = 0;
  std::uint32_t decode_max_us = 0;
  std::uint32_t decoded_frames = 0;
  std::uint32_t decoded_pcm_bytes = 0;
  std::uint32_t stack_high_water_bytes = 0;
  std::uint32_t heap_begin_free = 0;
  std::uint32_t heap_terminal_free = 0;
  std::uint32_t heap_largest_block = 0;
  std::uint32_t heap_minimum_free = 0;
  std::uint32_t decoded_abs_peak = 0;
  // RMS amplitude relative to signed 16-bit full scale, in thousandths.
  std::uint32_t decoded_rms_permille = 0;
};

struct SpeakerProbeCleanupOutcome {
  bool failed = false;
  std::int32_t first_raw_error = 0;
};

struct SpeakerProbeTerminalTransition {
  SpeakerProbeStage stage = SpeakerProbeStage::None;
  SpeakerProbeResult result = SpeakerProbeResult::Failed;
  SpeakerProbeError error = SpeakerProbeError::Unknown;
  std::int32_t raw_error = 0;
};

static_assert(std::is_standard_layout_v<SpeakerProbeSnapshot>);
static_assert(std::is_trivially_copyable_v<SpeakerProbeSnapshot>);
static_assert(std::is_trivially_copyable_v<SpeakerProbeCleanupOutcome>);
static_assert(std::is_trivially_copyable_v<SpeakerProbeTerminalTransition>);

const char* speaker_probe_stage_name(SpeakerProbeStage stage);
const char* speaker_probe_result_name(SpeakerProbeResult result);
const char* speaker_probe_error_name(SpeakerProbeError error);

// Apply one state transition only to the expected playback generation. The
// first real failure is sticky: a concurrent microphone cancellation or later
// cleanup failure must not erase the stage/raw code that explains why playback
// actually failed. A cancellation may still be upgraded to a subsequently
// observed real failure.
bool apply_speaker_probe_state(SpeakerProbeSnapshot* snapshot,
                               std::uint32_t expected_generation,
                               SpeakerProbeStage stage,
                               SpeakerProbeResult result,
                               SpeakerProbeError error,
                               std::int32_t raw_error);

// Fold every fallible cleanup result without losing the first cleanup error.
// Callers pass zero for success and for platform-specific benign outcomes.
SpeakerProbeCleanupOutcome observe_speaker_probe_cleanup(
    SpeakerProbeCleanupOutcome current,
    std::int32_t raw_error);

// Resolve the worker's requested terminal result using the already-observed
// snapshot. Real failure dominates cancellation, and cancellation dominates
// success. A worker failure without a real recorded cause is converted to
// Unknown with the supplied non-zero fallback.
SpeakerProbeTerminalTransition resolve_speaker_probe_terminal(
    const SpeakerProbeSnapshot& current,
    SpeakerProbeResult worker_result,
    std::int32_t fallback_raw_error);

}  // namespace ai_keyboard
