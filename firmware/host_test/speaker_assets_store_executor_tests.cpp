#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "speaker_assets/sound_asset_crypto.h"
#include "speaker_assets/speaker_assets_store_executor.h"

namespace {

using easy_input::speaker_assets::SoundAssetStore;
using easy_input::speaker_assets::SoundBankId;
using easy_input::speaker_assets::SoundBankStorage;
using easy_input::speaker_assets::SoundBundlePlan;
using easy_input::speaker_assets::SoundSha256;
using easy_input::speaker_assets::SoundSha256Digest;
using easy_input::speaker_assets::SoundStorageIoResult;
using easy_input::speaker_assets::SoundStoreResult;
using easy_input::speaker_assets::SoundUpdateIdentity;
using easy_input::speaker_assets::SoundUpdateProgress;
using easy_input::speaker_assets::SpeakerAssetsActionCompletion;
using easy_input::speaker_assets::SpeakerAssetsActionKind;
using easy_input::speaker_assets::SpeakerAssetsActionView;
using easy_input::speaker_assets::SpeakerAssetsResumeQueryMode;
using easy_input::speaker_assets::SpeakerAssetsTransactionOutcome;
using easy_input::speaker_assets::execute_speaker_assets_store_action;
using easy_input::speaker_assets::kSoundBankSize;
using easy_input::speaker_assets::kSoundPayloadBlockSize;
using easy_input::speaker_assets::kSoundSectorSize;
using easy_input::speaker_assets::kSoundStagingHeaderOffset;
using easy_input::speaker_assets::sound_crc32_iso_hdlc;

constexpr std::array<std::uint8_t, 28> kGoldenAsset{{
    'E', 'I', 'A', 'D', 1, 1,
    0x80, 0xBB, 0x00, 0x00,
    0xE0, 0x01,
    0x01, 0x00,
    0x05, 0x00, 0x00, 0x00,
    0x14, 0x00,
    0x05, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0x10, 0x32,
}};

constexpr std::array<std::uint8_t, 16> kBundleDomain{{
    'E', 'A', 'S', 'Y', 'I', 'N', 'P', 'U',
    'T', '-', 'S', 'N', 'D', '-', 'V', '1',
}};

std::size_t bank_index(SoundBankId bank) {
  return bank == SoundBankId::A ? 0U : 1U;
}

void write_le16(std::uint8_t* output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_le32(std::uint8_t* output, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
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

TestBundle make_bundle(std::uint8_t seed) {
  TestBundle bundle{};
  bundle.payload.assign(kGoldenAsset.begin(), kGoldenAsset.end());
  bundle.payload[22U] = seed;
  bundle.payload[23U] = 0U;

  constexpr std::size_t kManifestBytes = 32U + 48U + 4U;
  bundle.manifest.assign(kManifestBytes, 0U);
  auto* manifest = bundle.manifest.data();
  manifest[0] = 'E';
  manifest[1] = 'I';
  manifest[2] = 'S';
  manifest[3] = 'M';
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
  std::copy(
      resource_digest.begin(),
      resource_digest.end(),
      bundle.manifest.begin() + 32U);
  write_le32(manifest + 64U, 0U);
  write_le32(
      manifest + 68U,
      static_cast<std::uint32_t>(bundle.payload.size()));
  write_le32(manifest + 72U, 5U);
  write_le16(manifest + 76U, 1U);
  manifest[78U] = 1U;
  manifest[79U] = 1U;
  manifest[80U] = 1U;
  manifest[81U] = 0U;
  write_le16(manifest + 82U, 0U);

  bundle.plan.manifest_bytes =
      static_cast<std::uint32_t>(bundle.manifest.size());
  bundle.plan.payload_bytes =
      static_cast<std::uint32_t>(bundle.payload.size());
  bundle.plan.manifest_crc32 = sound_crc32_iso_hdlc(
      bundle.manifest.data(), bundle.manifest.size());
  bundle.plan.manifest_sha256 =
      sha256(bundle.manifest.data(), bundle.manifest.size());
  bundle.plan.payload_block_crc32[0U] =
      sound_crc32_iso_hdlc(
          bundle.payload.data(), bundle.payload.size());

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

class MemorySoundBankStorage final : public SoundBankStorage {
 public:
  MemorySoundBankStorage() {
    for (auto& bank : banks_) {
      bank.assign(kSoundBankSize, 0xFFU);
    }
  }

  SoundStorageIoResult read(SoundBankId bank,
                            std::uint32_t offset,
                            std::uint8_t* destination,
                            std::size_t length) override {
    ++read_calls_;
    if (destination == nullptr && length != 0U) {
      return SoundStorageIoResult::InvalidArgument;
    }
    const auto* source = range(bank, offset, length);
    if (source == nullptr) {
      return SoundStorageIoResult::OutOfBounds;
    }
    std::copy(source, source + length, destination);
    return SoundStorageIoResult::Ok;
  }

  SoundStorageIoResult write(SoundBankId bank,
                             std::uint32_t offset,
                             const std::uint8_t* source,
                             std::size_t length) override {
    ++write_calls_;
    if (source == nullptr && length != 0U) {
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
    }
    for (std::size_t index = 0U; index < length; ++index) {
      destination[index] &= source[index];
    }
    return SoundStorageIoResult::Ok;
  }

  SoundStorageIoResult erase(SoundBankId bank,
                             std::uint32_t offset,
                             std::size_t length) override {
    ++erase_calls_;
    if ((offset % kSoundSectorSize) != 0U ||
        (length % kSoundSectorSize) != 0U) {
      return SoundStorageIoResult::NotAligned;
    }
    auto* destination = mutable_range(bank, offset, length);
    if (destination == nullptr) {
      return SoundStorageIoResult::OutOfBounds;
    }
    std::fill(destination, destination + length, 0xFFU);
    return SoundStorageIoResult::Ok;
  }

  std::size_t io_calls() const {
    return read_calls_ + write_calls_ + erase_calls_;
  }

  std::size_t mutation_calls() const {
    return write_calls_ + erase_calls_;
  }

  void corrupt(SoundBankId bank,
               std::uint32_t offset,
               std::uint8_t value) {
    auto* destination = mutable_range(bank, offset, 1U);
    assert(destination != nullptr);
    *destination = value;
  }

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
        static_cast<const MemorySoundBankStorage*>(this)->range(
            bank, offset, length));
  }

  std::array<std::vector<std::uint8_t>, 2> banks_{};
  std::size_t read_calls_ = 0U;
  std::size_t write_calls_ = 0U;
  std::size_t erase_calls_ = 0U;
};

bool identity_equal(const SoundUpdateIdentity& first,
                    const SoundUpdateIdentity& second) {
  return first.generation == second.generation &&
         first.target_bank == second.target_bank &&
         first.transaction_id == second.transaction_id;
}

void assert_progress_equal(const SoundUpdateProgress& first,
                           const SoundUpdateProgress& second) {
  assert(identity_equal(first.identity, second.identity));
  assert(first.manifest_bytes == second.manifest_bytes);
  assert(first.payload_bytes == second.payload_bytes);
  assert(first.payload_block_count == second.payload_block_count);
  assert(first.manifest_complete == second.manifest_complete);
  assert(first.payload_complete_bitmap ==
         second.payload_complete_bitmap);
  assert(first.bundle_sha256 == second.bundle_sha256);
}

SpeakerAssetsActionView make_action(
    SpeakerAssetsActionKind kind,
    std::uint32_t token,
    std::uint32_t request_id) {
  SpeakerAssetsActionView action{};
  action.kind = kind;
  action.token = token;
  action.request_id = request_id;
  return action;
}

SpeakerAssetsActionCompletion run_action(
    SoundAssetStore& store,
    const SpeakerAssetsActionView& action) {
  SpeakerAssetsActionCompletion completion{};
  assert(execute_speaker_assets_store_action(
      store, action, &completion));
  assert(completion.token == action.token);
  assert(completion.kind == action.kind);
  return completion;
}

std::array<SoundUpdateIdentity, 3> wrong_identities(
    const SoundUpdateIdentity& identity) {
  auto wrong_generation = identity;
  ++wrong_generation.generation;

  auto wrong_bank = identity;
  wrong_bank.target_bank =
      identity.target_bank == SoundBankId::A
          ? SoundBankId::B
          : SoundBankId::A;

  auto wrong_transaction = identity;
  wrong_transaction.transaction_id[0U] ^= 0x80U;
  assert(wrong_transaction.transaction_id !=
         identity.transaction_id);
  return {{
      wrong_generation,
      wrong_bank,
      wrong_transaction,
  }};
}

void invalid_actions_do_not_touch_the_store() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(11U);
  const auto io_before = flash.io_calls();

  auto invalid = make_action(
      SpeakerAssetsActionKind::Begin, 0U, 101U);
  invalid.plan = &bundle.plan;
  SpeakerAssetsActionCompletion untouched{};
  untouched.token = 0xC0FFEEU;
  untouched.kind = SpeakerAssetsActionKind::Abort;
  untouched.result = SoundStoreResult::Busy;
  untouched.progress_valid = true;
  assert(!execute_speaker_assets_store_action(
      store, invalid, &untouched));
  assert(untouched.token == 0xC0FFEEU);
  assert(untouched.kind == SpeakerAssetsActionKind::Abort);
  assert(untouched.result == SoundStoreResult::Busy);
  assert(untouched.progress_valid);

  invalid.token = 101U;
  invalid.request_id = 0U;
  assert(!execute_speaker_assets_store_action(
      store, invalid, &untouched));
  assert(!execute_speaker_assets_store_action(
      store, invalid, nullptr));
  assert(flash.io_calls() == io_before);
  assert(!store.update_active());

  auto missing_plan = make_action(
      SpeakerAssetsActionKind::Begin, 102U, 202U);
  const auto missing_plan_completion =
      run_action(store, missing_plan);
  assert(missing_plan_completion.result ==
         SoundStoreResult::InvalidArgument);
  assert(!missing_plan_completion.progress_valid);
  assert(flash.io_calls() == io_before);
  assert(!store.update_active());

  auto unknown = make_action(
      static_cast<SpeakerAssetsActionKind>(0xFFU),
      103U,
      203U);
  const auto unknown_completion = run_action(store, unknown);
  assert(unknown_completion.result ==
         SoundStoreResult::InvalidArgument);
  assert(!unknown_completion.progress_valid);
  assert(flash.io_calls() == io_before);
  assert(!store.update_active());
}

void discard_invalid_staging_is_explicit_and_safe() {
  const auto bundle = make_bundle(17U);

  {
    MemorySoundBankStorage flash;
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    const auto io_before = flash.io_calls();
    const auto discard = make_action(
        SpeakerAssetsActionKind::DiscardInvalidStaging,
        151U,
        251U);
    const auto completion = run_action(store, discard);
    assert(completion.result ==
           SoundStoreResult::InvalidArgument);
    assert(!completion.identity_valid);
    assert(!completion.progress_valid);
    assert(!completion.outcome_valid);
    assert(flash.io_calls() == io_before);
    assert(!store.update_active());
  }

  {
    MemorySoundBankStorage flash;
    flash.corrupt(
        SoundBankId::A, kSoundStagingHeaderOffset, 'E');
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);

    auto discard = make_action(
        SpeakerAssetsActionKind::DiscardInvalidStaging,
        152U,
        252U);
    discard.plan = &bundle.plan;
    const auto mutations_before = flash.mutation_calls();
    const auto completion = run_action(store, discard);
    assert(completion.result == SoundStoreResult::Ok);
    assert(!completion.identity_valid);
    assert(!completion.progress_valid);
    assert(!completion.outcome_valid);
    assert(flash.mutation_calls() > mutations_before);
    assert(!store.update_active());

    std::array<std::uint8_t, 4> staging_magic{};
    assert(flash.read(
               SoundBankId::A,
               kSoundStagingHeaderOffset,
               staging_magic.data(),
               staging_magic.size()) ==
           SoundStorageIoResult::Ok);
    assert(std::all_of(
        staging_magic.begin(),
        staging_magic.end(),
        [](std::uint8_t value) { return value == 0xFFU; }));

    auto begin = make_action(
        SpeakerAssetsActionKind::Begin, 153U, 253U);
    begin.plan = &bundle.plan;
    const auto begun = run_action(store, begin);
    assert(begun.result == SoundStoreResult::Ok);
    assert(begun.identity_valid);
    assert(begun.progress_valid);
    assert(store.update_active());
  }

  {
    MemorySoundBankStorage flash;
    SoundAssetStore first_boot(flash);
    assert(first_boot.scan() == SoundStoreResult::Ok);
    auto begin = make_action(
        SpeakerAssetsActionKind::Begin, 154U, 254U);
    begin.plan = &bundle.plan;
    const auto begun = run_action(first_boot, begin);
    assert(begun.result == SoundStoreResult::Ok);
    assert(begun.identity_valid);
    assert(begun.progress_valid);

    SoundAssetStore after_restart(flash);
    assert(after_restart.scan() == SoundStoreResult::Ok);
    assert(!after_restart.update_active());
    auto discard = make_action(
        SpeakerAssetsActionKind::DiscardInvalidStaging,
        155U,
        255U);
    discard.plan = &bundle.plan;
    const auto mutations_before = flash.mutation_calls();
    const auto completion = run_action(after_restart, discard);
    assert(completion.result == SoundStoreResult::Busy);
    assert(!completion.identity_valid);
    assert(!completion.progress_valid);
    assert(!completion.outcome_valid);
    assert(flash.mutation_calls() == mutations_before);
    assert(!after_restart.update_active());

    std::array<std::uint8_t, 4> staging_magic{};
    assert(flash.read(
               SoundBankId::A,
               kSoundStagingHeaderOffset,
               staging_magic.data(),
               staging_magic.size()) ==
           SoundStorageIoResult::Ok);
    assert(staging_magic ==
           (std::array<std::uint8_t, 4>{{
               'E', 'I', 'S', 'H',
           }}));
  }
}

void begin_active_resume_current_identity_and_abort() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(21U);

  auto begin = make_action(
      SpeakerAssetsActionKind::Begin, 201U, 301U);
  begin.plan = &bundle.plan;
  const auto begun = run_action(store, begin);
  assert(begun.result == SoundStoreResult::Ok);
  assert(begun.progress_valid);
  assert(store.update_active());
  assert(begun.progress.identity.generation == 1U);
  assert(begun.progress.identity.target_bank == SoundBankId::A);
  assert(std::any_of(
      begun.progress.identity.transaction_id.begin(),
      begun.progress.identity.transaction_id.end(),
      [](std::uint8_t value) { return value != 0U; }));
  assert(begun.progress.manifest_bytes ==
         bundle.plan.manifest_bytes);
  assert(begun.progress.payload_bytes ==
         bundle.plan.payload_bytes);
  assert(begun.progress.payload_block_count == 1U);
  assert(!begun.progress.manifest_complete);
  assert(std::all_of(
      begun.progress.payload_complete_bitmap.begin(),
      begun.progress.payload_complete_bitmap.end(),
      [](std::uint8_t value) { return value == 0U; }));
  assert(begun.progress.bundle_sha256 ==
         bundle.plan.bundle_sha256);

  auto repeated_begin = begin;
  repeated_begin.token = 210U;
  repeated_begin.request_id = 310U;
  const auto io_before_repeated_begin = flash.io_calls();
  const auto rebound_begin = run_action(store, repeated_begin);
  assert(rebound_begin.result == SoundStoreResult::Ok);
  assert(rebound_begin.identity_valid);
  assert(rebound_begin.progress_valid);
  assert_progress_equal(rebound_begin.progress, begun.progress);
  assert(flash.io_calls() == io_before_repeated_begin);

  // A wire RESUME carries only the transaction ID. When this Store is
  // already active the executor must read its RAM progress instead of
  // calling resume_update(), which deliberately reports Busy in this state.
  auto active_resume = make_action(
      SpeakerAssetsActionKind::ResumeQuery, 202U, 302U);
  active_resume.query_mode =
      SpeakerAssetsResumeQueryMode::ResumeOrRebind;
  active_resume.expected_identity.generation = 999U;
  active_resume.expected_identity.target_bank = SoundBankId::B;
  active_resume.expected_identity.transaction_id =
      begun.progress.identity.transaction_id;
  const auto io_before_active_resume = flash.io_calls();
  const auto resumed_active = run_action(store, active_resume);
  assert(resumed_active.result == SoundStoreResult::Ok);
  assert(resumed_active.progress_valid);
  assert_progress_equal(
      resumed_active.progress, begun.progress);
  assert(flash.io_calls() == io_before_active_resume);

  auto wrong_transaction_resume = active_resume;
  wrong_transaction_resume.token = 203U;
  wrong_transaction_resume.request_id = 303U;
  wrong_transaction_resume.expected_identity.transaction_id[0U] ^=
      0x40U;
  const auto wrong_resume =
      run_action(store, wrong_transaction_resume);
  assert(wrong_resume.result == SoundStoreResult::Ok);
  assert(wrong_resume.identity_valid);
  assert(wrong_resume.outcome_valid);
  assert(wrong_resume.outcome ==
         SpeakerAssetsTransactionOutcome::Unknown);
  assert(!wrong_resume.progress_valid);
  assert(flash.io_calls() == io_before_active_resume);

  auto current = make_action(
      SpeakerAssetsActionKind::ResumeQuery, 204U, 304U);
  current.query_mode =
      SpeakerAssetsResumeQueryMode::CurrentProgress;
  current.expected_identity = begun.progress.identity;
  const auto current_completion = run_action(store, current);
  assert(current_completion.result == SoundStoreResult::Ok);
  assert(current_completion.progress_valid);
  assert_progress_equal(
      current_completion.progress, begun.progress);

  std::uint32_t next_token = 205U;
  for (const auto& wrong :
       wrong_identities(begun.progress.identity)) {
    auto mismatch = current;
    mismatch.token = next_token;
    mismatch.request_id = next_token + 100U;
    mismatch.expected_identity = wrong;
    ++next_token;
    const auto completion = run_action(store, mismatch);
    assert(completion.result ==
           SoundStoreResult::TransactionMismatch);
    assert(!completion.progress_valid);
  }
  assert(flash.io_calls() == io_before_active_resume);

  auto abort = make_action(
      SpeakerAssetsActionKind::Abort, 208U, 308U);
  abort.expected_identity = begun.progress.identity;
  auto wrong_abort = abort;
  wrong_abort.token = 209U;
  wrong_abort.request_id = 309U;
  ++wrong_abort.expected_identity.generation;
  const auto mutations_before_wrong_abort =
      flash.mutation_calls();
  const auto rejected_abort = run_action(store, wrong_abort);
  assert(rejected_abort.result ==
         SoundStoreResult::TransactionMismatch);
  assert(store.update_active());
  assert(flash.mutation_calls() ==
         mutations_before_wrong_abort);

  const auto aborted = run_action(store, abort);
  assert(aborted.result == SoundStoreResult::Ok);
  assert(!aborted.progress_valid);
  assert(!store.update_active());

  auto unknown_query = active_resume;
  unknown_query.token = 211U;
  unknown_query.request_id = 311U;
  const auto unknown_completion =
      run_action(store, unknown_query);
  assert(unknown_completion.result == SoundStoreResult::Ok);
  assert(unknown_completion.identity_valid);
  assert(unknown_completion.outcome_valid);
  assert(unknown_completion.outcome ==
         SpeakerAssetsTransactionOutcome::Unknown);
  assert(!unknown_completion.progress_valid);
}

void writes_reboot_resume_and_commit() {
  MemorySoundBankStorage flash;
  SoundAssetStore first_boot(flash);
  assert(first_boot.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(31U);

  auto current_active = make_action(
      SpeakerAssetsActionKind::QueryCurrentActive, 300U, 400U);
  const auto no_active =
      run_action(first_boot, current_active);
  assert(no_active.result == SoundStoreResult::Ok);
  assert(no_active.current_active_valid);
  assert(!no_active.current_active.valid);
  assert(!no_active.identity_valid);
  assert(!no_active.progress_valid);
  assert(!no_active.outcome_valid);

  auto begin = make_action(
      SpeakerAssetsActionKind::Begin, 301U, 401U);
  begin.plan = &bundle.plan;
  const auto begun = run_action(first_boot, begin);
  assert(begun.result == SoundStoreResult::Ok);
  assert(begun.progress_valid);
  const auto identity = begun.progress.identity;

  auto manifest = make_action(
      SpeakerAssetsActionKind::WriteManifest, 302U, 402U);
  manifest.expected_identity = identity;
  manifest.bytes = bundle.manifest.data();
  manifest.length =
      static_cast<std::uint16_t>(bundle.manifest.size());
  std::uint32_t next_token = 303U;
  for (const auto& wrong : wrong_identities(identity)) {
    auto mismatch = manifest;
    mismatch.token = next_token;
    mismatch.request_id = next_token + 100U;
    mismatch.expected_identity = wrong;
    ++next_token;
    const auto io_before = flash.io_calls();
    const auto completion = run_action(first_boot, mismatch);
    assert(completion.result ==
           SoundStoreResult::TransactionMismatch);
    assert(!completion.progress_valid);
    assert(flash.io_calls() == io_before);
  }

  auto missing_manifest = manifest;
  missing_manifest.token = 306U;
  missing_manifest.request_id = 406U;
  missing_manifest.bytes = nullptr;
  const auto io_before_missing_manifest = flash.io_calls();
  const auto missing_manifest_completion =
      run_action(first_boot, missing_manifest);
  assert(missing_manifest_completion.result ==
         SoundStoreResult::InvalidArgument);
  assert(!missing_manifest_completion.progress_valid);
  assert(flash.io_calls() == io_before_missing_manifest);

  const auto manifest_written =
      run_action(first_boot, manifest);
  assert(manifest_written.result == SoundStoreResult::Ok);
  assert(manifest_written.progress_valid);
  assert(manifest_written.progress.manifest_complete);
  assert(manifest_written.progress.payload_complete_bitmap[0U] ==
         0U);

  auto block = make_action(
      SpeakerAssetsActionKind::WritePayloadBlock, 307U, 407U);
  block.expected_identity = identity;
  block.bytes = bundle.payload.data();
  block.length =
      static_cast<std::uint16_t>(bundle.payload.size());
  block.block_index = 0U;
  next_token = 308U;
  for (const auto& wrong : wrong_identities(identity)) {
    auto mismatch = block;
    mismatch.token = next_token;
    mismatch.request_id = next_token + 100U;
    mismatch.expected_identity = wrong;
    ++next_token;
    const auto io_before = flash.io_calls();
    const auto completion = run_action(first_boot, mismatch);
    assert(completion.result ==
           SoundStoreResult::TransactionMismatch);
    assert(!completion.progress_valid);
    assert(flash.io_calls() == io_before);
  }

  auto missing_block = block;
  missing_block.token = 311U;
  missing_block.request_id = 411U;
  missing_block.length = 0U;
  const auto io_before_missing_block = flash.io_calls();
  const auto missing_block_completion =
      run_action(first_boot, missing_block);
  assert(missing_block_completion.result ==
         SoundStoreResult::InvalidArgument);
  assert(!missing_block_completion.progress_valid);
  assert(flash.io_calls() == io_before_missing_block);

  const auto block_written = run_action(first_boot, block);
  assert(block_written.result == SoundStoreResult::Ok);
  assert(block_written.progress_valid);
  assert(block_written.progress.manifest_complete);
  assert(block_written.progress.payload_complete_bitmap[0U] ==
         0x01U);
  const auto durable_progress = block_written.progress;

  SoundAssetStore after_restart(flash);
  assert(after_restart.scan() == SoundStoreResult::Ok);
  assert(!after_restart.update_active());

  auto resume = make_action(
      SpeakerAssetsActionKind::ResumeQuery, 312U, 412U);
  resume.query_mode =
      SpeakerAssetsResumeQueryMode::ResumeOrRebind;
  resume.expected_identity.transaction_id =
      identity.transaction_id;

  auto wrong_resume = resume;
  wrong_resume.token = 313U;
  wrong_resume.request_id = 413U;
  wrong_resume.expected_identity.transaction_id[0U] ^= 0x20U;
  const auto mutations_before_resume = flash.mutation_calls();
  const auto rejected_resume =
      run_action(after_restart, wrong_resume);
  assert(rejected_resume.result == SoundStoreResult::Ok);
  assert(rejected_resume.identity_valid);
  assert(rejected_resume.outcome_valid);
  assert(rejected_resume.outcome ==
         SpeakerAssetsTransactionOutcome::Unknown);
  assert(!rejected_resume.progress_valid);
  assert(!after_restart.update_active());
  assert(flash.mutation_calls() == mutations_before_resume);

  auto restart_begin = begin;
  restart_begin.token = 317U;
  restart_begin.request_id = 417U;
  const auto rebound_after_restart =
      run_action(after_restart, restart_begin);
  assert(rebound_after_restart.result == SoundStoreResult::Ok);
  assert(rebound_after_restart.identity_valid);
  assert(rebound_after_restart.progress_valid);
  assert_progress_equal(
      rebound_after_restart.progress, durable_progress);
  assert(after_restart.update_active());
  assert(flash.mutation_calls() == mutations_before_resume);

  const auto resumed = run_action(after_restart, resume);
  assert(resumed.result == SoundStoreResult::Ok);
  assert(resumed.progress_valid);
  assert(after_restart.update_active());
  assert_progress_equal(resumed.progress, durable_progress);
  assert(flash.mutation_calls() == mutations_before_resume);

  auto commit = make_action(
      SpeakerAssetsActionKind::Commit, 314U, 414U);
  commit.expected_identity = identity;
  auto wrong_commit = commit;
  wrong_commit.token = 315U;
  wrong_commit.request_id = 415U;
  wrong_commit.expected_identity.target_bank =
      identity.target_bank == SoundBankId::A
          ? SoundBankId::B
          : SoundBankId::A;
  const auto mutations_before_wrong_commit =
      flash.mutation_calls();
  const auto rejected_commit =
      run_action(after_restart, wrong_commit);
  assert(rejected_commit.result ==
         SoundStoreResult::TransactionMismatch);
  assert(after_restart.update_active());
  assert(flash.mutation_calls() ==
         mutations_before_wrong_commit);

  const auto committed = run_action(after_restart, commit);
  assert(committed.result == SoundStoreResult::Ok);
  assert(!committed.progress_valid);
  assert(!after_restart.update_active());

  current_active.token = 318U;
  current_active.request_id = 418U;
  const auto active = run_action(after_restart, current_active);
  assert(active.result == SoundStoreResult::Ok);
  assert(active.current_active_valid);
  assert(active.current_active.valid);
  assert(active.current_active.bank == identity.target_bank);
  assert(active.current_active.generation == identity.generation);
  assert(active.current_active.bundle_sha256 ==
         bundle.plan.bundle_sha256);
  assert(!active.identity_valid);
  assert(!active.progress_valid);
  assert(!active.outcome_valid);

  auto committed_query = resume;
  committed_query.token = 316U;
  committed_query.request_id = 416U;
  const auto committed_outcome =
      run_action(after_restart, committed_query);
  assert(committed_outcome.result == SoundStoreResult::Ok);
  assert(committed_outcome.identity_valid);
  assert(committed_outcome.outcome_valid);
  assert(committed_outcome.outcome ==
         SpeakerAssetsTransactionOutcome::Committed);
  assert(!committed_outcome.progress_valid);
  assert(identity_equal(
      committed_outcome.identity, identity));
  assert(committed_outcome.outcome_manifest_bytes ==
         bundle.plan.manifest_bytes);
  assert(committed_outcome.outcome_payload_bytes ==
         bundle.plan.payload_bytes);
  assert(after_restart.selection().active_valid);
  assert(after_restart.selection().active.generation ==
         identity.generation);
  assert(after_restart.selection().active.bank ==
         identity.target_bank);
  assert(after_restart.selection().active.transaction_id ==
         identity.transaction_id);
  assert(after_restart.selection().active.bundle_sha256 ==
         bundle.plan.bundle_sha256);

  SoundAssetStore final_boot(flash);
  assert(final_boot.scan() == SoundStoreResult::Ok);
  assert(final_boot.selection().active_valid);
  assert(final_boot.selection().active.generation ==
         identity.generation);
  assert(final_boot.selection().active.transaction_id ==
         identity.transaction_id);
  assert(final_boot.selection().active.bundle_sha256 ==
         bundle.plan.bundle_sha256);
}

}  // namespace

int main() {
  invalid_actions_do_not_touch_the_store();
  discard_invalid_staging_is_explicit_and_safe();
  begin_active_resume_current_identity_and_abort();
  writes_reboot_resume_and_commit();
  return 0;
}
