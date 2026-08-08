#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "speaker_assets/sound_asset_crypto.h"
#include "speaker_assets/sound_asset_store.h"

namespace {

using easy_input::speaker_assets::SoundAssetStore;
using easy_input::speaker_assets::SoundBankId;
using easy_input::speaker_assets::SoundBankSnapshot;
using easy_input::speaker_assets::SoundBankStorage;
using easy_input::speaker_assets::SoundBundlePlan;
using easy_input::speaker_assets::SoundReadLease;
using easy_input::speaker_assets::SoundSha256;
using easy_input::speaker_assets::SoundSha256Digest;
using easy_input::speaker_assets::SoundStorageIoResult;
using easy_input::speaker_assets::SoundStoreResult;
using easy_input::speaker_assets::SoundTransactionOutcome;
using easy_input::speaker_assets::SoundTransactionState;
using easy_input::speaker_assets::SoundUpdateIdentity;
using easy_input::speaker_assets::kSoundBankSize;
using easy_input::speaker_assets::kSoundCommitOffset;
using easy_input::speaker_assets::kSoundJournalOffset;
using easy_input::speaker_assets::kSoundManifestOffset;
using easy_input::speaker_assets::kSoundPayloadBlockSize;
using easy_input::speaker_assets::kSoundPayloadMaxSize;
using easy_input::speaker_assets::kSoundPayloadOffset;
using easy_input::speaker_assets::kSoundSectorSize;
using easy_input::speaker_assets::kSoundStagingHeaderOffset;
using easy_input::speaker_assets::select_sound_banks;
using easy_input::speaker_assets::sound_crc32_iso_hdlc;
using easy_input::speaker_assets::sound_digest_equal;

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
  for (std::size_t index = 0; index < 4U; ++index) {
    output[index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

SoundSha256Digest sha256(const std::uint8_t* data, std::size_t length) {
  SoundSha256 hash;
  assert(hash.update(data, length));
  return hash.finish();
}

struct TestBundle {
  std::vector<std::uint8_t> manifest;
  std::vector<std::uint8_t> payload;
  SoundBundlePlan plan{};
};

TestBundle make_bundle(
    std::uint8_t seed,
    std::uint64_t base_generation = 0U,
    const SoundSha256Digest& base_digest = {}) {
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
  write_le32(manifest + 8U,
             static_cast<std::uint32_t>(bundle.manifest.size()));
  write_le32(manifest + 12U,
             static_cast<std::uint32_t>(bundle.payload.size()));
  write_le16(manifest + 16U, 1U);
  write_le16(manifest + 18U, 1U);

  const auto resource_digest =
      sha256(bundle.payload.data(), bundle.payload.size());
  std::copy(resource_digest.begin(),
            resource_digest.end(),
            bundle.manifest.begin() + 32U);
  write_le32(manifest + 64U, 0U);
  write_le32(manifest + 68U,
             static_cast<std::uint32_t>(bundle.payload.size()));
  write_le32(manifest + 72U, 5U);
  write_le16(manifest + 76U, 1U);
  manifest[78U] = 1U;
  manifest[79U] = 1U;
  manifest[80U] = 1U;
  manifest[81U] = 0U;
  write_le16(manifest + 82U, 0U);

  bundle.plan.base_generation = base_generation;
  bundle.plan.base_bundle_sha256 = base_digest;
  bundle.plan.manifest_bytes =
      static_cast<std::uint32_t>(bundle.manifest.size());
  bundle.plan.payload_bytes =
      static_cast<std::uint32_t>(bundle.payload.size());
  bundle.plan.manifest_crc32 =
      sound_crc32_iso_hdlc(
          bundle.manifest.data(), bundle.manifest.size());
  bundle.plan.manifest_sha256 =
      sha256(bundle.manifest.data(), bundle.manifest.size());
  bundle.plan.payload_block_crc32[0] =
      sound_crc32_iso_hdlc(
          bundle.payload.data(), bundle.payload.size());

  SoundSha256 bundle_hash;
  std::array<std::uint8_t, 4> length{};
  assert(bundle_hash.update(kBundleDomain.data(), kBundleDomain.size()));
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

void rebuild_plan_digests(TestBundle* bundle) {
  assert(bundle != nullptr);
  bundle->plan.payload_block_crc32.fill(0U);
  for (std::size_t offset = 0U, block_index = 0U;
       offset < bundle->payload.size();
       offset += kSoundPayloadBlockSize, ++block_index) {
    const auto block_bytes = std::min<std::size_t>(
        kSoundPayloadBlockSize, bundle->payload.size() - offset);
    bundle->plan.payload_block_crc32[block_index] =
        sound_crc32_iso_hdlc(
            bundle->payload.data() + offset, block_bytes);
  }
  bundle->plan.manifest_crc32 =
      sound_crc32_iso_hdlc(
          bundle->manifest.data(), bundle->manifest.size());
  bundle->plan.manifest_sha256 =
      sha256(bundle->manifest.data(), bundle->manifest.size());

  SoundSha256 bundle_hash;
  std::array<std::uint8_t, 4> length{};
  assert(bundle_hash.update(kBundleDomain.data(), kBundleDomain.size()));
  write_le32(length.data(), bundle->plan.manifest_bytes);
  assert(bundle_hash.update(length.data(), length.size()));
  assert(bundle_hash.update(
      bundle->manifest.data(), bundle->manifest.size()));
  write_le32(length.data(), bundle->plan.payload_bytes);
  assert(bundle_hash.update(length.data(), length.size()));
  assert(bundle_hash.update(
      bundle->payload.data(), bundle->payload.size()));
  bundle->plan.bundle_sha256 = bundle_hash.finish();
}

void refresh_resource_digest(TestBundle* bundle) {
  assert(bundle != nullptr);
  assert(bundle->manifest.size() >= 64U);
  const auto resource_digest =
      sha256(bundle->payload.data(), bundle->payload.size());
  std::copy(resource_digest.begin(),
            resource_digest.end(),
            bundle->manifest.begin() + 32U);
  rebuild_plan_digests(bundle);
}

std::vector<std::uint8_t> make_eiad_asset(
    std::uint32_t decoded_samples,
    std::uint8_t seed) {
  assert(decoded_samples != 0U);
  const auto frame_count = static_cast<std::uint16_t>(
      1U + (decoded_samples - 1U) / 480U);
  std::vector<std::uint8_t> payload(20U, 0U);
  auto* header = payload.data();
  header[0] = 'E';
  header[1] = 'I';
  header[2] = 'A';
  header[3] = 'D';
  header[4] = 1U;
  header[5] = 1U;
  write_le32(header + 6U, 48000U);
  write_le16(header + 10U, 480U);
  write_le16(header + 12U, frame_count);
  write_le32(header + 14U, decoded_samples);
  write_le16(header + 18U, 20U);

  std::uint32_t remaining = decoded_samples;
  for (std::uint16_t frame_index = 0U;
       frame_index < frame_count;
       ++frame_index) {
    const auto samples = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(remaining, 480U));
    const auto cursor = payload.size();
    const auto encoded_bytes =
        static_cast<std::size_t>(samples) / 2U;
    payload.resize(cursor + 6U + encoded_bytes, 0U);
    write_le16(payload.data() + cursor, samples);
    write_le16(payload.data() + cursor + 2U,
               static_cast<std::uint16_t>(seed + frame_index));
    payload[cursor + 4U] =
        static_cast<std::uint8_t>((seed + frame_index) % 89U);
    remaining -= samples;
  }
  assert(remaining == 0U);
  return payload;
}

TestBundle make_two_block_eiad_bundle(
    std::uint8_t seed,
    std::uint64_t base_generation,
    const SoundSha256Digest& base_digest) {
  constexpr std::uint32_t kDecodedSamples = 13369U;
  constexpr std::uint16_t kFrameCount = 28U;
  auto bundle = make_bundle(seed, base_generation, base_digest);
  bundle.payload = make_eiad_asset(kDecodedSamples, seed);
  assert(bundle.payload.size() == 6872U);

  write_le32(bundle.manifest.data() + 12U,
             static_cast<std::uint32_t>(bundle.payload.size()));
  write_le32(bundle.manifest.data() + 68U,
             static_cast<std::uint32_t>(bundle.payload.size()));
  write_le32(bundle.manifest.data() + 72U, kDecodedSamples);
  write_le16(bundle.manifest.data() + 76U, kFrameCount);
  bundle.plan.payload_bytes =
      static_cast<std::uint32_t>(bundle.payload.size());
  refresh_resource_digest(&bundle);
  return bundle;
}

TestBundle make_full_mapping_boundary_bundle() {
  constexpr std::uint32_t kBootSamples = 384000U;
  constexpr std::uint32_t kActionSamples = 96000U;
  const auto boot = make_eiad_asset(kBootSamples, 37U);
  const auto action = make_eiad_asset(kActionSamples, 73U);

  TestBundle bundle{};
  bundle.payload.reserve(boot.size() + action.size());
  bundle.payload.insert(bundle.payload.end(), boot.begin(), boot.end());
  bundle.payload.insert(
      bundle.payload.end(), action.begin(), action.end());

  constexpr std::uint16_t kResourceCount = 2U;
  constexpr std::uint16_t kMappingCount = 12U;
  constexpr std::size_t kManifestBytes =
      32U + kResourceCount * 48U + kMappingCount * 4U;
  bundle.manifest.assign(kManifestBytes, 0U);
  auto* manifest = bundle.manifest.data();
  manifest[0] = 'E';
  manifest[1] = 'I';
  manifest[2] = 'S';
  manifest[3] = 'M';
  write_le16(manifest + 4U, 1U);
  write_le16(manifest + 6U, 32U);
  write_le32(manifest + 8U,
             static_cast<std::uint32_t>(kManifestBytes));
  write_le32(manifest + 12U,
             static_cast<std::uint32_t>(bundle.payload.size()));
  write_le16(manifest + 16U, kResourceCount);
  write_le16(manifest + 18U, kMappingCount);

  const auto write_resource =
      [&bundle](std::size_t resource_index,
                std::size_t payload_offset,
                const std::vector<std::uint8_t>& asset,
                std::uint32_t decoded_samples) {
        const auto record_offset = 32U + resource_index * 48U;
        const auto digest = sha256(asset.data(), asset.size());
        std::copy(digest.begin(),
                  digest.end(),
                  bundle.manifest.data() + record_offset);
        write_le32(bundle.manifest.data() + record_offset + 32U,
                   static_cast<std::uint32_t>(payload_offset));
        write_le32(bundle.manifest.data() + record_offset + 36U,
                   static_cast<std::uint32_t>(asset.size()));
        write_le32(bundle.manifest.data() + record_offset + 40U,
                   decoded_samples);
        write_le16(bundle.manifest.data() + record_offset + 44U,
                   static_cast<std::uint16_t>(
                       1U + (decoded_samples - 1U) / 480U));
        bundle.manifest[record_offset + 46U] = 1U;
        bundle.manifest[record_offset + 47U] = 1U;
      };
  write_resource(0U, 0U, boot, kBootSamples);
  write_resource(1U, boot.size(), action, kActionSamples);

  const auto mapping_offset = 32U + kResourceCount * 48U;
  const auto write_mapping =
      [&bundle](std::size_t mapping_index,
                std::uint8_t trigger,
                std::uint8_t index,
                std::uint16_t resource_index) {
        const auto offset = mapping_offset + mapping_index * 4U;
        bundle.manifest[offset] = trigger;
        bundle.manifest[offset + 1U] = index;
        write_le16(bundle.manifest.data() + offset + 2U,
                   resource_index);
      };
  write_mapping(0U, 1U, 0U, 0U);
  for (std::size_t key_index = 1U; key_index <= 8U; ++key_index) {
    write_mapping(
        key_index, 2U, static_cast<std::uint8_t>(key_index), 1U);
  }
  write_mapping(9U, 3U, 0U, 1U);
  write_mapping(10U, 4U, 0U, 1U);
  write_mapping(11U, 5U, 0U, 1U);

  bundle.plan.manifest_bytes =
      static_cast<std::uint32_t>(bundle.manifest.size());
  bundle.plan.payload_bytes =
      static_cast<std::uint32_t>(bundle.payload.size());
  rebuild_plan_digests(&bundle);
  return bundle;
}

TestBundle make_payload_boundary_plan(std::size_t payload_bytes,
                                      std::uint8_t seed) {
  assert(payload_bytes <= kSoundPayloadMaxSize);
  auto bundle = make_bundle(seed);
  bundle.payload.resize(payload_bytes);
  for (std::size_t index = 0U; index < bundle.payload.size(); ++index) {
    bundle.payload[index] =
        static_cast<std::uint8_t>(seed + index * 17U);
  }
  bundle.plan.payload_bytes =
      static_cast<std::uint32_t>(bundle.payload.size());
  write_le32(bundle.manifest.data() + 12U,
             bundle.plan.payload_bytes);
  write_le32(bundle.manifest.data() + 68U,
             bundle.plan.payload_bytes);
  refresh_resource_digest(&bundle);
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
    if (fail_all_reads_) {
      return SoundStorageIoResult::Unavailable;
    }
    if (read_failure_armed_ && bank == read_failure_bank_ &&
        offset == read_failure_offset_) {
      read_failure_armed_ = false;
      return SoundStorageIoResult::IoError;
    }
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
    if (source == nullptr && length != 0U) {
      return SoundStorageIoResult::InvalidArgument;
    }
    auto* destination = mutable_range(bank, offset, length);
    if (destination == nullptr) {
      return SoundStorageIoResult::OutOfBounds;
    }

    std::size_t programmed = length;
    bool fail_after_write = false;
    ++mutation_calls_;
    if (mutation_cut_countdown_ != 0U) {
      --mutation_cut_countdown_;
      if (mutation_cut_countdown_ == 0U) {
        programmed = length / 2U;
        mutation_cut_triggered_ = true;
        fail_after_write = true;
      }
    } else if (partial_write_armed_ && bank == partial_write_bank_ &&
        offset == partial_write_offset_) {
      programmed = std::min(length, partial_write_bytes_);
      partial_write_armed_ = false;
      fail_after_write = true;
    }
    for (std::size_t index = 0; index < programmed; ++index) {
      if ((destination[index] & source[index]) != source[index]) {
        return SoundStorageIoResult::IoError;
      }
    }
    for (std::size_t index = 0; index < programmed; ++index) {
      destination[index] &= source[index];
    }
    return fail_after_write
               ? SoundStorageIoResult::IoError
               : SoundStorageIoResult::Ok;
  }

  SoundStorageIoResult erase(SoundBankId bank,
                             std::uint32_t offset,
                             std::size_t length) override {
    if ((offset % kSoundSectorSize) != 0U ||
        (length % kSoundSectorSize) != 0U) {
      return SoundStorageIoResult::NotAligned;
    }
    auto* destination = mutable_range(bank, offset, length);
    if (destination == nullptr) {
      return SoundStorageIoResult::OutOfBounds;
    }
    if (erase_failure_armed_ && bank == erase_failure_bank_ &&
        offset == erase_failure_offset_) {
      erase_failure_armed_ = false;
      return SoundStorageIoResult::IoError;
    }
    ++mutation_calls_;
    std::size_t erased = length;
    bool fail_after_erase = false;
    if (mutation_cut_countdown_ != 0U) {
      --mutation_cut_countdown_;
      if (mutation_cut_countdown_ == 0U) {
        erased = length / 2U;
        mutation_cut_triggered_ = true;
        fail_after_erase = true;
      }
    }
    std::fill(destination, destination + erased, 0xFFU);
    ++erase_calls_;
    return fail_after_erase
               ? SoundStorageIoResult::IoError
               : SoundStorageIoResult::Ok;
  }

  void arm_partial_write(SoundBankId bank,
                         std::uint32_t offset,
                         std::size_t bytes) {
    partial_write_armed_ = true;
    partial_write_bank_ = bank;
    partial_write_offset_ = offset;
    partial_write_bytes_ = bytes;
  }

  void arm_mutation_cut(std::size_t mutation_ordinal) {
    assert(mutation_ordinal != 0U);
    mutation_cut_countdown_ = mutation_ordinal;
    mutation_cut_triggered_ = false;
  }

  bool mutation_cut_triggered() const {
    return mutation_cut_triggered_;
  }

  std::size_t mutation_calls() const {
    return mutation_calls_;
  }

  std::size_t read_calls() const {
    return read_calls_;
  }

  void set_fail_all_reads(bool fail) {
    fail_all_reads_ = fail;
  }

  void arm_read_failure(SoundBankId bank, std::uint32_t offset) {
    read_failure_armed_ = true;
    read_failure_bank_ = bank;
    read_failure_offset_ = offset;
  }

  void arm_erase_failure(SoundBankId bank, std::uint32_t offset) {
    erase_failure_armed_ = true;
    erase_failure_bank_ = bank;
    erase_failure_offset_ = offset;
  }

  void corrupt(SoundBankId bank,
               std::uint32_t offset,
               std::uint8_t value) {
    auto* destination = mutable_range(bank, offset, 1U);
    assert(destination != nullptr);
    *destination = value;
  }

  std::size_t erase_calls() const {
    return erase_calls_;
  }

 private:
  const std::uint8_t* range(SoundBankId bank,
                            std::uint32_t offset,
                            std::size_t length) const {
    if ((bank != SoundBankId::A && bank != SoundBankId::B) ||
        offset > kSoundBankSize ||
        length > static_cast<std::size_t>(kSoundBankSize - offset)) {
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
  bool fail_all_reads_ = false;
  bool read_failure_armed_ = false;
  SoundBankId read_failure_bank_ = SoundBankId::A;
  std::uint32_t read_failure_offset_ = 0U;
  bool erase_failure_armed_ = false;
  SoundBankId erase_failure_bank_ = SoundBankId::A;
  std::uint32_t erase_failure_offset_ = 0U;
  bool partial_write_armed_ = false;
  SoundBankId partial_write_bank_ = SoundBankId::A;
  std::uint32_t partial_write_offset_ = 0U;
  std::size_t partial_write_bytes_ = 0U;
  std::size_t mutation_cut_countdown_ = 0U;
  bool mutation_cut_triggered_ = false;
  std::size_t read_calls_ = 0U;
  std::size_t mutation_calls_ = 0U;
  std::size_t erase_calls_ = 0U;
};

void write_bundle(SoundAssetStore* store, const TestBundle& bundle) {
  assert(store != nullptr);
  assert(store->write_manifest(
             bundle.manifest.data(), bundle.manifest.size()) ==
         SoundStoreResult::Ok);
  for (std::size_t offset = 0U, block_index = 0U;
       offset < bundle.payload.size();
       offset += kSoundPayloadBlockSize, ++block_index) {
    const auto block_bytes = std::min<std::size_t>(
        kSoundPayloadBlockSize, bundle.payload.size() - offset);
    assert(store->write_payload_block(
               block_index,
               bundle.payload.data() + offset,
               block_bytes) ==
           SoundStoreResult::Ok);
  }
}

void install_bundle(SoundAssetStore* store, const TestBundle& bundle) {
  assert(store != nullptr);
  SoundUpdateIdentity identity{};
  assert(store->begin_or_resume_update(bundle.plan, &identity) ==
         SoundStoreResult::Ok);
  assert(identity.generation == bundle.plan.base_generation + 1U);
  assert(std::any_of(identity.transaction_id.begin(),
                     identity.transaction_id.end(),
                     [](std::uint8_t value) { return value != 0U; }));
  write_bundle(store, bundle);
  assert(store->commit_update() == SoundStoreResult::Ok);
}

void crypto_matches_standard_vectors() {
  constexpr std::array<std::uint8_t, 32> kEmptyDigest{{
      0xE3, 0xB0, 0xC4, 0x42, 0x98, 0xFC, 0x1C, 0x14,
      0x9A, 0xFB, 0xF4, 0xC8, 0x99, 0x6F, 0xB9, 0x24,
      0x27, 0xAE, 0x41, 0xE4, 0x64, 0x9B, 0x93, 0x4C,
      0xA4, 0x95, 0x99, 0x1B, 0x78, 0x52, 0xB8, 0x55,
  }};
  constexpr std::array<std::uint8_t, 32> kAbcDigest{{
      0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
      0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
      0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
      0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
  }};
  constexpr std::array<std::uint8_t, 32> kNist56Digest{{
      0x24, 0x8D, 0x6A, 0x61, 0xD2, 0x06, 0x38, 0xB8,
      0xE5, 0xC0, 0x26, 0x93, 0x0C, 0x3E, 0x60, 0x39,
      0xA3, 0x3C, 0xE4, 0x59, 0x64, 0xFF, 0x21, 0x67,
      0xF6, 0xEC, 0xED, 0xD4, 0x19, 0xDB, 0x06, 0xC1,
  }};
  SoundSha256 empty_hash;
  assert(empty_hash.finish() == kEmptyDigest);
  assert(empty_hash.finish() == kEmptyDigest);

  constexpr std::array<std::uint8_t, 3> kAbc{{'a', 'b', 'c'}};
  SoundSha256 abc_hash;
  assert(abc_hash.update(kAbc.data(), 1U));
  assert(abc_hash.update(kAbc.data() + 1U, 2U));
  assert(abc_hash.finish() == kAbcDigest);
  assert(!abc_hash.update(kAbc.data(), kAbc.size()));

  constexpr std::array<std::uint8_t, 56> kNist56{{
      'a', 'b', 'c', 'd', 'b', 'c', 'd', 'e',
      'c', 'd', 'e', 'f', 'd', 'e', 'f', 'g',
      'e', 'f', 'g', 'h', 'f', 'g', 'h', 'i',
      'g', 'h', 'i', 'j', 'h', 'i', 'j', 'k',
      'i', 'j', 'k', 'l', 'j', 'k', 'l', 'm',
      'k', 'l', 'm', 'n', 'l', 'm', 'n', 'o',
      'm', 'n', 'o', 'p', 'n', 'o', 'p', 'q',
  }};
  SoundSha256 nist56_hash;
  assert(nist56_hash.update(kNist56.data(), 13U));
  assert(nist56_hash.update(
      kNist56.data() + 13U, kNist56.size() - 13U));
  assert(nist56_hash.finish() == kNist56Digest);

  constexpr std::array<std::uint8_t, 9> kCrcInput{{
      '1', '2', '3', '4', '5', '6', '7', '8', '9',
  }};
  assert(sound_crc32_iso_hdlc(
             kCrcInput.data(), kCrcInput.size()) ==
         UINT32_C(0xCBF43926));
}

void firmware_assigns_stable_bound_transaction_identity() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);

  auto invalid = make_bundle(29U);
  invalid.plan.manifest_bytes = 0U;
  SoundUpdateIdentity identity{};
  identity.generation = 99U;
  identity.target_bank = SoundBankId::B;
  identity.transaction_id.fill(0xA5U);
  assert(store.begin_or_resume_update(invalid.plan, &identity) ==
         SoundStoreResult::InvalidArgument);
  assert(identity.generation == 0U);
  assert(identity.target_bank == SoundBankId::A);
  assert(std::none_of(identity.transaction_id.begin(),
                      identity.transaction_id.end(),
                      [](std::uint8_t value) { return value != 0U; }));

  const auto first = make_bundle(30U);
  assert(store.begin_or_resume_update(first.plan, &identity) ==
         SoundStoreResult::Ok);
  const auto first_transaction = identity.transaction_id;
  assert(identity.generation == 1U);
  assert(identity.target_bank == SoundBankId::A);
  assert(store.abort_update() == SoundStoreResult::Ok);

  identity = {};
  assert(store.begin_or_resume_update(first.plan, &identity) ==
         SoundStoreResult::Ok);
  assert(identity.transaction_id == first_transaction);
  assert(store.abort_update() == SoundStoreResult::Ok);

  const auto different_bundle = make_bundle(31U);
  identity = {};
  assert(store.begin_or_resume_update(different_bundle.plan, &identity) ==
         SoundStoreResult::Ok);
  assert(identity.transaction_id != first_transaction);
  assert(store.abort_update() == SoundStoreResult::Ok);
}

void begin_ack_loss_is_idempotent_and_survives_restart() {
  MemorySoundBankStorage flash;
  SoundAssetStore first_boot(flash);
  assert(first_boot.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(44U);

  SoundUpdateIdentity begin_identity{};
  assert(first_boot.begin_or_resume_update(
             bundle.plan, &begin_identity) ==
         SoundStoreResult::Ok);
  assert(begin_identity.generation == 1U);
  assert(begin_identity.target_bank == SoundBankId::A);

  const auto reads_after_begin = flash.read_calls();
  const auto mutations_after_begin = flash.mutation_calls();
  SoundUpdateIdentity retry_identity{};
  assert(first_boot.begin_or_resume_update(
             bundle.plan, &retry_identity) ==
         SoundStoreResult::Ok);
  assert(retry_identity.generation == begin_identity.generation);
  assert(retry_identity.target_bank == begin_identity.target_bank);
  assert(retry_identity.transaction_id ==
         begin_identity.transaction_id);
  assert(flash.read_calls() == reads_after_begin);
  assert(flash.mutation_calls() == mutations_after_begin);

  // CRC fields are deliberately not part of transaction-ID derivation. They
  // must still participate in the exact in-memory plan comparison.
  auto changed_plan = bundle.plan;
  changed_plan.manifest_crc32 ^= 1U;
  retry_identity.generation = 99U;
  retry_identity.target_bank = SoundBankId::B;
  retry_identity.transaction_id.fill(0xA5U);
  assert(first_boot.begin_or_resume_update(
             changed_plan, &retry_identity) ==
         SoundStoreResult::Busy);
  assert(retry_identity.generation == 0U);
  assert(retry_identity.target_bank == SoundBankId::A);
  assert(std::none_of(
      retry_identity.transaction_id.begin(),
      retry_identity.transaction_id.end(),
      [](std::uint8_t value) { return value != 0U; }));
  assert(flash.read_calls() == reads_after_begin);
  assert(flash.mutation_calls() == mutations_after_begin);

  assert(first_boot.write_manifest(
             bundle.manifest.data(), bundle.manifest.size()) ==
         SoundStoreResult::Ok);

  // A disconnect/reboot loses all RAM state. A valid but different durable
  // staging transaction must not be erased to make way for a new plan.
  SoundAssetStore after_restart(flash);
  assert(after_restart.scan() == SoundStoreResult::Ok);
  const auto different_bundle = make_bundle(45U);
  const auto mutations_before_foreign =
      flash.mutation_calls();
  retry_identity = {};
  assert(after_restart.begin_or_resume_update(
             different_bundle.plan, &retry_identity) ==
         SoundStoreResult::Busy);
  assert(flash.mutation_calls() == mutations_before_foreign);
  assert(!after_restart.update_active());

  const auto mutations_before_resume =
      flash.mutation_calls();
  retry_identity = {};
  assert(after_restart.begin_or_resume_update(
             bundle.plan, &retry_identity) ==
         SoundStoreResult::Ok);
  assert(retry_identity.generation == begin_identity.generation);
  assert(retry_identity.target_bank == begin_identity.target_bank);
  assert(retry_identity.transaction_id ==
         begin_identity.transaction_id);
  assert(flash.mutation_calls() == mutations_before_resume);

  easy_input::speaker_assets::SoundUpdateProgress progress{};
  assert(after_restart.update_progress(&progress) ==
         SoundStoreResult::Ok);
  assert(progress.manifest_complete);
  assert(after_restart.abort_update() == SoundStoreResult::Ok);
}

void begin_or_resume_never_erases_corrupt_or_unreadable_staging() {
  {
    MemorySoundBankStorage flash;
    flash.corrupt(
        SoundBankId::A, kSoundStagingHeaderOffset, 'E');
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    const auto bundle = make_bundle(46U);
    const auto mutations_before = flash.mutation_calls();
    SoundUpdateIdentity identity{};
    assert(store.begin_or_resume_update(bundle.plan, &identity) ==
           SoundStoreResult::InvalidStaging);
    assert(flash.mutation_calls() == mutations_before);
    assert(!store.update_active());
  }

  {
    MemorySoundBankStorage flash;
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    const auto bundle = make_bundle(47U);
    flash.arm_read_failure(
        SoundBankId::A, kSoundStagingHeaderOffset);
    const auto mutations_before = flash.mutation_calls();
    SoundUpdateIdentity identity{};
    assert(store.begin_or_resume_update(bundle.plan, &identity) ==
           SoundStoreResult::IoError);
    assert(flash.mutation_calls() == mutations_before);
    assert(!store.update_active());
  }
}

void begin_reclaims_stale_staging_after_torn_committed_reclamation() {
  MemorySoundBankStorage flash;
  SoundAssetStore first_boot(flash);
  assert(first_boot.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(64U);
  install_bundle(&first_boot, first);
  const auto second = make_bundle(
      65U,
      first_boot.selection().active.generation,
      first_boot.selection().active.bundle_sha256);
  install_bundle(&first_boot, second);
  assert(first_boot.selection().active.bank == SoundBankId::B);
  assert(first_boot.selection().active.generation == 2U);
  assert(first_boot.selection().bank_a.valid);

  const auto third = make_bundle(
      66U,
      first_boot.selection().active.generation,
      first_boot.selection().active.bundle_sha256);
  flash.arm_mutation_cut(1U);
  SoundUpdateIdentity identity{};
  assert(first_boot.begin_or_resume_update(third.plan, &identity) ==
         SoundStoreResult::IoError);
  assert(flash.mutation_cut_triggered());
  assert(!first_boot.selection().active_valid);
  assert(!first_boot.update_active());

  // The torn final-sector erase invalidated A's committed marker but left its
  // old generation-1 staging header intact.
  std::array<std::uint8_t, 4> staging_magic{};
  assert(flash.read(
             SoundBankId::A,
             kSoundStagingHeaderOffset,
             staging_magic.data(),
             staging_magic.size()) ==
         SoundStorageIoResult::Ok);
  assert(staging_magic ==
         (std::array<std::uint8_t, 4>{{'E', 'I', 'S', 'H'}}));

  SoundAssetStore after_restart(flash);
  assert(after_restart.scan() == SoundStoreResult::Ok);
  assert(after_restart.selection().active_valid);
  assert(after_restart.selection().active.bank == SoundBankId::B);
  assert(after_restart.selection().active.generation == 2U);
  assert(!after_restart.selection().bank_a.valid);

  // B/gen2 proves A's valid gen1 staging is stale. A normal BEGIN may finish
  // reclaiming it instead of returning Busy forever.
  const auto mutations_before_retry = flash.mutation_calls();
  identity = {};
  assert(after_restart.begin_or_resume_update(third.plan, &identity) ==
         SoundStoreResult::Ok);
  assert(identity.target_bank == SoundBankId::A);
  assert(identity.generation == 3U);
  assert(flash.mutation_calls() > mutations_before_retry);
  assert(after_restart.abort_update() == SoundStoreResult::Ok);
}

void invalid_staging_discard_is_explicit_safe_and_idempotent() {
  {
    MemorySoundBankStorage flash;
    flash.corrupt(
        SoundBankId::A, kSoundStagingHeaderOffset, 'E');
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    const auto bundle = make_bundle(51U);

    auto invalid_plan = bundle.plan;
    invalid_plan.manifest_bytes = 0U;
    const auto mutations_before_invalid_plan =
        flash.mutation_calls();
    assert(store.discard_invalid_staging(invalid_plan) ==
           SoundStoreResult::InvalidArgument);
    assert(flash.mutation_calls() ==
           mutations_before_invalid_plan);

    const auto erase_calls_before = flash.erase_calls();
    const auto mutations_before = flash.mutation_calls();
    assert(store.discard_invalid_staging(bundle.plan) ==
           SoundStoreResult::Ok);
    assert(flash.erase_calls() == erase_calls_before + 1U);
    assert(flash.mutation_calls() == mutations_before + 1U);
    assert(!store.selection().active_valid);
    assert(!store.selection().split_brain);

    // Repeating the repair against an erased header is a true no-op.
    const auto reads_before_retry = flash.read_calls();
    const auto mutations_before_retry = flash.mutation_calls();
    assert(store.discard_invalid_staging(bundle.plan) ==
           SoundStoreResult::Ok);
    assert(flash.read_calls() == reads_before_retry + 1U);
    assert(flash.mutation_calls() == mutations_before_retry);

    // The repaired target is immediately usable by the ordinary BEGIN path.
    SoundUpdateIdentity identity{};
    assert(store.begin_or_resume_update(bundle.plan, &identity) ==
           SoundStoreResult::Ok);
    assert(identity.target_bank == SoundBankId::A);
    assert(identity.generation == 1U);
    assert(store.abort_update() == SoundStoreResult::Ok);
  }

  {
    MemorySoundBankStorage flash;
    const auto staged = make_bundle(52U);
    SoundUpdateIdentity staged_identity{};
    {
      SoundAssetStore first_boot(flash);
      assert(first_boot.scan() == SoundStoreResult::Ok);
      assert(first_boot.begin_or_resume_update(
                 staged.plan, &staged_identity) ==
             SoundStoreResult::Ok);
    }

    // A valid durable staging transaction, including a foreign plan, is never
    // erased by the repair operation.
    SoundAssetStore after_restart(flash);
    assert(after_restart.scan() == SoundStoreResult::Ok);
    const auto foreign = make_bundle(53U);
    const auto mutations_before = flash.mutation_calls();
    assert(after_restart.discard_invalid_staging(foreign.plan) ==
           SoundStoreResult::Busy);
    assert(flash.mutation_calls() == mutations_before);
    assert(after_restart.discard_invalid_staging(staged.plan) ==
           SoundStoreResult::Busy);
    assert(flash.mutation_calls() == mutations_before);

    SoundUpdateIdentity resumed{};
    assert(after_restart.resume_update(
               staged_identity.transaction_id, &resumed) ==
           SoundStoreResult::Ok);
    assert(after_restart.abort_update() == SoundStoreResult::Ok);
  }

  {
    MemorySoundBankStorage flash;
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    const auto bundle = make_bundle(54U);
    SoundUpdateIdentity identity{};
    assert(store.begin_or_resume_update(bundle.plan, &identity) ==
           SoundStoreResult::Ok);
    const auto mutations_before = flash.mutation_calls();
    assert(store.discard_invalid_staging(bundle.plan) ==
           SoundStoreResult::Busy);
    assert(flash.mutation_calls() == mutations_before);
    assert(store.abort_update() == SoundStoreResult::Ok);
  }
}

void invalid_staging_discard_respects_pins_and_io_failures() {
  {
    MemorySoundBankStorage flash;
    SoundAssetStore first_boot(flash);
    assert(first_boot.scan() == SoundStoreResult::Ok);
    const auto first = make_bundle(60U);
    install_bundle(&first_boot, first);
    const auto second = make_bundle(
        61U,
        first_boot.selection().active.generation,
        first_boot.selection().active.bundle_sha256);
    install_bundle(&first_boot, second);
    assert(first_boot.selection().active.bank == SoundBankId::B);
    assert(first_boot.selection().bank_a.valid);

    // Bank A remains a valid committed history snapshot. Its staging sector
    // is not part of committed-bank validation, so damage there must never
    // turn RECOVER into permission to erase the committed bank.
    flash.corrupt(
        SoundBankId::A, kSoundStagingHeaderOffset, 0x00U);
    SoundAssetStore after_restart(flash);
    assert(after_restart.scan() == SoundStoreResult::Ok);
    assert(after_restart.selection().bank_a.valid);
    assert(after_restart.selection().bank_b.valid);
    assert(after_restart.selection().active.bank == SoundBankId::B);

    const auto third = make_bundle(
        62U,
        after_restart.selection().active.generation,
        after_restart.selection().active.bundle_sha256);
    const auto reads_before = flash.read_calls();
    const auto erases_before = flash.erase_calls();
    const auto mutations_before = flash.mutation_calls();
    assert(after_restart.discard_invalid_staging(third.plan) ==
           SoundStoreResult::Busy);
    assert(flash.read_calls() == reads_before);
    assert(flash.erase_calls() == erases_before);
    assert(flash.mutation_calls() == mutations_before);
    assert(after_restart.selection().bank_a.valid);
    assert(after_restart.selection().bank_b.valid);
    assert(after_restart.selection().active.bank == SoundBankId::B);
  }

  {
    MemorySoundBankStorage flash;
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    const auto first = make_bundle(55U);
    install_bundle(&store, first);

    SoundReadLease old_audio{};
    assert(store.acquire_active_read(&old_audio) ==
           SoundStoreResult::Ok);
    assert(old_audio.bank == SoundBankId::A);

    const auto second = make_bundle(
        56U,
        store.selection().active.generation,
        store.selection().active.bundle_sha256);
    install_bundle(&store, second);
    assert(store.selection().active.bank == SoundBankId::B);

    const auto third = make_bundle(
        57U,
        store.selection().active.generation,
        store.selection().active.bundle_sha256);
    flash.corrupt(
        SoundBankId::A, kSoundStagingHeaderOffset, 0x00U);
    const auto mutations_before = flash.mutation_calls();
    assert(store.discard_invalid_staging(third.plan) ==
           SoundStoreResult::BankPinned);
    assert(flash.mutation_calls() == mutations_before);
    assert(store.selection().active.bank == SoundBankId::B);

    assert(store.release_read(old_audio) == SoundStoreResult::Ok);
    const auto mutations_before_committed_reject =
        flash.mutation_calls();
    assert(store.discard_invalid_staging(third.plan) ==
           SoundStoreResult::Busy);
    assert(flash.mutation_calls() ==
           mutations_before_committed_reject);
    assert(store.selection().active_valid);
    assert(store.selection().active.bank == SoundBankId::B);
    assert(store.selection().bank_a.valid);

    // Ordinary BEGIN owns committed-history reclamation once no reader pins
    // that inactive bank. RECOVER never gains that authority.
    SoundUpdateIdentity identity{};
    assert(store.begin_or_resume_update(third.plan, &identity) ==
           SoundStoreResult::Ok);
    assert(identity.target_bank == SoundBankId::A);
    assert(identity.generation == 3U);
    assert(!store.selection().bank_a.valid);
    assert(store.abort_update() == SoundStoreResult::Ok);

    SoundAssetStore after_restart(flash);
    assert(after_restart.scan() == SoundStoreResult::Ok);
    assert(after_restart.selection().active_valid);
    assert(after_restart.selection().active.bank == SoundBankId::B);
    assert(after_restart.selection().active.generation == 2U);
  }

  {
    MemorySoundBankStorage flash;
    flash.corrupt(
        SoundBankId::A, kSoundStagingHeaderOffset, 'E');
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    const auto bundle = make_bundle(58U);

    flash.arm_read_failure(
        SoundBankId::A, kSoundStagingHeaderOffset);
    const auto mutations_before_read_failure =
        flash.mutation_calls();
    assert(store.discard_invalid_staging(bundle.plan) ==
           SoundStoreResult::IoError);
    assert(flash.mutation_calls() ==
           mutations_before_read_failure);

    // Model a backend that erases half the bank and only then reports failure.
    // Recovery must invalidate all cached selection state before any retry.
    flash.arm_mutation_cut(1U);
    const auto mutations_before_erase_failure =
        flash.mutation_calls();
    assert(store.discard_invalid_staging(bundle.plan) ==
           SoundStoreResult::IoError);
    assert(flash.mutation_calls() ==
           mutations_before_erase_failure + 1U);
    assert(flash.mutation_cut_triggered());
    // An ambiguous erase failure invalidates the cached selection.
    assert(!store.selection().active_valid);
    assert(!store.selection().split_brain);

    // A fresh retry re-scans Flash and can complete the repair.
    assert(store.discard_invalid_staging(bundle.plan) ==
           SoundStoreResult::Ok);
    SoundUpdateIdentity identity{};
    assert(store.begin_or_resume_update(bundle.plan, &identity) ==
           SoundStoreResult::Ok);
    assert(store.abort_update() == SoundStoreResult::Ok);
  }
}

void transaction_outcomes_cover_active_committed_and_unknown() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(48U);

  SoundUpdateIdentity first_identity{};
  assert(store.begin_or_resume_update(
             first.plan, &first_identity) ==
         SoundStoreResult::Ok);
  SoundTransactionOutcome outcome{};
  assert(store.query_transaction_outcome(
             first_identity.transaction_id, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Active);
  assert(outcome.identity.generation == first_identity.generation);
  assert(outcome.identity.target_bank == first_identity.target_bank);
  assert(outcome.identity.transaction_id ==
         first_identity.transaction_id);
  assert(outcome.manifest_bytes == first.plan.manifest_bytes);
  assert(outcome.payload_bytes == first.plan.payload_bytes);
  assert(outcome.bundle_sha256 == first.plan.bundle_sha256);

  write_bundle(&store, first);
  assert(store.commit_update() == SoundStoreResult::Ok);
  outcome = {};
  assert(store.query_transaction_outcome(
             first_identity.transaction_id, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Committed);
  assert(outcome.identity.generation == 1U);
  assert(outcome.identity.target_bank == SoundBankId::A);
  assert(outcome.identity.transaction_id ==
         first_identity.transaction_id);
  assert(outcome.manifest_bytes == first.plan.manifest_bytes);
  assert(outcome.payload_bytes == first.plan.payload_bytes);
  assert(outcome.bundle_sha256 == first.plan.bundle_sha256);

  const auto second = make_bundle(
      49U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);
  SoundUpdateIdentity second_identity{};
  assert(store.begin_or_resume_update(
             second.plan, &second_identity) ==
         SoundStoreResult::Ok);
  write_bundle(&store, second);
  assert(store.commit_update() == SoundStoreResult::Ok);

  // Both valid bank snapshots remain queryable, not only the selected bank.
  outcome = {};
  assert(store.query_transaction_outcome(
             first_identity.transaction_id, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Committed);
  assert(outcome.identity.target_bank == SoundBankId::A);
  outcome = {};
  assert(store.query_transaction_outcome(
             second_identity.transaction_id, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Committed);
  assert(outcome.identity.generation == 2U);
  assert(outcome.identity.target_bank == SoundBankId::B);

  const auto third = make_bundle(
      50U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);
  SoundUpdateIdentity third_identity{};
  assert(store.begin_or_resume_update(
             third.plan, &third_identity) ==
         SoundStoreResult::Ok);
  assert(third_identity.generation == 3U);
  assert(third_identity.target_bank == SoundBankId::A);

  // Reusing the old bank invalidates its committed snapshot immediately.
  outcome = {};
  assert(store.query_transaction_outcome(
             first_identity.transaction_id, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Unknown);
  outcome = {};
  assert(store.query_transaction_outcome(
             second_identity.transaction_id, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Committed);
  assert(store.abort_update() == SoundStoreResult::Ok);

  std::array<std::uint8_t,
             easy_input::speaker_assets::kSoundTransactionIdBytes>
      absent_transaction{};
  absent_transaction.fill(0x5AU);
  outcome.state = SoundTransactionState::Active;
  outcome.manifest_bytes = 99U;
  assert(store.query_transaction_outcome(
             absent_transaction, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Unknown);
  assert(outcome.identity.generation == 0U);
  assert(outcome.identity.target_bank == SoundBankId::A);
  assert(outcome.identity.transaction_id == absent_transaction);
  assert(outcome.manifest_bytes == 0U);
  assert(outcome.payload_bytes == 0U);
  assert(std::all_of(
      outcome.bundle_sha256.begin(),
      outcome.bundle_sha256.end(),
      [](std::uint8_t value) { return value == 0U; }));

  SoundAssetStore after_restart(flash);
  assert(after_restart.scan() == SoundStoreResult::Ok);
  outcome = {};
  assert(after_restart.query_transaction_outcome(
             second_identity.transaction_id, &outcome) ==
         SoundStoreResult::Ok);
  assert(outcome.state == SoundTransactionState::Committed);
  assert(outcome.identity.transaction_id ==
         second_identity.transaction_id);

  std::array<std::uint8_t,
             easy_input::speaker_assets::kSoundTransactionIdBytes>
      invalid_transaction{};
  outcome.state = SoundTransactionState::Active;
  outcome.identity.transaction_id.fill(0xA5U);
  assert(after_restart.query_transaction_outcome(
             invalid_transaction, &outcome) ==
         SoundStoreResult::InvalidArgument);
  assert(outcome.state == SoundTransactionState::Unknown);
  assert(std::none_of(
      outcome.identity.transaction_id.begin(),
      outcome.identity.transaction_id.end(),
      [](std::uint8_t value) { return value != 0U; }));
  assert(after_restart.query_transaction_outcome(
             second_identity.transaction_id, nullptr) ==
         SoundStoreResult::InvalidArgument);
}

void valid_bundle_installs_and_survives_reboot() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  assert(!store.selection().active_valid);

  const auto first = make_bundle(1U);
  install_bundle(&store, first);
  assert(store.selection().active_valid);
  assert(store.selection().active.bank == SoundBankId::A);
  assert(store.selection().active.generation == 1U);
  assert(sound_digest_equal(
      store.selection().active.bundle_sha256,
      first.plan.bundle_sha256));

  SoundAssetStore rebooted(flash);
  assert(rebooted.scan() == SoundStoreResult::Ok);
  assert(rebooted.selection().active_valid);
  assert(rebooted.selection().active.bank == SoundBankId::A);
  assert(rebooted.selection().active.generation == 1U);
}

void partial_final_marker_keeps_old_bank_and_resumes() {
  MemorySoundBankStorage flash;
  SoundAssetStore first_boot(flash);
  assert(first_boot.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(2U);
  install_bundle(&first_boot, first);

  const auto second = make_bundle(
      3U,
      first_boot.selection().active.generation,
      first_boot.selection().active.bundle_sha256);
  SoundUpdateIdentity begin_identity{};
  assert(first_boot.begin_or_resume_update(second.plan, &begin_identity) ==
         SoundStoreResult::Ok);
  assert(begin_identity.generation == 2U);
  assert(begin_identity.target_bank == SoundBankId::B);
  assert(first_boot.update_bank() == SoundBankId::B);
  write_bundle(&first_boot, second);
  flash.arm_partial_write(
      SoundBankId::B,
      kSoundCommitOffset + 128U,
      8U);
  assert(first_boot.commit_update() == SoundStoreResult::IoError);

  SoundAssetStore after_power_loss(flash);
  assert(after_power_loss.scan() == SoundStoreResult::Ok);
  assert(after_power_loss.selection().active_valid);
  assert(after_power_loss.selection().active.bank == SoundBankId::A);
  assert(after_power_loss.selection().active.generation == 1U);

  SoundUpdateIdentity resume_identity{};
  assert(after_power_loss.resume_update(
             begin_identity.transaction_id, &resume_identity) ==
         SoundStoreResult::Ok);
  assert(resume_identity.generation == 2U);
  assert(resume_identity.target_bank == SoundBankId::B);
  assert(resume_identity.transaction_id ==
         begin_identity.transaction_id);
  assert(after_power_loss.commit_update() == SoundStoreResult::Ok);

  SoundAssetStore final_boot(flash);
  assert(final_boot.scan() == SoundStoreResult::Ok);
  assert(final_boot.selection().active.bank == SoundBankId::B);
  assert(final_boot.selection().active.generation == 2U);
  assert(sound_digest_equal(
      final_boot.selection().active.bundle_sha256,
      second.plan.bundle_sha256));
}

void two_block_asset_resumes_out_of_order_without_rewriting_complete_data() {
  MemorySoundBankStorage flash;
  SoundAssetStore first_boot(flash);
  assert(first_boot.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(33U);
  install_bundle(&first_boot, first);

  const auto second = make_two_block_eiad_bundle(
      34U,
      first_boot.selection().active.generation,
      first_boot.selection().active.bundle_sha256);
  SoundUpdateIdentity begin_identity{};
  assert(first_boot.begin_or_resume_update(second.plan, &begin_identity) ==
         SoundStoreResult::Ok);
  assert(begin_identity.target_bank == SoundBankId::B);
  easy_input::speaker_assets::SoundUpdateProgress progress{};
  assert(first_boot.update_progress(&progress) ==
         SoundStoreResult::Ok);
  assert(progress.identity.transaction_id ==
         begin_identity.transaction_id);
  assert(progress.identity.generation == 2U);
  assert(progress.identity.target_bank == SoundBankId::B);
  assert(progress.manifest_bytes == second.manifest.size());
  assert(progress.payload_bytes == second.payload.size());
  assert(progress.payload_block_count == 2U);
  assert(!progress.manifest_complete);
  assert(std::all_of(
      progress.payload_complete_bitmap.begin(),
      progress.payload_complete_bitmap.end(),
      [](std::uint8_t value) { return value == 0U; }));
  assert(first_boot.write_manifest(
             second.manifest.data(), second.manifest.size()) ==
         SoundStoreResult::Ok);

  const auto second_block_bytes =
      second.payload.size() - kSoundPayloadBlockSize;
  assert(first_boot.write_payload_block(
             1U,
             second.payload.data() + kSoundPayloadBlockSize,
             second_block_bytes) ==
         SoundStoreResult::Ok);
  progress = {};
  assert(first_boot.update_progress(&progress) ==
         SoundStoreResult::Ok);
  assert(progress.manifest_complete);
  assert(progress.payload_complete_bitmap[0] == 0x02U);
  assert(std::all_of(
      progress.payload_complete_bitmap.begin() + 1U,
      progress.payload_complete_bitmap.end(),
      [](std::uint8_t value) { return value == 0U; }));
  std::uint8_t first_bitmap_byte = 0U;
  assert(flash.read(SoundBankId::B,
                    kSoundJournalOffset + 1U,
                    &first_bitmap_byte,
                    sizeof(first_bitmap_byte)) ==
         SoundStorageIoResult::Ok);
  assert(first_bitmap_byte == 0xFDU);

  SoundAssetStore after_power_loss(flash);
  assert(after_power_loss.scan() == SoundStoreResult::Ok);
  assert(after_power_loss.selection().active.bank == SoundBankId::A);
  progress = {};
  assert(after_power_loss.update_progress(&progress) ==
         SoundStoreResult::TransactionMismatch);
  assert(progress.identity.generation == 0U);
  const auto mutations_before_resume = flash.mutation_calls();
  SoundUpdateIdentity resume_identity{};
  assert(after_power_loss.resume_update(
             begin_identity.transaction_id, &resume_identity) ==
         SoundStoreResult::Ok);
  assert(resume_identity.transaction_id ==
         begin_identity.transaction_id);
  assert(flash.mutation_calls() == mutations_before_resume);
  progress = {};
  assert(after_power_loss.update_progress(&progress) ==
         SoundStoreResult::Ok);
  assert(progress.identity.transaction_id ==
         begin_identity.transaction_id);
  assert(progress.manifest_complete);
  assert(progress.payload_complete_bitmap[0] == 0x02U);

  const auto erase_calls_before_duplicate = flash.erase_calls();
  assert(after_power_loss.write_payload_block(
             1U,
             second.payload.data() + kSoundPayloadBlockSize,
             second_block_bytes) ==
         SoundStoreResult::Ok);
  assert(flash.erase_calls() == erase_calls_before_duplicate);
  assert(after_power_loss.write_payload_block(
             0U,
             second.payload.data(),
             kSoundPayloadBlockSize) ==
         SoundStoreResult::Ok);
  assert(after_power_loss.commit_update() == SoundStoreResult::Ok);
  progress = {};
  assert(after_power_loss.update_progress(&progress) ==
         SoundStoreResult::TransactionMismatch);

  SoundAssetStore final_boot(flash);
  assert(final_boot.scan() == SoundStoreResult::Ok);
  assert(final_boot.selection().active.bank == SoundBankId::B);
  assert(final_boot.selection().active.generation == 2U);
  assert(sound_digest_equal(
      final_boot.selection().active.bundle_sha256,
      second.plan.bundle_sha256));
}

void all_mapping_targets_accept_exact_duration_boundaries() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto bundle = make_full_mapping_boundary_bundle();
  assert(bundle.payload.size() < kSoundPayloadMaxSize);
  install_bundle(&store, bundle);

  SoundAssetStore rebooted(flash);
  assert(rebooted.scan() == SoundStoreResult::Ok);
  assert(rebooted.selection().active_valid);
  assert(rebooted.selection().active.bank == SoundBankId::A);
  assert(sound_digest_equal(
      rebooted.selection().active.bundle_sha256,
      bundle.plan.bundle_sha256));
}

void payload_bitmap_and_final_block_boundaries_are_exact() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto exact = make_payload_boundary_plan(
      kSoundPayloadMaxSize, 35U);
  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(exact.plan, &identity) ==
         SoundStoreResult::Ok);

  for (const std::size_t block_index : {127U, 8U, 7U}) {
    const auto offset = block_index * kSoundPayloadBlockSize;
    assert(store.write_payload_block(
               block_index,
               exact.payload.data() + offset,
               kSoundPayloadBlockSize) ==
           SoundStoreResult::Ok);
  }
  const auto mutations_before_invalid = flash.mutation_calls();
  assert(store.write_payload_block(
             128U,
             exact.payload.data(),
             kSoundPayloadBlockSize) ==
         SoundStoreResult::InvalidArgument);
  assert(flash.mutation_calls() == mutations_before_invalid);

  std::array<std::uint8_t, 16> bitmap{};
  assert(flash.read(SoundBankId::A,
                    kSoundJournalOffset + 1U,
                    bitmap.data(),
                    bitmap.size()) ==
         SoundStorageIoResult::Ok);
  assert(bitmap[0] == 0x7FU);
  assert(bitmap[1] == 0xFEU);
  assert(bitmap[15] == 0x7FU);
  for (std::size_t index = 2U; index < 15U; ++index) {
    assert(bitmap[index] == 0xFFU);
  }
  assert(store.abort_update() == SoundStoreResult::Ok);

  const auto partial = make_payload_boundary_plan(
      kSoundPayloadMaxSize - 1U, 36U);
  identity = {};
  assert(store.begin_or_resume_update(partial.plan, &identity) ==
         SoundStoreResult::Ok);
  const auto final_offset = 127U * kSoundPayloadBlockSize;
  assert(store.write_payload_block(
             127U,
             partial.payload.data() + final_offset,
             kSoundPayloadBlockSize) ==
         SoundStoreResult::InvalidArgument);
  assert(store.write_payload_block(
             127U,
             partial.payload.data() + final_offset,
             kSoundPayloadBlockSize - 1U) ==
         SoundStoreResult::Ok);
  std::vector<std::uint8_t> readback(kSoundPayloadBlockSize - 1U);
  assert(flash.read(SoundBankId::A,
                    kSoundPayloadOffset +
                        static_cast<std::uint32_t>(final_offset),
                    readback.data(),
                    readback.size()) ==
         SoundStorageIoResult::Ok);
  assert(std::equal(readback.begin(),
                    readback.end(),
                    partial.payload.begin() + final_offset));
  std::uint8_t untouched_tail = 0U;
  assert(flash.read(
             SoundBankId::A,
             kSoundPayloadOffset +
                 static_cast<std::uint32_t>(final_offset) +
                 static_cast<std::uint32_t>(readback.size()),
             &untouched_tail,
             sizeof(untouched_tail)) ==
         SoundStorageIoResult::Ok);
  assert(untouched_tail == 0xFFU);
  assert(store.abort_update() == SoundStoreResult::Ok);
}

void resume_propagates_manifest_read_failure() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(18U);
  install_bundle(&store, first);
  const auto second = make_bundle(
      19U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);
  SoundUpdateIdentity begin_identity{};
  assert(store.begin_or_resume_update(second.plan, &begin_identity) ==
         SoundStoreResult::Ok);

  SoundAssetStore rebooted(flash);
  assert(rebooted.scan() == SoundStoreResult::Ok);
  flash.arm_read_failure(
      SoundBankId::B, kSoundManifestOffset);
  SoundUpdateIdentity resume_identity{};
  assert(rebooted.resume_update(
             begin_identity.transaction_id, &resume_identity) ==
         SoundStoreResult::IoError);
  assert(resume_identity.generation == 0U);
  assert(std::none_of(resume_identity.transaction_id.begin(),
                      resume_identity.transaction_id.end(),
                      [](std::uint8_t value) { return value != 0U; }));
  assert(!rebooted.update_active());
  assert(rebooted.selection().active.bank == SoundBankId::A);
}

void repeated_complete_block_read_failure_is_not_rewritten_or_hidden() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(32U);
  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(bundle.plan, &identity) ==
         SoundStoreResult::Ok);
  write_bundle(&store, bundle);

  const auto erase_calls_before = flash.erase_calls();
  flash.arm_read_failure(
      SoundBankId::A, kSoundPayloadOffset);
  assert(store.write_payload_block(
             0U, bundle.payload.data(), bundle.payload.size()) ==
         SoundStoreResult::IoError);
  assert(flash.erase_calls() == erase_calls_before);
  assert(store.abort_update() == SoundStoreResult::Ok);
}

void repeated_complete_manifest_read_failure_is_not_rewritten_or_hidden() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(38U);
  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(bundle.plan, &identity) ==
         SoundStoreResult::Ok);
  assert(store.write_manifest(
             bundle.manifest.data(), bundle.manifest.size()) ==
         SoundStoreResult::Ok);

  const auto erase_calls_before = flash.erase_calls();
  flash.arm_read_failure(
      SoundBankId::A, kSoundManifestOffset);
  assert(store.write_manifest(
             bundle.manifest.data(), bundle.manifest.size()) ==
         SoundStoreResult::IoError);
  assert(flash.erase_calls() == erase_calls_before);
  assert(store.abort_update() == SoundStoreResult::Ok);
}

void failed_abort_keeps_the_transaction_retryable() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(39U);
  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(bundle.plan, &identity) ==
         SoundStoreResult::Ok);
  flash.arm_erase_failure(
      SoundBankId::A, kSoundStagingHeaderOffset);
  assert(store.abort_update() == SoundStoreResult::IoError);
  assert(store.update_active());

  std::array<std::uint8_t, 4> staging_magic{};
  assert(flash.read(SoundBankId::A,
                    kSoundStagingHeaderOffset,
                    staging_magic.data(),
                    staging_magic.size()) ==
         SoundStorageIoResult::Ok);
  assert(staging_magic ==
         (std::array<std::uint8_t, 4>{{'E', 'I', 'S', 'H'}}));

  assert(store.abort_update() == SoundStoreResult::Ok);
  assert(!store.update_active());
  assert(flash.read(SoundBankId::A,
                    kSoundStagingHeaderOffset,
                    staging_magic.data(),
                    staging_magic.size()) ==
         SoundStorageIoResult::Ok);
  assert(std::all_of(staging_magic.begin(),
                     staging_magic.end(),
                     [](std::uint8_t value) {
                       return value == 0xFFU;
                     }));
}

void active_read_lease_prevents_erasing_old_audio() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(4U);
  install_bundle(&store, first);

  SoundReadLease old_audio{};
  assert(store.acquire_active_read(&old_audio) ==
         SoundStoreResult::Ok);
  assert(old_audio.bank == SoundBankId::A);
  SoundReadLease second_reader{};
  assert(store.acquire_active_read(&second_reader) ==
         SoundStoreResult::Ok);
  assert(second_reader.lease_id != old_audio.lease_id);

  const auto second = make_bundle(
      5U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);
  install_bundle(&store, second);
  assert(store.selection().active.bank == SoundBankId::B);

  const auto third = make_bundle(
      6U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);
  SoundUpdateIdentity identity{};
  const auto erase_calls_before = flash.erase_calls();
  assert(store.begin_or_resume_update(third.plan, &identity) ==
         SoundStoreResult::BankPinned);
  assert(identity.generation == 0U);
  assert(flash.erase_calls() == erase_calls_before);

  assert(store.release_read(old_audio) == SoundStoreResult::Ok);
  assert(store.release_read(old_audio) ==
         SoundStoreResult::TransactionMismatch);
  assert(store.begin_or_resume_update(third.plan, &identity) ==
         SoundStoreResult::BankPinned);
  assert(store.release_read(second_reader) == SoundStoreResult::Ok);
  assert(store.begin_or_resume_update(third.plan, &identity) ==
         SoundStoreResult::Ok);
  assert(identity.generation == 3U);
  assert(store.abort_update() == SoundStoreResult::Ok);
}

void completed_marker_wins_even_when_readback_temporarily_fails() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(11U);
  install_bundle(&store, first);
  const auto second = make_bundle(
      12U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);

  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(second.plan, &identity) ==
         SoundStoreResult::Ok);
  write_bundle(&store, second);
  flash.arm_read_failure(
      SoundBankId::B, kSoundCommitOffset + 128U);
  assert(store.commit_update() == SoundStoreResult::IoError);
  assert(store.update_active());

  // A route can disappear immediately after the lost commit reply. The
  // authoritative CurrentActive query must re-read Flash, discover the marker
  // that actually reached storage and reconcile the in-memory transaction.
  SoundBankSnapshot current{};
  assert(store.query_current_active(&current) ==
         SoundStoreResult::Ok);
  assert(current.valid);
  assert(current.bank == SoundBankId::B);
  assert(current.generation == 2U);
  assert(current.transaction_id == identity.transaction_id);
  assert(current.bundle_sha256 == second.plan.bundle_sha256);
  assert(!store.update_active());

  // With the committed generation reconciled, a later cancel is a no-op and
  // can never erase the new bundle.
  assert(store.abort_update() == SoundStoreResult::Ok);
  assert(store.selection().active_valid);
  assert(store.selection().active.bank == SoundBankId::B);
  assert(store.selection().active.generation == 2U);
}

SoundStoreResult attempt_bundle_update(
    SoundAssetStore* store,
    const TestBundle& bundle) {
  assert(store != nullptr);
  SoundUpdateIdentity identity{};
  auto result = store->begin_or_resume_update(bundle.plan, &identity);
  if (result != SoundStoreResult::Ok) {
    return result;
  }
  result = store->write_manifest(
      bundle.manifest.data(), bundle.manifest.size());
  if (result != SoundStoreResult::Ok) {
    return result;
  }
  for (std::size_t offset = 0U, block_index = 0U;
       offset < bundle.payload.size();
       offset += kSoundPayloadBlockSize, ++block_index) {
    const auto block_bytes = std::min<std::size_t>(
        kSoundPayloadBlockSize, bundle.payload.size() - offset);
    result = store->write_payload_block(
        block_index, bundle.payload.data() + offset, block_bytes);
    if (result != SoundStoreResult::Ok) {
      return result;
    }
  }
  return store->commit_update();
}

void every_mutating_power_cut_keeps_one_committed_generation() {
  MemorySoundBankStorage baseline;
  SoundAssetStore baseline_store(baseline);
  assert(baseline_store.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(13U);
  install_bundle(&baseline_store, first);
  const auto second = make_bundle(
      14U,
      baseline_store.selection().active.generation,
      baseline_store.selection().active.bundle_sha256);
  install_bundle(&baseline_store, second);
  assert(baseline_store.selection().active.bank == SoundBankId::B);
  assert(baseline_store.selection().active.generation == 2U);

  const auto third = make_bundle(
      15U,
      baseline_store.selection().active.generation,
      baseline_store.selection().active.bundle_sha256);
  MemorySoundBankStorage successful = baseline;
  SoundAssetStore successful_store(successful);
  assert(successful_store.scan() == SoundStoreResult::Ok);
  const auto mutations_before = successful.mutation_calls();
  assert(attempt_bundle_update(&successful_store, third) ==
         SoundStoreResult::Ok);
  const auto update_mutations =
      successful.mutation_calls() - mutations_before;
  assert(update_mutations >= 10U);

  for (std::size_t ordinal = 1U;
       ordinal <= update_mutations;
       ++ordinal) {
    MemorySoundBankStorage interrupted = baseline;
    interrupted.arm_mutation_cut(ordinal);
    SoundAssetStore interrupted_store(interrupted);
    assert(interrupted_store.scan() == SoundStoreResult::Ok);
    const auto result =
        attempt_bundle_update(&interrupted_store, third);
    assert(result != SoundStoreResult::Ok);
    assert(interrupted.mutation_cut_triggered());

    // Boot playback uses the immutable fast-read path before the App performs
    // a strict reconciliation scan. Every torn update must therefore expose
    // the last committed user sound there as well, never the factory default.
    SoundAssetStore fast_reboot(interrupted);
    SoundReadLease fast_lease{};
    assert(fast_reboot.acquire_active_read(&fast_lease) ==
           SoundStoreResult::Ok);
    assert(fast_lease.valid);
    assert(fast_lease.bank == SoundBankId::B);
    assert(fast_lease.generation == 2U);
    assert(sound_digest_equal(
        fast_lease.bundle_sha256, second.plan.bundle_sha256));
    assert(fast_reboot.release_read(fast_lease) ==
           SoundStoreResult::Ok);

    SoundAssetStore rebooted(interrupted);
    assert(rebooted.scan() == SoundStoreResult::Ok);
    assert(rebooted.selection().active_valid);
    assert(!rebooted.selection().split_brain);
    assert(rebooted.selection().active.bank == SoundBankId::B);
    assert(rebooted.selection().active.generation == 2U);
    assert(sound_digest_equal(
        rebooted.selection().active.bundle_sha256,
        second.plan.bundle_sha256));
  }
}

void corrupt_newer_bank_falls_back_to_older_valid_bank() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(7U);
  install_bundle(&store, first);
  const auto second = make_bundle(
      8U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);
  install_bundle(&store, second);
  assert(store.selection().active.bank == SoundBankId::B);

  flash.corrupt(
      SoundBankId::B,
      kSoundPayloadOffset + 26U,
      static_cast<std::uint8_t>(second.payload[26U] ^ 0x01U));
  SoundAssetStore rebooted(flash);
  assert(rebooted.scan() == SoundStoreResult::Ok);
  assert(rebooted.selection().active_valid);
  assert(rebooted.selection().active.bank == SoundBankId::A);
  assert(rebooted.selection().active.generation == 1U);
}

void malformed_manifest_never_receives_commit_marker() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  auto malformed = make_bundle(9U);
  write_le16(malformed.manifest.data() + 82U, 1U);
  rebuild_plan_digests(&malformed);

  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(malformed.plan, &identity) ==
         SoundStoreResult::Ok);
  write_bundle(&store, malformed);
  assert(store.commit_update() ==
         SoundStoreResult::InvalidManifest);
  std::array<std::uint8_t, 16> marker{};
  assert(flash.read(SoundBankId::A,
                    kSoundCommitOffset + 128U,
                    marker.data(),
                    marker.size()) ==
         SoundStorageIoResult::Ok);
  assert(std::all_of(marker.begin(),
                     marker.end(),
                     [](std::uint8_t value) {
                       return value == 0xFFU;
                     }));
  assert(store.abort_update() == SoundStoreResult::Ok);

  SoundAssetStore rebooted(flash);
  assert(rebooted.scan() == SoundStoreResult::Ok);
  assert(!rebooted.selection().active_valid);
}

void expect_bundle_rejected(
    const TestBundle& bundle,
    SoundStoreResult expected_result) {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(bundle.plan, &identity) ==
         SoundStoreResult::Ok);
  write_bundle(&store, bundle);
  assert(store.commit_update() == expected_result);

  SoundAssetStore rebooted(flash);
  assert(rebooted.scan() == SoundStoreResult::Ok);
  assert(!rebooted.selection().active_valid);
}

void canonical_manifest_and_eiad_reject_noncanonical_inputs() {
  auto reserved_header = make_bundle(20U);
  reserved_header.manifest[20U] = 1U;
  rebuild_plan_digests(&reserved_header);
  expect_bundle_rejected(
      reserved_header, SoundStoreResult::InvalidManifest);

  auto payload_gap = make_bundle(21U);
  write_le32(payload_gap.manifest.data() + 68U, 27U);
  rebuild_plan_digests(&payload_gap);
  expect_bundle_rejected(
      payload_gap, SoundStoreResult::InvalidManifest);

  auto invalid_eiad = make_bundle(22U);
  invalid_eiad.payload[0U] = 'X';
  refresh_resource_digest(&invalid_eiad);
  expect_bundle_rejected(
      invalid_eiad, SoundStoreResult::InvalidManifest);

  auto invalid_step_index = make_bundle(23U);
  invalid_step_index.payload[24U] = 89U;
  refresh_resource_digest(&invalid_step_index);
  expect_bundle_rejected(
      invalid_step_index, SoundStoreResult::InvalidManifest);

  auto resource_hash_mismatch = make_bundle(24U);
  resource_hash_mismatch.payload[26U] ^= 0x01U;
  rebuild_plan_digests(&resource_hash_mismatch);
  expect_bundle_rejected(
      resource_hash_mismatch, SoundStoreResult::HashMismatch);

  auto orphan_resource = make_bundle(25U);
  orphan_resource.manifest.resize(80U);
  orphan_resource.plan.manifest_bytes = 80U;
  write_le32(orphan_resource.manifest.data() + 8U, 80U);
  write_le16(orphan_resource.manifest.data() + 18U, 0U);
  rebuild_plan_digests(&orphan_resource);
  expect_bundle_rejected(
      orphan_resource, SoundStoreResult::InvalidManifest);

  auto duplicate_target = make_bundle(26U);
  duplicate_target.manifest.resize(88U);
  duplicate_target.plan.manifest_bytes = 88U;
  write_le32(duplicate_target.manifest.data() + 8U, 88U);
  write_le16(duplicate_target.manifest.data() + 18U, 2U);
  duplicate_target.manifest[84U] = 1U;
  duplicate_target.manifest[85U] = 0U;
  write_le16(duplicate_target.manifest.data() + 86U, 0U);
  rebuild_plan_digests(&duplicate_target);
  expect_bundle_rejected(
      duplicate_target, SoundStoreResult::InvalidManifest);
}

void storage_read_failure_is_fail_closed_and_never_erases() {
  MemorySoundBankStorage flash;
  flash.set_fail_all_reads(true);
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Unavailable);
  const auto erase_calls_before = flash.erase_calls();
  const auto bundle = make_bundle(10U);
  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(bundle.plan, &identity) ==
         SoundStoreResult::Unavailable);
  assert(identity.generation == 0U);
  assert(flash.erase_calls() == erase_calls_before);
}

void factory_blank_requires_two_erased_commit_records() {
  {
    MemorySoundBankStorage flash;
    SoundAssetStore store(flash);
    SoundReadLease lease{};
    assert(store.acquire_active_read(&lease) ==
           SoundStoreResult::FactoryBlank);
    assert(!lease.valid);
  }

  {
    MemorySoundBankStorage flash;
    SoundAssetStore store(flash);
    assert(store.scan() == SoundStoreResult::Ok);
    SoundReadLease lease{};
    assert(store.acquire_active_read(&lease) ==
           SoundStoreResult::FactoryBlank);
    assert(!lease.valid);
  }

  {
    MemorySoundBankStorage flash;
    SoundAssetStore store(flash);
    SoundBankSnapshot snapshot{};
    assert(store.query_current_active(&snapshot) ==
           SoundStoreResult::Ok);
    assert(!snapshot.valid);
    SoundReadLease lease{};
    assert(store.acquire_active_read(&lease) ==
           SoundStoreResult::FactoryBlank);
    assert(!lease.valid);
  }

  {
    MemorySoundBankStorage flash;
    // An interrupted first update has no successful preference yet. A
    // non-erased staging control byte must not turn the erased final records
    // into a corrupt committed preference.
    flash.corrupt(
        SoundBankId::A, kSoundStagingHeaderOffset, 0x00U);
    SoundAssetStore store(flash);
    SoundReadLease lease{};
    assert(store.acquire_active_read(&lease) ==
           SoundStoreResult::FactoryBlank);
    assert(!lease.valid);
  }

  {
    MemorySoundBankStorage flash;
    // Any programmed byte in a final record that cannot validate is
    // corruption, not factory absence.
    flash.corrupt(SoundBankId::A, kSoundCommitOffset, 0x00U);
    SoundAssetStore store(flash);
    SoundReadLease lease{};
    assert(store.acquire_active_read(&lease) ==
           SoundStoreResult::Unavailable);
    assert(!lease.valid);
  }

  {
    MemorySoundBankStorage flash;
    flash.arm_read_failure(SoundBankId::A, kSoundCommitOffset);
    SoundAssetStore store(flash);
    SoundReadLease lease{};
    assert(store.acquire_active_read(&lease) ==
           SoundStoreResult::IoError);
    assert(!lease.valid);
  }
}

void interrupted_first_staging_keeps_factory_default_read_only() {
  MemorySoundBankStorage flash;
  {
    SoundAssetStore first_attempt(flash);
    assert(first_attempt.scan() == SoundStoreResult::Ok);
    const auto bundle = make_bundle(64U);
    SoundUpdateIdentity identity{};
    assert(first_attempt.begin_or_resume_update(
               bundle.plan, &identity) ==
           SoundStoreResult::Ok);
    write_bundle(&first_attempt, bundle);
    assert(first_attempt.update_active());
  }

  const auto mutations_before_boot = flash.mutation_calls();
  const auto erases_before_boot = flash.erase_calls();
  SoundAssetStore rebooted(flash);
  SoundReadLease lease{};
  assert(rebooted.acquire_active_read(&lease) ==
         SoundStoreResult::FactoryBlank);
  assert(!lease.valid);
  assert(flash.mutation_calls() == mutations_before_boot);
  assert(flash.erase_calls() == erases_before_boot);
}

void failed_rescan_invalidates_a_previously_valid_selection() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto first = make_bundle(27U);
  install_bundle(&store, first);
  const auto replacement = make_bundle(
      28U,
      store.selection().active.generation,
      store.selection().active.bundle_sha256);

  flash.set_fail_all_reads(true);
  const auto erase_calls_before = flash.erase_calls();
  assert(store.scan() == SoundStoreResult::Unavailable);

  SoundReadLease lease{};
  assert(store.acquire_active_read(&lease) ==
         SoundStoreResult::Unavailable);
  assert(!lease.valid);

  SoundUpdateIdentity identity{};
  assert(store.begin_or_resume_update(replacement.plan, &identity) ==
         SoundStoreResult::Unavailable);
  assert(identity.generation == 0U);
  assert(flash.erase_calls() == erase_calls_before);
}

void io_failure_is_not_hidden_by_an_earlier_corrupt_tail() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  assert(store.scan() == SoundStoreResult::Ok);
  const auto bundle = make_bundle(16U);
  install_bundle(&store, bundle);

  flash.corrupt(
      SoundBankId::A,
      kSoundManifestOffset +
          static_cast<std::uint32_t>(bundle.manifest.size()),
      0x00U);
  flash.arm_read_failure(
      SoundBankId::A, kSoundCommitOffset + 144U);
  SoundAssetStore rebooted(flash);
  assert(rebooted.scan() == SoundStoreResult::IoError);
  const auto erase_calls_before = flash.erase_calls();
  SoundUpdateIdentity identity{};
  const auto replacement = make_bundle(17U);
  flash.arm_read_failure(
      SoundBankId::A, kSoundCommitOffset + 144U);
  assert(rebooted.begin_or_resume_update(replacement.plan, &identity) ==
         SoundStoreResult::IoError);
  assert(identity.generation == 0U);
  assert(flash.erase_calls() == erase_calls_before);
}

void selection_is_deterministic_and_split_brain_fails_closed() {
  SoundBankSnapshot bank_a{};
  bank_a.valid = true;
  bank_a.bank = SoundBankId::A;
  bank_a.generation = 11U;
  bank_a.bundle_sha256.fill(0x11U);
  SoundBankSnapshot bank_b = bank_a;
  bank_b.bank = SoundBankId::B;

  auto selection = select_sound_banks(bank_a, bank_b);
  assert(selection.active_valid);
  assert(!selection.split_brain);
  assert(selection.active.bank == SoundBankId::A);

  bank_b.bundle_sha256.fill(0x22U);
  selection = select_sound_banks(bank_a, bank_b);
  assert(!selection.active_valid);
  assert(selection.split_brain);

  bank_b.generation = 12U;
  selection = select_sound_banks(bank_a, bank_b);
  assert(selection.active_valid);
  assert(!selection.split_brain);
  assert(selection.active.bank == SoundBankId::B);

  bank_b.generation = 13U;
  selection = select_sound_banks(bank_a, bank_b);
  assert(!selection.active_valid);
  assert(selection.split_brain);
}

void current_active_query_distinguishes_absence_from_storage_failure() {
  MemorySoundBankStorage flash;
  SoundAssetStore store(flash);
  SoundBankSnapshot snapshot{};
  snapshot.valid = true;
  snapshot.generation = 99U;

  assert(store.query_current_active(nullptr) ==
         SoundStoreResult::InvalidArgument);
  assert(store.query_current_active(&snapshot) ==
         SoundStoreResult::Ok);
  assert(!snapshot.valid);
  assert(snapshot.generation == 0U);

  const auto bundle = make_bundle(63U);
  install_bundle(&store, bundle);
  snapshot = {};
  assert(store.query_current_active(&snapshot) ==
         SoundStoreResult::Ok);
  assert(snapshot.valid);
  assert(snapshot.bank == SoundBankId::A);
  assert(snapshot.generation == 1U);
  assert(snapshot.bundle_sha256 == bundle.plan.bundle_sha256);

  MemorySoundBankStorage unavailable_flash;
  unavailable_flash.set_fail_all_reads(true);
  SoundAssetStore unavailable(unavailable_flash);
  snapshot.valid = true;
  snapshot.generation = 88U;
  assert(unavailable.query_current_active(&snapshot) ==
         SoundStoreResult::Unavailable);
  assert(!snapshot.valid);
  assert(snapshot.generation == 0U);
}

}  // namespace

int main() {
  static_assert(sizeof(SoundAssetStore) <= 2048U);
  crypto_matches_standard_vectors();
  firmware_assigns_stable_bound_transaction_identity();
  begin_ack_loss_is_idempotent_and_survives_restart();
  begin_or_resume_never_erases_corrupt_or_unreadable_staging();
  begin_reclaims_stale_staging_after_torn_committed_reclamation();
  invalid_staging_discard_is_explicit_safe_and_idempotent();
  invalid_staging_discard_respects_pins_and_io_failures();
  transaction_outcomes_cover_active_committed_and_unknown();
  valid_bundle_installs_and_survives_reboot();
  partial_final_marker_keeps_old_bank_and_resumes();
  two_block_asset_resumes_out_of_order_without_rewriting_complete_data();
  all_mapping_targets_accept_exact_duration_boundaries();
  payload_bitmap_and_final_block_boundaries_are_exact();
  resume_propagates_manifest_read_failure();
  repeated_complete_block_read_failure_is_not_rewritten_or_hidden();
  repeated_complete_manifest_read_failure_is_not_rewritten_or_hidden();
  failed_abort_keeps_the_transaction_retryable();
  active_read_lease_prevents_erasing_old_audio();
  completed_marker_wins_even_when_readback_temporarily_fails();
  every_mutating_power_cut_keeps_one_committed_generation();
  corrupt_newer_bank_falls_back_to_older_valid_bank();
  malformed_manifest_never_receives_commit_marker();
  canonical_manifest_and_eiad_reject_noncanonical_inputs();
  storage_read_failure_is_fail_closed_and_never_erases();
  factory_blank_requires_two_erased_commit_records();
  interrupted_first_staging_keeps_factory_default_read_only();
  failed_rescan_invalidates_a_previously_valid_selection();
  io_failure_is_not_hidden_by_an_earlier_corrupt_tail();
  selection_is_deterministic_and_split_brain_fails_closed();
  current_active_query_distinguishes_absence_from_storage_failure();
  return 0;
}
