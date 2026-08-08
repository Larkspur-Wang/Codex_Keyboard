#include "speaker_assets/speaker_assets_flash_runner.h"

#include <algorithm>
#include <limits>

namespace easy_input::speaker_assets {
namespace {

bool bank_is_valid(SoundBankId bank) {
  return bank == SoundBankId::A || bank == SoundBankId::B;
}

bool cpu_kind(SoundStorageWorkKind kind) {
  return kind == SoundStorageWorkKind::Crc32 ||
         kind == SoundStorageWorkKind::Sha256;
}

constexpr std::uint64_t kPriorityAllowedMask = 1U;
constexpr std::uint64_t kPriorityClaimedMask = 2U;
constexpr unsigned kPriorityEpochShift = 2U;
constexpr std::uint64_t kPriorityMaximumEpoch =
    std::numeric_limits<std::uint64_t>::max() >>
    kPriorityEpochShift;

std::uint64_t priority_epoch_from_state(std::uint64_t state) {
  return state >> kPriorityEpochShift;
}

class ExchangeLock final {
 public:
  explicit ExchangeLock(
      SpeakerAssetsFlashRunnerSynchronization& synchronization)
      : synchronization_(synchronization) {
    synchronization_.lock();
  }
  ~ExchangeLock() {
    synchronization_.unlock();
  }
  ExchangeLock(const ExchangeLock&) = delete;
  ExchangeLock& operator=(const ExchangeLock&) = delete;
  ExchangeLock(ExchangeLock&&) = delete;
  ExchangeLock& operator=(ExchangeLock&&) = delete;

 private:
  SpeakerAssetsFlashRunnerSynchronization& synchronization_;
};

bool update_identity_equal(const SoundUpdateIdentity& first,
                           const SoundUpdateIdentity& second) {
  return first.generation == second.generation &&
         first.target_bank == second.target_bank &&
         first.transaction_id == second.transaction_id;
}

}  // namespace

bool speaker_assets_flash_permit_equal(
    const SpeakerAssetsFlashPermit& first,
    const SpeakerAssetsFlashPermit& second) {
  return first.job_id == second.job_id &&
         first.unit_sequence == second.unit_sequence &&
         first.kind == second.kind &&
         first.bank == second.bank &&
         first.offset == second.offset &&
         first.bytes == second.bytes;
}

SpeakerAssetsChunkedSoundBankStorage::
    SpeakerAssetsChunkedSoundBankStorage(
        SoundBankStorage& backend,
        SpeakerAssetsFlashPermitGate& permit_gate)
    : backend_(backend), permit_gate_(permit_gate) {}

bool SpeakerAssetsChunkedSoundBankStorage::range_is_valid(
    SoundBankId bank,
    std::uint32_t offset,
    std::size_t length) const {
  return bank_is_valid(bank) &&
         offset <= kSoundBankSize &&
         length <=
             static_cast<std::size_t>(kSoundBankSize - offset);
}

SoundStorageIoResult SpeakerAssetsChunkedSoundBankStorage::read(
    SoundBankId bank,
    std::uint32_t offset,
    std::uint8_t* destination,
    std::size_t length) {
  if (!bank_is_valid(bank) ||
      (destination == nullptr && length != 0U)) {
    return SoundStorageIoResult::InvalidArgument;
  }
  if (!range_is_valid(bank, offset, length)) {
    return SoundStorageIoResult::OutOfBounds;
  }

  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto amount = std::min(
        kSpeakerAssetsFlashReadWriteUnitBytes,
        length - consumed);
    const auto current = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(offset) + consumed);
    if (!permit_gate_.worker_claim_permit(
            SoundStorageWorkKind::Read,
            bank,
            current,
            amount)) {
      return SoundStorageIoResult::Unavailable;
    }
    const auto result = backend_.read(
        bank, current, destination + consumed, amount);
    permit_gate_.worker_complete_permitted_unit();
    if (result != SoundStorageIoResult::Ok) {
      return result;
    }
    consumed += amount;
  }
  return SoundStorageIoResult::Ok;
}

SoundStorageIoResult SpeakerAssetsChunkedSoundBankStorage::write(
    SoundBankId bank,
    std::uint32_t offset,
    const std::uint8_t* source,
    std::size_t length) {
  if (!bank_is_valid(bank) ||
      (source == nullptr && length != 0U)) {
    return SoundStorageIoResult::InvalidArgument;
  }
  if (!range_is_valid(bank, offset, length)) {
    return SoundStorageIoResult::OutOfBounds;
  }

  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto amount = std::min(
        kSpeakerAssetsFlashReadWriteUnitBytes,
        length - consumed);
    const auto current = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(offset) + consumed);
    if (!permit_gate_.worker_claim_permit(
            SoundStorageWorkKind::Write,
            bank,
            current,
            amount)) {
      return SoundStorageIoResult::Unavailable;
    }
    const auto result = backend_.write(
        bank, current, source + consumed, amount);
    permit_gate_.worker_complete_permitted_unit();
    if (result != SoundStorageIoResult::Ok) {
      return result;
    }
    consumed += amount;
  }
  return SoundStorageIoResult::Ok;
}

SoundStorageIoResult SpeakerAssetsChunkedSoundBankStorage::erase(
    SoundBankId bank,
    std::uint32_t offset,
    std::size_t length) {
  if (!bank_is_valid(bank)) {
    return SoundStorageIoResult::InvalidArgument;
  }
  if ((offset % kSpeakerAssetsFlashEraseUnitBytes) != 0U ||
      (length % kSpeakerAssetsFlashEraseUnitBytes) != 0U) {
    return SoundStorageIoResult::NotAligned;
  }
  if (!range_is_valid(bank, offset, length)) {
    return SoundStorageIoResult::OutOfBounds;
  }

  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto current = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(offset) + consumed);
    if (!permit_gate_.worker_claim_permit(
            SoundStorageWorkKind::Erase,
            bank,
            current,
            kSpeakerAssetsFlashEraseUnitBytes)) {
      return SoundStorageIoResult::Unavailable;
    }
    const auto result = backend_.erase(
        bank, current, kSpeakerAssetsFlashEraseUnitBytes);
    permit_gate_.worker_complete_permitted_unit();
    if (result != SoundStorageIoResult::Ok) {
      return result;
    }
    consumed += kSpeakerAssetsFlashEraseUnitBytes;
  }
  return SoundStorageIoResult::Ok;
}

SoundStorageIoResult
SpeakerAssetsChunkedSoundBankStorage::checkpoint(
    SoundStorageWorkKind kind,
    SoundBankId bank,
    std::uint32_t offset,
    std::size_t length) {
  if (!cpu_kind(kind) || !bank_is_valid(bank) ||
      length > kSpeakerAssetsFlashReadWriteUnitBytes) {
    return SoundStorageIoResult::InvalidArgument;
  }
  if (!range_is_valid(bank, offset, length)) {
    return SoundStorageIoResult::OutOfBounds;
  }
  if (length == 0U) {
    return SoundStorageIoResult::Ok;
  }
  if (cpu_checkpoint_active_ ||
      !permit_gate_.worker_claim_permit(
          kind, bank, offset, length)) {
    return SoundStorageIoResult::Unavailable;
  }
  cpu_checkpoint_active_ = true;
  return SoundStorageIoResult::Ok;
}

void SpeakerAssetsChunkedSoundBankStorage::checkpoint_complete() {
  if (cpu_checkpoint_active_) {
    cpu_checkpoint_active_ = false;
    permit_gate_.worker_complete_permitted_unit();
  }
}

SpeakerAssetsCooperativeStoreRunner::
    SpeakerAssetsCooperativeStoreRunner(
        SoundBankStorage& backend,
        SpeakerAssetsFlashRunnerSynchronization& synchronization)
    : synchronization_(synchronization),
      storage_(backend, *this),
      store_(storage_) {}

void SpeakerAssetsCooperativeStoreRunner::
    publish_priority_allowed(bool allowed) {
  auto state = priority_state_.load(std::memory_order_acquire);
  for (;;) {
    const bool previous =
        (state & kPriorityAllowedMask) != 0U;
    if (previous == allowed) {
      return;
    }
    const auto epoch = priority_epoch_from_state(state);
    std::uint64_t desired =
        state & kPriorityClaimedMask;
    if (epoch == kPriorityMaximumEpoch) {
      desired |= kPriorityMaximumEpoch << kPriorityEpochShift;
    } else {
      desired |= (epoch + 1U) << kPriorityEpochShift;
      if (allowed) {
        desired |= kPriorityAllowedMask;
      }
    }
    if (priority_state_.compare_exchange_weak(
            state,
            desired,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      break;
    }
  }
  synchronization_.notify_worker();
}

bool SpeakerAssetsCooperativeStoreRunner::priority_allowed() const {
  return (priority_state_.load(std::memory_order_acquire) &
          kPriorityAllowedMask) != 0U;
}

std::uint64_t
SpeakerAssetsCooperativeStoreRunner::priority_epoch() const {
  return priority_epoch_from_state(
      priority_state_.load(std::memory_order_acquire));
}

std::uint32_t SpeakerAssetsCooperativeStoreRunner::
    completed_unit_generation() const {
  return completed_unit_generation_.load(
      std::memory_order_acquire);
}

bool SpeakerAssetsCooperativeStoreRunner::action_is_valid(
    const SpeakerAssetsActionView& action) const {
  if (action.token == 0U || action.request_id == 0U) {
    return false;
  }
  switch (action.kind) {
    case SpeakerAssetsActionKind::Begin:
    case SpeakerAssetsActionKind::DiscardInvalidStaging:
    case SpeakerAssetsActionKind::QueryCurrentActive:
    case SpeakerAssetsActionKind::ResumeQuery:
    case SpeakerAssetsActionKind::WriteManifest:
    case SpeakerAssetsActionKind::WritePayloadBlock:
    case SpeakerAssetsActionKind::Commit:
    case SpeakerAssetsActionKind::Abort:
      return true;
  }
  return false;
}

bool SpeakerAssetsCooperativeStoreRunner::action_matches(
    const SpeakerAssetsActionView& action) const {
  return action.token == action_.token &&
         action.kind == action_.kind &&
         speaker_assets_route_equal(action.route, action_.route) &&
         action.request_id == action_.request_id &&
         action.session_cookie == action_.session_cookie &&
         action.plan == action_.plan &&
         update_identity_equal(
             action.expected_identity,
             action_.expected_identity) &&
         action.query_mode == action_.query_mode &&
         action.bytes == action_.bytes &&
         action.length == action_.length &&
         action.block_index == action_.block_index;
}

bool SpeakerAssetsCooperativeStoreRunner::next_job_id(
    std::uint64_t* job_id) {
  if (job_id == nullptr || next_job_id_ == 0U) {
    return false;
  }
  *job_id = next_job_id_;
  if (next_job_id_ ==
      std::numeric_limits<std::uint64_t>::max()) {
    next_job_id_ = 0U;
  } else {
    ++next_job_id_;
  }
  return true;
}

bool SpeakerAssetsCooperativeStoreRunner::next_unit_sequence(
    std::uint32_t* unit_sequence) {
  if (unit_sequence == nullptr || next_unit_sequence_ == 0U) {
    return false;
  }
  *unit_sequence = next_unit_sequence_;
  if (next_unit_sequence_ ==
      std::numeric_limits<std::uint32_t>::max()) {
    next_unit_sequence_ = 0U;
  } else {
    ++next_unit_sequence_;
  }
  return true;
}

bool SpeakerAssetsCooperativeStoreRunner::exact_priority(
    std::uint64_t epoch) const {
  const auto state =
      priority_state_.load(std::memory_order_acquire);
  return (state & kPriorityAllowedMask) != 0U &&
         (state & kPriorityClaimedMask) == 0U &&
         priority_epoch_from_state(state) == epoch;
}

bool SpeakerAssetsCooperativeStoreRunner::claim_priority_unit(
    std::uint64_t epoch) {
  auto state = priority_state_.load(std::memory_order_acquire);
  for (;;) {
    if ((state & kPriorityAllowedMask) == 0U ||
        (state & kPriorityClaimedMask) != 0U ||
        priority_epoch_from_state(state) != epoch) {
      return false;
    }
    if (priority_state_.compare_exchange_weak(
            state,
            state | kPriorityClaimedMask,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
}

void SpeakerAssetsCooperativeStoreRunner::release_priority_unit() {
  priority_state_.fetch_and(
      ~kPriorityClaimedMask, std::memory_order_release);
  synchronization_.notify_supervisor();
}

bool SpeakerAssetsCooperativeStoreRunner::
    grant_requested_permit_locked(bool* notify_worker) {
  if (notify_worker == nullptr ||
      phase_ != Phase::PermitRequested ||
      !priority_allowed()) {
    return false;
  }
  granted_permit_ = requested_permit_;
  granted_priority_epoch_ = priority_epoch();
  phase_ = Phase::PermitGranted;
  *notify_worker = true;
  return true;
}

void SpeakerAssetsCooperativeStoreRunner::
    reset_exchange_locked() {
  phase_ = Phase::Idle;
  active_job_kind_ = ActiveJobKind::None;
  action_ = {};
  completion_ = {};
  prepare_boot_completion_ = {};
  release_read_request_ = {};
  release_read_completion_ =
      SoundStoreResult::InvalidArgument;
  requested_permit_ = {};
  granted_permit_ = {};
  requested_priority_epoch_ = 0U;
  granted_priority_epoch_ = 0U;
  active_job_id_ = 0U;
}

bool SpeakerAssetsCooperativeStoreRunner::
    internal_handle_matches_locked(
        const SpeakerAssetsInternalJobHandle& handle,
        ActiveJobKind expected_kind) const {
  if (handle.job_id == 0U ||
      active_job_id_ != handle.job_id ||
      active_job_kind_ != expected_kind) {
    return false;
  }
  switch (expected_kind) {
    case ActiveJobKind::PrepareBootRead:
      return handle.kind ==
             SpeakerAssetsInternalJobKind::PrepareBootRead;
    case ActiveJobKind::ReleaseRead:
      return handle.kind ==
             SpeakerAssetsInternalJobKind::ReleaseRead;
    case ActiveJobKind::None:
    case ActiveJobKind::ProtocolAction:
      return false;
  }
  return false;
}

SpeakerAssetsActionExecutionResult
SpeakerAssetsCooperativeStoreRunner::step(
    const SpeakerAssetsActionView& action,
    SpeakerAssetsActionCompletion* completion) {
  bool notify_worker = false;
  SpeakerAssetsActionExecutionResult result =
      SpeakerAssetsActionExecutionResult::Pending;
  {
    ExchangeLock lock(synchronization_);
    // A protocol poll is never allowed to reject, reset, grant or consume an
    // internal job. This check intentionally precedes argument validation.
    if (phase_ != Phase::Idle &&
        active_job_kind_ != ActiveJobKind::ProtocolAction) {
      return SpeakerAssetsActionExecutionResult::Pending;
    }
    if (completion == nullptr || !action_is_valid(action)) {
      return SpeakerAssetsActionExecutionResult::Rejected;
    }
    if (phase_ == Phase::Idle) {
      std::uint64_t job_id = 0U;
      if (!next_job_id(&job_id)) {
        return SpeakerAssetsActionExecutionResult::Rejected;
      }
      active_job_kind_ = ActiveJobKind::ProtocolAction;
      action_ = action;
      completion_ = {};
      prepare_boot_completion_ = {};
      release_read_request_ = {};
      release_read_completion_ =
          SoundStoreResult::InvalidArgument;
      requested_permit_ = {};
      granted_permit_ = {};
      requested_priority_epoch_ = 0U;
      granted_priority_epoch_ = 0U;
      active_job_id_ = job_id;
      phase_ = Phase::Accepted;
      notify_worker = true;
    } else if (!action_matches(action)) {
      // Rejected is reserved for a side-effect-free action. Once one borrowed
      // action has been accepted, never let a mismatched poll unwind it while
      // the Store owner may still be mutating Flash.
      result = SpeakerAssetsActionExecutionResult::Pending;
    } else if (phase_ == Phase::Completed) {
      *completion = completion_;
      reset_exchange_locked();
      result = SpeakerAssetsActionExecutionResult::Completed;
    } else {
      static_cast<void>(
          grant_requested_permit_locked(&notify_worker));
    }
  }
  if (notify_worker) {
    synchronization_.notify_worker();
  }
  return result;
}

SpeakerAssetsInternalJobStartResult
SpeakerAssetsCooperativeStoreRunner::start_prepare_boot_read(
    SpeakerAssetsInternalJobHandle* handle) {
  if (handle == nullptr) {
    return SpeakerAssetsInternalJobStartResult::InvalidArgument;
  }
  *handle = {};
  bool notify_worker = false;
  SpeakerAssetsInternalJobStartResult result =
      SpeakerAssetsInternalJobStartResult::Busy;
  {
    ExchangeLock lock(synchronization_);
    if (phase_ == Phase::Idle) {
      std::uint64_t job_id = 0U;
      if (!next_job_id(&job_id)) {
        return SpeakerAssetsInternalJobStartResult::InvalidArgument;
      }
      active_job_kind_ = ActiveJobKind::PrepareBootRead;
      action_ = {};
      completion_ = {};
      prepare_boot_completion_ = {};
      release_read_request_ = {};
      release_read_completion_ =
          SoundStoreResult::InvalidArgument;
      requested_permit_ = {};
      granted_permit_ = {};
      requested_priority_epoch_ = 0U;
      granted_priority_epoch_ = 0U;
      active_job_id_ = job_id;
      phase_ = Phase::Accepted;
      handle->job_id = job_id;
      handle->kind =
          SpeakerAssetsInternalJobKind::PrepareBootRead;
      notify_worker = true;
      result = SpeakerAssetsInternalJobStartResult::Accepted;
    }
  }
  if (notify_worker) {
    synchronization_.notify_worker();
  }
  return result;
}

SpeakerAssetsInternalJobPollResult
SpeakerAssetsCooperativeStoreRunner::poll_prepare_boot_read(
    const SpeakerAssetsInternalJobHandle& handle,
    SpeakerAssetsPrepareBootReadCompletion* completion) {
  if (completion == nullptr || handle.job_id == 0U ||
      handle.kind !=
          SpeakerAssetsInternalJobKind::PrepareBootRead) {
    return SpeakerAssetsInternalJobPollResult::InvalidArgument;
  }

  bool notify_worker = false;
  SpeakerAssetsInternalJobPollResult result =
      SpeakerAssetsInternalJobPollResult::Pending;
  {
    ExchangeLock lock(synchronization_);
    if (!internal_handle_matches_locked(
            handle, ActiveJobKind::PrepareBootRead)) {
      result = SpeakerAssetsInternalJobPollResult::Mismatched;
    } else if (phase_ == Phase::Completed) {
      *completion = prepare_boot_completion_;
      reset_exchange_locked();
      result = SpeakerAssetsInternalJobPollResult::Completed;
    } else {
      static_cast<void>(
          grant_requested_permit_locked(&notify_worker));
    }
  }
  if (notify_worker) {
    synchronization_.notify_worker();
  }
  return result;
}

SpeakerAssetsInternalJobStartResult
SpeakerAssetsCooperativeStoreRunner::start_release_read(
    const SoundReadLease& lease,
    SpeakerAssetsInternalJobHandle* handle) {
  if (handle == nullptr) {
    return SpeakerAssetsInternalJobStartResult::InvalidArgument;
  }
  *handle = {};
  bool notify_worker = false;
  SpeakerAssetsInternalJobStartResult result =
      SpeakerAssetsInternalJobStartResult::Busy;
  {
    ExchangeLock lock(synchronization_);
    if (phase_ == Phase::Idle) {
      std::uint64_t job_id = 0U;
      if (!next_job_id(&job_id)) {
        return SpeakerAssetsInternalJobStartResult::InvalidArgument;
      }
      active_job_kind_ = ActiveJobKind::ReleaseRead;
      action_ = {};
      completion_ = {};
      prepare_boot_completion_ = {};
      release_read_request_ = lease;
      release_read_completion_ =
          SoundStoreResult::InvalidArgument;
      requested_permit_ = {};
      granted_permit_ = {};
      requested_priority_epoch_ = 0U;
      granted_priority_epoch_ = 0U;
      active_job_id_ = job_id;
      phase_ = Phase::Accepted;
      handle->job_id = job_id;
      handle->kind =
          SpeakerAssetsInternalJobKind::ReleaseRead;
      notify_worker = true;
      result = SpeakerAssetsInternalJobStartResult::Accepted;
    }
  }
  if (notify_worker) {
    synchronization_.notify_worker();
  }
  return result;
}

SpeakerAssetsInternalJobPollResult
SpeakerAssetsCooperativeStoreRunner::poll_release_read(
    const SpeakerAssetsInternalJobHandle& handle,
    SoundStoreResult* completion) {
  if (completion == nullptr || handle.job_id == 0U ||
      handle.kind != SpeakerAssetsInternalJobKind::ReleaseRead) {
    return SpeakerAssetsInternalJobPollResult::InvalidArgument;
  }

  bool notify_worker = false;
  SpeakerAssetsInternalJobPollResult result =
      SpeakerAssetsInternalJobPollResult::Pending;
  {
    ExchangeLock lock(synchronization_);
    if (!internal_handle_matches_locked(
            handle, ActiveJobKind::ReleaseRead)) {
      result = SpeakerAssetsInternalJobPollResult::Mismatched;
    } else if (phase_ == Phase::Completed) {
      *completion = release_read_completion_;
      reset_exchange_locked();
      result = SpeakerAssetsInternalJobPollResult::Completed;
    } else {
      static_cast<void>(
          grant_requested_permit_locked(&notify_worker));
    }
  }
  if (notify_worker) {
    synchronization_.notify_worker();
  }
  return result;
}

SpeakerAssetsFlashWorkerResult
SpeakerAssetsCooperativeStoreRunner::worker_run_once() {
  SpeakerAssetsActionView action{};
  SoundReadLease release_request{};
  ActiveJobKind active_job_kind = ActiveJobKind::None;
  std::uint64_t job_id = 0U;
  {
    ExchangeLock lock(synchronization_);
    if (phase_ != Phase::Accepted) {
      return SpeakerAssetsFlashWorkerResult::NoJob;
    }
    phase_ = Phase::Running;
    active_job_kind = active_job_kind_;
    action = action_;
    release_request = release_read_request_;
    job_id = active_job_id_;
    worker_job_id_ = job_id;
    next_unit_sequence_ = 1U;
  }

  SpeakerAssetsActionCompletion completion{};
  SpeakerAssetsPrepareBootReadCompletion
      prepare_boot_completion{};
  SoundStoreResult release_read_completion =
      SoundStoreResult::InvalidArgument;
  switch (active_job_kind) {
    case ActiveJobKind::ProtocolAction:
      if (!execute_speaker_assets_store_action(
              store_, action, &completion)) {
        completion = {};
        completion.token = action.token;
        completion.kind = action.kind;
        completion.result = SoundStoreResult::InvalidArgument;
      }
      break;
    case ActiveJobKind::PrepareBootRead: {
      SoundReadLease lease{};
      prepare_boot_completion.acquire_result =
          store_.acquire_active_read(&lease);
      if (prepare_boot_completion.acquire_result ==
          SoundStoreResult::Ok) {
        SoundResolvedAsset asset{};
        prepare_boot_completion.resolve_result =
            resolve_sound_asset(
                storage_,
                lease,
                SoundAssetTrigger::Boot,
                0U,
                &asset);
        if (prepare_boot_completion.resolve_result ==
            SoundAssetReadResult::Ok) {
          prepare_boot_completion.prepared = true;
          prepare_boot_completion.lease = lease;
          prepare_boot_completion.asset = asset;
        } else {
          prepare_boot_completion.cleanup_result =
              store_.release_read(lease);
        }
      }
      break;
    }
    case ActiveJobKind::ReleaseRead:
      release_read_completion =
          store_.release_read(release_request);
      break;
    case ActiveJobKind::None:
      break;
  }

  {
    ExchangeLock lock(synchronization_);
    if (active_job_id_ == job_id &&
        active_job_kind_ == active_job_kind &&
        phase_ == Phase::Running) {
      switch (active_job_kind) {
        case ActiveJobKind::ProtocolAction:
          completion_ = completion;
          break;
        case ActiveJobKind::PrepareBootRead:
          prepare_boot_completion_ =
              prepare_boot_completion;
          break;
        case ActiveJobKind::ReleaseRead:
          release_read_completion_ =
              release_read_completion;
          break;
        case ActiveJobKind::None:
          break;
      }
      phase_ = Phase::Completed;
    }
    worker_job_id_ = 0U;
    next_unit_sequence_ = 1U;
  }
  synchronization_.notify_supervisor();
  return SpeakerAssetsFlashWorkerResult::Completed;
}

bool SpeakerAssetsCooperativeStoreRunner::worker_claim_permit(
    SoundStorageWorkKind kind,
    SoundBankId bank,
    std::uint32_t offset,
    std::size_t bytes) {
  if (worker_job_id_ == 0U || !bank_is_valid(bank) ||
      bytes == 0U ||
      bytes > kSpeakerAssetsFlashEraseUnitBytes ||
      (kind == SoundStorageWorkKind::Erase &&
       bytes != kSpeakerAssetsFlashEraseUnitBytes) ||
      ((kind == SoundStorageWorkKind::Read ||
        kind == SoundStorageWorkKind::Write ||
        cpu_kind(kind)) &&
       bytes > kSpeakerAssetsFlashReadWriteUnitBytes)) {
    return false;
  }

  for (;;) {
    const auto request_epoch = priority_epoch();
    if (!exact_priority(request_epoch)) {
      synchronization_.wait_worker();
      continue;
    }

    std::uint32_t unit_sequence = 0U;
    if (!next_unit_sequence(&unit_sequence)) {
      return false;
    }
    SpeakerAssetsFlashPermit permit{};
    permit.job_id = worker_job_id_;
    permit.unit_sequence = unit_sequence;
    permit.kind = kind;
    permit.bank = bank;
    permit.offset = offset;
    permit.bytes = static_cast<std::uint16_t>(bytes);
    {
      ExchangeLock lock(synchronization_);
      if (phase_ != Phase::Running ||
          active_job_id_ != worker_job_id_) {
        return false;
      }
      requested_permit_ = permit;
      requested_priority_epoch_ = request_epoch;
      granted_permit_ = {};
      granted_priority_epoch_ = 0U;
      phase_ = Phase::PermitRequested;
    }
    synchronization_.notify_supervisor();

    std::uint64_t grant_epoch = 0U;
    for (;;) {
      bool granted = false;
      {
        ExchangeLock lock(synchronization_);
        if (phase_ == Phase::PermitGranted &&
            active_job_id_ == worker_job_id_ &&
            speaker_assets_flash_permit_equal(
                granted_permit_, permit)) {
          grant_epoch = granted_priority_epoch_;
          phase_ = Phase::Running;
          granted = true;
        } else if (
            active_job_id_ != worker_job_id_ ||
            (phase_ != Phase::PermitRequested &&
             phase_ != Phase::PermitGranted)) {
          return false;
        }
      }
      if (granted) {
        break;
      }
      synchronization_.wait_worker();
    }

    // First check rejects a grant issued from a different priority epoch.
    // The immediate second check closes the window between consuming the
    // exact grant and entering the backend/<=256-byte CPU unit.
    if (grant_epoch != requested_priority_epoch_ ||
        !exact_priority(grant_epoch) ||
        !exact_priority(grant_epoch) ||
        !claim_priority_unit(grant_epoch)) {
      continue;
    }
    return true;
  }
}

void SpeakerAssetsCooperativeStoreRunner::
    worker_complete_permitted_unit() {
  completed_unit_generation_.fetch_add(
      1U, std::memory_order_release);
  release_priority_unit();
}

bool SpeakerAssetsCooperativeStoreRunner::requested_permit(
    SpeakerAssetsFlashPermit* permit) const {
  if (permit == nullptr) {
    return false;
  }
  ExchangeLock lock(synchronization_);
  if (phase_ != Phase::PermitRequested) {
    return false;
  }
  *permit = requested_permit_;
  return true;
}

bool SpeakerAssetsCooperativeStoreRunner::job_active() const {
  ExchangeLock lock(synchronization_);
  return phase_ != Phase::Idle;
}

}  // namespace easy_input::speaker_assets
