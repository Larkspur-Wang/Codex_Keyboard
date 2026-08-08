#include "speaker_assets/sound_asset_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
constexpr std::uint16_t kEiadHeaderBytes = 20U;
constexpr std::uint8_t kEiadVersion = 1U;
constexpr std::uint8_t kEiadChannels = 1U;
constexpr std::uint8_t kImaAdpcmCodec = 1U;
constexpr std::uint8_t kImaStepCount = 89U;

constexpr std::array<std::uint8_t, 4> kManifestMagic{{
    'E', 'I', 'S', 'M',
}};
constexpr std::array<std::uint8_t, 4> kEiadMagic{{
    'E', 'I', 'A', 'D',
}};

constexpr std::array<std::int32_t, kImaStepCount> kStepTable{{
    7,     8,     9,     10,    11,    12,    13,    14,    16,
    17,    19,    21,    23,    25,    28,    31,    34,    37,
    41,    45,    50,    55,    60,    66,    73,    80,    88,
    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,
    1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,
    3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}};

constexpr std::array<std::int8_t, 16> kIndexDelta{{
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
}};

bool bank_id_is_valid(SoundBankId bank) {
  return bank == SoundBankId::A || bank == SoundBankId::B;
}

std::uint16_t read_le16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(data[0]) |
      (static_cast<std::uint16_t>(data[1]) << 8U));
}

std::uint32_t read_le32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::int32_t read_signed_le16(const std::uint8_t* data) {
  const std::uint16_t raw = read_le16(data);
  return (raw & 0x8000U) == 0U
             ? static_cast<std::int32_t>(raw)
             : static_cast<std::int32_t>(raw) - 65536;
}

bool bytes_equal(const std::uint8_t* first,
                 const std::uint8_t* second,
                 std::size_t length) {
  if (first == nullptr || second == nullptr) {
    return false;
  }
  for (std::size_t index = 0U; index < length; ++index) {
    if (first[index] != second[index]) {
      return false;
    }
  }
  return true;
}

bool bytes_are_zero(const std::uint8_t* data, std::size_t length) {
  if (data == nullptr) {
    return false;
  }
  std::uint8_t combined = 0U;
  for (std::size_t index = 0U; index < length; ++index) {
    combined = static_cast<std::uint8_t>(combined | data[index]);
  }
  return combined == 0U;
}

bool digest_is_nonzero(const SoundSha256Digest& digest) {
  std::uint8_t combined = 0U;
  for (const std::uint8_t byte : digest) {
    combined = static_cast<std::uint8_t>(combined | byte);
  }
  return combined != 0U;
}

bool lease_is_valid(const SoundReadLease& lease) {
  return lease.valid && lease.lease_id != 0U &&
         lease.generation != 0U && bank_id_is_valid(lease.bank) &&
         digest_is_nonzero(lease.bundle_sha256);
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

SoundAssetReadResult read_exact(SoundBankStorage& storage,
                                SoundBankId bank,
                                std::uint32_t offset,
                                std::uint8_t* destination,
                                std::size_t length) {
  return storage.read(bank, offset, destination, length) ==
                 SoundStorageIoResult::Ok
             ? SoundAssetReadResult::Ok
             : SoundAssetReadResult::IoError;
}

bool manifest_shape_is_valid(std::uint16_t resource_count,
                             std::uint16_t mapping_count,
                             std::uint32_t manifest_bytes) {
  if (resource_count > kMaximumResourceCount ||
      mapping_count > kMaximumMappingCount) {
    return false;
  }
  std::uint32_t resource_bytes = 0U;
  std::uint32_t mapping_bytes = 0U;
  std::uint32_t expected_bytes = kManifestHeaderBytes;
  return checked_multiply(resource_count,
                          kResourceRecordBytes,
                          &resource_bytes) &&
         checked_multiply(mapping_count,
                          kMappingRecordBytes,
                          &mapping_bytes) &&
         checked_add(expected_bytes, resource_bytes, &expected_bytes) &&
         checked_add(expected_bytes, mapping_bytes, &expected_bytes) &&
         expected_bytes == manifest_bytes;
}

bool mapping_target(const std::uint8_t trigger,
                    const std::uint8_t index,
                    std::uint16_t* target_bit) {
  if (target_bit == nullptr) {
    return false;
  }
  std::uint8_t bit_index = 0U;
  switch (static_cast<SoundAssetTrigger>(trigger)) {
    case SoundAssetTrigger::Boot:
      if (index != 0U) {
        return false;
      }
      bit_index = 0U;
      break;
    case SoundAssetTrigger::Key:
      if (index < 1U || index > 8U) {
        return false;
      }
      bit_index = index;
      break;
    case SoundAssetTrigger::EncoderLeft:
    case SoundAssetTrigger::EncoderRight:
    case SoundAssetTrigger::EncoderPress:
      if (index != 0U) {
        return false;
      }
      bit_index = static_cast<std::uint8_t>(
          9U + trigger -
          static_cast<std::uint8_t>(SoundAssetTrigger::EncoderLeft));
      break;
    default:
      return false;
  }
  *target_bit = static_cast<std::uint16_t>(1U << bit_index);
  return true;
}

std::uint32_t sample_limit_for(SoundAssetTrigger trigger,
                               std::uint8_t trigger_index) {
  std::uint16_t ignored_bit = 0U;
  if (!mapping_target(static_cast<std::uint8_t>(trigger),
                      trigger_index,
                      &ignored_bit)) {
    return 0U;
  }
  return trigger == SoundAssetTrigger::Boot
             ? kMaximumBootSamples
             : kMaximumActionSamples;
}

std::int16_t decode_nibble(std::uint8_t code,
                           std::int32_t* predictor,
                           std::int32_t* step_index) {
  const std::int32_t step =
      kStepTable[static_cast<std::size_t>(*step_index)];
  std::int32_t difference = step >> 3U;
  if ((code & 0x01U) != 0U) {
    difference += step >> 2U;
  }
  if ((code & 0x02U) != 0U) {
    difference += step >> 1U;
  }
  if ((code & 0x04U) != 0U) {
    difference += step;
  }

  *predictor += (code & 0x08U) != 0U
                    ? -difference
                    : difference;
  *predictor = std::clamp<std::int32_t>(
      *predictor,
      std::numeric_limits<std::int16_t>::min(),
      std::numeric_limits<std::int16_t>::max());
  *step_index = std::clamp<std::int32_t>(
      *step_index + kIndexDelta[code],
      0,
      static_cast<std::int32_t>(kStepTable.size() - 1U));
  return static_cast<std::int16_t>(*predictor);
}

}  // namespace

SoundAssetReadResult resolve_sound_asset(
    SoundBankStorage& storage,
    const SoundReadLease& lease,
    SoundAssetTrigger trigger,
    std::uint8_t trigger_index,
    SoundResolvedAsset* asset) {
  if (asset == nullptr) {
    return SoundAssetReadResult::InvalidArgument;
  }
  *asset = {};
  if (!lease_is_valid(lease)) {
    return SoundAssetReadResult::InvalidLease;
  }
  const std::uint32_t sample_limit =
      sample_limit_for(trigger, trigger_index);
  if (sample_limit == 0U) {
    return SoundAssetReadResult::InvalidArgument;
  }

  std::array<std::uint8_t, kManifestHeaderBytes> manifest_header{};
  auto result = read_exact(storage,
                           lease.bank,
                           kSoundManifestOffset,
                           manifest_header.data(),
                           manifest_header.size());
  if (result != SoundAssetReadResult::Ok) {
    return result;
  }
  const std::uint32_t manifest_bytes =
      read_le32(manifest_header.data() + 8U);
  const std::uint32_t payload_bytes =
      read_le32(manifest_header.data() + 12U);
  const std::uint16_t resource_count =
      read_le16(manifest_header.data() + 16U);
  const std::uint16_t mapping_count =
      read_le16(manifest_header.data() + 18U);
  if (!bytes_equal(manifest_header.data(),
                   kManifestMagic.data(),
                   kManifestMagic.size()) ||
      read_le16(manifest_header.data() + 4U) != kManifestVersion ||
      read_le16(manifest_header.data() + 6U) != kManifestHeaderBytes ||
      manifest_bytes < kManifestHeaderBytes ||
      manifest_bytes > kSoundSectorSize ||
      payload_bytes > kSoundPayloadMaxSize ||
      !bytes_are_zero(manifest_header.data() + 20U, 12U) ||
      !manifest_shape_is_valid(resource_count,
                               mapping_count,
                               manifest_bytes)) {
    return SoundAssetReadResult::InvalidManifest;
  }

  std::uint32_t resource_table_bytes = 0U;
  std::uint32_t mapping_table_offset = kManifestHeaderBytes;
  if (!checked_multiply(resource_count,
                        kResourceRecordBytes,
                        &resource_table_bytes) ||
      !checked_add(mapping_table_offset,
                   resource_table_bytes,
                   &mapping_table_offset)) {
    return SoundAssetReadResult::InvalidManifest;
  }

  bool found = false;
  std::uint16_t resource_index = 0U;
  std::uint16_t seen_targets = 0U;
  for (std::uint16_t mapping_index = 0U;
       mapping_index < mapping_count;
       ++mapping_index) {
    std::uint32_t mapping_delta = 0U;
    std::uint32_t manifest_offset = 0U;
    std::uint32_t bank_offset = 0U;
    if (!checked_multiply(mapping_index,
                          kMappingRecordBytes,
                          &mapping_delta) ||
        !checked_add(mapping_table_offset,
                     mapping_delta,
                     &manifest_offset) ||
        !checked_add(kSoundManifestOffset,
                     manifest_offset,
                     &bank_offset)) {
      return SoundAssetReadResult::InvalidManifest;
    }
    std::array<std::uint8_t, kMappingRecordBytes> mapping{};
    result = read_exact(storage,
                        lease.bank,
                        bank_offset,
                        mapping.data(),
                        mapping.size());
    if (result != SoundAssetReadResult::Ok) {
      return result;
    }
    const std::uint16_t mapped_resource =
        read_le16(mapping.data() + 2U);
    std::uint16_t target_bit = 0U;
    if (!mapping_target(mapping[0U], mapping[1U], &target_bit) ||
        (seen_targets & target_bit) != 0U ||
        mapped_resource >= resource_count) {
      return SoundAssetReadResult::InvalidManifest;
    }
    seen_targets =
        static_cast<std::uint16_t>(seen_targets | target_bit);
    if (mapping[0U] == static_cast<std::uint8_t>(trigger) &&
        mapping[1U] == trigger_index) {
      if (found) {
        return SoundAssetReadResult::InvalidManifest;
      }
      found = true;
      resource_index = mapped_resource;
    }
  }
  if (!found) {
    return SoundAssetReadResult::NotMapped;
  }

  std::uint32_t resource_delta = 0U;
  std::uint32_t resource_manifest_offset = kManifestHeaderBytes;
  std::uint32_t resource_bank_offset = 0U;
  if (!checked_multiply(resource_index,
                        kResourceRecordBytes,
                        &resource_delta) ||
      !checked_add(resource_manifest_offset,
                   resource_delta,
                   &resource_manifest_offset) ||
      !checked_add(kSoundManifestOffset,
                   resource_manifest_offset,
                   &resource_bank_offset)) {
    return SoundAssetReadResult::InvalidManifest;
  }
  std::array<std::uint8_t, kResourceRecordBytes> resource{};
  result = read_exact(storage,
                      lease.bank,
                      resource_bank_offset,
                      resource.data(),
                      resource.size());
  if (result != SoundAssetReadResult::Ok) {
    return result;
  }

  const std::uint32_t payload_offset =
      read_le32(resource.data() + 32U);
  const std::uint32_t encoded_bytes =
      read_le32(resource.data() + 36U);
  const std::uint32_t decoded_samples =
      read_le32(resource.data() + 40U);
  const std::uint16_t frame_count =
      read_le16(resource.data() + 44U);
  std::uint32_t encoded_bank_offset = 0U;
  std::uint32_t encoded_bank_end = 0U;
  if (encoded_bytes < kEiadHeaderBytes ||
      decoded_samples == 0U ||
      decoded_samples > sample_limit ||
      frame_count == 0U ||
      frame_count !=
          1U + ((decoded_samples - 1U) /
                kSoundAssetFrameSamples) ||
      resource[46U] != kImaAdpcmCodec ||
      resource[47U] != kEiadChannels ||
      payload_offset > payload_bytes ||
      encoded_bytes > payload_bytes - payload_offset ||
      !checked_add(kSoundPayloadOffset,
                   payload_offset,
                   &encoded_bank_offset) ||
      !checked_add(encoded_bank_offset,
                   encoded_bytes,
                   &encoded_bank_end) ||
      encoded_bank_end > kSoundPayloadOffset + kSoundPayloadMaxSize) {
    return SoundAssetReadResult::InvalidResource;
  }

  SoundResolvedAsset resolved{};
  resolved.valid = true;
  resolved.lease_id = lease.lease_id;
  resolved.bank = lease.bank;
  resolved.generation = lease.generation;
  resolved.bundle_sha256 = lease.bundle_sha256;
  std::copy(resource.begin(),
            resource.begin() + 32U,
            resolved.resource_sha256.begin());
  resolved.resource_index = resource_index;
  resolved.payload_offset = payload_offset;
  resolved.encoded_bank_offset = encoded_bank_offset;
  resolved.encoded_bytes = encoded_bytes;
  resolved.decoded_samples = decoded_samples;
  resolved.frame_count = frame_count;
  *asset = resolved;
  return SoundAssetReadResult::Ok;
}

SoundAssetReadResult SoundAssetStreamDecoder::open(
    SoundBankStorage& storage,
    const SoundReadLease& lease,
    const SoundResolvedAsset& asset) {
  close();
  if (!lease_is_valid(lease)) {
    return SoundAssetReadResult::InvalidLease;
  }
  std::uint32_t expected_bank_offset = 0U;
  std::uint32_t encoded_end = 0U;
  if (!asset.valid || asset.lease_id != lease.lease_id ||
      asset.bank != lease.bank ||
      asset.generation != lease.generation ||
      !sound_digest_equal(asset.bundle_sha256,
                          lease.bundle_sha256)) {
    return SoundAssetReadResult::InvalidLease;
  }
  if (asset.encoded_bytes < kEiadHeaderBytes ||
      asset.decoded_samples == 0U ||
      asset.decoded_samples > kMaximumBootSamples ||
      asset.frame_count == 0U ||
      asset.frame_count !=
          1U + ((asset.decoded_samples - 1U) /
                kSoundAssetFrameSamples) ||
      !checked_add(kSoundPayloadOffset,
                   asset.payload_offset,
                   &expected_bank_offset) ||
      expected_bank_offset != asset.encoded_bank_offset ||
      !checked_add(asset.encoded_bank_offset,
                   asset.encoded_bytes,
                   &encoded_end) ||
      encoded_end > kSoundPayloadOffset + kSoundPayloadMaxSize) {
    return SoundAssetReadResult::InvalidResource;
  }

  std::array<std::uint8_t, kEiadHeaderBytes> header{};
  const auto read_result = read_exact(storage,
                                      lease.bank,
                                      asset.encoded_bank_offset,
                                      header.data(),
                                      header.size());
  if (read_result != SoundAssetReadResult::Ok) {
    return read_result;
  }
  if (!bytes_equal(header.data(),
                   kEiadMagic.data(),
                   kEiadMagic.size()) ||
      header[4U] != kEiadVersion ||
      header[5U] != kEiadChannels ||
      read_le32(header.data() + 6U) != kSoundAssetSampleRate ||
      read_le16(header.data() + 10U) != kSoundAssetFrameSamples ||
      read_le16(header.data() + 12U) != asset.frame_count ||
      read_le32(header.data() + 14U) != asset.decoded_samples ||
      read_le16(header.data() + 18U) != kEiadHeaderBytes) {
    return SoundAssetReadResult::InvalidResource;
  }

  storage_ = &storage;
  asset_ = asset;
  ready_ = true;
  return reset();
}

SoundAssetReadResult SoundAssetStreamDecoder::open_embedded(
    const std::uint8_t* encoded,
    std::size_t encoded_bytes) {
  close();
  if (encoded == nullptr) {
    return SoundAssetReadResult::InvalidArgument;
  }
  if (encoded_bytes < kEiadHeaderBytes ||
      encoded_bytes > kEmbeddedSoundAssetMaximumBytes ||
      encoded_bytes >
          std::numeric_limits<std::uint32_t>::max()) {
    return SoundAssetReadResult::InvalidResource;
  }

  const std::uint16_t frame_count =
      read_le16(encoded + 12U);
  const std::uint32_t decoded_samples =
      read_le32(encoded + 14U);
  if (!bytes_equal(
          encoded, kEiadMagic.data(), kEiadMagic.size()) ||
      encoded[4U] != kEiadVersion ||
      encoded[5U] != kEiadChannels ||
      read_le32(encoded + 6U) != kSoundAssetSampleRate ||
      read_le16(encoded + 10U) != kSoundAssetFrameSamples ||
      frame_count == 0U ||
      decoded_samples == 0U ||
      decoded_samples > kEmbeddedSoundAssetMaximumSamples ||
      frame_count !=
          1U + ((decoded_samples - 1U) /
                kSoundAssetFrameSamples) ||
      read_le16(encoded + 18U) != kEiadHeaderBytes) {
    return SoundAssetReadResult::InvalidResource;
  }

  embedded_data_ = encoded;
  asset_.encoded_bytes =
      static_cast<std::uint32_t>(encoded_bytes);
  asset_.decoded_samples = decoded_samples;
  asset_.frame_count = frame_count;
  ready_ = true;
  return reset();
}

SoundAssetReadResult SoundAssetStreamDecoder::reset() {
  if (!ready_) {
    return SoundAssetReadResult::NotReady;
  }
  next_encoded_offset_ = kEiadHeaderBytes;
  decoded_samples_ = 0U;
  next_frame_index_ = 0U;
  return SoundAssetReadResult::Ok;
}

void SoundAssetStreamDecoder::close() {
  storage_ = nullptr;
  embedded_data_ = nullptr;
  asset_ = {};
  next_encoded_offset_ = 0U;
  decoded_samples_ = 0U;
  next_frame_index_ = 0U;
  ready_ = false;
  encoded_frame_.fill(0U);
}

SoundAssetReadResult SoundAssetStreamDecoder::read_encoded(
    std::uint32_t offset,
    std::uint8_t* destination,
    std::size_t length) {
  if (!ready_ || destination == nullptr ||
      offset > asset_.encoded_bytes ||
      length >
          static_cast<std::size_t>(
              asset_.encoded_bytes - offset)) {
    return SoundAssetReadResult::InvalidResource;
  }
  if (storage_ != nullptr) {
    std::uint32_t bank_offset = 0U;
    if (!checked_add(
            asset_.encoded_bank_offset, offset, &bank_offset)) {
      return SoundAssetReadResult::InvalidResource;
    }
    return read_exact(
        *storage_,
        asset_.bank,
        bank_offset,
        destination,
        length);
  }
  if (embedded_data_ == nullptr) {
    return SoundAssetReadResult::NotReady;
  }
  std::memcpy(destination, embedded_data_ + offset, length);
  return SoundAssetReadResult::Ok;
}

SoundAssetReadResult SoundAssetStreamDecoder::decode_next(
    std::int16_t* output,
    std::size_t output_capacity_samples,
    std::size_t* output_samples) {
  if (output_samples == nullptr) {
    return SoundAssetReadResult::InvalidArgument;
  }
  *output_samples = 0U;
  if (!ready_ ||
      (storage_ == nullptr && embedded_data_ == nullptr)) {
    return SoundAssetReadResult::NotReady;
  }
  if (next_frame_index_ >= asset_.frame_count) {
    return next_frame_index_ == asset_.frame_count &&
                   decoded_samples_ == asset_.decoded_samples &&
                   next_encoded_offset_ == asset_.encoded_bytes
               ? SoundAssetReadResult::End
               : SoundAssetReadResult::InvalidResource;
  }
  if (output == nullptr) {
    return SoundAssetReadResult::InvalidArgument;
  }
  if (output_capacity_samples < kSoundAssetFrameSamples) {
    return SoundAssetReadResult::OutputTooSmall;
  }
  if (decoded_samples_ >= asset_.decoded_samples ||
      next_encoded_offset_ > asset_.encoded_bytes ||
      kSoundAssetFrameHeaderBytes >
          asset_.encoded_bytes - next_encoded_offset_) {
    return SoundAssetReadResult::InvalidResource;
  }

  auto result = read_encoded(
      next_encoded_offset_,
      encoded_frame_.data(),
      kSoundAssetFrameHeaderBytes);
  if (result != SoundAssetReadResult::Ok) {
    return result;
  }

  const std::uint16_t sample_count =
      read_le16(encoded_frame_.data());
  const std::uint32_t remaining_samples =
      asset_.decoded_samples - decoded_samples_;
  const std::uint16_t expected_samples =
      remaining_samples < kSoundAssetFrameSamples
          ? static_cast<std::uint16_t>(remaining_samples)
          : kSoundAssetFrameSamples;
  const std::size_t payload_bytes =
      static_cast<std::size_t>(sample_count) / 2U;
  const std::size_t frame_bytes =
      kSoundAssetFrameHeaderBytes + payload_bytes;
  if (sample_count == 0U ||
      sample_count != expected_samples ||
      sample_count > kSoundAssetFrameSamples ||
      encoded_frame_[4U] >= kImaStepCount ||
      encoded_frame_[5U] != 0U ||
      payload_bytes > kSoundAssetMaximumFramePayloadBytes ||
      frame_bytes > encoded_frame_.size() ||
      frame_bytes >
          asset_.encoded_bytes - next_encoded_offset_) {
    return SoundAssetReadResult::InvalidResource;
  }

  if (payload_bytes != 0U) {
    std::uint32_t payload_offset = 0U;
    if (!checked_add(next_encoded_offset_,
                     kSoundAssetFrameHeaderBytes,
                     &payload_offset)) {
      return SoundAssetReadResult::InvalidResource;
    }
    result = read_encoded(
        payload_offset,
        encoded_frame_.data() + kSoundAssetFrameHeaderBytes,
        payload_bytes);
    if (result != SoundAssetReadResult::Ok) {
      return result;
    }
  }
  if ((sample_count % 2U) == 0U && payload_bytes != 0U &&
      (encoded_frame_[frame_bytes - 1U] & 0xF0U) != 0U) {
    return SoundAssetReadResult::InvalidResource;
  }

  const std::uint32_t next_encoded_offset =
      next_encoded_offset_ + static_cast<std::uint32_t>(frame_bytes);
  const std::uint32_t next_decoded_samples =
      decoded_samples_ + sample_count;
  const std::uint16_t next_frame_index =
      static_cast<std::uint16_t>(next_frame_index_ + 1U);
  const bool final_frame = next_frame_index == asset_.frame_count;
  if ((final_frame &&
       (next_encoded_offset != asset_.encoded_bytes ||
        next_decoded_samples != asset_.decoded_samples)) ||
      (!final_frame &&
       (next_encoded_offset >= asset_.encoded_bytes ||
        next_decoded_samples >= asset_.decoded_samples))) {
    return SoundAssetReadResult::InvalidResource;
  }

  std::int32_t predictor =
      read_signed_le16(encoded_frame_.data() + 2U);
  std::int32_t step_index = encoded_frame_[4U];
  output[0U] = static_cast<std::int16_t>(predictor);
  for (std::size_t sample_index = 1U;
       sample_index < sample_count;
       ++sample_index) {
    const std::size_t nibble_index = sample_index - 1U;
    const std::uint8_t packed = encoded_frame_[
        kSoundAssetFrameHeaderBytes + nibble_index / 2U];
    const std::uint8_t code = static_cast<std::uint8_t>(
        (nibble_index % 2U) == 0U
            ? packed & 0x0FU
            : packed >> 4U);
    output[sample_index] =
        decode_nibble(code, &predictor, &step_index);
  }

  next_encoded_offset_ = next_encoded_offset;
  decoded_samples_ = next_decoded_samples;
  next_frame_index_ = next_frame_index;
  *output_samples = sample_count;
  return SoundAssetReadResult::Ok;
}

bool SoundAssetStreamDecoder::ready() const {
  return ready_;
}

std::uint16_t SoundAssetStreamDecoder::next_frame_index() const {
  return next_frame_index_;
}

std::uint32_t SoundAssetStreamDecoder::decoded_samples() const {
  return decoded_samples_;
}

const SoundResolvedAsset& SoundAssetStreamDecoder::asset() const {
  return asset_;
}

}  // namespace easy_input::speaker_assets
