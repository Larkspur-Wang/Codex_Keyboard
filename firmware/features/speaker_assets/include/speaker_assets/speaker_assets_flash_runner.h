#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/speaker_assets_runtime.h"
#include "speaker_assets/speaker_assets_store_executor.h"
#include "speaker_assets/sound_asset_reader.h"

namespace easy_input::speaker_assets {

inline constexpr std::size_t
    kSpeakerAssetsFlashReadWriteUnitBytes = 256U;
inline constexpr std::size_t
    kSpeakerAssetsFlashEraseUnitBytes = kSoundSectorSize;

// Exact, single-use authority for one bounded worker unit. job_id and
// unit_sequence are boot-lifetime non-zero identities. A granted permit may
// cause at most one physical storage operation; Crc32/Sha256 permits authorize
// one <=256-byte RAM-only hash operation (SHA finalize uses 128) and never
// touch the backend.
struct SpeakerAssetsFlashPermit {
  std::uint64_t job_id = 0U;
  std::uint32_t unit_sequence = 0U;
  SoundStorageWorkKind kind = SoundStorageWorkKind::Read;
  SoundBankId bank = SoundBankId::A;
  std::uint32_t offset = 0U;
  std::uint16_t bytes = 0U;
};

bool speaker_assets_flash_permit_equal(
    const SpeakerAssetsFlashPermit& first,
    const SpeakerAssetsFlashPermit& second);

// Platform-owned synchronization only; the runner and Store remain allocation
// free. lock()/unlock() protect the runner exchange. notify_worker() must be
// latched so a signal immediately before wait_worker() cannot be lost.
// notify_supervisor() wakes/pokes the low-priority resource supervisor but may
// be a no-op when that supervisor already polls.
class SpeakerAssetsFlashRunnerSynchronization {
 public:
  virtual ~SpeakerAssetsFlashRunnerSynchronization() = default;

  virtual void lock() = 0;
  virtual void unlock() = 0;
  virtual void notify_worker() = 0;
  virtual void notify_supervisor() = 0;
  virtual void wait_worker() = 0;
};

// Worker-side permit source used by the storage decorator. Implementations
// must not return true until an exact supervisor grant survived the second
// priority-epoch check immediately before the unit.
class SpeakerAssetsFlashPermitGate {
 public:
  virtual ~SpeakerAssetsFlashPermitGate() = default;

  // A true result leaves one linearized bounded-unit claim held. The caller
  // must perform exactly one described operation, then call
  // worker_complete_permitted_unit().
  virtual bool worker_claim_permit(
      SoundStorageWorkKind kind,
      SoundBankId bank,
      std::uint32_t offset,
      std::size_t bytes) = 0;
  virtual void worker_complete_permitted_unit() = 0;
};

// Range-validating decorator for the existing synchronous Store. It preserves
// Store call order and power-loss boundaries while splitting read/write into
// <=256-byte calls and erase into exactly one 4 KiB sector per call. The
// complete caller range is validated before the first permit or backend call.
class SpeakerAssetsChunkedSoundBankStorage final
    : public SoundBankStorage {
 public:
  SpeakerAssetsChunkedSoundBankStorage(
      SoundBankStorage& backend,
      SpeakerAssetsFlashPermitGate& permit_gate);
  SpeakerAssetsChunkedSoundBankStorage(
      const SpeakerAssetsChunkedSoundBankStorage&) = delete;
  SpeakerAssetsChunkedSoundBankStorage& operator=(
      const SpeakerAssetsChunkedSoundBankStorage&) = delete;
  SpeakerAssetsChunkedSoundBankStorage(
      SpeakerAssetsChunkedSoundBankStorage&&) = delete;
  SpeakerAssetsChunkedSoundBankStorage& operator=(
      SpeakerAssetsChunkedSoundBankStorage&&) = delete;

  SoundStorageIoResult read(SoundBankId bank,
                            std::uint32_t offset,
                            std::uint8_t* destination,
                            std::size_t length) override;
  SoundStorageIoResult write(SoundBankId bank,
                             std::uint32_t offset,
                             const std::uint8_t* source,
                             std::size_t length) override;
  SoundStorageIoResult erase(SoundBankId bank,
                             std::uint32_t offset,
                             std::size_t length) override;
  SoundStorageIoResult checkpoint(
      SoundStorageWorkKind kind,
      SoundBankId bank,
      std::uint32_t offset,
      std::size_t length) override;
  void checkpoint_complete() override;

 private:
  bool range_is_valid(SoundBankId bank,
                      std::uint32_t offset,
                      std::size_t length) const;

  SoundBankStorage& backend_;
  SpeakerAssetsFlashPermitGate& permit_gate_;
  bool cpu_checkpoint_active_ = false;
};

enum class SpeakerAssetsFlashWorkerResult : std::uint8_t {
  NoJob,
  Completed,
};

// Internal Store-owner work uses a handle so a late poll can never consume a
// completion from another boot-read or release request. Handles and
// completions contain values only; no caller-owned buffer is retained.
enum class SpeakerAssetsInternalJobKind : std::uint8_t {
  PrepareBootRead,
  ReleaseRead,
};

struct SpeakerAssetsInternalJobHandle {
  std::uint64_t job_id = 0U;
  SpeakerAssetsInternalJobKind kind =
      SpeakerAssetsInternalJobKind::PrepareBootRead;
};

enum class SpeakerAssetsInternalJobStartResult : std::uint8_t {
  Accepted,
  Busy,
  InvalidArgument,
};

enum class SpeakerAssetsInternalJobPollResult : std::uint8_t {
  Pending,
  Completed,
  Mismatched,
  InvalidArgument,
};

// A successful result owns one exact SoundReadLease in the runner's private
// SoundAssetStore. The immutable address and lease are copied out together;
// the caller must later marshal that exact lease through start_release_read().
// On any resolve failure the worker releases the acquired lease before
// publishing this completion and leaves prepared == false.
struct SpeakerAssetsPrepareBootReadCompletion {
  SoundStoreResult acquire_result = SoundStoreResult::InvalidArgument;
  SoundAssetReadResult resolve_result = SoundAssetReadResult::NotReady;
  SoundStoreResult cleanup_result = SoundStoreResult::Ok;
  bool prepared = false;
  SoundReadLease lease{};
  SoundResolvedAsset asset{};
};

// Stackful cooperative Store runner.
//
// Runtime/supervisor owner:
//   - publish_priority_allowed() on every priority transition;
//   - call step() only as Runtime's non-blocking executor grant/poll.
//
// Dedicated Store owner worker:
//   - wake on notify_worker();
//   - call worker_run_once(), which may block only in wait_worker().
//
// The synchronous SoundAssetStore stack stays entirely on that worker. Every
// backend/CRC/SHA unit crosses the exact-permit exchange, so no Store refactor
// can accidentally rejoin TinyUSB/NimBLE or the Runtime owner stack.
class SpeakerAssetsCooperativeStoreRunner final
    : public SpeakerAssetsRuntimeActionExecutor,
      private SpeakerAssetsFlashPermitGate {
 public:
  SpeakerAssetsCooperativeStoreRunner(
      SoundBankStorage& backend,
      SpeakerAssetsFlashRunnerSynchronization& synchronization);
  SpeakerAssetsCooperativeStoreRunner(
      const SpeakerAssetsCooperativeStoreRunner&) = delete;
  SpeakerAssetsCooperativeStoreRunner& operator=(
      const SpeakerAssetsCooperativeStoreRunner&) = delete;
  SpeakerAssetsCooperativeStoreRunner(
      SpeakerAssetsCooperativeStoreRunner&&) = delete;
  SpeakerAssetsCooperativeStoreRunner& operator=(
      SpeakerAssetsCooperativeStoreRunner&&) = delete;

  // This is the only priority publication path and it has one supervisor
  // publisher. The epoch changes on each allowed/blocked transition,
  // invalidating a permit granted across a late input, HID, recording or
  // microphone-priority change.
  void publish_priority_allowed(bool allowed);
  bool priority_allowed() const;
  std::uint64_t priority_epoch() const;
  std::uint32_t completed_unit_generation() const;

  // Non-blocking Runtime executor entry. It only accepts one borrowed action,
  // grants at most one requested unit, or polls one completion. It never calls
  // Store/Flash, waits, hashes, copies payload bytes, or accumulates actions.
  SpeakerAssetsActionExecutionResult step(
      const SpeakerAssetsActionView& action,
      SpeakerAssetsActionCompletion* completion) override;

  // Non-blocking supervisor APIs for boot playback. start_* only publishes a
  // fixed-size job to the dedicated Store worker. poll_* only grants one
  // requested bounded unit or copies one matching completion; it never runs
  // Store/Flash work. Protocol actions and internal jobs share one slot.
  SpeakerAssetsInternalJobStartResult start_prepare_boot_read(
      SpeakerAssetsInternalJobHandle* handle);
  SpeakerAssetsInternalJobPollResult poll_prepare_boot_read(
      const SpeakerAssetsInternalJobHandle& handle,
      SpeakerAssetsPrepareBootReadCompletion* completion);
  SpeakerAssetsInternalJobStartResult start_release_read(
      const SoundReadLease& lease,
      SpeakerAssetsInternalJobHandle* handle);
  SpeakerAssetsInternalJobPollResult poll_release_read(
      const SpeakerAssetsInternalJobHandle& handle,
      SoundStoreResult* completion);

  // Dedicated Store owner entry. Once it claims an accepted job, it preserves
  // the existing synchronous Store transaction semantics while blocking
  // stackfully between bounded units. Any error after acceptance is published
  // as Completed with SoundStoreResult; it is never converted to Rejected.
  SpeakerAssetsFlashWorkerResult worker_run_once();

  // Read-only diagnostics/test observation. A true result is the exact permit
  // currently awaiting a supervisor grant; no grant is consumed here.
  bool requested_permit(
      SpeakerAssetsFlashPermit* permit) const;
  bool job_active() const;

 private:
  enum class Phase : std::uint8_t {
    Idle,
    Accepted,
    Running,
    PermitRequested,
    PermitGranted,
    Completed,
  };

  enum class ActiveJobKind : std::uint8_t {
    None,
    ProtocolAction,
    PrepareBootRead,
    ReleaseRead,
  };

  bool worker_claim_permit(
      SoundStorageWorkKind kind,
      SoundBankId bank,
      std::uint32_t offset,
      std::size_t bytes) override;
  void worker_complete_permitted_unit() override;
  bool action_is_valid(
      const SpeakerAssetsActionView& action) const;
  bool action_matches(
      const SpeakerAssetsActionView& action) const;
  bool next_job_id(std::uint64_t* job_id);
  bool next_unit_sequence(std::uint32_t* unit_sequence);
  bool exact_priority(std::uint64_t epoch) const;
  bool claim_priority_unit(std::uint64_t epoch);
  void release_priority_unit();
  bool grant_requested_permit_locked(bool* notify_worker);
  void reset_exchange_locked();
  bool internal_handle_matches_locked(
      const SpeakerAssetsInternalJobHandle& handle,
      ActiveJobKind expected_kind) const;

  SpeakerAssetsFlashRunnerSynchronization& synchronization_;
  SpeakerAssetsChunkedSoundBankStorage storage_;
  SoundAssetStore store_;

  // Exchange fields are protected by synchronization_.lock().
  Phase phase_ = Phase::Idle;
  ActiveJobKind active_job_kind_ = ActiveJobKind::None;
  SpeakerAssetsActionView action_{};
  SpeakerAssetsActionCompletion completion_{};
  SpeakerAssetsPrepareBootReadCompletion
      prepare_boot_completion_{};
  SoundReadLease release_read_request_{};
  SoundStoreResult release_read_completion_ =
      SoundStoreResult::InvalidArgument;
  SpeakerAssetsFlashPermit requested_permit_{};
  SpeakerAssetsFlashPermit granted_permit_{};
  std::uint64_t requested_priority_epoch_ = 0U;
  std::uint64_t granted_priority_epoch_ = 0U;
  std::uint64_t active_job_id_ = 0U;
  std::uint64_t next_job_id_ = 1U;

  // Worker-owner only after it claims one accepted job.
  std::uint64_t worker_job_id_ = 0U;
  std::uint32_t next_unit_sequence_ = 1U;

  // One packed atomic is the priority/operation linearization point:
  // [63:2] epoch, [1] one bounded unit claimed, [0] allowed.
  std::atomic<std::uint64_t> priority_state_{4U};
  // Advances after every successfully completed bounded Flash/hash unit.
  // Consumers compare snapshots only; uint32 wrap remains a valid change.
  std::atomic<std::uint32_t> completed_unit_generation_{0U};
};

}  // namespace easy_input::speaker_assets
