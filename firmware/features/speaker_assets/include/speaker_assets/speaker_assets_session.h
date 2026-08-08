#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/speaker_assets_protocol.h"

namespace easy_input::speaker_assets {

inline constexpr std::size_t kSpeakerAssetsReplayEntries = 8U;
inline constexpr std::size_t kSpeakerAssetsProgressBitmapBytes =
    kSoundPayloadBlockCount / 8U;

enum class SpeakerAssetsEmissionKind : std::uint8_t {
  None,
  Reply,
  Action,
};

enum class SpeakerAssetsActionKind : std::uint8_t {
  Begin,
  DiscardInvalidStaging,
  QueryCurrentActive,
  ResumeQuery,
  WriteManifest,
  WritePayloadBlock,
  Commit,
  Abort,
};

enum class SpeakerAssetsResumeQueryMode : std::uint8_t {
  ResumeOrRebind,
  CurrentProgress,
};

enum class SpeakerAssetsSessionPhase : std::uint8_t {
  Idle,
  BeginAssembly,
  Ready,
  UnitAssembly,
  ActionPending,
};

enum class SpeakerAssetsSessionResult : std::uint8_t {
  Ok,
  InvalidArgument,
  NoPendingAction,
  StaleCompletion,
};

enum class SpeakerAssetsTransactionOutcome : std::uint8_t {
  Active = 1U,
  Committed = 2U,
  // The transaction is not retained in the active staging record or either
  // committed A/B snapshot. This is not proof that it never committed: an
  // older committed bank may already have been reused by a later generation.
  Unknown = 3U,
};

// A borrowed, immutable action produced by consume(). The supervisor must
// execute it outside TinyUSB/NimBLE callbacks and call complete() before
// consuming another frame. Borrowed plan/bytes remain stable until complete().
struct SpeakerAssetsActionView {
  std::uint32_t token = 0U;
  SpeakerAssetsActionKind kind = SpeakerAssetsActionKind::Begin;
  SpeakerAssetsRouteToken route{};
  std::uint32_t request_id = 0U;
  std::uint32_t session_cookie = 0U;
  const SoundBundlePlan* plan = nullptr;
  SoundUpdateIdentity expected_identity{};
  SpeakerAssetsResumeQueryMode query_mode =
      SpeakerAssetsResumeQueryMode::CurrentProgress;
  const std::uint8_t* bytes = nullptr;
  std::uint16_t length = 0U;
  std::uint16_t block_index = 0U;
};

struct SpeakerAssetsActionCompletion {
  std::uint32_t token = 0U;
  SpeakerAssetsActionKind kind = SpeakerAssetsActionKind::Begin;
  SoundStoreResult result = SoundStoreResult::InvalidArgument;
  // Begin must return the identity produced by Store separately from the
  // progress snapshot so the session can verify both views agree.
  bool identity_valid = false;
  SoundUpdateIdentity identity{};
  // Required after successful Begin, ResumeQuery, WriteManifest and
  // WritePayloadBlock. The snapshot must describe durable, Flash-verified
  // progress and is rejected if it does not match the pending transaction.
  bool progress_valid = false;
  SoundUpdateProgress progress{};
  // Required for successful ResumeQuery. Committed resolves a lost terminal
  // ACK while the transaction remains in the two-bank history window.
  // Unknown means no authoritative record is retained and must not be treated
  // as proof that COMMIT never succeeded.
  bool outcome_valid = false;
  SpeakerAssetsTransactionOutcome outcome =
      SpeakerAssetsTransactionOutcome::Active;
  std::uint32_t outcome_manifest_bytes = 0U;
  std::uint32_t outcome_payload_bytes = 0U;
  // Required after a successful QueryCurrentActive action. The separate
  // contract flag distinguishes "query completed with no active bundle"
  // (current_active.valid == false) from a missing executor result.
  bool current_active_valid = false;
  SoundBankSnapshot current_active{};
};

struct SpeakerAssetsEmission {
  SpeakerAssetsEmissionKind kind = SpeakerAssetsEmissionKind::None;
  SpeakerAssetsFrame reply{};
  SpeakerAssetsActionView action{};
};

// Pure, allocation-free protocol/session state. This class never owns a Store
// and never performs Flash I/O. It accepts normalized logical frames after USB
// or BLE physical decoding and emits either an immediate reply or one borrowed
// Store action for a low-priority resource supervisor.
class SpeakerAssetsSession {
 public:
  explicit SpeakerAssetsSession(std::uint32_t cookie_seed = 1U,
                                std::uint32_t action_token_seed = 1U);

  [[nodiscard]] SpeakerAssetsSessionResult consume(
      const SpeakerAssetsRouteToken& exact_route,
      const SpeakerAssetsFrame& normalized_frame,
      SpeakerAssetsEmission* output);

  [[nodiscard]] SpeakerAssetsSessionResult complete(
      const SpeakerAssetsActionCompletion& completion,
      SpeakerAssetsEmission* output);

  // Revokes route ownership and all partial RAM state. A pending Store action
  // becomes orphaned and is allowed to finish, but it cannot emit a reply or
  // revive a cookie. This never implies Abort and never erases staging.
  void route_closed(const SpeakerAssetsRouteToken& exact_route);
  // Fail-closed transport teardown. Revokes every binding and partial
  // assembler regardless of whether a route has reached the bound phase. A
  // pending Store action is orphaned and may finish only for cleanup; its
  // completion cannot emit a reply or revive session state.
  void all_routes_closed();
  // Clears only an incomplete plan/unit owned by this exact route. The
  // supervisor calls this after a bounded inactivity timeout. It never emits
  // an action, never aborts staging and never affects an action in flight.
  bool expire_partial(const SpeakerAssetsRouteToken& exact_route);

  SpeakerAssetsSessionPhase phase() const;
  bool route_bound() const;
  const SpeakerAssetsRouteToken& route() const;
  std::uint32_t session_cookie() const;
  bool action_pending() const;
  std::uint32_t pending_action_token() const;
  const SoundUpdateProgress* progress() const;
  std::size_t replay_entry_count() const;
  // Monotonic RAM-only signal changed only when a BEGIN or DATA fragment adds
  // bytes or a previously unseen required boundary marker. Supervisors use it
  // to avoid unrelated traffic or exact duplicates extending inactivity.
  std::uint32_t partial_activity_counter() const;

 private:
  struct PendingAction {
    bool active = false;
    bool orphaned = false;
    SpeakerAssetsActionKind kind = SpeakerAssetsActionKind::Begin;
    SpeakerAssetsRouteToken route{};
    std::uint32_t token = 0U;
    std::uint32_t request_id = 0U;
    std::uint32_t request_cookie = 0U;
    std::uint32_t request_offset = 0U;
    std::uint8_t opcode = 0U;
    SpeakerAssetsRegion region = SpeakerAssetsRegion::Manifest;
    std::uint16_t unit_index = 0U;
    SoundSha256Digest fingerprint{};
    SoundUpdateIdentity expected_identity{};
  };

  struct ReplayEntry {
    bool valid = false;
    SpeakerAssetsRouteToken route{};
    std::uint32_t request_cookie = 0U;
    std::uint32_t request_id = 0U;
    std::uint8_t opcode = 0U;
    SpeakerAssetsRegion region = SpeakerAssetsRegion::Manifest;
    std::uint16_t unit_index = 0U;
    SoundSha256Digest fingerprint{};
    SpeakerAssetsFrame reply{};
  };

  enum class ReplayLookup : std::uint8_t {
    Miss,
    Exact,
    Conflict,
  };

  [[nodiscard]] SpeakerAssetsSessionResult consume_capabilities(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_begin(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_recover_invalid_staging(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_current_active(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_plan_action(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsActionKind action_kind,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_resume(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_data(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_query(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsEmission* output);
  [[nodiscard]] SpeakerAssetsSessionResult consume_terminal_action(
      const SpeakerAssetsRouteToken& route,
      const SpeakerAssetsFrame& frame,
      SpeakerAssetsActionKind kind,
      SpeakerAssetsEmission* output);

  void emit_status_reply(const SpeakerAssetsFrame& request,
                         SpeakerAssetsStatus status,
                         std::uint32_t response_cookie,
                         SpeakerAssetsEmission* output) const;
  void emit_fragment_reply(const SpeakerAssetsFrame& request,
                           SpeakerAssetsStatus status,
                           std::uint32_t response_cookie,
                           std::uint32_t first_missing,
                           std::uint16_t bitmap_byte_offset,
                           const std::uint8_t* bitmap,
                           std::uint8_t bitmap_bytes,
                           SpeakerAssetsEmission* output) const;
  void emit_capabilities_reply(const SpeakerAssetsFrame& request,
                               SpeakerAssetsEmission* output) const;
  void emit_current_active_reply(
      const SpeakerAssetsFrame& request,
      const SoundBankSnapshot& active,
      SpeakerAssetsEmission* output) const;
  void emit_identity_reply(const SpeakerAssetsFrame& request,
                           const SoundUpdateProgress& progress,
                           std::uint32_t response_cookie,
                           SpeakerAssetsEmission* output) const;
  void emit_progress_reply(const SpeakerAssetsFrame& request,
                           const SoundUpdateProgress& progress,
                           std::uint32_t response_cookie,
                           SpeakerAssetsEmission* output) const;
  void emit_outcome_reply(
      const SpeakerAssetsFrame& request,
      SpeakerAssetsTransactionOutcome outcome,
      const SoundUpdateIdentity& identity,
      std::uint32_t manifest_bytes,
      std::uint32_t payload_bytes,
      SpeakerAssetsEmission* output) const;
  void emit_action(const SpeakerAssetsActionView& action,
                   SpeakerAssetsEmission* output) const;

  void begin_pending_action(SpeakerAssetsActionKind kind,
                            const SpeakerAssetsRouteToken& route,
                            const SpeakerAssetsFrame& request,
                            SpeakerAssetsRegion region,
                            std::uint16_t unit_index,
                            const SoundSha256Digest& fingerprint,
                            SpeakerAssetsActionView* action);
  void clear_pending_action();
  void note_partial_activity();
  void clear_partial_assembly();
  void clear_binding();
  void restore_after_begin_assembly();
  std::uint32_t next_cookie();
  std::uint32_t next_action_token();

  ReplayLookup lookup_replay(const SpeakerAssetsRouteToken& route,
                             std::uint32_t request_cookie,
                             std::uint32_t request_id,
                             std::uint8_t opcode,
                             SpeakerAssetsRegion region,
                             std::uint16_t unit_index,
                             const SoundSha256Digest& fingerprint,
                             SpeakerAssetsFrame* reply) const;
  void clear_replay_for_route(
      const SpeakerAssetsRouteToken& exact_route);
  void remember_replay(const PendingAction& pending,
                       const SpeakerAssetsFrame& reply);

  SpeakerAssetsSessionPhase phase_ = SpeakerAssetsSessionPhase::Idle;
  SpeakerAssetsRouteToken route_{};
  bool route_bound_ = false;
  std::uint32_t session_cookie_ = 0U;
  std::uint32_t next_cookie_ = 1U;
  std::uint32_t next_action_token_ = 1U;

  SoundUpdateProgress progress_{};
  bool progress_valid_ = false;
  std::uint32_t partial_activity_counter_ = 0U;
  SoundBundlePlan decoded_plan_{};
  SpeakerAssetsPlanAssembler plan_assembler_{};
  SpeakerAssetsBlockAssembler block_assembler_{};
  bool begin_saw_first_ = false;
  bool begin_saw_last_ = false;
  bool begin_replay_from_ready_ = false;
  SpeakerAssetsActionKind plan_action_kind_ =
      SpeakerAssetsActionKind::Begin;
  std::uint32_t unit_request_id_ = 0U;
  std::uint32_t unit_base_offset_ = 0U;
  bool unit_saw_first_ = false;
  bool unit_saw_last_ = false;

  PendingAction pending_{};
  std::array<ReplayEntry, kSpeakerAssetsReplayEntries> replay_{};
  std::size_t next_replay_slot_ = 0U;
};

SpeakerAssetsStatus speaker_assets_status_from_store_result(
    SoundStoreResult result);

}  // namespace easy_input::speaker_assets
