#include "speaker_assets/sound_asset_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace easy_input::speaker_assets {
namespace {

constexpr std::uint32_t kManifestHeaderBytes = 32U;
constexpr std::uint32_t kResourceRecordBytes = 48U;
constexpr std::uint32_t kMappingRecordBytes = 4U;
constexpr std::uint16_t kManifestVersion = 1U;
constexpr std::uint16_t kMaximumResourceCount = 64U;
constexpr std::uint16_t kMaximumMappingCount = 64U;
constexpr std::uint32_t kMaximumBootSamples = 384000U;
constexpr std::uint32_t kMaximumActionSamples = 96000U;

constexpr std::uint8_t kImaAdpcmCodec = 1U;
constexpr std::uint8_t kMonoChannels = 1U;
constexpr std::uint8_t kImaAdpcmVersion = 1U;
constexpr std::uint32_t kImaAdpcmSampleRate = 48000U;
constexpr std::uint16_t kImaAdpcmFrameSamples = 480U;
constexpr std::uint16_t kImaAdpcmHeaderBytes = 20U;
constexpr std::uint16_t kImaAdpcmFrameHeaderBytes = 6U;
constexpr std::uint8_t kImaAdpcmStepCount = 89U;

constexpr std::uint8_t kBootTrigger = 1U;
constexpr std::uint8_t kKeyTrigger = 2U;
constexpr std::uint8_t kEncoderLeftTrigger = 3U;
constexpr std::uint8_t kEncoderRightTrigger = 4U;
constexpr std::uint8_t kEncoderPressTrigger = 5U;
constexpr std::size_t kSha256FinishBudgetBytes = 128U;

constexpr std::array<std::uint8_t, 4> kManifestMagic{{
    'E', 'I', 'S', 'M',
}};
constexpr std::array<std::uint8_t, 4> kImaAdpcmMagic{{
    'E', 'I', 'A', 'D',
}};
constexpr std::array<std::uint8_t, 16> kBundleDomain{{
    'E', 'A', 'S', 'Y', 'I', 'N', 'P', 'U',
    'T', '-', 'S', 'N', 'D', '-', 'V', '1',
}};

struct ResourceRecord {
  SoundSha256Digest sha256{};
  std::uint32_t payload_offset = 0;
  std::uint32_t encoded_bytes = 0;
  std::uint32_t decoded_samples = 0;
  std::uint16_t frame_count = 0;
  std::uint8_t codec = 0;
  std::uint8_t channels = 0;
};

struct MappingRecord {
  std::uint8_t trigger = 0;
  std::uint8_t index = 0;
  std::uint16_t resource_index = 0;
};

bool bank_id_is_valid(SoundBankId bank) {
  return bank == SoundBankId::A || bank == SoundBankId::B;
}

std::uint16_t read_le16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(bytes[0]) |
      (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::array<std::uint8_t, 4> write_le32(std::uint32_t value) {
  return {{
      static_cast<std::uint8_t>(value),
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value >> 16U),
      static_cast<std::uint8_t>(value >> 24U),
  }};
}

bool checked_add(std::uint32_t first,
                 std::uint32_t second,
                 std::uint32_t* result) {
  if (result == nullptr ||
      second > std::numeric_limits<std::uint32_t>::max() - first) {
    return false;
  }
  *result = first + second;
  return true;
}

bool checked_multiply(std::uint32_t first,
                      std::uint32_t second,
                      std::uint32_t* result) {
  if (result == nullptr ||
      (first != 0U &&
       second > std::numeric_limits<std::uint32_t>::max() / first)) {
    return false;
  }
  *result = first * second;
  return true;
}

SoundFormatResult read_exact(SoundBankStorage& storage,
                             SoundBankId bank,
                             std::uint32_t offset,
                             std::uint8_t* destination,
                             std::size_t length) {
  return storage.read(bank, offset, destination, length) ==
                 SoundStorageIoResult::Ok
             ? SoundFormatResult::Ok
             : SoundFormatResult::IoError;
}

bool bytes_equal(const std::uint8_t* bytes,
                 const std::uint8_t* expected,
                 std::size_t length) {
  if (bytes == nullptr || expected == nullptr) {
    return false;
  }
  for (std::size_t index = 0; index < length; ++index) {
    if (bytes[index] != expected[index]) {
      return false;
    }
  }
  return true;
}

bool bytes_are_zero(const std::uint8_t* bytes, std::size_t length) {
  if (bytes == nullptr) {
    return false;
  }
  std::uint8_t combined = 0U;
  for (std::size_t index = 0; index < length; ++index) {
    combined = static_cast<std::uint8_t>(combined | bytes[index]);
  }
  return combined == 0U;
}

bool manifest_shape_is_valid(std::uint16_t resource_count,
                             std::uint16_t mapping_count,
                             std::uint32_t manifest_bytes) {
  if (resource_count > kMaximumResourceCount ||
      mapping_count > kMaximumMappingCount) {
    return false;
  }

  std::uint32_t resource_bytes = 0;
  std::uint32_t mapping_bytes = 0;
  std::uint32_t exact_bytes = kManifestHeaderBytes;
  return checked_multiply(resource_count,
                          kResourceRecordBytes,
                          &resource_bytes) &&
         checked_multiply(mapping_count,
                          kMappingRecordBytes,
                          &mapping_bytes) &&
         checked_add(exact_bytes, resource_bytes, &exact_bytes) &&
         checked_add(exact_bytes, mapping_bytes, &exact_bytes) &&
         exact_bytes == manifest_bytes;
}

SoundFormatResult read_resource_record(SoundBankStorage& storage,
                                       SoundBankId bank,
                                       std::uint16_t resource_index,
                                       ResourceRecord* record) {
  if (record == nullptr) {
    return SoundFormatResult::InvalidArgument;
  }

  std::uint32_t record_delta = 0;
  std::uint32_t manifest_offset = 0;
  std::uint32_t bank_offset = 0;
  if (!checked_multiply(resource_index,
                        kResourceRecordBytes,
                        &record_delta) ||
      !checked_add(kManifestHeaderBytes,
                   record_delta,
                   &manifest_offset) ||
      !checked_add(kSoundManifestOffset,
                   manifest_offset,
                   &bank_offset)) {
    return SoundFormatResult::InvalidManifest;
  }

  std::array<std::uint8_t, kResourceRecordBytes> encoded{};
  const auto read_result =
      read_exact(storage, bank, bank_offset, encoded.data(), encoded.size());
  if (read_result != SoundFormatResult::Ok) {
    return read_result;
  }

  for (std::size_t index = 0; index < record->sha256.size(); ++index) {
    record->sha256[index] = encoded[index];
  }
  record->payload_offset = read_le32(encoded.data() + 32U);
  record->encoded_bytes = read_le32(encoded.data() + 36U);
  record->decoded_samples = read_le32(encoded.data() + 40U);
  record->frame_count = read_le16(encoded.data() + 44U);
  record->codec = encoded[46U];
  record->channels = encoded[47U];
  return SoundFormatResult::Ok;
}

SoundFormatResult read_mapping_record(SoundBankStorage& storage,
                                      SoundBankId bank,
                                      std::uint16_t resource_count,
                                      std::uint16_t mapping_index,
                                      MappingRecord* record) {
  if (record == nullptr) {
    return SoundFormatResult::InvalidArgument;
  }

  std::uint32_t resource_bytes = 0;
  std::uint32_t mapping_delta = 0;
  std::uint32_t manifest_offset = kManifestHeaderBytes;
  std::uint32_t bank_offset = 0;
  if (!checked_multiply(resource_count,
                        kResourceRecordBytes,
                        &resource_bytes) ||
      !checked_multiply(mapping_index,
                        kMappingRecordBytes,
                        &mapping_delta) ||
      !checked_add(manifest_offset,
                   resource_bytes,
                   &manifest_offset) ||
      !checked_add(manifest_offset,
                   mapping_delta,
                   &manifest_offset) ||
      !checked_add(kSoundManifestOffset,
                   manifest_offset,
                   &bank_offset)) {
    return SoundFormatResult::InvalidManifest;
  }

  std::array<std::uint8_t, kMappingRecordBytes> encoded{};
  const auto read_result =
      read_exact(storage, bank, bank_offset, encoded.data(), encoded.size());
  if (read_result != SoundFormatResult::Ok) {
    return read_result;
  }

  record->trigger = encoded[0];
  record->index = encoded[1];
  record->resource_index = read_le16(encoded.data() + 2U);
  return SoundFormatResult::Ok;
}

SoundFormatResult hash_bank_range(SoundBankStorage& storage,
                                  SoundBankId bank,
                                  std::uint32_t offset,
                                  std::uint32_t length,
                                  SoundSha256* primary,
                                  SoundSha256* secondary) {
  if (primary == nullptr) {
    return SoundFormatResult::InvalidArgument;
  }

  std::array<std::uint8_t, 256> buffer{};
  std::uint32_t consumed = 0;
  while (consumed < length) {
    const std::uint32_t remaining = length - consumed;
    const std::size_t chunk =
        remaining < buffer.size()
            ? static_cast<std::size_t>(remaining)
            : buffer.size();
    std::uint32_t read_offset = 0;
    if (!checked_add(offset, consumed, &read_offset)) {
      return SoundFormatResult::InvalidArgument;
    }
    const auto read_result =
        read_exact(storage, bank, read_offset, buffer.data(), chunk);
    if (read_result != SoundFormatResult::Ok) {
      return read_result;
    }
    if (storage.checkpoint(
            SoundStorageWorkKind::Sha256,
            bank,
            read_offset,
            chunk) != SoundStorageIoResult::Ok) {
      return SoundFormatResult::IoError;
    }
    const bool primary_updated =
        primary->update(buffer.data(), chunk);
    storage.checkpoint_complete();
    if (!primary_updated) {
      return SoundFormatResult::InvalidArgument;
    }
    if (secondary != nullptr) {
      if (storage.checkpoint(
              SoundStorageWorkKind::Sha256,
              bank,
              read_offset,
              chunk) != SoundStorageIoResult::Ok) {
        return SoundFormatResult::IoError;
      }
      const bool secondary_updated =
          secondary->update(buffer.data(), chunk);
      storage.checkpoint_complete();
      if (!secondary_updated) {
        return SoundFormatResult::InvalidArgument;
      }
    }
    consumed += static_cast<std::uint32_t>(chunk);
  }
  return SoundFormatResult::Ok;
}

SoundFormatResult finish_hash(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t offset,
    SoundSha256* hash,
    SoundSha256Digest* digest) {
  if (hash == nullptr || digest == nullptr) {
    return SoundFormatResult::InvalidArgument;
  }
  if (storage.checkpoint(
          SoundStorageWorkKind::Sha256,
          bank,
          offset,
          kSha256FinishBudgetBytes) != SoundStorageIoResult::Ok) {
    return SoundFormatResult::IoError;
  }
  *digest = hash->finish();
  storage.checkpoint_complete();
  return SoundFormatResult::Ok;
}

SoundFormatResult calculate_resource_digest(
    SoundBankStorage& storage,
    SoundBankId bank,
    const ResourceRecord& record,
    SoundSha256Digest* digest) {
  if (digest == nullptr) {
    return SoundFormatResult::InvalidArgument;
  }

  std::uint32_t resource_offset = 0;
  if (!checked_add(kSoundPayloadOffset,
                   record.payload_offset,
                   &resource_offset)) {
    return SoundFormatResult::InvalidResource;
  }
  SoundSha256 hash;
  const auto hash_result =
      hash_bank_range(storage,
                      bank,
                      resource_offset,
                      record.encoded_bytes,
                      &hash,
                      nullptr);
  if (hash_result != SoundFormatResult::Ok) {
    return hash_result;
  }
  return finish_hash(
      storage, bank, resource_offset, &hash, digest);
}

SoundFormatResult validate_eiad_resource(SoundBankStorage& storage,
                                         SoundBankId bank,
                                         const ResourceRecord& record) {
  if (record.encoded_bytes < kImaAdpcmHeaderBytes ||
      record.decoded_samples == 0U ||
      record.decoded_samples > kMaximumBootSamples ||
      record.frame_count == 0U ||
      record.frame_count !=
          1U + ((record.decoded_samples - 1U) /
                kImaAdpcmFrameSamples) ||
      record.codec != kImaAdpcmCodec ||
      record.channels != kMonoChannels) {
    return SoundFormatResult::InvalidResource;
  }

  std::uint32_t resource_offset = 0;
  if (!checked_add(kSoundPayloadOffset,
                   record.payload_offset,
                   &resource_offset)) {
    return SoundFormatResult::InvalidResource;
  }

  std::array<std::uint8_t, kImaAdpcmHeaderBytes> header{};
  auto read_result = read_exact(
      storage, bank, resource_offset, header.data(), header.size());
  if (read_result != SoundFormatResult::Ok) {
    return read_result;
  }
  if (!bytes_equal(header.data(),
                   kImaAdpcmMagic.data(),
                   kImaAdpcmMagic.size()) ||
      header[4U] != kImaAdpcmVersion ||
      header[5U] != kMonoChannels ||
      read_le32(header.data() + 6U) != kImaAdpcmSampleRate ||
      read_le16(header.data() + 10U) != kImaAdpcmFrameSamples ||
      read_le16(header.data() + 12U) != record.frame_count ||
      read_le32(header.data() + 14U) != record.decoded_samples ||
      read_le16(header.data() + 18U) != kImaAdpcmHeaderBytes) {
    return SoundFormatResult::InvalidResource;
  }

  std::uint32_t cursor = kImaAdpcmHeaderBytes;
  std::uint32_t observed_samples = 0;
  for (std::uint16_t frame_index = 0;
       frame_index < record.frame_count;
       ++frame_index) {
    if (cursor > record.encoded_bytes ||
        kImaAdpcmFrameHeaderBytes > record.encoded_bytes - cursor) {
      return SoundFormatResult::InvalidResource;
    }

    std::uint32_t frame_offset = 0;
    if (!checked_add(resource_offset, cursor, &frame_offset)) {
      return SoundFormatResult::InvalidResource;
    }
    std::array<std::uint8_t, kImaAdpcmFrameHeaderBytes> frame_header{};
    read_result = read_exact(storage,
                             bank,
                             frame_offset,
                             frame_header.data(),
                             frame_header.size());
    if (read_result != SoundFormatResult::Ok) {
      return read_result;
    }

    const std::uint16_t sample_count = read_le16(frame_header.data());
    const bool final_frame =
        frame_index + 1U == record.frame_count;
    const std::uint32_t expected_samples =
        final_frame
            ? record.decoded_samples - observed_samples
            : kImaAdpcmFrameSamples;
    if (sample_count == 0U ||
        sample_count > kImaAdpcmFrameSamples ||
        sample_count != expected_samples ||
        frame_header[4U] >= kImaAdpcmStepCount ||
        frame_header[5U] != 0U) {
      return SoundFormatResult::InvalidResource;
    }

    if (!checked_add(cursor,
                     kImaAdpcmFrameHeaderBytes,
                     &cursor)) {
      return SoundFormatResult::InvalidResource;
    }
    const std::uint32_t encoded_sample_bytes =
        static_cast<std::uint32_t>(sample_count) / 2U;
    if (cursor > record.encoded_bytes ||
        encoded_sample_bytes > record.encoded_bytes - cursor) {
      return SoundFormatResult::InvalidResource;
    }

    if ((sample_count % 2U) == 0U && encoded_sample_bytes != 0U) {
      std::uint32_t final_payload_byte = 0;
      std::uint32_t padding_offset = 0;
      if (!checked_add(cursor,
                       encoded_sample_bytes - 1U,
                       &final_payload_byte) ||
          !checked_add(resource_offset,
                       final_payload_byte,
                       &padding_offset)) {
        return SoundFormatResult::InvalidResource;
      }
      std::uint8_t padding_byte = 0;
      read_result =
          read_exact(storage, bank, padding_offset, &padding_byte, 1U);
      if (read_result != SoundFormatResult::Ok) {
        return read_result;
      }
      if ((padding_byte & 0xF0U) != 0U) {
        return SoundFormatResult::InvalidResource;
      }
    }

    if (!checked_add(cursor, encoded_sample_bytes, &cursor) ||
        !checked_add(observed_samples,
                     sample_count,
                     &observed_samples)) {
      return SoundFormatResult::InvalidResource;
    }
  }

  return cursor == record.encoded_bytes &&
                 observed_samples == record.decoded_samples
             ? SoundFormatResult::Ok
             : SoundFormatResult::InvalidResource;
}

SoundFormatResult validate_unique_resource_sha(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint16_t resource_index,
    const SoundSha256Digest& digest) {
  for (std::uint16_t previous_index = 0;
       previous_index < resource_index;
       ++previous_index) {
    ResourceRecord previous{};
    const auto read_result =
        read_resource_record(storage, bank, previous_index, &previous);
    if (read_result != SoundFormatResult::Ok) {
      return read_result;
    }
    if (sound_digest_equal(previous.sha256, digest)) {
      return SoundFormatResult::InvalidResource;
    }
  }
  return SoundFormatResult::Ok;
}

bool mapping_target_bit(const MappingRecord& mapping,
                        std::uint16_t* target_bit) {
  if (target_bit == nullptr) {
    return false;
  }

  std::uint8_t bit_index = 0;
  switch (mapping.trigger) {
    case kBootTrigger:
      if (mapping.index != 0U) {
        return false;
      }
      bit_index = 0U;
      break;
    case kKeyTrigger:
      if (mapping.index < 1U || mapping.index > 8U) {
        return false;
      }
      bit_index = mapping.index;
      break;
    case kEncoderLeftTrigger:
    case kEncoderRightTrigger:
    case kEncoderPressTrigger:
      if (mapping.index != 0U) {
        return false;
      }
      bit_index = static_cast<std::uint8_t>(
          9U + mapping.trigger - kEncoderLeftTrigger);
      break;
    default:
      return false;
  }

  *target_bit = static_cast<std::uint16_t>(1U << bit_index);
  return true;
}

std::uint64_t complete_resource_mask(std::uint16_t resource_count) {
  if (resource_count == 0U) {
    return 0U;
  }
  if (resource_count == 64U) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return (UINT64_C(1) << resource_count) - UINT64_C(1);
}

}  // namespace

SoundFormatResult validate_sound_manifest(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t manifest_bytes,
    std::uint32_t payload_bytes,
    SoundManifestSummary* summary) {
  if (summary == nullptr || !bank_id_is_valid(bank) ||
      manifest_bytes < kManifestHeaderBytes ||
      manifest_bytes > kSoundSectorSize ||
      payload_bytes > kSoundPayloadMaxSize) {
    return SoundFormatResult::InvalidArgument;
  }
  *summary = {};

  std::array<std::uint8_t, kManifestHeaderBytes> header{};
  auto result = read_exact(storage,
                           bank,
                           kSoundManifestOffset,
                           header.data(),
                           header.size());
  if (result != SoundFormatResult::Ok) {
    return result;
  }

  const std::uint16_t resource_count = read_le16(header.data() + 16U);
  const std::uint16_t mapping_count = read_le16(header.data() + 18U);
  if (!bytes_equal(header.data(),
                   kManifestMagic.data(),
                   kManifestMagic.size()) ||
      read_le16(header.data() + 4U) != kManifestVersion ||
      read_le16(header.data() + 6U) != kManifestHeaderBytes ||
      read_le32(header.data() + 8U) != manifest_bytes ||
      read_le32(header.data() + 12U) != payload_bytes ||
      !bytes_are_zero(header.data() + 20U, 12U) ||
      !manifest_shape_is_valid(resource_count,
                               mapping_count,
                               manifest_bytes)) {
    return SoundFormatResult::InvalidManifest;
  }

  std::uint32_t expected_payload_offset = 0;
  for (std::uint16_t resource_index = 0;
       resource_index < resource_count;
       ++resource_index) {
    ResourceRecord resource{};
    result =
        read_resource_record(storage, bank, resource_index, &resource);
    if (result != SoundFormatResult::Ok) {
      return result;
    }

    if (resource.payload_offset != expected_payload_offset ||
        resource.encoded_bytes == 0U ||
        resource.payload_offset > payload_bytes ||
        resource.encoded_bytes >
            payload_bytes - resource.payload_offset ||
        !checked_add(resource.payload_offset,
                     resource.encoded_bytes,
                     &expected_payload_offset)) {
      return SoundFormatResult::InvalidResource;
    }

    result = validate_unique_resource_sha(
        storage, bank, resource_index, resource.sha256);
    if (result != SoundFormatResult::Ok) {
      return result;
    }
    result = validate_eiad_resource(storage, bank, resource);
    if (result != SoundFormatResult::Ok) {
      return result;
    }

    SoundSha256Digest observed_digest{};
    result =
        calculate_resource_digest(storage, bank, resource, &observed_digest);
    if (result != SoundFormatResult::Ok) {
      return result;
    }
    if (!sound_digest_equal(resource.sha256, observed_digest)) {
      return SoundFormatResult::HashMismatch;
    }
  }
  if (expected_payload_offset != payload_bytes) {
    return SoundFormatResult::InvalidResource;
  }

  std::uint16_t seen_targets = 0U;
  std::uint64_t referenced_resources = 0U;
  for (std::uint16_t mapping_index = 0;
       mapping_index < mapping_count;
       ++mapping_index) {
    MappingRecord mapping{};
    result = read_mapping_record(storage,
                                 bank,
                                 resource_count,
                                 mapping_index,
                                 &mapping);
    if (result != SoundFormatResult::Ok) {
      return result;
    }

    std::uint16_t target_bit = 0U;
    if (!mapping_target_bit(mapping, &target_bit) ||
        (seen_targets & target_bit) != 0U ||
        mapping.resource_index >= resource_count) {
      return SoundFormatResult::InvalidMapping;
    }

    ResourceRecord referenced{};
    result = read_resource_record(
        storage, bank, mapping.resource_index, &referenced);
    if (result != SoundFormatResult::Ok) {
      return result;
    }
    const std::uint32_t sample_limit =
        mapping.trigger == kBootTrigger
            ? kMaximumBootSamples
            : kMaximumActionSamples;
    if (referenced.decoded_samples > sample_limit) {
      return SoundFormatResult::InvalidMapping;
    }

    seen_targets =
        static_cast<std::uint16_t>(seen_targets | target_bit);
    referenced_resources |= UINT64_C(1) << mapping.resource_index;
  }
  if (referenced_resources != complete_resource_mask(resource_count)) {
    return SoundFormatResult::InvalidMapping;
  }

  summary->manifest_bytes = manifest_bytes;
  summary->payload_bytes = payload_bytes;
  summary->resource_count = resource_count;
  summary->mapping_count = mapping_count;
  return SoundFormatResult::Ok;
}

SoundFormatResult calculate_sound_bank_digests(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t manifest_bytes,
    std::uint32_t payload_bytes,
    SoundSha256Digest* manifest_digest,
    SoundSha256Digest* bundle_digest) {
  if (manifest_digest == nullptr || bundle_digest == nullptr ||
      manifest_digest == bundle_digest || !bank_id_is_valid(bank) ||
      manifest_bytes < kManifestHeaderBytes ||
      manifest_bytes > kSoundSectorSize ||
      payload_bytes > kSoundPayloadMaxSize) {
    return SoundFormatResult::InvalidArgument;
  }
  manifest_digest->fill(0U);
  bundle_digest->fill(0U);

  SoundSha256 manifest_hash;
  SoundSha256 bundle_hash;
  const auto manifest_length = write_le32(manifest_bytes);
  const auto payload_length = write_le32(payload_bytes);
  std::array<std::uint8_t,
             kBundleDomain.size() + manifest_length.size()>
      bundle_prefix{};
  std::copy(
      kBundleDomain.begin(),
      kBundleDomain.end(),
      bundle_prefix.begin());
  std::copy(
      manifest_length.begin(),
      manifest_length.end(),
      bundle_prefix.begin() + kBundleDomain.size());
  if (storage.checkpoint(
          SoundStorageWorkKind::Sha256,
          bank,
          kSoundManifestOffset,
          bundle_prefix.size()) != SoundStorageIoResult::Ok) {
    return SoundFormatResult::IoError;
  }
  const bool prefix_updated =
      bundle_hash.update(bundle_prefix.data(), bundle_prefix.size());
  storage.checkpoint_complete();
  if (!prefix_updated) {
    return SoundFormatResult::InvalidArgument;
  }

  auto result = hash_bank_range(storage,
                                bank,
                                kSoundManifestOffset,
                                manifest_bytes,
                                &manifest_hash,
                                &bundle_hash);
  if (result != SoundFormatResult::Ok) {
    return result;
  }
  result = finish_hash(
      storage,
      bank,
      kSoundManifestOffset,
      &manifest_hash,
      manifest_digest);
  if (result != SoundFormatResult::Ok) {
    return result;
  }

  if (storage.checkpoint(
          SoundStorageWorkKind::Sha256,
          bank,
          kSoundPayloadOffset,
          payload_length.size()) != SoundStorageIoResult::Ok) {
    manifest_digest->fill(0U);
    return SoundFormatResult::IoError;
  }
  const bool length_updated =
      bundle_hash.update(payload_length.data(), payload_length.size());
  storage.checkpoint_complete();
  if (!length_updated) {
    manifest_digest->fill(0U);
    return SoundFormatResult::InvalidArgument;
  }
  result = hash_bank_range(storage,
                           bank,
                           kSoundPayloadOffset,
                           payload_bytes,
                           &bundle_hash,
                           nullptr);
  if (result != SoundFormatResult::Ok) {
    manifest_digest->fill(0U);
    return result;
  }
  return finish_hash(
      storage,
      bank,
      kSoundPayloadOffset,
      &bundle_hash,
      bundle_digest);
}

}  // namespace easy_input::speaker_assets
