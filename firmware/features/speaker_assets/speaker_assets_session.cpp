#include "speaker_assets/speaker_assets_session.h"

#include <algorithm>
#include <array>
#include <limits>

#include "speaker_assets/sound_asset_crypto.h"

namespace easy_input::speaker_assets {
namespace {

constexpr std::uint8_t kSingleFrameRequestFlags =
    kSpeakerAssetsFlagAckRequested |
    kSpeakerAssetsFlagFirstFragment |
    kSpeakerAssetsFlagLastFragment;
constexpr std::uint8_t kProgressManifestComplete = 0x01U;
constexpr std::size_t kFragmentAckBitmapMaxBytes =
    kSpeakerAssetsUsbFrameBodyBytes - 4U;

void write_le16(std::uint8_t* destination, std::uint16_t value) {
  destination[0] = static_cast<std::uint8_t>(value & 0xFFU);
  destination[1] =
      static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void write_le32(std::uint8_t* destination, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    destination[index] = static_cast<std::uint8_t>(
        (value >> (index * 8U)) & 0xFFU);
  }
}

void write_le64(std::uint8_t* destination, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    destination[index] = static_cast<std::uint8_t>(
        (value >> (index * 8U)) & 0xFFU);
  }
}

bool route_is_valid(const SpeakerAssetsRouteToken& route) {
  if (route.generation == 0U) {
    return false;
  }
  if (route.transport == SpeakerAssetsTransport::Usb) {
    return route.route_id == 0U;
  }
  return route.transport == SpeakerAssetsTransport::Wifi;
}

bool transaction_id_is_valid(
    const std::array<std::uint8_t, kSoundTransactionIdBytes>&
        transaction_id) {
  return std::any_of(
      transaction_id.begin(),
      transaction_id.end(),
      [](std::uint8_t value) { return value != 0U; });
}

bool digest_is_valid(const SoundSha256Digest& digest) {
  return std::any_of(
      digest.begin(),
      digest.end(),
      [](std::uint8_t value) { return value != 0U; });
}

std::uint16_t payload_block_count(std::uint32_t payload_bytes) {
  if (payload_bytes == 0U) {
    return 0U;
  }
  return static_cast<std::uint16_t>(
      (payload_bytes + kSoundPayloadBlockSize - 1U) /
      kSoundPayloadBlockSize);
}

std::uint16_t payload_block_length(std::uint32_t payload_bytes,
                                   std::uint16_t block_index) {
  const auto offset =
      static_cast<std::uint32_t>(block_index) *
      kSoundPayloadBlockSize;
  if (offset >= payload_bytes) {
    return 0U;
  }
  return static_cast<std::uint16_t>(
      std::min<std::uint32_t>(
          kSoundPayloadBlockSize, payload_bytes - offset));
}

bool bitmap_tail_is_zero(const SoundUpdateProgress& progress) {
  const auto first_unused_bit =
      static_cast<std::size_t>(progress.payload_block_count);
  for (std::size_t bit = first_unused_bit;
       bit < kSoundPayloadBlockCount;
       ++bit) {
    const auto mask =
        static_cast<std::uint8_t>(1U << (bit % 8U));
    if ((progress.payload_complete_bitmap[bit / 8U] & mask) != 0U) {
      return false;
    }
  }
  return true;
}

bool progress_is_well_formed(const SoundUpdateProgress& progress) {
  const auto bank = static_cast<std::uint8_t>(
      progress.identity.target_bank);
  return progress.identity.generation != 0U &&
         bank <= static_cast<std::uint8_t>(SoundBankId::B) &&
         transaction_id_is_valid(progress.identity.transaction_id) &&
         progress.manifest_bytes >= 32U &&
         progress.manifest_bytes <= kSoundSectorSize &&
         progress.payload_bytes <= kSoundPayloadMaxSize &&
         progress.payload_block_count ==
             payload_block_count(progress.payload_bytes) &&
         digest_is_valid(progress.bundle_sha256) &&
         bitmap_tail_is_zero(progress);
}

bool active_snapshot_completion_is_well_formed(
    const SoundBankSnapshot& snapshot) {
  if (!snapshot.valid) {
    const auto transaction_is_zero = std::none_of(
        snapshot.transaction_id.begin(),
        snapshot.transaction_id.end(),
        [](std::uint8_t value) { return value != 0U; });
    const auto manifest_digest_is_zero = std::none_of(
        snapshot.manifest_sha256.begin(),
        snapshot.manifest_sha256.end(),
        [](std::uint8_t value) { return value != 0U; });
    const auto bundle_digest_is_zero = std::none_of(
        snapshot.bundle_sha256.begin(),
        snapshot.bundle_sha256.end(),
        [](std::uint8_t value) { return value != 0U; });
    return snapshot.generation == 0U &&
           snapshot.base_generation == 0U &&
           transaction_is_zero &&
           snapshot.manifest_bytes == 0U &&
           snapshot.payload_bytes == 0U &&
           manifest_digest_is_zero &&
           bundle_digest_is_zero;
  }
  const auto bank = static_cast<std::uint8_t>(snapshot.bank);
  return bank <= static_cast<std::uint8_t>(SoundBankId::B) &&
         snapshot.generation != 0U &&
         snapshot.base_generation == snapshot.generation - 1U &&
         transaction_id_is_valid(snapshot.transaction_id) &&
         snapshot.manifest_bytes >= 32U &&
         snapshot.manifest_bytes <= kSoundSectorSize &&
         snapshot.payload_bytes <= kSoundPayloadMaxSize &&
         digest_is_valid(snapshot.manifest_sha256) &&
         digest_is_valid(snapshot.bundle_sha256);
}

bool identity_equal(const SoundUpdateIdentity& first,
                    const SoundUpdateIdentity& second) {
  return first.generation == second.generation &&
         first.target_bank == second.target_bank &&
         first.transaction_id == second.transaction_id;
}

bool progress_matches_plan(const SoundUpdateProgress& progress,
                           const SoundBundlePlan& plan) {
  return progress_is_well_formed(progress) &&
         progress.manifest_bytes == plan.manifest_bytes &&
         progress.payload_bytes == plan.payload_bytes &&
         progress.bundle_sha256 == plan.bundle_sha256;
}

bool progress_matches_session(
    const SoundUpdateProgress& candidate,
    const SoundUpdateProgress& current) {
  return progress_is_well_formed(candidate) &&
         identity_equal(candidate.identity, current.identity) &&
         candidate.manifest_bytes == current.manifest_bytes &&
         candidate.payload_bytes == current.payload_bytes &&
         candidate.payload_block_count ==
             current.payload_block_count &&
         candidate.bundle_sha256 == current.bundle_sha256;
}

bool progress_does_not_regress(
    const SoundUpdateProgress& candidate,
    const SoundUpdateProgress& current) {
  if (current.manifest_complete &&
      !candidate.manifest_complete) {
    return false;
  }
  for (std::size_t index = 0U;
       index < current.payload_complete_bitmap.size();
       ++index) {
    if ((candidate.payload_complete_bitmap[index] &
         current.payload_complete_bitmap[index]) !=
        current.payload_complete_bitmap[index]) {
      return false;
    }
  }
  return true;
}

bool payload_block_is_complete(
    const SoundUpdateProgress& progress,
    std::uint16_t block_index) {
  if (block_index >= progress.payload_block_count) {
    return false;
  }
  const auto mask =
      static_cast<std::uint8_t>(1U << (block_index % 8U));
  return (progress.payload_complete_bitmap[block_index / 8U] &
          mask) != 0U;
}

SoundSha256Digest frame_fingerprint(
    const SpeakerAssetsFrame& frame) {
  std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes>
      encoded{};
  std::size_t encoded_length = 0U;
  if (encode_speaker_assets_wifi_frame(
          frame, &encoded, &encoded_length) !=
      SpeakerAssetsProtocolResult::Ok) {
    return {};
  }
  SoundSha256 hash;
  if (!hash.update(encoded.data(), encoded_length)) {
    return {};
  }
  return hash.finish();
}

SoundSha256Digest plan_fingerprint(const SoundBundlePlan& plan) {
  std::array<std::uint8_t, kSpeakerAssetsPlanWireBytes> encoded{};
  if (encode_sound_bundle_plan_wire(plan, &encoded) !=
      SpeakerAssetsProtocolResult::Ok) {
    return {};
  }
  SoundSha256 hash;
  if (!hash.update(encoded.data(), encoded.size())) {
    return {};
  }
  return hash.finish();
}

SoundSha256Digest unit_fingerprint(
    SpeakerAssetsRegion region,
    std::uint16_t unit_index,
    const std::uint8_t* bytes,
    std::size_t length) {
  if (bytes == nullptr || length == 0U) {
    return {};
  }
  constexpr std::array<std::uint8_t, 8> kDomain{{
      'E', 'I', 'A', '-', 'U', 'N', 'I', 'T'}};
  std::array<std::uint8_t, 9> identity{};
  identity[0] = static_cast<std::uint8_t>(region);
  write_le16(identity.data() + 1U, unit_index);
  write_le32(
      identity.data() + 3U,
      static_cast<std::uint32_t>(length));
  SoundSha256 hash;
  if (!hash.update(kDomain.data(), kDomain.size()) ||
      !hash.update(identity.data(), identity.size()) ||
      !hash.update(bytes, length)) {
    return {};
  }
  return hash.finish();
}

}  // namespace

SpeakerAssetsSession::SpeakerAssetsSession(
    std::uint32_t cookie_seed,
    std::uint32_t action_token_seed)
    : next_cookie_(cookie_seed == 0U ? 1U : cookie_seed),
      next_action_token_(
          action_token_seed == 0U ? 1U : action_token_seed) {}

SpeakerAssetsSessionResult SpeakerAssetsSession::consume(
    const SpeakerAssetsRouteToken& exact_route,
    const SpeakerAssetsFrame& normalized_frame,
    SpeakerAssetsEmission* output) {
  if (output == nullptr ||
      !route_is_valid(exact_route) ||
      normalized_frame.request_id == 0U ||
      normalized_frame.body_length >
          kSpeakerAssetsWifiFrameBodyBytes) {
    return SpeakerAssetsSessionResult::InvalidArgument;
  }
  *output = {};

  if ((normalized_frame.flags & ~kSpeakerAssetsKnownFlags) != 0U ||
      (normalized_frame.flags &
       (kSpeakerAssetsFlagResponse | kSpeakerAssetsFlagError)) != 0U) {
    emit_status_reply(
        normalized_frame,
        SpeakerAssetsStatus::BadRequest,
        normalized_frame.session_cookie,
        output);
    return SpeakerAssetsSessionResult::Ok;
  }

  const auto opcode =
      static_cast<SpeakerAssetsOpcode>(normalized_frame.opcode);
  if (opcode == SpeakerAssetsOpcode::Capabilities) {
    return consume_capabilities(
        exact_route, normalized_frame, output);
  }
  if (phase_ == SpeakerAssetsSessionPhase::ActionPending) {
    emit_status_reply(
        normalized_frame,
        SpeakerAssetsStatus::Busy,
        normalized_frame.session_cookie,
        output);
    return SpeakerAssetsSessionResult::Ok;
  }

  switch (opcode) {
    case SpeakerAssetsOpcode::Capabilities:
      break;
    case SpeakerAssetsOpcode::Begin:
      return consume_begin(exact_route, normalized_frame, output);
    case SpeakerAssetsOpcode::Resume:
      return consume_resume(exact_route, normalized_frame, output);
    case SpeakerAssetsOpcode::Data:
      return consume_data(exact_route, normalized_frame, output);
    case SpeakerAssetsOpcode::Query:
      return consume_query(exact_route, normalized_frame, output);
    case SpeakerAssetsOpcode::Commit:
      return consume_terminal_action(
          exact_route,
          normalized_frame,
          SpeakerAssetsActionKind::Commit,
          output);
    case SpeakerAssetsOpcode::Abort:
      return consume_terminal_action(
          exact_route,
          normalized_frame,
          SpeakerAssetsActionKind::Abort,
          output);
    case SpeakerAssetsOpcode::RecoverInvalidStaging:
      return consume_recover_invalid_staging(
          exact_route, normalized_frame, output);
    case SpeakerAssetsOpcode::CurrentActive:
      return consume_current_active(
          exact_route, normalized_frame, output);
  }
  emit_status_reply(
      normalized_frame,
      SpeakerAssetsStatus::BadRequest,
      normalized_frame.session_cookie,
      output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult
SpeakerAssetsSession::consume_capabilities(
    const SpeakerAssetsRouteToken&,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsEmission* output) {
  if (frame.flags != kSingleFrameRequestFlags ||
      frame.session_cookie != 0U ||
      frame.object_offset != 0U ||
      frame.body_length != 0U) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  emit_capabilities_reply(frame, output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult SpeakerAssetsSession::consume_begin(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsEmission* output) {
  return consume_plan_action(
      route, frame, SpeakerAssetsActionKind::Begin, output);
}

SpeakerAssetsSessionResult
SpeakerAssetsSession::consume_recover_invalid_staging(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsEmission* output) {
  return consume_plan_action(
      route,
      frame,
      SpeakerAssetsActionKind::DiscardInvalidStaging,
      output);
}

SpeakerAssetsSessionResult
SpeakerAssetsSession::consume_current_active(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsEmission* output) {
  if (frame.flags != kSingleFrameRequestFlags ||
      frame.session_cookie != 0U ||
      frame.object_offset != 0U ||
      frame.body_length != 0U) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  const auto fingerprint = frame_fingerprint(frame);
  SpeakerAssetsFrame replay{};
  const auto replay_result = lookup_replay(
      route,
      0U,
      frame.request_id,
      frame.opcode,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &replay);
  if (replay_result == ReplayLookup::Exact) {
    output->kind = SpeakerAssetsEmissionKind::Reply;
    output->reply = replay;
    return SpeakerAssetsSessionResult::Ok;
  }
  if (replay_result == ReplayLookup::Conflict) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::IntegrityError, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  if (phase_ != SpeakerAssetsSessionPhase::Idle ||
      route_bound_ || plan_assembler_.bound()) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::Busy, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  SpeakerAssetsActionView action{};
  begin_pending_action(
      SpeakerAssetsActionKind::QueryCurrentActive,
      route,
      frame,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &action);
  emit_action(action, output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult SpeakerAssetsSession::consume_plan_action(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsActionKind action_kind,
    SpeakerAssetsEmission* output) {
  if (action_kind != SpeakerAssetsActionKind::Begin &&
      action_kind !=
          SpeakerAssetsActionKind::DiscardInvalidStaging) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  constexpr std::uint8_t kAllowedFlags =
      kSpeakerAssetsFlagAckRequested |
      kSpeakerAssetsFlagFirstFragment |
      kSpeakerAssetsFlagLastFragment;
  const auto end =
      static_cast<std::uint64_t>(frame.object_offset) +
      frame.body_length;
  const auto has_first =
      (frame.flags & kSpeakerAssetsFlagFirstFragment) != 0U;
  const auto has_last =
      (frame.flags & kSpeakerAssetsFlagLastFragment) != 0U;
  const auto boundary_flags_valid =
      has_first == (frame.object_offset == 0U) &&
      has_last == (end == kSpeakerAssetsPlanWireBytes);
  if ((frame.flags & ~kAllowedFlags) != 0U ||
      (frame.flags & kSpeakerAssetsFlagAckRequested) == 0U ||
      frame.session_cookie != 0U ||
      frame.body_length == 0U ||
      frame.object_offset >= kSpeakerAssetsPlanWireBytes ||
      end > kSpeakerAssetsPlanWireBytes ||
      !boundary_flags_valid) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  if (phase_ == SpeakerAssetsSessionPhase::Idle) {
    if (!has_first) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::BadRequest, 0U, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    if (plan_assembler_.begin(route, frame.request_id) !=
        SpeakerAssetsProtocolResult::Ok) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::Busy, 0U, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    phase_ = SpeakerAssetsSessionPhase::BeginAssembly;
    begin_replay_from_ready_ = false;
    plan_action_kind_ = action_kind;
  } else if (phase_ == SpeakerAssetsSessionPhase::Ready) {
    if (action_kind != SpeakerAssetsActionKind::Begin) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::Busy, 0U, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    if (!route_bound_ ||
        !speaker_assets_route_equal(route_, route) ||
        !has_first) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::Busy, 0U, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    plan_assembler_.reset();
    if (plan_assembler_.begin(route, frame.request_id) !=
        SpeakerAssetsProtocolResult::Ok) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::Busy, 0U, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    phase_ = SpeakerAssetsSessionPhase::BeginAssembly;
    begin_replay_from_ready_ = true;
    plan_action_kind_ = action_kind;
  } else if (phase_ !=
             SpeakerAssetsSessionPhase::BeginAssembly) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::Busy, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  } else if (plan_action_kind_ != action_kind) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::Busy, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  const auto received_before =
      plan_assembler_.received_bytes();
  const auto first_before = begin_saw_first_;
  const auto last_before = begin_saw_last_;
  const auto accept = plan_assembler_.accept_fragment(
      route,
      frame.request_id,
      static_cast<std::uint16_t>(frame.object_offset),
      frame.body.data(),
      frame.body_length);
  if (accept != SpeakerAssetsProtocolResult::Ok) {
    emit_status_reply(
        frame,
        accept == SpeakerAssetsProtocolResult::Conflict
            ? SpeakerAssetsStatus::IntegrityError
            : SpeakerAssetsStatus::BadRequest,
        0U,
        output);
    return SpeakerAssetsSessionResult::Ok;
  }
  const auto made_progress =
      plan_assembler_.received_bytes() > received_before ||
      (has_first && !first_before) ||
      (has_last && !last_before);
  if (made_progress) {
    note_partial_activity();
  }
  begin_saw_first_ = begin_saw_first_ || has_first;
  begin_saw_last_ = begin_saw_last_ || has_last;
  if (!plan_assembler_.complete() ||
      !begin_saw_first_ ||
      !begin_saw_last_) {
    const auto first_missing =
        plan_assembler_.first_missing_offset();
    const auto bitmap_offset = first_missing / 8U;
    const auto total_bitmap_bytes =
        kSpeakerAssetsPlanWireBytes / 8U;
    const auto remaining_bitmap_bytes =
        bitmap_offset < total_bitmap_bytes
            ? total_bitmap_bytes - bitmap_offset
            : 0U;
    const auto bitmap_bytes = std::min<std::size_t>(
        kFragmentAckBitmapMaxBytes,
        remaining_bitmap_bytes);
    std::array<std::uint8_t, kFragmentAckBitmapMaxBytes>
        bitmap{};
    if (plan_assembler_.copy_received_bitmap(
            bitmap_offset,
            bitmap.data(),
            bitmap_bytes) != SpeakerAssetsProtocolResult::Ok) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::InternalError, 0U, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    emit_fragment_reply(
        frame,
        SpeakerAssetsStatus::Ok,
        0U,
        static_cast<std::uint32_t>(first_missing),
        static_cast<std::uint16_t>(bitmap_offset),
        bitmap.data(),
        static_cast<std::uint8_t>(bitmap_bytes),
        output);
    return SpeakerAssetsSessionResult::Ok;
  }

  if (plan_assembler_.decode(&decoded_plan_) !=
      SpeakerAssetsProtocolResult::Ok) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::IntegrityError, 0U, output);
    restore_after_begin_assembly();
    return SpeakerAssetsSessionResult::Ok;
  }
  const auto fingerprint = plan_fingerprint(decoded_plan_);
  SpeakerAssetsFrame replay{};
  const auto replay_result = lookup_replay(
      route,
      0U,
      frame.request_id,
      frame.opcode,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &replay);
  if (replay_result == ReplayLookup::Exact) {
    output->kind = SpeakerAssetsEmissionKind::Reply;
    output->reply = replay;
    restore_after_begin_assembly();
    return SpeakerAssetsSessionResult::Ok;
  }
  if (replay_result == ReplayLookup::Conflict ||
      begin_replay_from_ready_) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::IntegrityError, 0U, output);
    restore_after_begin_assembly();
    return SpeakerAssetsSessionResult::Ok;
  }

  SpeakerAssetsActionView action{};
  begin_pending_action(
      action_kind,
      route,
      frame,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &action);
  action.plan = &decoded_plan_;
  plan_assembler_.reset();
  begin_saw_first_ = false;
  begin_saw_last_ = false;
  plan_action_kind_ = SpeakerAssetsActionKind::Begin;
  emit_action(action, output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult SpeakerAssetsSession::consume_resume(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsEmission* output) {
  if (frame.flags != kSingleFrameRequestFlags ||
      frame.session_cookie != 0U ||
      frame.object_offset != 0U ||
      frame.body_length != kSoundTransactionIdBytes) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  SoundUpdateIdentity expected{};
  std::copy_n(
      frame.body.begin(),
      kSoundTransactionIdBytes,
      expected.transaction_id.begin());
  if (!transaction_id_is_valid(expected.transaction_id)) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  const auto fingerprint = frame_fingerprint(frame);
  SpeakerAssetsFrame replay{};
  const auto replay_result = lookup_replay(
      route,
      0U,
      frame.request_id,
      frame.opcode,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &replay);
  if (replay_result == ReplayLookup::Exact) {
    output->kind = SpeakerAssetsEmissionKind::Reply;
    output->reply = replay;
    return SpeakerAssetsSessionResult::Ok;
  }
  if (replay_result == ReplayLookup::Conflict) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::IntegrityError, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  if (phase_ != SpeakerAssetsSessionPhase::Idle) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::Busy, 0U, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  SpeakerAssetsActionView action{};
  begin_pending_action(
      SpeakerAssetsActionKind::ResumeQuery,
      route,
      frame,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &action);
  action.expected_identity = expected;
  pending_.expected_identity = expected;
  action.query_mode =
      SpeakerAssetsResumeQueryMode::ResumeOrRebind;
  emit_action(action, output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult SpeakerAssetsSession::consume_data(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsEmission* output) {
  constexpr std::uint8_t kAllowedFlags =
      kSpeakerAssetsFlagAckRequested |
      kSpeakerAssetsFlagFirstFragment |
      kSpeakerAssetsFlagLastFragment |
      kSpeakerAssetsFlagPayloadRegion;
  if ((frame.flags & ~kAllowedFlags) != 0U ||
      (frame.flags & kSpeakerAssetsFlagAckRequested) == 0U ||
      frame.body_length == 0U ||
      !route_bound_ ||
      !speaker_assets_route_equal(route_, route) ||
      frame.session_cookie != session_cookie_ ||
      !progress_valid_ ||
      (phase_ != SpeakerAssetsSessionPhase::Ready &&
       phase_ != SpeakerAssetsSessionPhase::UnitAssembly)) {
    emit_status_reply(
        frame,
        route_bound_ &&
                speaker_assets_route_equal(route_, route) &&
                frame.session_cookie == session_cookie_
            ? SpeakerAssetsStatus::BadRequest
            : SpeakerAssetsStatus::TransactionMismatch,
        frame.session_cookie,
        output);
    return SpeakerAssetsSessionResult::Ok;
  }

  const auto region =
      (frame.flags & kSpeakerAssetsFlagPayloadRegion) != 0U
          ? SpeakerAssetsRegion::Payload
          : SpeakerAssetsRegion::Manifest;
  std::uint16_t unit_index = 0U;
  std::uint32_t unit_base = 0U;
  std::uint16_t expected_bytes = 0U;
  std::uint16_t unit_offset = 0U;
  if (region == SpeakerAssetsRegion::Manifest) {
    if (frame.object_offset >= progress_.manifest_bytes) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::BadRequest,
          session_cookie_, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    expected_bytes =
        static_cast<std::uint16_t>(progress_.manifest_bytes);
    unit_offset =
        static_cast<std::uint16_t>(frame.object_offset);
  } else {
    if (frame.object_offset >= progress_.payload_bytes) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::BadRequest,
          session_cookie_, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    unit_index = static_cast<std::uint16_t>(
        frame.object_offset / kSoundPayloadBlockSize);
    unit_base =
        static_cast<std::uint32_t>(unit_index) *
        kSoundPayloadBlockSize;
    unit_offset = static_cast<std::uint16_t>(
        frame.object_offset - unit_base);
    expected_bytes =
        payload_block_length(progress_.payload_bytes, unit_index);
  }
  const auto unit_end =
      static_cast<std::size_t>(unit_offset) + frame.body_length;
  const auto has_first =
      (frame.flags & kSpeakerAssetsFlagFirstFragment) != 0U;
  const auto has_last =
      (frame.flags & kSpeakerAssetsFlagLastFragment) != 0U;
  if (expected_bytes == 0U ||
      unit_end > expected_bytes ||
      has_first != (unit_offset == 0U) ||
      has_last != (unit_end == expected_bytes)) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest,
        session_cookie_, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  if (phase_ == SpeakerAssetsSessionPhase::Ready) {
    if (!has_first ||
        block_assembler_.begin(
            region, unit_index, expected_bytes) !=
            SpeakerAssetsProtocolResult::Ok) {
      emit_status_reply(
          frame, SpeakerAssetsStatus::BadRequest,
          session_cookie_, output);
      return SpeakerAssetsSessionResult::Ok;
    }
    phase_ = SpeakerAssetsSessionPhase::UnitAssembly;
    unit_request_id_ = frame.request_id;
    unit_base_offset_ = unit_base;
    unit_saw_first_ = false;
    unit_saw_last_ = false;
  } else if (unit_request_id_ != frame.request_id ||
             block_assembler_.region() != region ||
             block_assembler_.unit_index() != unit_index) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::Busy,
        session_cookie_, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  const auto received_before =
      block_assembler_.received_bytes();
  const auto first_before = unit_saw_first_;
  const auto last_before = unit_saw_last_;
  const auto accept = block_assembler_.accept_fragment(
      region,
      unit_index,
      unit_offset,
      frame.body.data(),
      frame.body_length);
  if (accept != SpeakerAssetsProtocolResult::Ok) {
    emit_status_reply(
        frame,
        accept == SpeakerAssetsProtocolResult::Conflict
            ? SpeakerAssetsStatus::IntegrityError
            : SpeakerAssetsStatus::BadRequest,
        session_cookie_,
        output);
    return SpeakerAssetsSessionResult::Ok;
  }
  const auto made_progress =
      block_assembler_.received_bytes() > received_before ||
      (has_first && !first_before) ||
      (has_last && !last_before);
  if (made_progress) {
    note_partial_activity();
  }
  unit_saw_first_ = unit_saw_first_ || has_first;
  unit_saw_last_ = unit_saw_last_ || has_last;
  if (!block_assembler_.complete() ||
      !unit_saw_first_ ||
      !unit_saw_last_) {
    const auto first_missing =
        block_assembler_.first_missing_offset();
    const auto bitmap_offset = first_missing / 8U;
    const auto total_bitmap_bytes =
        (static_cast<std::size_t>(
             block_assembler_.expected_bytes()) +
         7U) /
        8U;
    const auto remaining_bitmap_bytes =
        bitmap_offset < total_bitmap_bytes
            ? total_bitmap_bytes - bitmap_offset
            : 0U;
    const auto bitmap_bytes = std::min<std::size_t>(
        kFragmentAckBitmapMaxBytes,
        remaining_bitmap_bytes);
    std::array<std::uint8_t, kFragmentAckBitmapMaxBytes>
        bitmap{};
    if (block_assembler_.copy_received_bitmap(
            bitmap_offset,
            bitmap.data(),
            bitmap_bytes) != SpeakerAssetsProtocolResult::Ok) {
      emit_status_reply(
          frame,
          SpeakerAssetsStatus::InternalError,
          session_cookie_,
          output);
      return SpeakerAssetsSessionResult::Ok;
    }
    emit_fragment_reply(
        frame,
        SpeakerAssetsStatus::Ok,
        session_cookie_,
        unit_base_offset_ +
            static_cast<std::uint32_t>(first_missing),
        static_cast<std::uint16_t>(bitmap_offset),
        bitmap.data(),
        static_cast<std::uint8_t>(bitmap_bytes),
        output);
    return SpeakerAssetsSessionResult::Ok;
  }

  const auto fingerprint = unit_fingerprint(
      region,
      unit_index,
      block_assembler_.data(),
      block_assembler_.expected_bytes());
  SpeakerAssetsFrame replay{};
  const auto replay_result = lookup_replay(
      route,
      session_cookie_,
      frame.request_id,
      frame.opcode,
      region,
      unit_index,
      fingerprint,
      &replay);
  if (replay_result == ReplayLookup::Exact) {
    output->kind = SpeakerAssetsEmissionKind::Reply;
    output->reply = replay;
    block_assembler_.reset();
    phase_ = SpeakerAssetsSessionPhase::Ready;
    return SpeakerAssetsSessionResult::Ok;
  }
  if (replay_result == ReplayLookup::Conflict) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::IntegrityError,
        session_cookie_, output);
    block_assembler_.reset();
    phase_ = SpeakerAssetsSessionPhase::Ready;
    return SpeakerAssetsSessionResult::Ok;
  }

  SpeakerAssetsActionView action{};
  begin_pending_action(
      region == SpeakerAssetsRegion::Manifest
          ? SpeakerAssetsActionKind::WriteManifest
          : SpeakerAssetsActionKind::WritePayloadBlock,
      route,
      frame,
      region,
      unit_index,
      fingerprint,
      &action);
  action.expected_identity = progress_.identity;
  pending_.expected_identity = progress_.identity;
  action.bytes = block_assembler_.data();
  action.length = block_assembler_.expected_bytes();
  action.block_index = unit_index;
  emit_action(action, output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult SpeakerAssetsSession::consume_query(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsEmission* output) {
  if (frame.flags != kSingleFrameRequestFlags ||
      frame.object_offset != 0U ||
      frame.body_length != 0U) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest,
        frame.session_cookie, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  const auto fingerprint = frame_fingerprint(frame);
  SpeakerAssetsFrame replay{};
  const auto replay_result = lookup_replay(
      route,
      frame.session_cookie,
      frame.request_id,
      frame.opcode,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &replay);
  if (replay_result == ReplayLookup::Exact) {
    output->kind = SpeakerAssetsEmissionKind::Reply;
    output->reply = replay;
    return SpeakerAssetsSessionResult::Ok;
  }
  if (replay_result == ReplayLookup::Conflict) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::IntegrityError,
        frame.session_cookie, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  if (phase_ != SpeakerAssetsSessionPhase::Ready ||
      !route_bound_ ||
      !speaker_assets_route_equal(route_, route) ||
      frame.session_cookie != session_cookie_) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::TransactionMismatch,
        frame.session_cookie, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  SpeakerAssetsActionView action{};
  begin_pending_action(
      SpeakerAssetsActionKind::ResumeQuery,
      route,
      frame,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &action);
  action.expected_identity = progress_.identity;
  pending_.expected_identity = progress_.identity;
  action.query_mode =
      SpeakerAssetsResumeQueryMode::CurrentProgress;
  emit_action(action, output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult
SpeakerAssetsSession::consume_terminal_action(
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& frame,
    SpeakerAssetsActionKind kind,
    SpeakerAssetsEmission* output) {
  if (frame.flags != kSingleFrameRequestFlags ||
      frame.object_offset != 0U ||
      frame.body_length != 0U) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::BadRequest,
        frame.session_cookie, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  const auto fingerprint = frame_fingerprint(frame);
  SpeakerAssetsFrame replay{};
  const auto replay_result = lookup_replay(
      route,
      frame.session_cookie,
      frame.request_id,
      frame.opcode,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &replay);
  if (replay_result == ReplayLookup::Exact) {
    output->kind = SpeakerAssetsEmissionKind::Reply;
    output->reply = replay;
    return SpeakerAssetsSessionResult::Ok;
  }
  if (replay_result == ReplayLookup::Conflict) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::IntegrityError,
        frame.session_cookie, output);
    return SpeakerAssetsSessionResult::Ok;
  }
  if (phase_ != SpeakerAssetsSessionPhase::Ready ||
      !route_bound_ ||
      !speaker_assets_route_equal(route_, route) ||
      frame.session_cookie != session_cookie_) {
    emit_status_reply(
        frame, SpeakerAssetsStatus::TransactionMismatch,
        frame.session_cookie, output);
    return SpeakerAssetsSessionResult::Ok;
  }

  SpeakerAssetsActionView action{};
  begin_pending_action(
      kind,
      route,
      frame,
      SpeakerAssetsRegion::Manifest,
      0U,
      fingerprint,
      &action);
  action.expected_identity = progress_.identity;
  pending_.expected_identity = progress_.identity;
  emit_action(action, output);
  return SpeakerAssetsSessionResult::Ok;
}

SpeakerAssetsSessionResult SpeakerAssetsSession::complete(
    const SpeakerAssetsActionCompletion& completion,
    SpeakerAssetsEmission* output) {
  if (output == nullptr) {
    return SpeakerAssetsSessionResult::InvalidArgument;
  }
  *output = {};
  if (!pending_.active) {
    return SpeakerAssetsSessionResult::NoPendingAction;
  }
  if (completion.token != pending_.token ||
      completion.kind != pending_.kind) {
    return SpeakerAssetsSessionResult::StaleCompletion;
  }

  const auto finished = pending_;
  if (finished.orphaned) {
    clear_pending_action();
    clear_partial_assembly();
    clear_binding();
    phase_ = SpeakerAssetsSessionPhase::Idle;
    return SpeakerAssetsSessionResult::Ok;
  }

  SpeakerAssetsFrame request{};
  request.opcode = finished.opcode;
  request.request_id = finished.request_id;
  request.session_cookie = finished.request_cookie;
  request.object_offset = finished.request_offset;
  if (completion.result != SoundStoreResult::Ok) {
    emit_status_reply(
        request,
        speaker_assets_status_from_store_result(completion.result),
        finished.request_cookie,
        output);
    switch (finished.kind) {
      case SpeakerAssetsActionKind::Begin:
      case SpeakerAssetsActionKind::DiscardInvalidStaging:
      case SpeakerAssetsActionKind::QueryCurrentActive:
      case SpeakerAssetsActionKind::ResumeQuery:
        if (finished.kind == SpeakerAssetsActionKind::ResumeQuery &&
            finished.request_cookie != 0U) {
          phase_ = SpeakerAssetsSessionPhase::Ready;
        } else {
          clear_binding();
          phase_ = SpeakerAssetsSessionPhase::Idle;
        }
        break;
      case SpeakerAssetsActionKind::WriteManifest:
      case SpeakerAssetsActionKind::WritePayloadBlock:
      case SpeakerAssetsActionKind::Commit:
      case SpeakerAssetsActionKind::Abort:
        phase_ = SpeakerAssetsSessionPhase::Ready;
        break;
    }
    if (finished.kind == SpeakerAssetsActionKind::WriteManifest ||
        finished.kind ==
            SpeakerAssetsActionKind::WritePayloadBlock) {
      block_assembler_.reset();
    }
    clear_pending_action();
    return SpeakerAssetsSessionResult::Ok;
  }

  bool completion_contract_valid = true;
  if (finished.kind == SpeakerAssetsActionKind::Begin) {
    completion_contract_valid =
        completion.identity_valid &&
        completion.progress_valid &&
        identity_equal(
            completion.identity,
            completion.progress.identity) &&
        progress_matches_plan(completion.progress, decoded_plan_);
  } else if (finished.kind ==
             SpeakerAssetsActionKind::QueryCurrentActive) {
    completion_contract_valid =
        completion.current_active_valid &&
        active_snapshot_completion_is_well_formed(
            completion.current_active) &&
        !completion.identity_valid &&
        !completion.progress_valid &&
        !completion.outcome_valid;
  } else if (finished.kind ==
             SpeakerAssetsActionKind::ResumeQuery) {
    completion_contract_valid =
        completion.outcome_valid &&
        completion.identity_valid &&
        completion.identity.transaction_id ==
            finished.expected_identity.transaction_id;
    if (completion_contract_valid &&
        completion.outcome ==
            SpeakerAssetsTransactionOutcome::Active) {
      completion_contract_valid =
          completion.progress_valid &&
          identity_equal(
              completion.identity,
              completion.progress.identity);
      if (completion_contract_valid &&
          finished.request_cookie == 0U) {
        completion_contract_valid =
            progress_is_well_formed(completion.progress);
      } else if (completion_contract_valid) {
        completion_contract_valid =
            progress_valid_ &&
            progress_matches_session(
                completion.progress, progress_) &&
            progress_does_not_regress(
                completion.progress, progress_);
      }
    } else if (completion_contract_valid) {
      completion_contract_valid =
          finished.request_cookie == 0U &&
          !completion.progress_valid &&
          (completion.outcome ==
               SpeakerAssetsTransactionOutcome::Committed ||
           completion.outcome ==
               SpeakerAssetsTransactionOutcome::Unknown);
      if (completion_contract_valid &&
          completion.outcome ==
              SpeakerAssetsTransactionOutcome::Committed) {
        completion_contract_valid =
            completion.identity.generation != 0U &&
            static_cast<std::uint8_t>(
                completion.identity.target_bank) <=
                static_cast<std::uint8_t>(SoundBankId::B) &&
            completion.outcome_manifest_bytes >= 32U &&
            completion.outcome_manifest_bytes <=
                kSoundSectorSize &&
            completion.outcome_payload_bytes <=
                kSoundPayloadMaxSize;
      }
      if (completion_contract_valid &&
          completion.outcome ==
              SpeakerAssetsTransactionOutcome::Unknown) {
        completion_contract_valid =
            completion.identity.generation == 0U &&
            completion.outcome_manifest_bytes == 0U &&
            completion.outcome_payload_bytes == 0U;
      }
    }
  } else if (finished.kind ==
                 SpeakerAssetsActionKind::WriteManifest ||
             finished.kind ==
                 SpeakerAssetsActionKind::WritePayloadBlock) {
    completion_contract_valid =
        completion.progress_valid &&
        progress_valid_ &&
        progress_matches_session(
            completion.progress, progress_) &&
        progress_does_not_regress(
            completion.progress, progress_);
    if (completion_contract_valid &&
        finished.kind ==
            SpeakerAssetsActionKind::WriteManifest) {
      completion_contract_valid =
          completion.progress.manifest_complete;
    }
    if (completion_contract_valid &&
        finished.kind ==
            SpeakerAssetsActionKind::WritePayloadBlock) {
      completion_contract_valid = payload_block_is_complete(
          completion.progress, finished.unit_index);
    }
  }

  if (!completion_contract_valid) {
    emit_status_reply(
        request,
        SpeakerAssetsStatus::InternalError,
        finished.request_cookie,
        output);
    clear_pending_action();
    clear_partial_assembly();
    clear_binding();
    phase_ = SpeakerAssetsSessionPhase::Idle;
    return SpeakerAssetsSessionResult::Ok;
  }

  switch (finished.kind) {
    case SpeakerAssetsActionKind::Begin:
      progress_ = completion.progress;
      progress_valid_ = true;
      route_ = finished.route;
      route_bound_ = true;
      session_cookie_ = next_cookie();
      phase_ = SpeakerAssetsSessionPhase::Ready;
      emit_identity_reply(
          request, progress_, session_cookie_, output);
      remember_replay(finished, output->reply);
      break;
    case SpeakerAssetsActionKind::DiscardInvalidStaging:
      emit_status_reply(
          request, SpeakerAssetsStatus::Ok, 0U, output);
      remember_replay(finished, output->reply);
      clear_partial_assembly();
      clear_binding();
      phase_ = SpeakerAssetsSessionPhase::Idle;
      break;
    case SpeakerAssetsActionKind::QueryCurrentActive:
      emit_current_active_reply(
          request, completion.current_active, output);
      remember_replay(finished, output->reply);
      clear_partial_assembly();
      clear_binding();
      phase_ = SpeakerAssetsSessionPhase::Idle;
      break;
    case SpeakerAssetsActionKind::ResumeQuery:
      if (completion.outcome !=
          SpeakerAssetsTransactionOutcome::Active) {
        emit_outcome_reply(
            request,
            completion.outcome,
            completion.identity,
            completion.outcome_manifest_bytes,
            completion.outcome_payload_bytes,
            output);
        remember_replay(finished, output->reply);
        clear_partial_assembly();
        clear_binding();
        phase_ = SpeakerAssetsSessionPhase::Idle;
        break;
      }
      progress_ = completion.progress;
      progress_valid_ = true;
      if (finished.request_cookie == 0U) {
        route_ = finished.route;
        route_bound_ = true;
        session_cookie_ = next_cookie();
        phase_ = SpeakerAssetsSessionPhase::Ready;
        emit_identity_reply(
            request, progress_, session_cookie_, output);
      } else {
        phase_ = SpeakerAssetsSessionPhase::Ready;
        emit_progress_reply(
            request, progress_, session_cookie_, output);
      }
      remember_replay(finished, output->reply);
      break;
    case SpeakerAssetsActionKind::WriteManifest:
    case SpeakerAssetsActionKind::WritePayloadBlock:
      progress_ = completion.progress;
      progress_valid_ = true;
      phase_ = SpeakerAssetsSessionPhase::Ready;
      emit_progress_reply(
          request, progress_, session_cookie_, output);
      remember_replay(finished, output->reply);
      block_assembler_.reset();
      break;
    case SpeakerAssetsActionKind::Commit:
    case SpeakerAssetsActionKind::Abort:
      emit_status_reply(
          request,
          SpeakerAssetsStatus::Ok,
          session_cookie_,
          output);
      remember_replay(finished, output->reply);
      clear_partial_assembly();
      clear_binding();
      phase_ = SpeakerAssetsSessionPhase::Idle;
      break;
  }
  clear_pending_action();
  return SpeakerAssetsSessionResult::Ok;
}

void SpeakerAssetsSession::route_closed(
    const SpeakerAssetsRouteToken& exact_route) {
  // Replay entries are part of the volatile route identity. Once that exact
  // lifetime is revoked, an old cookie/request pair must not bypass the normal
  // binding checks and receive a stale successful response.
  clear_replay_for_route(exact_route);
  const auto owns_binding =
      route_bound_ && speaker_assets_route_equal(route_, exact_route);
  const auto owns_begin =
      plan_assembler_.bound() &&
      speaker_assets_route_equal(
          plan_assembler_.owner(), exact_route);
  const auto owns_pending =
      pending_.active &&
      speaker_assets_route_equal(pending_.route, exact_route);
  if (!owns_binding && !owns_begin && !owns_pending) {
    return;
  }
  if (owns_pending) {
    pending_.orphaned = true;
    if (pending_.kind != SpeakerAssetsActionKind::WriteManifest &&
        pending_.kind !=
            SpeakerAssetsActionKind::WritePayloadBlock) {
      plan_assembler_.reset();
      begin_saw_first_ = false;
      begin_saw_last_ = false;
    }
    clear_binding();
    return;
  }
  clear_partial_assembly();
  clear_binding();
  phase_ = SpeakerAssetsSessionPhase::Idle;
}

void SpeakerAssetsSession::all_routes_closed() {
  replay_ = {};
  next_replay_slot_ = 0U;
  if (pending_.active) {
    pending_.orphaned = true;
    // Write actions borrow bytes from block_assembler_. Keep that storage
    // intact until the matching cooperative completion has returned.
    if (pending_.kind != SpeakerAssetsActionKind::WriteManifest &&
        pending_.kind !=
            SpeakerAssetsActionKind::WritePayloadBlock) {
      clear_partial_assembly();
    }
    clear_binding();
    return;
  }
  clear_partial_assembly();
  clear_binding();
  phase_ = SpeakerAssetsSessionPhase::Idle;
}

bool SpeakerAssetsSession::expire_partial(
    const SpeakerAssetsRouteToken& exact_route) {
  if (phase_ == SpeakerAssetsSessionPhase::BeginAssembly &&
      plan_assembler_.bound() &&
      speaker_assets_route_equal(
          plan_assembler_.owner(), exact_route)) {
    restore_after_begin_assembly();
    return true;
  }
  if (phase_ == SpeakerAssetsSessionPhase::UnitAssembly &&
      route_bound_ &&
      speaker_assets_route_equal(route_, exact_route)) {
    block_assembler_.reset();
    unit_request_id_ = 0U;
    unit_base_offset_ = 0U;
    unit_saw_first_ = false;
    unit_saw_last_ = false;
    phase_ = SpeakerAssetsSessionPhase::Ready;
    return true;
  }
  return false;
}

SpeakerAssetsSessionPhase SpeakerAssetsSession::phase() const {
  return phase_;
}

bool SpeakerAssetsSession::route_bound() const {
  return route_bound_;
}

const SpeakerAssetsRouteToken& SpeakerAssetsSession::route() const {
  return route_;
}

std::uint32_t SpeakerAssetsSession::session_cookie() const {
  return session_cookie_;
}

bool SpeakerAssetsSession::action_pending() const {
  return pending_.active;
}

std::uint32_t SpeakerAssetsSession::pending_action_token() const {
  return pending_.active ? pending_.token : 0U;
}

const SoundUpdateProgress* SpeakerAssetsSession::progress() const {
  return progress_valid_ ? &progress_ : nullptr;
}

std::size_t SpeakerAssetsSession::replay_entry_count() const {
  return static_cast<std::size_t>(std::count_if(
      replay_.begin(),
      replay_.end(),
      [](const ReplayEntry& entry) { return entry.valid; }));
}

std::uint32_t
SpeakerAssetsSession::partial_activity_counter() const {
  return partial_activity_counter_;
}

void SpeakerAssetsSession::emit_status_reply(
    const SpeakerAssetsFrame& request,
    SpeakerAssetsStatus status,
    std::uint32_t response_cookie,
    SpeakerAssetsEmission* output) const {
  output->kind = SpeakerAssetsEmissionKind::Reply;
  output->reply = {};
  output->reply.opcode = request.opcode;
  output->reply.flags = kSpeakerAssetsFlagResponse;
  if (status != SpeakerAssetsStatus::Ok) {
    output->reply.flags |= kSpeakerAssetsFlagError;
  }
  output->reply.request_id = request.request_id;
  output->reply.session_cookie = response_cookie;
  output->reply.object_offset = request.object_offset;
  output->reply.body_length = 1U;
  output->reply.body[0] = static_cast<std::uint8_t>(status);
}

void SpeakerAssetsSession::emit_fragment_reply(
    const SpeakerAssetsFrame& request,
    SpeakerAssetsStatus status,
    std::uint32_t response_cookie,
    std::uint32_t first_missing,
    std::uint16_t bitmap_byte_offset,
    const std::uint8_t* bitmap,
    std::uint8_t bitmap_bytes,
    SpeakerAssetsEmission* output) const {
  emit_status_reply(request, status, response_cookie, output);
  output->reply.object_offset = first_missing;
  if (bitmap == nullptr ||
      bitmap_bytes > kFragmentAckBitmapMaxBytes) {
    return;
  }
  write_le16(
      output->reply.body.data() + 1U,
      bitmap_byte_offset);
  output->reply.body[3] = bitmap_bytes;
  std::copy_n(
      bitmap,
      bitmap_bytes,
      output->reply.body.begin() + 4U);
  output->reply.body_length =
      static_cast<std::uint16_t>(4U + bitmap_bytes);
}

void SpeakerAssetsSession::emit_capabilities_reply(
    const SpeakerAssetsFrame& request,
    SpeakerAssetsEmission* output) const {
  emit_status_reply(
      request, SpeakerAssetsStatus::Ok, 0U, output);
  auto& body = output->reply.body;
  body[0] = static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok);
  body[1] = kSpeakerAssetsProtocolVersion;
  body[2] = kSpeakerAssetsCapabilities;
  body[3] =
      static_cast<std::uint8_t>(kSpeakerAssetsUsbFrameBodyBytes);
  body[4] =
      static_cast<std::uint8_t>(kSpeakerAssetsWifiFrameBodyBytes);
  write_le16(
      body.data() + 5U,
      static_cast<std::uint16_t>(
          kSpeakerAssetsPlanWireBytes));
  write_le16(
      body.data() + 7U,
      static_cast<std::uint16_t>(
          kSoundPayloadBlockSize));
  body[9] = static_cast<std::uint8_t>(
      kSpeakerAssetsProgressBitmapBytes);
  body[10] =
      static_cast<std::uint8_t>(kSpeakerAssetsReplayEntries);
  output->reply.body_length = 11U;
}

void SpeakerAssetsSession::emit_current_active_reply(
    const SpeakerAssetsFrame& request,
    const SoundBankSnapshot& active,
    SpeakerAssetsEmission* output) const {
  emit_status_reply(
      request, SpeakerAssetsStatus::Ok, 0U, output);
  const auto generation = active.valid ? active.generation : 0U;
  output->reply.object_offset =
      static_cast<std::uint32_t>(generation & 0xFFFFFFFFULL);
  auto& body = output->reply.body;
  body[0] = static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok);
  body[1] = active.valid
                ? static_cast<std::uint8_t>(active.bank)
                : 0xFFU;
  write_le32(
      body.data() + 2U,
      static_cast<std::uint32_t>(generation >> 32U));
  if (active.valid) {
    std::copy(
        active.bundle_sha256.begin(),
        active.bundle_sha256.end(),
        body.begin() + 6U);
  } else {
    std::fill_n(body.begin() + 6U, active.bundle_sha256.size(), 0U);
  }
  output->reply.body_length = 38U;
}

void SpeakerAssetsSession::emit_identity_reply(
    const SpeakerAssetsFrame& request,
    const SoundUpdateProgress& progress,
    std::uint32_t response_cookie,
    SpeakerAssetsEmission* output) const {
  emit_status_reply(
      request, SpeakerAssetsStatus::Ok,
      response_cookie, output);
  output->reply.object_offset = 0U;
  auto& body = output->reply.body;
  body[0] = static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok);
  body[1] =
      static_cast<std::uint8_t>(progress.identity.target_bank);
  write_le16(body.data() + 2U, progress.payload_block_count);
  write_le64(body.data() + 4U, progress.identity.generation);
  write_le32(body.data() + 12U, progress.manifest_bytes);
  write_le32(body.data() + 16U, progress.payload_bytes);
  std::copy(
      progress.identity.transaction_id.begin(),
      progress.identity.transaction_id.end(),
      body.begin() + 20U);
  output->reply.body_length = 36U;
}

void SpeakerAssetsSession::emit_progress_reply(
    const SpeakerAssetsFrame& request,
    const SoundUpdateProgress& progress,
    std::uint32_t response_cookie,
    SpeakerAssetsEmission* output) const {
  emit_status_reply(
      request, SpeakerAssetsStatus::Ok,
      response_cookie, output);
  output->reply.object_offset = 0U;
  auto& body = output->reply.body;
  body[0] = static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok);
  body[1] = progress.manifest_complete
                ? kProgressManifestComplete
                : 0U;
  body[2] =
      static_cast<std::uint8_t>(progress.identity.target_bank);
  body[3] = static_cast<std::uint8_t>(
      kSpeakerAssetsProgressBitmapBytes);
  write_le64(body.data() + 4U, progress.identity.generation);
  write_le32(body.data() + 12U, progress.manifest_bytes);
  write_le32(body.data() + 16U, progress.payload_bytes);
  write_le16(body.data() + 20U, progress.payload_block_count);
  std::copy(
      progress.payload_complete_bitmap.begin(),
      progress.payload_complete_bitmap.end(),
      body.begin() + 22U);
  output->reply.body_length = 38U;
}

void SpeakerAssetsSession::emit_outcome_reply(
    const SpeakerAssetsFrame& request,
    SpeakerAssetsTransactionOutcome outcome,
    const SoundUpdateIdentity& identity,
    std::uint32_t manifest_bytes,
    std::uint32_t payload_bytes,
    SpeakerAssetsEmission* output) const {
  emit_status_reply(
      request, SpeakerAssetsStatus::Ok, 0U, output);
  output->reply.object_offset = 0U;
  auto& body = output->reply.body;
  body[0] = static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok);
  body[1] = static_cast<std::uint8_t>(outcome);
  body[2] =
      outcome == SpeakerAssetsTransactionOutcome::Unknown
          ? 0xFFU
          : static_cast<std::uint8_t>(identity.target_bank);
  body[3] = 0U;
  write_le64(body.data() + 4U, identity.generation);
  std::copy(
      identity.transaction_id.begin(),
      identity.transaction_id.end(),
      body.begin() + 12U);
  write_le32(body.data() + 28U, manifest_bytes);
  write_le32(body.data() + 32U, payload_bytes);
  output->reply.body_length = 36U;
}

void SpeakerAssetsSession::emit_action(
    const SpeakerAssetsActionView& action,
    SpeakerAssetsEmission* output) const {
  output->kind = SpeakerAssetsEmissionKind::Action;
  output->action = action;
}

void SpeakerAssetsSession::begin_pending_action(
    SpeakerAssetsActionKind kind,
    const SpeakerAssetsRouteToken& route,
    const SpeakerAssetsFrame& request,
    SpeakerAssetsRegion region,
    std::uint16_t unit_index,
    const SoundSha256Digest& fingerprint,
    SpeakerAssetsActionView* action) {
  pending_ = {};
  pending_.active = true;
  pending_.kind = kind;
  pending_.route = route;
  pending_.token = next_action_token();
  pending_.request_id = request.request_id;
  pending_.request_cookie = request.session_cookie;
  pending_.request_offset = request.object_offset;
  pending_.opcode = request.opcode;
  pending_.region = region;
  pending_.unit_index = unit_index;
  pending_.fingerprint = fingerprint;
  phase_ = SpeakerAssetsSessionPhase::ActionPending;

  *action = {};
  action->token = pending_.token;
  action->kind = kind;
  action->route = route;
  action->request_id = request.request_id;
  action->session_cookie = request.session_cookie;
}

void SpeakerAssetsSession::clear_pending_action() {
  pending_ = {};
}

void SpeakerAssetsSession::note_partial_activity() {
  ++partial_activity_counter_;
  if (partial_activity_counter_ == 0U) {
    ++partial_activity_counter_;
  }
}

void SpeakerAssetsSession::clear_partial_assembly() {
  plan_assembler_.reset();
  block_assembler_.reset();
  begin_saw_first_ = false;
  begin_saw_last_ = false;
  begin_replay_from_ready_ = false;
  plan_action_kind_ = SpeakerAssetsActionKind::Begin;
  unit_request_id_ = 0U;
  unit_base_offset_ = 0U;
  unit_saw_first_ = false;
  unit_saw_last_ = false;
}

void SpeakerAssetsSession::clear_binding() {
  route_ = {};
  route_bound_ = false;
  session_cookie_ = 0U;
  progress_ = {};
  progress_valid_ = false;
}

void SpeakerAssetsSession::restore_after_begin_assembly() {
  plan_assembler_.reset();
  begin_saw_first_ = false;
  begin_saw_last_ = false;
  const auto restore_ready =
      begin_replay_from_ready_ && route_bound_;
  begin_replay_from_ready_ = false;
  plan_action_kind_ = SpeakerAssetsActionKind::Begin;
  phase_ = restore_ready
               ? SpeakerAssetsSessionPhase::Ready
               : SpeakerAssetsSessionPhase::Idle;
}

std::uint32_t SpeakerAssetsSession::next_cookie() {
  const auto value = next_cookie_;
  ++next_cookie_;
  if (next_cookie_ == 0U) {
    next_cookie_ = 1U;
  }
  return value == 0U ? next_cookie() : value;
}

std::uint32_t SpeakerAssetsSession::next_action_token() {
  const auto value = next_action_token_;
  ++next_action_token_;
  if (next_action_token_ == 0U) {
    next_action_token_ = 1U;
  }
  return value == 0U ? next_action_token() : value;
}

SpeakerAssetsSession::ReplayLookup
SpeakerAssetsSession::lookup_replay(
    const SpeakerAssetsRouteToken& route,
    std::uint32_t request_cookie,
    std::uint32_t request_id,
    std::uint8_t opcode,
    SpeakerAssetsRegion region,
    std::uint16_t unit_index,
    const SoundSha256Digest& fingerprint,
    SpeakerAssetsFrame* reply) const {
  bool same_request_seen = false;
  for (const auto& entry : replay_) {
    if (!entry.valid ||
        !speaker_assets_route_equal(entry.route, route) ||
        entry.request_cookie != request_cookie ||
        entry.request_id != request_id) {
      continue;
    }
    same_request_seen = true;
    if (entry.opcode == opcode &&
        entry.region == region &&
        entry.unit_index == unit_index &&
        entry.fingerprint == fingerprint) {
      if (reply != nullptr) {
        *reply = entry.reply;
      }
      return ReplayLookup::Exact;
    }
  }
  return same_request_seen
             ? ReplayLookup::Conflict
             : ReplayLookup::Miss;
}

void SpeakerAssetsSession::clear_replay_for_route(
    const SpeakerAssetsRouteToken& exact_route) {
  for (auto& entry : replay_) {
    if (entry.valid &&
        speaker_assets_route_equal(
            entry.route, exact_route)) {
      entry = {};
    }
  }
}

void SpeakerAssetsSession::remember_replay(
    const PendingAction& pending,
    const SpeakerAssetsFrame& reply) {
  auto& entry = replay_[next_replay_slot_];
  entry = {};
  entry.valid = true;
  entry.route = pending.route;
  entry.request_cookie = pending.request_cookie;
  entry.request_id = pending.request_id;
  entry.opcode = pending.opcode;
  entry.region = pending.region;
  entry.unit_index = pending.unit_index;
  entry.fingerprint = pending.fingerprint;
  entry.reply = reply;
  next_replay_slot_ =
      (next_replay_slot_ + 1U) % replay_.size();
}

SpeakerAssetsStatus speaker_assets_status_from_store_result(
    SoundStoreResult result) {
  switch (result) {
    case SoundStoreResult::Ok:
      return SpeakerAssetsStatus::Ok;
    case SoundStoreResult::InvalidArgument:
      return SpeakerAssetsStatus::BadRequest;
    case SoundStoreResult::Unavailable:
    case SoundStoreResult::IoError:
      return SpeakerAssetsStatus::StorageUnavailable;
    case SoundStoreResult::InvalidBank:
      return SpeakerAssetsStatus::IntegrityError;
    case SoundStoreResult::InvalidStaging:
      return SpeakerAssetsStatus::RecoveryRequired;
    case SoundStoreResult::InvalidManifest:
    case SoundStoreResult::CrcMismatch:
    case SoundStoreResult::HashMismatch:
      return SpeakerAssetsStatus::IntegrityError;
    case SoundStoreResult::Incomplete:
      return SpeakerAssetsStatus::Incomplete;
    case SoundStoreResult::Busy:
      return SpeakerAssetsStatus::Busy;
    case SoundStoreResult::BankPinned:
      return SpeakerAssetsStatus::BankPinned;
    case SoundStoreResult::StaleBase:
      return SpeakerAssetsStatus::StaleBase;
    case SoundStoreResult::GenerationExhausted:
      return SpeakerAssetsStatus::GenerationExhausted;
    case SoundStoreResult::TransactionMismatch:
      return SpeakerAssetsStatus::TransactionMismatch;
    case SoundStoreResult::SplitBrain:
      return SpeakerAssetsStatus::SplitBrain;
    case SoundStoreResult::FactoryBlank:
      // FactoryBlank is an internal boot-read disposition, not a protocol
      // operation result. Fail closed if it ever crosses this boundary.
      return SpeakerAssetsStatus::StorageUnavailable;
  }
  return SpeakerAssetsStatus::InternalError;
}

}  // namespace easy_input::speaker_assets
