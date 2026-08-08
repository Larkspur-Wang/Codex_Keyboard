#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>

#include "speaker_assets/sound_asset_crypto.h"
#include "speaker_assets/sound_asset_format.h"
#include "speaker_assets/speaker_assets_flash_runner.h"

namespace {

using easy_input::speaker_assets::SoundBankId;
using easy_input::speaker_assets::SoundBankStorage;
using easy_input::speaker_assets::SoundAssetReadResult;
using easy_input::speaker_assets::SoundBundlePlan;
using easy_input::speaker_assets::SoundFormatResult;
using easy_input::speaker_assets::SoundReadLease;
using easy_input::speaker_assets::SoundResolvedAsset;
using easy_input::speaker_assets::SoundSha256;
using easy_input::speaker_assets::SoundSha256Digest;
using easy_input::speaker_assets::SoundStorageIoResult;
using easy_input::speaker_assets::SoundStorageWorkKind;
using easy_input::speaker_assets::SoundStoreResult;
using easy_input::speaker_assets::SpeakerAssetsActionCompletion;
using easy_input::speaker_assets::SpeakerAssetsActionExecutionResult;
using easy_input::speaker_assets::SpeakerAssetsActionKind;
using easy_input::speaker_assets::SpeakerAssetsActionView;
using easy_input::speaker_assets::
    SpeakerAssetsChunkedSoundBankStorage;
using easy_input::speaker_assets::
    SpeakerAssetsCooperativeStoreRunner;
using easy_input::speaker_assets::SpeakerAssetsFlashPermit;
using easy_input::speaker_assets::SpeakerAssetsFlashPermitGate;
using easy_input::speaker_assets::
    SpeakerAssetsFlashRunnerSynchronization;
using easy_input::speaker_assets::SpeakerAssetsFlashWorkerResult;
using easy_input::speaker_assets::SpeakerAssetsFrame;
using easy_input::speaker_assets::SpeakerAssetsInternalJobHandle;
using easy_input::speaker_assets::SpeakerAssetsInternalJobPollResult;
using easy_input::speaker_assets::SpeakerAssetsInternalJobStartResult;
using easy_input::speaker_assets::SpeakerAssetsOpcode;
using easy_input::speaker_assets::
    SpeakerAssetsPrepareBootReadCompletion;
using easy_input::speaker_assets::SpeakerAssetsRouteToken;
using easy_input::speaker_assets::SpeakerAssetsRuntimeActionExecutor;
using easy_input::speaker_assets::SpeakerAssetsRuntimeCore;
using easy_input::speaker_assets::SpeakerAssetsRuntimeEnqueueResult;
using easy_input::speaker_assets::SpeakerAssetsRuntimeStepResult;
using easy_input::speaker_assets::SpeakerAssetsTransport;
using easy_input::speaker_assets::encode_speaker_assets_usb_frame;
using easy_input::speaker_assets::calculate_sound_bank_digests;
using easy_input::speaker_assets::kSoundBankSize;
using easy_input::speaker_assets::kSoundCommitOffset;
using easy_input::speaker_assets::kSoundManifestOffset;
using easy_input::speaker_assets::kSoundPayloadBlockSize;
using easy_input::speaker_assets::kSoundPayloadMaxSize;
using easy_input::speaker_assets::kSoundPayloadOffset;
using easy_input::speaker_assets::kSoundSectorSize;
using easy_input::speaker_assets::
    kSpeakerAssetsFlashEraseUnitBytes;
using easy_input::speaker_assets::
    kSpeakerAssetsFlashReadWriteUnitBytes;
using easy_input::speaker_assets::kSpeakerAssetsUsbFrameBytes;
using easy_input::speaker_assets::sound_crc32_iso_hdlc;

static_assert(!std::is_copy_constructible_v<
              SpeakerAssetsChunkedSoundBankStorage>);
static_assert(!std::is_move_constructible_v<
              SpeakerAssetsChunkedSoundBankStorage>);
static_assert(!std::is_copy_constructible_v<
              SpeakerAssetsCooperativeStoreRunner>);
static_assert(!std::is_move_constructible_v<
              SpeakerAssetsCooperativeStoreRunner>);
static_assert(std::is_trivially_copyable_v<
              SpeakerAssetsInternalJobHandle>);
static_assert(std::is_trivially_copyable_v<
              SpeakerAssetsPrepareBootReadCompletion>);
static_assert(sizeof(SpeakerAssetsPrepareBootReadCompletion) <= 256U);

std::size_t bank_index(SoundBankId bank) {
  return bank == SoundBankId::A ? 0U : 1U;
}

struct PhysicalOperation {
  SoundStorageWorkKind kind = SoundStorageWorkKind::Read;
  SoundBankId bank = SoundBankId::A;
  std::uint32_t offset = 0U;
  std::size_t bytes = 0U;
};

class RecordingStorage final : public SoundBankStorage {
 public:
  RecordingStorage() {
    for (auto& bank : banks_) {
      bank.assign(kSoundBankSize, 0xFFU);
    }
  }

  SoundStorageIoResult read(SoundBankId bank,
                            std::uint32_t offset,
                            std::uint8_t* destination,
                            std::size_t length) override {
    operations.push_back(
        {SoundStorageWorkKind::Read, bank, offset, length});
    if (length > kSpeakerAssetsFlashReadWriteUnitBytes) {
      return SoundStorageIoResult::InvalidArgument;
    }
    const auto* source = range(bank, offset, length);
    if (source == nullptr ||
        (destination == nullptr && length != 0U)) {
      return SoundStorageIoResult::OutOfBounds;
    }
    std::copy(source, source + length, destination);
    return SoundStorageIoResult::Ok;
  }

  SoundStorageIoResult write(SoundBankId bank,
                             std::uint32_t offset,
                             const std::uint8_t* source,
                             std::size_t length) override {
    operations.push_back(
        {SoundStorageWorkKind::Write, bank, offset, length});
    if (fail_next_mutation_) {
      fail_next_mutation_ = false;
      return SoundStorageIoResult::IoError;
    }
    if (length > kSpeakerAssetsFlashReadWriteUnitBytes ||
        (source == nullptr && length != 0U)) {
      return SoundStorageIoResult::InvalidArgument;
    }
    auto* destination = mutable_range(bank, offset, length);
    if (destination == nullptr) {
      return SoundStorageIoResult::OutOfBounds;
    }
    for (std::size_t index = 0U; index < length; ++index) {
      if ((destination[index] & source[index]) != source[index]) {
        return SoundStorageIoResult::IoError;
      }
      destination[index] &= source[index];
    }
    return SoundStorageIoResult::Ok;
  }

  SoundStorageIoResult erase(SoundBankId bank,
                             std::uint32_t offset,
                             std::size_t length) override {
    operations.push_back(
        {SoundStorageWorkKind::Erase, bank, offset, length});
    if (fail_next_mutation_) {
      fail_next_mutation_ = false;
      return SoundStorageIoResult::IoError;
    }
    if (length != kSpeakerAssetsFlashEraseUnitBytes ||
        (offset % kSpeakerAssetsFlashEraseUnitBytes) != 0U) {
      return SoundStorageIoResult::NotAligned;
    }
    auto* destination = mutable_range(bank, offset, length);
    if (destination == nullptr) {
      return SoundStorageIoResult::OutOfBounds;
    }
    std::fill(destination, destination + length, 0xFFU);
    return SoundStorageIoResult::Ok;
  }

  void fail_next_mutation() {
    fail_next_mutation_ = true;
  }

  void corrupt_byte(SoundBankId bank,
                    std::uint32_t offset,
                    std::uint8_t mask = 0x01U) {
    auto* byte = mutable_range(bank, offset, 1U);
    assert(byte != nullptr);
    *byte = static_cast<std::uint8_t>(*byte ^ mask);
  }

  std::uint8_t* mutable_bank_for_test(SoundBankId bank) {
    return mutable_range(bank, 0U, kSoundBankSize);
  }

  std::vector<PhysicalOperation> operations;

 private:
  const std::uint8_t* range(SoundBankId bank,
                            std::uint32_t offset,
                            std::size_t length) const {
    if ((bank != SoundBankId::A && bank != SoundBankId::B) ||
        offset > kSoundBankSize ||
        length >
            static_cast<std::size_t>(kSoundBankSize - offset)) {
      return nullptr;
    }
    return banks_[bank_index(bank)].data() + offset;
  }

  std::uint8_t* mutable_range(SoundBankId bank,
                              std::uint32_t offset,
                              std::size_t length) {
    return const_cast<std::uint8_t*>(
        static_cast<const RecordingStorage*>(this)->range(
            bank, offset, length));
  }

  std::array<std::vector<std::uint8_t>, 2> banks_{};
  bool fail_next_mutation_ = false;
};

class RecordingPermitGate final : public SpeakerAssetsFlashPermitGate {
 public:
  bool worker_claim_permit(SoundStorageWorkKind kind,
                           SoundBankId bank,
                           std::uint32_t offset,
                           std::size_t bytes) override {
    assert(!claimed);
    operations.push_back({kind, bank, offset, bytes});
    if (allow) {
      claimed = true;
    }
    return allow;
  }

  void worker_complete_permitted_unit() override {
    assert(claimed);
    claimed = false;
    ++completed_units;
  }

  bool allow = true;
  bool claimed = false;
  std::size_t completed_units = 0U;
  std::vector<PhysicalOperation> operations;
};

class PumpSynchronization final
    : public SpeakerAssetsFlashRunnerSynchronization {
 public:
  void bind(SpeakerAssetsCooperativeStoreRunner* runner,
            const SpeakerAssetsActionView* action) {
    runner_ = runner;
    action_ = action;
    internal_handle_ = {};
    poll_kind_ = PollKind::ProtocolAction;
  }

  void bind_prepare(
      SpeakerAssetsCooperativeStoreRunner* runner,
      const SpeakerAssetsInternalJobHandle& handle) {
    runner_ = runner;
    action_ = nullptr;
    internal_handle_ = handle;
    poll_kind_ = PollKind::PrepareBootRead;
  }

  void bind_release(
      SpeakerAssetsCooperativeStoreRunner* runner,
      const SpeakerAssetsInternalJobHandle& handle) {
    runner_ = runner;
    action_ = nullptr;
    internal_handle_ = handle;
    poll_kind_ = PollKind::ReleaseRead;
  }

  void lock() override {
    mutex_.lock();
  }

  void unlock() override {
    mutex_.unlock();
  }

  void notify_worker() override {
    ++worker_notifications;
  }

  void notify_supervisor() override {
    ++supervisor_notifications;
  }

  void wait_worker() override {
    assert(runner_ != nullptr);
    SpeakerAssetsFlashPermit permit{};
    assert(runner_->requested_permit(&permit));
    permits.push_back(permit);
    switch (poll_kind_) {
      case PollKind::ProtocolAction: {
        assert(action_ != nullptr);
        SpeakerAssetsActionCompletion ignored{};
        assert(runner_->step(*action_, &ignored) ==
               SpeakerAssetsActionExecutionResult::Pending);
        break;
      }
      case PollKind::PrepareBootRead: {
        SpeakerAssetsPrepareBootReadCompletion ignored{};
        assert(runner_->poll_prepare_boot_read(
                   internal_handle_, &ignored) ==
               SpeakerAssetsInternalJobPollResult::Pending);
        break;
      }
      case PollKind::ReleaseRead: {
        SoundStoreResult ignored =
            SoundStoreResult::InvalidArgument;
        assert(runner_->poll_release_read(
                   internal_handle_, &ignored) ==
               SpeakerAssetsInternalJobPollResult::Pending);
        break;
      }
      case PollKind::None:
        assert(false);
        break;
    }
    if (pulse_priority_after_next_grant) {
      pulse_priority_after_next_grant = false;
      runner_->publish_priority_allowed(false);
      runner_->publish_priority_allowed(true);
    }
  }

  enum class PollKind : std::uint8_t {
    None,
    ProtocolAction,
    PrepareBootRead,
    ReleaseRead,
  };

  std::mutex mutex_;
  SpeakerAssetsCooperativeStoreRunner* runner_ = nullptr;
  const SpeakerAssetsActionView* action_ = nullptr;
  SpeakerAssetsInternalJobHandle internal_handle_{};
  PollKind poll_kind_ = PollKind::None;
  std::vector<SpeakerAssetsFlashPermit> permits;
  std::size_t worker_notifications = 0U;
  std::size_t supervisor_notifications = 0U;
  bool pulse_priority_after_next_grant = false;
};

SoundBundlePlan make_plan(
    const std::array<std::uint8_t, kSoundSectorSize>& manifest,
    const std::array<std::uint8_t, kSoundPayloadBlockSize>& payload) {
  SoundBundlePlan plan{};
  plan.manifest_bytes =
      static_cast<std::uint32_t>(manifest.size());
  plan.payload_bytes =
      static_cast<std::uint32_t>(payload.size());
  plan.manifest_crc32 =
      sound_crc32_iso_hdlc(manifest.data(), manifest.size());
  SoundSha256 manifest_sha;
  assert(manifest_sha.update(manifest.data(), manifest.size()));
  plan.manifest_sha256 = manifest_sha.finish();
  plan.payload_block_crc32[0U] =
      sound_crc32_iso_hdlc(payload.data(), payload.size());
  plan.bundle_sha256[0U] = 0xA5U;
  return plan;
}

constexpr std::array<std::uint8_t, 16> kBundleDomain{{
    'E', 'A', 'S', 'Y', 'I', 'N', 'P', 'U',
    'T', '-', 'S', 'N', 'D', '-', 'V', '1',
}};

void write_le16(std::uint8_t* output, std::uint16_t value) {
  output[0U] = static_cast<std::uint8_t>(value);
  output[1U] = static_cast<std::uint8_t>(value >> 8U);
}

void write_le32(std::uint8_t* output, std::uint32_t value) {
  output[0U] = static_cast<std::uint8_t>(value);
  output[1U] = static_cast<std::uint8_t>(value >> 8U);
  output[2U] = static_cast<std::uint8_t>(value >> 16U);
  output[3U] = static_cast<std::uint8_t>(value >> 24U);
}

void write_le64(std::uint8_t* output, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    output[index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

SoundSha256Digest sha256(const std::uint8_t* data,
                         std::size_t length) {
  SoundSha256 hash;
  assert(hash.update(data, length));
  return hash.finish();
}

struct TestBundle {
  std::vector<std::uint8_t> manifest;
  std::vector<std::uint8_t> payload;
  SoundBundlePlan plan{};
};

TestBundle make_boot_bundle(
    bool map_boot,
    std::uint16_t predictor_seed = 1234U,
    std::uint32_t decoded_samples = 481U) {
  assert(decoded_samples != 0U);
  assert(decoded_samples <= 384000U);
  const auto frame_count = static_cast<std::uint16_t>(
      1U + (decoded_samples - 1U) / 480U);
  TestBundle bundle{};
  bundle.payload.assign(20U, 0U);
  auto* header = bundle.payload.data();
  header[0U] = 'E';
  header[1U] = 'I';
  header[2U] = 'A';
  header[3U] = 'D';
  header[4U] = 1U;
  header[5U] = 1U;
  write_le32(header + 6U, 48000U);
  write_le16(header + 10U, 480U);
  write_le16(header + 12U, frame_count);
  write_le32(header + 14U, decoded_samples);
  write_le16(header + 18U, 20U);

  std::uint32_t remaining_samples = decoded_samples;
  for (std::uint16_t frame_index = 0U;
       frame_index < frame_count;
       ++frame_index) {
    const auto samples = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(remaining_samples, 480U));
    const auto frame_offset = bundle.payload.size();
    bundle.payload.resize(
        frame_offset + 6U + samples / 2U, 0U);
    write_le16(bundle.payload.data() + frame_offset, samples);
    write_le16(
        bundle.payload.data() + frame_offset + 2U,
        static_cast<std::uint16_t>(
            predictor_seed + frame_index));
    remaining_samples -= samples;
  }
  assert(remaining_samples == 0U);

  bundle.manifest.assign(32U + 48U + 4U, 0U);
  auto* manifest = bundle.manifest.data();
  manifest[0U] = 'E';
  manifest[1U] = 'I';
  manifest[2U] = 'S';
  manifest[3U] = 'M';
  write_le16(manifest + 4U, 1U);
  write_le16(manifest + 6U, 32U);
  write_le32(
      manifest + 8U,
      static_cast<std::uint32_t>(bundle.manifest.size()));
  write_le32(
      manifest + 12U,
      static_cast<std::uint32_t>(bundle.payload.size()));
  write_le16(manifest + 16U, 1U);
  write_le16(manifest + 18U, 1U);

  const auto resource_digest =
      sha256(bundle.payload.data(), bundle.payload.size());
  std::copy(resource_digest.begin(),
            resource_digest.end(),
            bundle.manifest.begin() + 32U);
  write_le32(manifest + 64U, 0U);
  write_le32(
      manifest + 68U,
      static_cast<std::uint32_t>(bundle.payload.size()));
  write_le32(manifest + 72U, decoded_samples);
  write_le16(manifest + 76U, frame_count);
  manifest[78U] = 1U;
  manifest[79U] = 1U;
  manifest[80U] = map_boot ? 1U : 2U;
  manifest[81U] = map_boot ? 0U : 1U;
  write_le16(manifest + 82U, 0U);

  bundle.plan.manifest_bytes =
      static_cast<std::uint32_t>(bundle.manifest.size());
  bundle.plan.payload_bytes =
      static_cast<std::uint32_t>(bundle.payload.size());
  bundle.plan.manifest_crc32 = sound_crc32_iso_hdlc(
      bundle.manifest.data(), bundle.manifest.size());
  bundle.plan.manifest_sha256 =
      sha256(bundle.manifest.data(), bundle.manifest.size());
  for (std::size_t offset = 0U, block_index = 0U;
       offset < bundle.payload.size();
       offset += kSoundPayloadBlockSize, ++block_index) {
    const auto amount = std::min<std::size_t>(
        kSoundPayloadBlockSize, bundle.payload.size() - offset);
    bundle.plan.payload_block_crc32[block_index] =
        sound_crc32_iso_hdlc(
            bundle.payload.data() + offset, amount);
  }

  SoundSha256 bundle_hash;
  std::array<std::uint8_t, 4> length{};
  assert(bundle_hash.update(
      kBundleDomain.data(), kBundleDomain.size()));
  write_le32(length.data(), bundle.plan.manifest_bytes);
  assert(bundle_hash.update(length.data(), length.size()));
  assert(bundle_hash.update(
      bundle.manifest.data(), bundle.manifest.size()));
  write_le32(length.data(), bundle.plan.payload_bytes);
  assert(bundle_hash.update(length.data(), length.size()));
  assert(bundle_hash.update(
      bundle.payload.data(), bundle.payload.size()));
  bundle.plan.bundle_sha256 = bundle_hash.finish();
  return bundle;
}

void rewrite_commit_generation_for_test(
    RecordingStorage* storage,
    SoundBankId bank,
    std::uint64_t generation) {
  assert(storage != nullptr);
  assert(generation != 0U);
  auto* bank_bytes = storage->mutable_bank_for_test(bank);
  assert(bank_bytes != nullptr);
  auto* body = bank_bytes + kSoundCommitOffset;
  write_le64(body + 8U, generation);
  write_le64(body + 16U, generation - 1U);
  const auto body_crc =
      sound_crc32_iso_hdlc(body, 112U);
  write_le32(body + 112U, body_crc);
  // Marker bytes [0..7] retain the valid magic/complement pair.
  write_le32(body + 128U + 8U, body_crc);
  write_le32(body + 128U + 12U, ~body_crc);
}

SpeakerAssetsActionView make_action(
    SpeakerAssetsActionKind kind,
    std::uint32_t token,
    std::uint32_t request_id) {
  SpeakerAssetsActionView action{};
  action.kind = kind;
  action.token = token;
  action.request_id = request_id;
  action.route.transport = SpeakerAssetsTransport::Usb;
  action.route.generation = 7U;
  return action;
}

SpeakerAssetsActionCompletion run_action(
    SpeakerAssetsCooperativeStoreRunner* runner,
    PumpSynchronization* synchronization,
    const SpeakerAssetsActionView& action,
    RecordingStorage* storage) {
  synchronization->bind(runner, &action);
  SpeakerAssetsActionCompletion completion{};
  const auto calls_before = storage->operations.size();
  assert(runner->step(action, &completion) ==
         SpeakerAssetsActionExecutionResult::Pending);
  // Runtime polling alone may only accept/poll; no Store or backend work can
  // happen until the dedicated Store owner claims the job.
  assert(runner->step(action, &completion) ==
         SpeakerAssetsActionExecutionResult::Pending);
  assert(storage->operations.size() == calls_before);
  assert(runner->worker_run_once() ==
         SpeakerAssetsFlashWorkerResult::Completed);
  assert(runner->step(action, &completion) ==
         SpeakerAssetsActionExecutionResult::Completed);
  assert(completion.token == action.token);
  assert(completion.kind == action.kind);
  assert(!runner->job_active());
  return completion;
}

void install_bundle(
    SpeakerAssetsCooperativeStoreRunner* runner,
    PumpSynchronization* synchronization,
    RecordingStorage* storage,
    const TestBundle& bundle,
    std::uint32_t first_token) {
  auto begin = make_action(
      SpeakerAssetsActionKind::Begin,
      first_token,
      first_token + 100U);
  begin.plan = &bundle.plan;
  const auto begun =
      run_action(runner, synchronization, begin, storage);
  assert(begun.result == SoundStoreResult::Ok);
  assert(begun.identity_valid);

  auto manifest = make_action(
      SpeakerAssetsActionKind::WriteManifest,
      first_token + 1U,
      first_token + 101U);
  manifest.expected_identity = begun.identity;
  manifest.bytes = bundle.manifest.data();
  manifest.length =
      static_cast<std::uint16_t>(bundle.manifest.size());
  assert(run_action(
             runner, synchronization, manifest, storage)
             .result == SoundStoreResult::Ok);

  std::size_t block_index = 0U;
  for (std::size_t offset = 0U;
       offset < bundle.payload.size();
       offset += kSoundPayloadBlockSize, ++block_index) {
    const auto amount = std::min<std::size_t>(
        kSoundPayloadBlockSize, bundle.payload.size() - offset);
    auto payload = make_action(
        SpeakerAssetsActionKind::WritePayloadBlock,
        first_token + 2U +
            static_cast<std::uint32_t>(block_index),
        first_token + 102U +
            static_cast<std::uint32_t>(block_index));
    payload.expected_identity = begun.identity;
    payload.bytes = bundle.payload.data() + offset;
    payload.length = static_cast<std::uint16_t>(amount);
    payload.block_index =
        static_cast<std::uint16_t>(block_index);
    assert(run_action(
               runner, synchronization, payload, storage)
               .result == SoundStoreResult::Ok);
  }

  auto commit = make_action(
      SpeakerAssetsActionKind::Commit,
      first_token + 2U +
          static_cast<std::uint32_t>(block_index),
      first_token + 102U +
          static_cast<std::uint32_t>(block_index));
  commit.expected_identity = begun.identity;
  assert(run_action(
             runner, synchronization, commit, storage)
             .result == SoundStoreResult::Ok);
}

SpeakerAssetsPrepareBootReadCompletion run_prepare_boot_read(
    SpeakerAssetsCooperativeStoreRunner* runner,
    PumpSynchronization* synchronization,
    RecordingStorage* storage,
    SpeakerAssetsInternalJobHandle* completed_handle = nullptr) {
  SpeakerAssetsInternalJobHandle handle{};
  const auto operations_before = storage->operations.size();
  assert(runner->start_prepare_boot_read(&handle) ==
         SpeakerAssetsInternalJobStartResult::Accepted);
  assert(handle.job_id != 0U);
  synchronization->bind_prepare(runner, handle);
  SpeakerAssetsPrepareBootReadCompletion completion{};
  assert(runner->poll_prepare_boot_read(handle, &completion) ==
         SpeakerAssetsInternalJobPollResult::Pending);
  assert(storage->operations.size() == operations_before);
  assert(runner->worker_run_once() ==
         SpeakerAssetsFlashWorkerResult::Completed);
  assert(runner->poll_prepare_boot_read(handle, &completion) ==
         SpeakerAssetsInternalJobPollResult::Completed);
  assert(!runner->job_active());
  if (completed_handle != nullptr) {
    *completed_handle = handle;
  }
  return completion;
}

SoundStoreResult run_release_read(
    SpeakerAssetsCooperativeStoreRunner* runner,
    PumpSynchronization* synchronization,
    const SoundReadLease& lease,
    SpeakerAssetsInternalJobHandle* completed_handle = nullptr) {
  SpeakerAssetsInternalJobHandle handle{};
  assert(runner->start_release_read(lease, &handle) ==
         SpeakerAssetsInternalJobStartResult::Accepted);
  synchronization->bind_release(runner, handle);
  SoundStoreResult completion = SoundStoreResult::InvalidArgument;
  assert(runner->poll_release_read(handle, &completion) ==
         SpeakerAssetsInternalJobPollResult::Pending);
  assert(runner->worker_run_once() ==
         SpeakerAssetsFlashWorkerResult::Completed);
  assert(runner->poll_release_read(handle, &completion) ==
         SpeakerAssetsInternalJobPollResult::Completed);
  assert(!runner->job_active());
  if (completed_handle != nullptr) {
    *completed_handle = handle;
  }
  return completion;
}

void decorator_prevalidates_full_ranges_and_splits_units() {
  RecordingStorage backend;
  RecordingPermitGate permits;
  SpeakerAssetsChunkedSoundBankStorage storage(backend, permits);
  std::array<std::uint8_t, 600U> bytes{};

  assert(storage.read(
             SoundBankId::A,
             kSoundBankSize - 10U,
             bytes.data(),
             20U) == SoundStorageIoResult::OutOfBounds);
  assert(storage.write(
             SoundBankId::A,
             kSoundBankSize - 10U,
             bytes.data(),
             20U) == SoundStorageIoResult::OutOfBounds);
  assert(storage.erase(
             SoundBankId::A,
             0U,
             static_cast<std::size_t>(kSoundBankSize) +
                 kSoundSectorSize) ==
         SoundStorageIoResult::OutOfBounds);
  assert(storage.erase(SoundBankId::A, 1U, kSoundSectorSize) ==
         SoundStorageIoResult::NotAligned);
  assert(storage.checkpoint(
             SoundStorageWorkKind::Crc32,
             SoundBankId::A,
             0U,
             kSpeakerAssetsFlashReadWriteUnitBytes + 1U) ==
         SoundStorageIoResult::InvalidArgument);
  assert(backend.operations.empty());
  assert(permits.operations.empty());

  assert(storage.read(
             SoundBankId::A,
             0U,
             bytes.data(),
             bytes.size()) == SoundStorageIoResult::Ok);
  assert(backend.operations.size() == 3U);
  assert(permits.completed_units == 3U);
  assert(!permits.claimed);
  assert(backend.operations[0U].bytes == 256U);
  assert(backend.operations[1U].bytes == 256U);
  assert(backend.operations[2U].bytes == 88U);
  backend.operations.clear();
  permits.operations.clear();
  permits.completed_units = 0U;

  assert(storage.erase(
             SoundBankId::A,
             0U,
             2U * kSoundSectorSize) ==
         SoundStorageIoResult::Ok);
  assert(backend.operations.size() == 2U);
  assert(permits.completed_units == 2U);
  assert(!permits.claimed);
  for (const auto& operation : backend.operations) {
    assert(operation.kind == SoundStorageWorkKind::Erase);
    assert(operation.bytes == kSoundSectorSize);
  }

  backend.operations.clear();
  permits.operations.clear();
  permits.completed_units = 0U;
  assert(storage.checkpoint(
             SoundStorageWorkKind::Sha256,
             SoundBankId::A,
             kSoundManifestOffset,
             128U) == SoundStorageIoResult::Ok);
  assert(permits.claimed);
  assert(permits.operations.size() == 1U);
  assert(permits.completed_units == 0U);
  SoundSha256 hash;
  assert(hash.update(bytes.data(), 128U));
  storage.checkpoint_complete();
  assert(!permits.claimed);
  assert(permits.completed_units == 1U);

  backend.operations.clear();
  permits.operations.clear();
  permits.completed_units = 0U;
  SoundSha256Digest manifest_digest{};
  SoundSha256Digest bundle_digest{};
  assert(calculate_sound_bank_digests(
             storage,
             SoundBankId::A,
             256U,
             256U,
             &manifest_digest,
             &bundle_digest) == SoundFormatResult::Ok);
  assert(!permits.claimed);
  assert(permits.completed_units == permits.operations.size());
  assert(backend.operations.size() == 2U);
  const auto sha_permits = std::count_if(
      permits.operations.begin(),
      permits.operations.end(),
      [](const PhysicalOperation& operation) {
        return operation.kind == SoundStorageWorkKind::Sha256;
      });
  // Prefix + manifest primary + manifest bundle-secondary + manifest finish +
  // payload-length + payload bundle + bundle finish. In particular, the two
  // manifest updates and both finalizations have independent exact permits.
  assert(sha_permits == 7);
}

void runner_bounds_every_io_and_cpu_checkpoint() {
  RecordingStorage backend;
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      backend, synchronization);
  runner.publish_priority_allowed(true);
  assert(runner.completed_unit_generation() == 0U);

  std::array<std::uint8_t, kSoundSectorSize> manifest{};
  std::array<std::uint8_t, kSoundPayloadBlockSize> payload{};
  for (std::size_t index = 0U; index < manifest.size(); ++index) {
    manifest[index] =
        static_cast<std::uint8_t>((index * 17U) & 0xFFU);
    payload[index] =
        static_cast<std::uint8_t>((index * 29U) & 0xFFU);
  }
  const auto plan = make_plan(manifest, payload);

  auto begin = make_action(
      SpeakerAssetsActionKind::Begin, 11U, 21U);
  begin.plan = &plan;
  const auto begun =
      run_action(&runner, &synchronization, begin, &backend);
  assert(begun.result == SoundStoreResult::Ok);
  assert(begun.identity_valid);
  const auto begin_permits = synchronization.permits;
  const auto generation_after_begin =
      runner.completed_unit_generation();
  assert(generation_after_begin == begin_permits.size());

  backend.operations.clear();
  synchronization.permits.clear();
  auto write_manifest = make_action(
      SpeakerAssetsActionKind::WriteManifest, 12U, 22U);
  write_manifest.expected_identity = begun.identity;
  write_manifest.bytes = manifest.data();
  write_manifest.length =
      static_cast<std::uint16_t>(manifest.size());
  const auto manifest_completion = run_action(
      &runner, &synchronization, write_manifest, &backend);
  assert(manifest_completion.result == SoundStoreResult::Ok);

  auto write_payload = make_action(
      SpeakerAssetsActionKind::WritePayloadBlock, 13U, 23U);
  write_payload.expected_identity = begun.identity;
  write_payload.bytes = payload.data();
  write_payload.length =
      static_cast<std::uint16_t>(payload.size());
  write_payload.block_index = 0U;
  const auto payload_completion = run_action(
      &runner, &synchronization, write_payload, &backend);
  assert(payload_completion.result == SoundStoreResult::Ok);
  assert(
      runner.completed_unit_generation() -
          generation_after_begin ==
      synchronization.permits.size());

  auto all_permits = begin_permits;
  all_permits.insert(
      all_permits.end(),
      synchronization.permits.begin(),
      synchronization.permits.end());
  bool saw_read = false;
  bool saw_write = false;
  bool saw_crc = false;
  bool saw_sha = false;
  bool saw_erase = false;
  for (const auto& permit : all_permits) {
    assert(permit.job_id != 0U);
    assert(permit.unit_sequence != 0U);
    switch (permit.kind) {
      case SoundStorageWorkKind::Read:
        saw_read = true;
        assert(permit.bytes <=
               kSpeakerAssetsFlashReadWriteUnitBytes);
        break;
      case SoundStorageWorkKind::Write:
        saw_write = true;
        assert(permit.bytes <=
               kSpeakerAssetsFlashReadWriteUnitBytes);
        break;
      case SoundStorageWorkKind::Erase:
        saw_erase = true;
        assert(permit.bytes ==
               kSpeakerAssetsFlashEraseUnitBytes);
        assert((permit.offset %
                kSpeakerAssetsFlashEraseUnitBytes) == 0U);
        break;
      case SoundStorageWorkKind::Crc32:
        saw_crc = true;
        assert(permit.bytes <=
               kSpeakerAssetsFlashReadWriteUnitBytes);
        break;
      case SoundStorageWorkKind::Sha256:
        saw_sha = true;
        assert(permit.bytes <=
               kSpeakerAssetsFlashReadWriteUnitBytes);
        break;
    }
  }
  assert(saw_read);
  assert(saw_write);
  assert(saw_crc);
  assert(saw_sha);
  assert(saw_erase);
  for (const auto& operation : backend.operations) {
    if (operation.kind == SoundStorageWorkKind::Erase) {
      assert(operation.bytes ==
             kSpeakerAssetsFlashEraseUnitBytes);
    } else {
      assert(operation.bytes <=
             kSpeakerAssetsFlashReadWriteUnitBytes);
    }
  }
}

void stale_priority_epoch_never_reuses_a_grant() {
  RecordingStorage backend;
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      backend, synchronization);
  runner.publish_priority_allowed(true);

  std::array<std::uint8_t, kSoundSectorSize> manifest{};
  std::array<std::uint8_t, kSoundPayloadBlockSize> payload{};
  const auto plan = make_plan(manifest, payload);
  auto begin = make_action(
      SpeakerAssetsActionKind::Begin, 31U, 41U);
  begin.plan = &plan;
  // Grant the exact permit first, then publish a higher-priority pulse before
  // the worker can claim the unit. The packed allowed/claimed/epoch CAS must
  // make the stale grant lose without starting backend work.
  synchronization.pulse_priority_after_next_grant = true;
  const auto completion =
      run_action(&runner, &synchronization, begin, &backend);
  assert(completion.result == SoundStoreResult::Ok);
  assert(synchronization.permits.size() >= 2U);
  const auto& stale = synchronization.permits[0U];
  const auto& replacement = synchronization.permits[1U];
  assert(stale.job_id == replacement.job_id);
  assert(stale.unit_sequence != replacement.unit_sequence);
  assert(stale.kind == replacement.kind);
  assert(stale.bank == replacement.bank);
  assert(stale.offset == replacement.offset);
  assert(stale.bytes == replacement.bytes);

  const auto matching_physical = std::count_if(
      backend.operations.begin(),
      backend.operations.end(),
      [&replacement](const PhysicalOperation& operation) {
        return operation.kind == replacement.kind &&
               operation.bank == replacement.bank &&
               operation.offset == replacement.offset &&
               operation.bytes == replacement.bytes;
      });
  // The stale permit was consumed without an operation. Only its fresh exact
  // replacement may authorize the first backend unit.
  assert(matching_physical == 1);
}

void accepted_storage_error_is_completed_not_rejected() {
  RecordingStorage backend;
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      backend, synchronization);
  runner.publish_priority_allowed(true);

  std::array<std::uint8_t, kSoundSectorSize> manifest{};
  std::array<std::uint8_t, kSoundPayloadBlockSize> payload{};
  const auto plan = make_plan(manifest, payload);
  auto begin = make_action(
      SpeakerAssetsActionKind::Begin, 51U, 61U);
  begin.plan = &plan;
  backend.fail_next_mutation();
  const auto completion =
      run_action(&runner, &synchronization, begin, &backend);
  assert(completion.result == SoundStoreResult::IoError);
}

class CapturingExecutor final
    : public SpeakerAssetsRuntimeActionExecutor {
 public:
  CapturingExecutor(
      SpeakerAssetsCooperativeStoreRunner& runner,
      PumpSynchronization& synchronization)
      : runner_(runner), synchronization_(synchronization) {}

  SpeakerAssetsActionExecutionResult step(
      const SpeakerAssetsActionView& action,
      SpeakerAssetsActionCompletion* completion) override {
    observed = action;
    observed_valid = true;
    synchronization_.bind(&runner_, &observed);
    return runner_.step(observed, completion);
  }

  SpeakerAssetsActionView observed{};
  bool observed_valid = false;

 private:
  SpeakerAssetsCooperativeStoreRunner& runner_;
  PumpSynchronization& synchronization_;
};

void route_close_orphans_but_never_aborts_store_action() {
  RecordingStorage backend;
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      backend, synchronization);
  CapturingExecutor executor(runner, synchronization);
  runner.publish_priority_allowed(true);
  SpeakerAssetsRuntimeCore runtime(0x100U, 0x200U);

  SpeakerAssetsRouteToken route{};
  route.transport = SpeakerAssetsTransport::Usb;
  route.generation = 9U;
  assert(runtime.enqueue_route_opened(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(0U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);

  SpeakerAssetsFrame request{};
  request.opcode =
      static_cast<std::uint8_t>(SpeakerAssetsOpcode::CurrentActive);
  request.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
      easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment |
      easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment;
  request.request_id = 71U;
  std::array<std::uint8_t, kSpeakerAssetsUsbFrameBytes> encoded{};
  assert(encode_speaker_assets_usb_frame(request, &encoded) ==
         easy_input::speaker_assets::SpeakerAssetsProtocolResult::Ok);
  assert(runtime.enqueue_usb_frame(
             route,
             encoded.data(),
             encoded.size(),
             1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(runtime.step(2U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionExecutionPending);
  assert(executor.observed_valid);
  assert(executor.observed.kind ==
         SpeakerAssetsActionKind::QueryCurrentActive);

  assert(runtime.enqueue_route_closed(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(3U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
  assert(runtime.action_pending());

  assert(runner.worker_run_once() ==
         SpeakerAssetsFlashWorkerResult::Completed);
  assert(runtime.step(4U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert(!runtime.action_pending());
  assert(runtime.reply_size() == 0U);
  // The worker completed the already accepted read-only action. Close never
  // manufactured or substituted an Abort action.
  assert(executor.observed.kind ==
         SpeakerAssetsActionKind::QueryCurrentActive);
}

void prepared_boot_read_and_exact_release_share_runner_store() {
  RecordingStorage backend;
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      backend, synchronization);
  runner.publish_priority_allowed(true);
  const auto bundle = make_boot_bundle(true);
  install_bundle(
      &runner, &synchronization, &backend, bundle, 101U);

  backend.operations.clear();
  synchronization.permits.clear();
  SpeakerAssetsInternalJobHandle prepare_handle{};
  const auto prepared = run_prepare_boot_read(
      &runner,
      &synchronization,
      &backend,
      &prepare_handle);
  assert(prepared.acquire_result == SoundStoreResult::Ok);
  assert(prepared.resolve_result == SoundAssetReadResult::Ok);
  assert(prepared.cleanup_result == SoundStoreResult::Ok);
  assert(prepared.prepared);
  assert(prepared.lease.valid);
  assert(prepared.asset.valid);
  assert(prepared.asset.lease_id == prepared.lease.lease_id);
  assert(prepared.asset.bank == prepared.lease.bank);
  assert(prepared.asset.generation ==
         prepared.lease.generation);
  assert(prepared.asset.encoded_bank_offset ==
         kSoundPayloadOffset);
  assert(!backend.operations.empty());
  assert(!synchronization.permits.empty());
  for (const auto& operation : backend.operations) {
    assert(operation.kind == SoundStorageWorkKind::Read);
    assert(operation.bytes <=
           kSpeakerAssetsFlashReadWriteUnitBytes);
  }

  SpeakerAssetsPrepareBootReadCompletion stale{};
  assert(runner.poll_prepare_boot_read(
             prepare_handle, &stale) ==
         SpeakerAssetsInternalJobPollResult::Mismatched);

  SpeakerAssetsInternalJobHandle second_handle{};
  assert(runner.start_prepare_boot_read(&second_handle) ==
         SpeakerAssetsInternalJobStartResult::Accepted);
  synchronization.bind_prepare(&runner, second_handle);
  // A stale handle for the same job kind neither grants nor consumes the
  // second request, before or after its worker completion.
  assert(runner.poll_prepare_boot_read(
             prepare_handle, &stale) ==
         SpeakerAssetsInternalJobPollResult::Mismatched);
  assert(runner.worker_run_once() ==
         SpeakerAssetsFlashWorkerResult::Completed);
  assert(runner.poll_prepare_boot_read(
             prepare_handle, &stale) ==
         SpeakerAssetsInternalJobPollResult::Mismatched);
  SpeakerAssetsPrepareBootReadCompletion second_prepared{};
  assert(runner.poll_prepare_boot_read(
             second_handle, &second_prepared) ==
         SpeakerAssetsInternalJobPollResult::Completed);
  assert(second_prepared.prepared);

  assert(run_release_read(
             &runner,
             &synchronization,
             prepared.lease) == SoundStoreResult::Ok);
  assert(run_release_read(
             &runner,
             &synchronization,
             second_prepared.lease) == SoundStoreResult::Ok);
  // The same immutable lease is no longer owned after the exact release.
  assert(run_release_read(
             &runner,
             &synchronization,
             prepared.lease) ==
         SoundStoreResult::TransactionMismatch);
}

void fresh_boot_read_has_payload_independent_permit_budget() {
  constexpr std::array<std::uint32_t, 2> kSampleCounts{{
      481U,
      384000U,
  }};
  for (const auto sample_count : kSampleCounts) {
    RecordingStorage backend;
    {
      PumpSynchronization writer_synchronization;
      SpeakerAssetsCooperativeStoreRunner writer(
          backend, writer_synchronization);
      writer.publish_priority_allowed(true);
      const auto bundle =
          make_boot_bundle(true, 1234U, sample_count);
      if (sample_count == 384000U) {
        assert(bundle.payload.size() > 195000U);
      }
      install_bundle(
          &writer,
          &writer_synchronization,
          &backend,
          bundle,
          1201U);
    }

    backend.operations.clear();
    PumpSynchronization synchronization;
    SpeakerAssetsCooperativeStoreRunner fresh_runner(
        backend, synchronization);
    fresh_runner.publish_priority_allowed(true);
    const auto prepared = run_prepare_boot_read(
        &fresh_runner, &synchronization, &backend);
    assert(prepared.prepared);
    assert(prepared.acquire_result == SoundStoreResult::Ok);
    assert(prepared.resolve_result == SoundAssetReadResult::Ok);

    const auto read_count = std::count_if(
        backend.operations.begin(),
        backend.operations.end(),
        [](const PhysicalOperation& operation) {
          return operation.kind == SoundStorageWorkKind::Read;
        });
    // Current 84-byte manifest: A commit (3 permits), erased B commit
    // (2), manifest authentication (3), and resolve (3) = exactly 11. Eight
    // are physical reads. The exact gate covers both the 481-sample fixture
    // and the maximum 384000-sample/8-second resource.
    assert(synchronization.permits.size() == 11U);
    assert(read_count == 8U);
    for (const auto& operation : backend.operations) {
      if (operation.kind != SoundStorageWorkKind::Read) {
        continue;
      }
      assert(
          operation.offset < kSoundPayloadOffset ||
          operation.offset >=
              kSoundPayloadOffset + kSoundPayloadMaxSize);
    }
    assert(run_release_read(
               &fresh_runner,
               &synchronization,
               prepared.lease) == SoundStoreResult::Ok);
  }
}

void damaged_new_manifest_falls_back_to_authenticated_old_bank() {
  RecordingStorage backend;
  {
    PumpSynchronization writer_synchronization;
    SpeakerAssetsCooperativeStoreRunner writer(
        backend, writer_synchronization);
    writer.publish_priority_allowed(true);
    const auto older = make_boot_bundle(true, 111U);
    install_bundle(
        &writer,
        &writer_synchronization,
        &backend,
        older,
        1301U);
    auto newer = make_boot_bundle(true, 222U);
    newer.plan.base_generation = 1U;
    newer.plan.base_bundle_sha256 =
        older.plan.bundle_sha256;
    install_bundle(
        &writer,
        &writer_synchronization,
        &backend,
        newer,
        1401U);
  }

  // The higher generation's commit remains intact, but its manifest no longer
  // matches the SHA-256 bound into that commit.
  backend.corrupt_byte(
      SoundBankId::B, kSoundManifestOffset + 23U);
  backend.operations.clear();
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner fresh_runner(
      backend, synchronization);
  fresh_runner.publish_priority_allowed(true);
  const auto prepared = run_prepare_boot_read(
      &fresh_runner, &synchronization, &backend);
  assert(prepared.prepared);
  assert(prepared.acquire_result == SoundStoreResult::Ok);
  assert(prepared.lease.bank == SoundBankId::A);
  assert(prepared.lease.generation == 1U);
  assert(synchronization.permits.size() <= 20U);
  assert(run_release_read(
             &fresh_runner,
             &synchronization,
             prepared.lease) == SoundStoreResult::Ok);
}

void authenticated_distant_generations_remain_split_brain() {
  RecordingStorage backend;
  {
    PumpSynchronization writer_synchronization;
    SpeakerAssetsCooperativeStoreRunner writer(
        backend, writer_synchronization);
    writer.publish_priority_allowed(true);
    const auto older = make_boot_bundle(true, 333U);
    install_bundle(
        &writer,
        &writer_synchronization,
        &backend,
        older,
        1501U);
    auto newer = make_boot_bundle(true, 444U);
    newer.plan.base_generation = 1U;
    newer.plan.base_bundle_sha256 =
        older.plan.bundle_sha256;
    install_bundle(
        &writer,
        &writer_synchronization,
        &backend,
        newer,
        1601U);
  }
  // Both commits and both manifests remain authentic, but the artificial gap
  // cannot be justified. Fast selection must not silently choose either bank.
  rewrite_commit_generation_for_test(
      &backend, SoundBankId::B, 4U);
  backend.operations.clear();
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner fresh_runner(
      backend, synchronization);
  fresh_runner.publish_priority_allowed(true);
  const auto prepared = run_prepare_boot_read(
      &fresh_runner, &synchronization, &backend);
  assert(!prepared.prepared);
  assert(prepared.acquire_result ==
         SoundStoreResult::SplitBrain);
  assert(synchronization.permits.size() <= 16U);
}

void fast_read_never_authorizes_update_without_strict_scan() {
  RecordingStorage backend;
  TestBundle active{};
  {
    PumpSynchronization writer_synchronization;
    SpeakerAssetsCooperativeStoreRunner writer(
        backend, writer_synchronization);
    writer.publish_priority_allowed(true);
    active = make_boot_bundle(true, 555U);
    install_bundle(
        &writer,
        &writer_synchronization,
        &backend,
        active,
        1701U);
  }

  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner fresh_runner(
      backend, synchronization);
  fresh_runner.publish_priority_allowed(true);
  const auto prepared = run_prepare_boot_read(
      &fresh_runner, &synchronization, &backend);
  assert(prepared.prepared);
  assert(run_release_read(
             &fresh_runner,
             &synchronization,
             prepared.lease) == SoundStoreResult::Ok);

  const auto corrupt_tail_offset = static_cast<std::uint32_t>(
      kSoundPayloadOffset + active.payload.size() + 7U);
  backend.corrupt_byte(
      SoundBankId::A, corrupt_tail_offset);
  backend.operations.clear();
  synchronization.permits.clear();

  auto next = make_boot_bundle(true, 666U);
  next.plan.base_generation = 1U;
  next.plan.base_bundle_sha256 =
      active.plan.bundle_sha256;
  auto begin = make_action(
      SpeakerAssetsActionKind::Begin,
      1801U,
      1901U);
  begin.plan = &next.plan;
  const auto completion = run_action(
      &fresh_runner, &synchronization, begin, &backend);
  // Strict full-bank validation notices the non-erased tail, invalidates the
  // apparent base, and refuses to erase either bank.
  assert(completion.result == SoundStoreResult::StaleBase);
  assert(std::none_of(
      backend.operations.begin(),
      backend.operations.end(),
      [](const PhysicalOperation& operation) {
        return operation.kind == SoundStorageWorkKind::Erase;
      }));
  assert(std::any_of(
      backend.operations.begin(),
      backend.operations.end(),
      [corrupt_tail_offset](const PhysicalOperation& operation) {
        return operation.kind == SoundStorageWorkKind::Read &&
               operation.offset <= corrupt_tail_offset &&
               corrupt_tail_offset <
                   operation.offset + operation.bytes;
      }));
}

void protocol_and_internal_jobs_are_mutually_exclusive() {
  RecordingStorage backend;
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      backend, synchronization);
  runner.publish_priority_allowed(true);

  SpeakerAssetsInternalJobHandle prepare_handle{};
  assert(runner.start_prepare_boot_read(&prepare_handle) ==
         SpeakerAssetsInternalJobStartResult::Accepted);
  synchronization.bind_prepare(&runner, prepare_handle);

  const auto query = make_action(
      SpeakerAssetsActionKind::QueryCurrentActive,
      201U,
      301U);
  SpeakerAssetsActionCompletion action_completion{};
  assert(runner.step(query, &action_completion) ==
         SpeakerAssetsActionExecutionResult::Pending);
  // Even malformed protocol polls cannot reject/reset an internal job.
  SpeakerAssetsActionView malformed{};
  assert(runner.step(malformed, nullptr) ==
         SpeakerAssetsActionExecutionResult::Pending);

  SpeakerAssetsInternalJobHandle busy_handle{};
  assert(runner.start_release_read({}, &busy_handle) ==
         SpeakerAssetsInternalJobStartResult::Busy);
  SpeakerAssetsInternalJobHandle wrong_handle = prepare_handle;
  wrong_handle.kind =
      easy_input::speaker_assets::
          SpeakerAssetsInternalJobKind::ReleaseRead;
  SoundStoreResult wrong_completion =
      SoundStoreResult::InvalidArgument;
  assert(runner.poll_release_read(
             wrong_handle, &wrong_completion) ==
         SpeakerAssetsInternalJobPollResult::Mismatched);

  assert(runner.worker_run_once() ==
         SpeakerAssetsFlashWorkerResult::Completed);
  SpeakerAssetsPrepareBootReadCompletion prepared{};
  assert(runner.poll_prepare_boot_read(
             prepare_handle, &prepared) ==
         SpeakerAssetsInternalJobPollResult::Completed);
  assert(prepared.acquire_result ==
         SoundStoreResult::FactoryBlank);

  synchronization.bind(&runner, &query);
  assert(runner.step(query, &action_completion) ==
         SpeakerAssetsActionExecutionResult::Pending);
  assert(runner.start_prepare_boot_read(&busy_handle) ==
         SpeakerAssetsInternalJobStartResult::Busy);
  assert(runner.worker_run_once() ==
         SpeakerAssetsFlashWorkerResult::Completed);
  assert(runner.step(query, &action_completion) ==
         SpeakerAssetsActionExecutionResult::Completed);
  assert(action_completion.result == SoundStoreResult::Ok);
}

void failed_boot_resolution_releases_lease_on_worker() {
  RecordingStorage backend;
  PumpSynchronization synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      backend, synchronization);
  runner.publish_priority_allowed(true);
  const auto bundle = make_boot_bundle(false);
  install_bundle(
      &runner, &synchronization, &backend, bundle, 401U);

  // More attempts than the Store's fixed lease table proves each failed
  // resolve released its acquired slot before publishing completion.
  for (std::size_t attempt = 0U;
       attempt < easy_input::speaker_assets::
                     kSoundMaximumReadLeases +
                     2U;
       ++attempt) {
    const auto prepared = run_prepare_boot_read(
        &runner, &synchronization, &backend);
    assert(prepared.acquire_result == SoundStoreResult::Ok);
    assert(prepared.resolve_result ==
           SoundAssetReadResult::NotMapped);
    assert(prepared.cleanup_result == SoundStoreResult::Ok);
    assert(!prepared.prepared);
    assert(!prepared.lease.valid);
    assert(!prepared.asset.valid);
  }
}

}  // namespace

int main() {
  decorator_prevalidates_full_ranges_and_splits_units();
  runner_bounds_every_io_and_cpu_checkpoint();
  stale_priority_epoch_never_reuses_a_grant();
  accepted_storage_error_is_completed_not_rejected();
  route_close_orphans_but_never_aborts_store_action();
  prepared_boot_read_and_exact_release_share_runner_store();
  fresh_boot_read_has_payload_independent_permit_budget();
  damaged_new_manifest_falls_back_to_authenticated_old_bank();
  authenticated_distant_generations_remain_split_brain();
  fast_read_never_authorizes_update_without_strict_scan();
  protocol_and_internal_jobs_are_mutually_exclusive();
  failed_boot_resolution_releases_lease_on_worker();
  return 0;
}
