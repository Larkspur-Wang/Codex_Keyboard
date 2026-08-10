#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "speaker_assets/sound_asset_crypto.h"
#include "speaker_assets/sound_asset_reader.h"
#include "speaker_assets/sound_asset_store.h"

namespace {

std::size_t g_allocation_count = 0U;

}  // namespace

void* operator new(std::size_t size) {
  ++g_allocation_count;
  if (void* memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  ++g_allocation_count;
  if (void* memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete[](void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {

using easy_input::speaker_assets::SoundAssetReadResult;
using easy_input::speaker_assets::SoundAssetStore;
using easy_input::speaker_assets::SoundAssetStreamDecoder;
using easy_input::speaker_assets::SoundAssetTrigger;
using easy_input::speaker_assets::SoundBankId;
using easy_input::speaker_assets::SoundBankStorage;
using easy_input::speaker_assets::SoundBundlePlan;
using easy_input::speaker_assets::SoundReadLease;
using easy_input::speaker_assets::SoundResolvedAsset;
using easy_input::speaker_assets::SoundSha256;
using easy_input::speaker_assets::SoundSha256Digest;
using easy_input::speaker_assets::SoundStorageIoResult;
using easy_input::speaker_assets::SoundStoreResult;
using easy_input::speaker_assets::SoundUpdateIdentity;
using easy_input::speaker_assets::kSoundAssetFrameSamples;
using easy_input::speaker_assets::kSoundBankSize;
using easy_input::speaker_assets::kSoundManifestOffset;
using easy_input::speaker_assets::kSoundPayloadBlockSize;
using easy_input::speaker_assets::kSoundPayloadOffset;
using easy_input::speaker_assets::kSoundSectorSize;
using easy_input::speaker_assets::resolve_sound_asset;
using easy_input::speaker_assets::sound_crc32_iso_hdlc;

struct StreamingFixture {
  const std::vector<std::uint8_t>* encoded = nullptr;
  std::size_t available = 0U;
  bool fail = false;
};

SoundAssetReadResult read_streaming_fixture(
    void* raw,
    std::uint32_t offset,
    std::uint8_t* destination,
    std::size_t length) {
  auto* fixture = static_cast<StreamingFixture*>(raw);
  if (fixture == nullptr || fixture->encoded == nullptr ||
      destination == nullptr || fixture->fail) {
    return SoundAssetReadResult::IoError;
  }
  const auto start = static_cast<std::size_t>(offset);
  if (start > fixture->encoded->size() ||
      length > fixture->encoded->size() - start ||
      start + length > fixture->available) {
    return SoundAssetReadResult::NotReady;
  }
  std::copy_n(fixture->encoded->data() + start, length, destination);
  return SoundAssetReadResult::Ok;
}

constexpr std::array<std::uint8_t, 16> kBundleDomain{{
    'E', 'A', 'S', 'Y', 'I', 'N', 'P', 'U',
    'T', '-', 'S', 'N', 'D', '-', 'V', '1',
}};

std::size_t bank_index(SoundBankId bank) {
  return bank == SoundBankId::A ? 0U : 1U;
}

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

SoundSha256Digest sha256(const std::uint8_t* data,
                         std::size_t length) {
  SoundSha256 hash;
  assert(hash.update(data, length));
  return hash.finish();
}

std::uint8_t hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(10 + value - 'a');
  }
  assert(false);
  return 0U;
}

void assert_digest_hex(
    const SoundSha256Digest& digest,
    const char* expected) {
  assert(expected != nullptr);
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    assert(digest[index] ==
           static_cast<std::uint8_t>(
               (hex_nibble(expected[index * 2U]) << 4U) |
               hex_nibble(expected[index * 2U + 1U])));
  }
  assert(expected[digest.size() * 2U] == '\0');
}

struct TestBundle {
  std::vector<std::uint8_t> manifest;
  std::vector<std::uint8_t> payload;
  SoundBundlePlan plan{};
};

TestBundle make_two_frame_boot_bundle() {
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
  write_le16(header + 12U, 2U);
  write_le32(header + 14U, 481U);
  write_le16(header + 18U, 20U);

  const std::size_t first_frame_offset = bundle.payload.size();
  bundle.payload.resize(
      first_frame_offset + 6U + 240U, 0U);
  write_le16(bundle.payload.data() + first_frame_offset, 480U);
  write_le16(bundle.payload.data() + first_frame_offset + 2U, 0U);
  bundle.payload[first_frame_offset + 4U] = 0U;
  bundle.payload[first_frame_offset + 5U] = 0U;
  bundle.payload[first_frame_offset + 6U] = 0x10U;
  bundle.payload[first_frame_offset + 7U] = 0x32U;

  const std::size_t second_frame_offset = bundle.payload.size();
  bundle.payload.resize(second_frame_offset + 6U, 0U);
  write_le16(bundle.payload.data() + second_frame_offset, 1U);
  write_le16(bundle.payload.data() + second_frame_offset + 2U, 1234U);
  bundle.payload[second_frame_offset + 4U] = 0U;
  bundle.payload[second_frame_offset + 5U] = 0U;
  assert(bundle.payload.size() == 272U);

  bundle.manifest.assign(32U + 48U + 4U, 0U);
  auto* manifest = bundle.manifest.data();
  manifest[0U] = 'E';
  manifest[1U] = 'I';
  manifest[2U] = 'S';
  manifest[3U] = 'M';
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
  write_le32(manifest + 72U, 481U);
  write_le16(manifest + 76U, 2U);
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
    max_read_bytes_ = std::max(max_read_bytes_, length);
    if (fail_read_ && bank == fail_bank_ && offset == fail_offset_) {
      fail_read_ = false;
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

  void reset_read_metrics() {
    read_calls_ = 0U;
    max_read_bytes_ = 0U;
  }

  void fail_next_read(SoundBankId bank, std::uint32_t offset) {
    fail_read_ = true;
    fail_bank_ = bank;
    fail_offset_ = offset;
  }

  void corrupt(SoundBankId bank,
               std::uint32_t offset,
               std::uint8_t value) {
    auto* destination = mutable_range(bank, offset, 1U);
    assert(destination != nullptr);
    *destination = value;
  }

  std::size_t read_calls() const {
    return read_calls_;
  }

  std::size_t max_read_bytes() const {
    return max_read_bytes_;
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
  bool fail_read_ = false;
  SoundBankId fail_bank_ = SoundBankId::A;
  std::uint32_t fail_offset_ = 0U;
  std::size_t read_calls_ = 0U;
  std::size_t max_read_bytes_ = 0U;
};

struct InstalledBundle {
  MemorySoundBankStorage storage{};
  SoundAssetStore store;
  TestBundle bundle;
  SoundReadLease lease{};

  InstalledBundle()
      : store(storage), bundle(make_two_frame_boot_bundle()) {
    assert(store.scan() == SoundStoreResult::Ok);
    SoundUpdateIdentity identity{};
    assert(store.begin_or_resume_update(bundle.plan, &identity) ==
           SoundStoreResult::Ok);
    assert(store.write_manifest(
               bundle.manifest.data(), bundle.manifest.size()) ==
           SoundStoreResult::Ok);
    assert(store.write_payload_block(
               0U, bundle.payload.data(), bundle.payload.size()) ==
           SoundStoreResult::Ok);
    assert(store.commit_update() == SoundStoreResult::Ok);
    assert(store.acquire_active_read(&lease) == SoundStoreResult::Ok);
  }

  ~InstalledBundle() {
    assert(store.release_read(lease) == SoundStoreResult::Ok);
  }
};

void committed_boot_mapping_resolves_and_streams_without_heap() {
  InstalledBundle installed;
  installed.storage.reset_read_metrics();
  const std::size_t allocations_before = g_allocation_count;

  SoundResolvedAsset asset{};
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::Ok);
  assert(asset.valid);
  assert(asset.lease_id == installed.lease.lease_id);
  assert(asset.bank == installed.lease.bank);
  assert(asset.generation == installed.lease.generation);
  assert(asset.resource_index == 0U);
  assert(asset.payload_offset == 0U);
  assert(asset.encoded_bank_offset == kSoundPayloadOffset);
  assert(asset.encoded_bytes == installed.bundle.payload.size());
  assert(asset.decoded_samples == 481U);
  assert(asset.frame_count == 2U);

  SoundAssetStreamDecoder decoder;
  assert(decoder.open(
             installed.storage, installed.lease, asset) ==
         SoundAssetReadResult::Ok);
  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::size_t samples = 0U;
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::Ok);
  assert(samples == 480U);
  assert((std::array<std::int16_t, 5>{{
              pcm[0U], pcm[1U], pcm[2U], pcm[3U], pcm[4U],
          }} ==
          std::array<std::int16_t, 5>{{0, 0, 1, 4, 8}}));
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::Ok);
  assert(samples == 1U);
  assert(pcm[0U] == 1234);
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::End);
  assert(samples == 0U);
  assert(decoder.decoded_samples() == 481U);
  assert(decoder.next_frame_index() == 2U);

  assert(g_allocation_count == allocations_before);
  assert(installed.storage.max_read_bytes() <= 256U);
}

void reset_replays_the_same_committed_frames() {
  InstalledBundle installed;
  SoundResolvedAsset asset{};
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::Ok);
  SoundAssetStreamDecoder decoder;
  assert(decoder.open(
             installed.storage, installed.lease, asset) ==
         SoundAssetReadResult::Ok);
  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::size_t samples = 0U;
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::Ok);
  assert(decoder.reset() == SoundAssetReadResult::Ok);
  pcm.fill(-1);
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::Ok);
  assert(samples == 480U);
  assert((std::array<std::int16_t, 5>{{
              pcm[0U], pcm[1U], pcm[2U], pcm[3U], pcm[4U],
          }} ==
          std::array<std::int16_t, 5>{{0, 0, 1, 4, 8}}));
}

void close_revokes_the_borrowed_storage_identity() {
  InstalledBundle installed;
  SoundResolvedAsset asset{};
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::Ok);
  SoundAssetStreamDecoder decoder;
  assert(decoder.open(
             installed.storage, installed.lease, asset) ==
         SoundAssetReadResult::Ok);
  assert(decoder.ready());

  decoder.close();
  assert(!decoder.ready());
  assert(decoder.asset().valid == false);
  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::size_t samples = 99U;
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::NotReady);
  assert(samples == 0U);
  assert(decoder.reset() == SoundAssetReadResult::NotReady);
}

void absent_mapping_and_invalid_lease_fail_without_flash_mutation() {
  InstalledBundle installed;
  SoundResolvedAsset asset{};
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Key,
             1U,
             &asset) == SoundAssetReadResult::NotMapped);
  assert(!asset.valid);

  installed.storage.reset_read_metrics();
  SoundReadLease invalid{};
  assert(resolve_sound_asset(
             installed.storage,
             invalid,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::InvalidLease);
  assert(installed.storage.read_calls() == 0U);
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Key,
             0U,
             &asset) == SoundAssetReadResult::InvalidArgument);
}

void output_backpressure_and_io_failure_do_not_advance() {
  InstalledBundle installed;
  SoundResolvedAsset asset{};
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::Ok);
  SoundAssetStreamDecoder decoder;
  assert(decoder.open(
             installed.storage, installed.lease, asset) ==
         SoundAssetReadResult::Ok);

  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::size_t samples = 99U;
  installed.storage.reset_read_metrics();
  assert(decoder.decode_next(
             pcm.data(), pcm.size() - 1U, &samples) ==
         SoundAssetReadResult::OutputTooSmall);
  assert(samples == 0U);
  assert(decoder.next_frame_index() == 0U);
  assert(installed.storage.read_calls() == 0U);

  installed.storage.fail_next_read(
      asset.bank,
      asset.encoded_bank_offset + 20U + 6U);
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::IoError);
  assert(samples == 0U);
  assert(decoder.next_frame_index() == 0U);
  assert(decoder.decoded_samples() == 0U);
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::Ok);
  assert(samples == 480U);
}

void corrupt_frame_isolated_to_audio_and_does_not_advance() {
  InstalledBundle installed;
  SoundResolvedAsset asset{};
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::Ok);
  SoundAssetStreamDecoder decoder;
  assert(decoder.open(
             installed.storage, installed.lease, asset) ==
         SoundAssetReadResult::Ok);

  installed.storage.corrupt(
      asset.bank, asset.encoded_bank_offset + 20U + 4U, 89U);
  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::size_t samples = 0U;
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::InvalidResource);
  assert(samples == 0U);
  assert(decoder.next_frame_index() == 0U);
  assert(decoder.decoded_samples() == 0U);
}

void resolver_and_stream_reject_foreign_identity() {
  InstalledBundle installed;
  SoundResolvedAsset asset{};
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::Ok);
  SoundReadLease foreign = installed.lease;
  ++foreign.lease_id;
  SoundAssetStreamDecoder decoder;
  assert(decoder.open(installed.storage, foreign, asset) ==
         SoundAssetReadResult::InvalidLease);
  assert(!decoder.ready());

  installed.storage.fail_next_read(
      installed.lease.bank, kSoundManifestOffset);
  assert(resolve_sound_asset(
             installed.storage,
             installed.lease,
             SoundAssetTrigger::Boot,
             0U,
             &asset) == SoundAssetReadResult::IoError);
}

void factory_waytoagi_asset_is_frozen_and_streams_without_heap() {
  const std::string path =
      std::string(EASY_INPUT_REPO_ROOT) +
      "/features/speaker_assets/assets/waytoagi.eiad";
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  assert(input.good());
  const auto end = input.tellg();
  assert(end == static_cast<std::streamoff>(42435));
  std::vector<std::uint8_t> encoded(
      static_cast<std::size_t>(end), 0U);
  input.seekg(0, std::ios::beg);
  input.read(
      reinterpret_cast<char*>(encoded.data()),
      static_cast<std::streamsize>(encoded.size()));
  assert(input.good());

  assert_digest_hex(
      sha256(encoded.data(), encoded.size()),
      "f29312efa6cb78eb1ac43ca762acbbfefa81769f00dee0930f81fd53bc311751");

  SoundAssetStreamDecoder decoder;
  const std::size_t allocations_before = g_allocation_count;
  assert(decoder.open_embedded(
             encoded.data(), encoded.size()) ==
         SoundAssetReadResult::Ok);
  assert(decoder.ready());
  assert(!decoder.asset().valid);

  SoundSha256 pcm_hash;
  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::array<std::uint8_t, kSoundAssetFrameSamples * 2U>
      pcm_bytes{};
  std::uint32_t total_samples = 0U;
  std::uint16_t frame_count = 0U;
  while (true) {
    std::size_t samples = 0U;
    const auto result = decoder.decode_next(
        pcm.data(), pcm.size(), &samples);
    if (result == SoundAssetReadResult::End) {
      break;
    }
    assert(result == SoundAssetReadResult::Ok);
    assert(samples != 0U && samples <= pcm.size());
    for (std::size_t index = 0U; index < samples; ++index) {
      const auto value = static_cast<std::uint16_t>(pcm[index]);
      pcm_bytes[index * 2U] =
          static_cast<std::uint8_t>(value);
      pcm_bytes[index * 2U + 1U] =
          static_cast<std::uint8_t>(value >> 8U);
    }
    assert(pcm_hash.update(pcm_bytes.data(), samples * 2U));
    total_samples += static_cast<std::uint32_t>(samples);
    ++frame_count;
  }
  assert(frame_count == 173U);
  assert(total_samples == 82755U);
  assert(decoder.next_frame_index() == 173U);
  assert(decoder.decoded_samples() == 82755U);
  assert_digest_hex(
      pcm_hash.finish(),
      "431c9f6bebb6eaa44e386252c49a2af9fc647da7a79a5796bbab2e1ea48fbd3f");
  assert(g_allocation_count == allocations_before);

  std::vector<std::uint8_t> corrupt = encoded;
  corrupt[0U] = 'X';
  assert(decoder.open_embedded(
             corrupt.data(), corrupt.size()) ==
         SoundAssetReadResult::InvalidResource);
  assert(decoder.open_embedded(nullptr, encoded.size()) ==
         SoundAssetReadResult::InvalidArgument);
  assert(decoder.open_embedded(encoded.data(), 19U) ==
         SoundAssetReadResult::InvalidResource);
}

void embedded_summary_longer_than_boot_limit_is_accepted() {
  constexpr std::uint16_t frame_count = 801U;
  constexpr std::uint32_t decoded_samples = 384001U;
  constexpr std::size_t full_frame_bytes = 6U + 240U;
  std::vector<std::uint8_t> encoded(
      20U + (800U * full_frame_bytes) + 6U, 0U);
  encoded[0U] = 'E';
  encoded[1U] = 'I';
  encoded[2U] = 'A';
  encoded[3U] = 'D';
  encoded[4U] = 1U;
  encoded[5U] = 1U;
  write_le32(encoded.data() + 6U, 48000U);
  write_le16(encoded.data() + 10U, 480U);
  write_le16(encoded.data() + 12U, frame_count);
  write_le32(encoded.data() + 14U, decoded_samples);
  write_le16(encoded.data() + 18U, 20U);
  for (std::size_t index = 0U; index < 800U; ++index) {
    const auto offset = 20U + (index * full_frame_bytes);
    write_le16(encoded.data() + offset, 480U);
  }
  write_le16(encoded.data() + 20U + (800U * full_frame_bytes), 1U);

  SoundAssetStreamDecoder decoder;
  assert(decoder.open_embedded(encoded.data(), encoded.size()) ==
         SoundAssetReadResult::Ok);
  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::uint32_t streamed_samples = 0U;
  while (true) {
    std::size_t samples = 0U;
    const auto result = decoder.decode_next(
        pcm.data(), pcm.size(), &samples);
    if (result == SoundAssetReadResult::End) {
      break;
    }
    assert(result == SoundAssetReadResult::Ok);
    streamed_samples += static_cast<std::uint32_t>(samples);
  }
  assert(streamed_samples == decoded_samples);
}

void streaming_embedded_reader_waits_for_each_published_range() {
  constexpr std::uint16_t frame_count = 2U;
  constexpr std::uint32_t decoded_samples = 960U;
  constexpr std::size_t frame_bytes = 246U;
  std::vector<std::uint8_t> encoded(20U + frame_count * frame_bytes, 0U);
  encoded[0U] = 'E';
  encoded[1U] = 'I';
  encoded[2U] = 'A';
  encoded[3U] = 'D';
  encoded[4U] = 1U;
  encoded[5U] = 1U;
  write_le32(encoded.data() + 6U, 48000U);
  write_le16(encoded.data() + 10U, 480U);
  write_le16(encoded.data() + 12U, frame_count);
  write_le32(encoded.data() + 14U, decoded_samples);
  write_le16(encoded.data() + 18U, 20U);
  write_le16(encoded.data() + 20U, 480U);
  write_le16(encoded.data() + 20U + frame_bytes, 480U);

  StreamingFixture fixture{&encoded, 20U, false};
  SoundAssetStreamDecoder decoder;
  assert(decoder.open_streaming(
             encoded.data(),
             encoded.size(),
             read_streaming_fixture,
             &fixture) == SoundAssetReadResult::Ok);
  std::array<std::int16_t, kSoundAssetFrameSamples> pcm{};
  std::size_t samples = 99U;
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::NotReady);
  assert(samples == 0U);
  assert(decoder.next_frame_index() == 0U);

  fixture.available = 20U + frame_bytes;
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::Ok);
  assert(samples == 480U);
  assert(decoder.next_frame_index() == 1U);

  fixture.fail = true;
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::IoError);
  assert(decoder.next_frame_index() == 1U);
  fixture.fail = false;
  fixture.available = encoded.size();
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::Ok);
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::End);
  decoder.close();
  assert(decoder.decode_next(pcm.data(), pcm.size(), &samples) ==
         SoundAssetReadResult::NotReady);
}

}  // namespace

int main() {
  committed_boot_mapping_resolves_and_streams_without_heap();
  reset_replays_the_same_committed_frames();
  close_revokes_the_borrowed_storage_identity();
  absent_mapping_and_invalid_lease_fail_without_flash_mutation();
  output_backpressure_and_io_failure_do_not_advance();
  corrupt_frame_isolated_to_audio_and_does_not_advance();
  resolver_and_stream_reject_foreign_identity();
  factory_waytoagi_asset_is_frozen_and_streams_without_heap();
  embedded_summary_longer_than_boot_limit_is_accepted();
  streaming_embedded_reader_waits_for_each_published_range();
  return 0;
}
