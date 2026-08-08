#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/sound_asset_crypto.h"

namespace easy_input::speaker_assets {

inline constexpr std::uint32_t kSoundBankSize = 0x90000U;
inline constexpr std::uint32_t kSoundSectorSize = 0x1000U;
inline constexpr std::uint32_t kSoundStagingHeaderOffset = 0x0000U;
inline constexpr std::uint32_t kSoundManifestOffset = 0x1000U;
inline constexpr std::uint32_t kSoundJournalOffset = 0x2000U;
inline constexpr std::uint32_t kSoundPayloadOffset = 0x3000U;
inline constexpr std::uint32_t kSoundPayloadMaxSize = 0x80000U;
inline constexpr std::uint32_t kSoundReservedOffset = 0x83000U;
inline constexpr std::uint32_t kSoundReservedEndOffset = 0x8F000U;
inline constexpr std::uint32_t kSoundCommitOffset = 0x8F000U;
inline constexpr std::uint32_t kSoundPayloadBlockSize = kSoundSectorSize;
inline constexpr std::size_t kSoundPayloadBlockCount =
    kSoundPayloadMaxSize / kSoundPayloadBlockSize;
inline constexpr std::size_t kSoundTransactionIdBytes = 16U;
inline constexpr std::size_t kSoundMaximumReadLeases = 8U;

enum class SoundBankId : std::uint8_t {
  A = 0,
  B = 1,
};

enum class SoundStorageIoResult : std::uint8_t {
  Ok,
  InvalidArgument,
  OutOfBounds,
  NotAligned,
  Unavailable,
  IoError,
};

// One bounded unit of Store work. Read/Write/Erase describe physical storage
// operations. Crc32/Sha256 are RAM-only checkpoints used before hashing at
// most kSpeakerAssetsFlashReadWriteUnitBytes bytes on the Store owner worker.
// The base storage ignores CPU checkpoints; the cooperative decorator turns
// every kind into an exact supervisor permit.
enum class SoundStorageWorkKind : std::uint8_t {
  Read,
  Write,
  Erase,
  Crc32,
  Sha256,
};

class SoundBankStorage {
 public:
  virtual ~SoundBankStorage() = default;

  virtual SoundStorageIoResult read(SoundBankId bank,
                                    std::uint32_t offset,
                                    std::uint8_t* destination,
                                    std::size_t length) = 0;
  virtual SoundStorageIoResult write(SoundBankId bank,
                                     std::uint32_t offset,
                                     const std::uint8_t* source,
                                     std::size_t length) = 0;
  virtual SoundStorageIoResult erase(SoundBankId bank,
                                     std::uint32_t offset,
                                     std::size_t length) = 0;

  // Called immediately before one bounded RAM-only CRC/SHA operation.
  // Existing synchronous backends intentionally accept it as a no-op. A
  // cooperative storage decorator overrides it so input/recording priority is
  // rechecked between every <=256-byte CPU unit (including SHA finalization)
  // as well as every physical I/O unit.
  virtual SoundStorageIoResult checkpoint(
      SoundStorageWorkKind kind,
      SoundBankId bank,
      std::uint32_t offset,
      std::size_t length) {
    static_cast<void>(kind);
    static_cast<void>(bank);
    static_cast<void>(offset);
    static_cast<void>(length);
    return SoundStorageIoResult::Ok;
  }

  // Closes the CPU unit opened by a successful checkpoint(). Existing
  // synchronous backends intentionally ignore it. Cooperative storage keeps
  // its linearized priority claim held until this call, so a late priority
  // transition races with the claim itself rather than with a pre-op check.
  virtual void checkpoint_complete() {}
};

enum class SoundStoreResult : std::uint8_t {
  Ok,
  InvalidArgument,
  Unavailable,
  IoError,
  InvalidBank,
  // A non-erased staging header exists but is not a valid transaction. This is
  // deliberately distinct from committed-bank integrity failures so only the
  // safe BEGIN path can authorize explicit staging recovery.
  InvalidStaging,
  InvalidManifest,
  CrcMismatch,
  HashMismatch,
  Incomplete,
  Busy,
  BankPinned,
  StaleBase,
  GenerationExhausted,
  TransactionMismatch,
  SplitBrain,
  // Both A/B final commit records are still fully erased. This is a
  // read-only boot result meaning no sound preference has ever committed;
  // it must never be produced for corrupt, split-brain, or unreadable banks.
  FactoryBlank,
};

struct SoundBankSnapshot {
  bool valid = false;
  SoundBankId bank = SoundBankId::A;
  std::uint64_t generation = 0;
  std::uint64_t base_generation = 0;
  std::array<std::uint8_t, kSoundTransactionIdBytes> transaction_id{};
  std::uint32_t manifest_bytes = 0;
  std::uint32_t payload_bytes = 0;
  SoundSha256Digest manifest_sha256{};
  SoundSha256Digest bundle_sha256{};
};

struct SoundStoreSelection {
  SoundBankSnapshot bank_a{};
  SoundBankSnapshot bank_b{};
  bool active_valid = false;
  bool split_brain = false;
  SoundBankSnapshot active{};
};

struct SoundBundlePlan {
  std::uint64_t base_generation = 0;
  SoundSha256Digest base_bundle_sha256{};
  std::uint32_t manifest_bytes = 0;
  std::uint32_t payload_bytes = 0;
  std::uint32_t manifest_crc32 = 0;
  SoundSha256Digest manifest_sha256{};
  SoundSha256Digest bundle_sha256{};
  std::array<std::uint32_t, kSoundPayloadBlockCount> payload_block_crc32{};
};

struct SoundUpdateIdentity {
  std::uint64_t generation = 0;
  SoundBankId target_bank = SoundBankId::A;
  std::array<std::uint8_t, kSoundTransactionIdBytes> transaction_id{};
};

struct SoundUpdateProgress {
  SoundUpdateIdentity identity{};
  std::uint32_t manifest_bytes = 0;
  std::uint32_t payload_bytes = 0;
  std::uint16_t payload_block_count = 0;
  bool manifest_complete = false;
  std::array<std::uint8_t, kSoundPayloadBlockCount / 8U>
      payload_complete_bitmap{};
  SoundSha256Digest bundle_sha256{};
};

enum class SoundTransactionState : std::uint8_t {
  Active,
  Committed,
  // No active or retained committed record was observed. This is not proof
  // that the transaction was never committed: an older bank may already have
  // been reclaimed.
  Unknown,
};

struct SoundTransactionOutcome {
  SoundTransactionState state = SoundTransactionState::Unknown;
  SoundUpdateIdentity identity{};
  std::uint32_t manifest_bytes = 0;
  std::uint32_t payload_bytes = 0;
  SoundSha256Digest bundle_sha256{};
};

struct SoundReadLease {
  bool valid = false;
  std::uint64_t lease_id = 0;
  SoundBankId bank = SoundBankId::A;
  std::uint64_t generation = 0;
  SoundSha256Digest bundle_sha256{};
};

SoundStoreResult validate_sound_bank(SoundBankStorage& storage,
                                     SoundBankId bank,
                                     SoundBankSnapshot* snapshot);
SoundStoreSelection select_sound_banks(const SoundBankSnapshot& bank_a,
                                       const SoundBankSnapshot& bank_b);

class SoundAssetStore {
 public:
  // Single-owner state machine: callers must serialize every method on the
  // resource supervisor. A worker may carry an immutable SoundReadLease while
  // reading audio, but acquisition/release and all update calls are marshalled
  // back to that owner; the store deliberately has no hidden task or lock.
  explicit SoundAssetStore(SoundBankStorage& storage);

  SoundStoreResult scan();
  const SoundStoreSelection& selection() const;

  // Idempotent BEGIN entry point. An exact in-memory retry performs no storage
  // I/O; after a restart, an exact durable staging header is resumed without
  // erasing or rewriting it. A non-empty corrupt or foreign staging header is
  // never erased implicitly.
  SoundStoreResult begin_or_resume_update(
      const SoundBundlePlan& plan,
      SoundUpdateIdentity* identity);
  SoundStoreResult resume_update(
      const std::array<std::uint8_t, kSoundTransactionIdBytes>& transaction_id,
      SoundUpdateIdentity* identity);
  SoundStoreResult query_transaction_outcome(
      const std::array<std::uint8_t, kSoundTransactionIdBytes>& transaction_id,
      SoundTransactionOutcome* outcome);
  // Returns the current committed active snapshot. Absence is a successful
  // query with snapshot.valid == false; split-brain and storage failures remain
  // explicit errors. This is the durable reconciliation source for the App.
  SoundStoreResult query_current_active(
      SoundBankSnapshot* snapshot);
  // Explicit repair operation for a torn/corrupt staging header. The complete
  // plan determines the one inactive target that may be inspected. Valid
  // staging is never discarded, and no repair is attempted while the target
  // is pinned or another update is active.
  SoundStoreResult discard_invalid_staging(
      const SoundBundlePlan& plan);
  SoundStoreResult write_manifest(const std::uint8_t* manifest,
                                  std::size_t length);
  SoundStoreResult write_payload_block(std::size_t block_index,
                                       const std::uint8_t* data,
                                       std::size_t length);
  SoundStoreResult commit_update();
  SoundStoreResult abort_update();

  SoundStoreResult acquire_active_read(SoundReadLease* lease);
  SoundStoreResult release_read(const SoundReadLease& lease);
  bool update_active() const;
  SoundBankId update_bank() const;
  std::uint64_t update_generation() const;
  // RAM-only snapshot of progress already revalidated from Flash by
  // begin/resume/write. This never performs storage I/O.
  SoundStoreResult update_progress(SoundUpdateProgress* progress) const;

 private:
  // Destructive primitive used only after begin_or_resume_update has proved
  // that normal generation reclamation is safe. Keeping it private prevents a
  // future runtime caller from bypassing torn-staging protection.
  SoundStoreResult begin_new_update(
      const SoundBundlePlan& plan,
      SoundUpdateIdentity* identity);
  SoundStoreResult load_staging_header(
      SoundBankId bank,
      const std::array<std::uint8_t, kSoundTransactionIdBytes>& transaction_id);
  SoundStoreResult verify_manifest();
  SoundStoreResult verify_payload_block(std::size_t block_index,
                                        bool* complete);
  SoundStoreResult verify_all_payload_blocks();
  SoundStoreResult mark_manifest_complete();
  SoundStoreResult mark_payload_complete(std::size_t block_index);
  SoundStoreResult clear_update_state();
  SoundStoreResult refresh_selection_after_commit();
  // Read-only cold-start selection. Commit records are arbitrated first and
  // the selected candidate's exact manifest is authenticated before a lease
  // is exposed. Payload structure remains checked by the streaming reader.
  // No destructive/update/query path may consume this read-only selection:
  // those paths continue to require scan() and its strict full-bank checks.
  SoundStoreResult scan_committed_content();
  bool target_is_pinned(SoundBankId bank) const;

  SoundBankStorage& storage_;
  // Authoritative selection created only by strict full-bank validation.
  SoundStoreSelection selection_{};
  bool scanned_ = false;
  // Independent immutable-read selection. Keeping it separate prevents a
  // fast boot decision from becoming authority for any erase or update.
  SoundStoreSelection read_selection_{};
  bool read_selection_scanned_ = false;
  // Preserve the exact read-only disposition as well as the selected bank.
  // In particular, an erased factory device has no active bank, but that is
  // semantically different from corruption or ordinary unavailability.
  SoundStoreResult read_selection_result_ =
      SoundStoreResult::Unavailable;
  bool update_active_ = false;
  SoundBankId update_bank_ = SoundBankId::A;
  std::uint64_t update_generation_ = 0;
  SoundBundlePlan update_plan_{};
  std::array<std::uint8_t, kSoundTransactionIdBytes>
      update_transaction_id_{};
  std::array<bool, kSoundPayloadBlockCount> payload_complete_{};
  bool manifest_complete_ = false;
  std::array<SoundReadLease, kSoundMaximumReadLeases> read_leases_{};
  std::uint64_t next_read_lease_id_ = 1U;
};

}  // namespace easy_input::speaker_assets
