#include "speaker_assets/sound_asset_store.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "speaker_assets/sound_asset_format.h"

namespace easy_input::speaker_assets {
namespace {

constexpr std::array<std::uint8_t, 4> kStagingMagic{{'E', 'I', 'S', 'H'}};
constexpr std::array<std::uint8_t, 4> kCommitMagic{{'E', 'I', 'S', 'C'}};
constexpr std::array<std::uint8_t, 16> kTransactionDomain{{
    'E', 'A', 'S', 'Y', 'I', 'N', 'P', 'U',
    'T', '-', 'T', 'X', 'N', '-', 'V', '1',
}};
constexpr std::uint16_t kStorageVersion = 1U;
constexpr std::size_t kStagingHeaderBytes = 704U;
constexpr std::size_t kStagingHeaderCrcOffset = 700U;
constexpr std::size_t kCommitBodyBytes = 128U;
constexpr std::size_t kCommitBodyCrcOffset = 112U;
constexpr std::size_t kCommitMarkerOffset = 128U;
constexpr std::size_t kCommitMarkerBytes = 16U;
constexpr std::size_t kJournalManifestByte = 0U;
constexpr std::size_t kJournalPayloadBitmap = 1U;
constexpr std::size_t kIoBufferBytes = 256U;
constexpr std::size_t kSha256FinishBudgetBytes = 128U;

static_assert(kSoundPayloadBlockCount == 128U);
static_assert(kSoundReservedEndOffset == kSoundCommitOffset);
static_assert(kSoundCommitOffset + kSoundSectorSize == kSoundBankSize);
static_assert(kCommitMarkerOffset % 16U == 0U);
static_assert(kCommitMarkerOffset + kCommitMarkerBytes <= kSoundSectorSize);

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

std::uint64_t read_le64(const std::uint8_t* data) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
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

void write_le64(std::uint8_t* output, std::uint64_t value) {
  for (std::size_t index = 0; index < 8U; ++index) {
    output[index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

bool all_zero(const std::uint8_t* data, std::size_t length) {
  std::uint8_t observed = 0U;
  for (std::size_t index = 0; index < length; ++index) {
    observed |= data[index];
  }
  return observed == 0U;
}

SoundStorageIoResult checkpointed_sha256_finish(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t offset,
    SoundSha256* hash,
    SoundSha256Digest* digest) {
  if (hash == nullptr || digest == nullptr) {
    return SoundStorageIoResult::InvalidArgument;
  }
  const auto checkpoint = storage.checkpoint(
      SoundStorageWorkKind::Sha256,
      bank,
      offset,
      kSha256FinishBudgetBytes);
  if (checkpoint != SoundStorageIoResult::Ok) {
    return checkpoint;
  }
  *digest = hash->finish();
  storage.checkpoint_complete();
  return SoundStorageIoResult::Ok;
}

bool transaction_id_valid(
    const std::array<std::uint8_t, kSoundTransactionIdBytes>& id) {
  return !all_zero(id.data(), id.size());
}

bool digest_is_zero(const SoundSha256Digest& digest) {
  return all_zero(digest.data(), digest.size());
}

bool bank_id_is_valid(SoundBankId bank) {
  return bank == SoundBankId::A || bank == SoundBankId::B;
}

SoundStoreResult map_io(SoundStorageIoResult result) {
  switch (result) {
    case SoundStorageIoResult::Ok:
      return SoundStoreResult::Ok;
    case SoundStorageIoResult::Unavailable:
      return SoundStoreResult::Unavailable;
    case SoundStorageIoResult::InvalidArgument:
    case SoundStorageIoResult::OutOfBounds:
    case SoundStorageIoResult::NotAligned:
      return SoundStoreResult::InvalidArgument;
    case SoundStorageIoResult::IoError:
    default:
      return SoundStoreResult::IoError;
  }
}

SoundStoreResult map_format(SoundFormatResult result) {
  switch (result) {
    case SoundFormatResult::Ok:
      return SoundStoreResult::Ok;
    case SoundFormatResult::InvalidArgument:
      return SoundStoreResult::InvalidArgument;
    case SoundFormatResult::IoError:
      return SoundStoreResult::IoError;
    case SoundFormatResult::HashMismatch:
      return SoundStoreResult::HashMismatch;
    case SoundFormatResult::InvalidManifest:
    case SoundFormatResult::InvalidResource:
    case SoundFormatResult::InvalidMapping:
    default:
      return SoundStoreResult::InvalidManifest;
  }
}

bool read_exact(SoundBankStorage& storage,
                SoundBankId bank,
                std::uint32_t offset,
                std::uint8_t* output,
                std::size_t length) {
  return storage.read(bank, offset, output, length) ==
         SoundStorageIoResult::Ok;
}

bool write_exact(SoundBankStorage& storage,
                 SoundBankId bank,
                 std::uint32_t offset,
                 const std::uint8_t* data,
                 std::size_t length) {
  return storage.write(bank, offset, data, length) ==
         SoundStorageIoResult::Ok;
}

SoundStoreResult verify_region_erased(SoundBankStorage& storage,
                                      SoundBankId bank,
                                      std::uint32_t offset,
                                      std::size_t length) {
  std::array<std::uint8_t, kIoBufferBytes> buffer{};
  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto amount = std::min(buffer.size(), length - consumed);
    const auto current = static_cast<std::uint64_t>(offset) + consumed;
    if (current > std::numeric_limits<std::uint32_t>::max()) {
      return SoundStoreResult::InvalidArgument;
    }
    const auto read_result = storage.read(
        bank,
        static_cast<std::uint32_t>(current),
        buffer.data(),
        amount);
    if (read_result != SoundStorageIoResult::Ok) {
      return map_io(read_result);
    }
    for (std::size_t index = 0; index < amount; ++index) {
      if (buffer[index] != 0xFFU) {
        return SoundStoreResult::InvalidBank;
      }
    }
    consumed += amount;
  }
  return SoundStoreResult::Ok;
}

bool region_sha256(SoundBankStorage& storage,
                   SoundBankId bank,
                   std::uint32_t offset,
                   std::size_t length,
                   SoundSha256Digest* digest) {
  if (digest == nullptr) {
    return false;
  }
  SoundSha256 sha;
  std::array<std::uint8_t, kIoBufferBytes> buffer{};
  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto amount = std::min(buffer.size(), length - consumed);
    const auto current = static_cast<std::uint64_t>(offset) + consumed;
    if (current > std::numeric_limits<std::uint32_t>::max() ||
        !read_exact(storage,
                    bank,
                    static_cast<std::uint32_t>(current),
                    buffer.data(),
                    amount)) {
      return false;
    }
    if (storage.checkpoint(
            SoundStorageWorkKind::Sha256,
            bank,
            static_cast<std::uint32_t>(current),
            amount) != SoundStorageIoResult::Ok) {
      return false;
    }
    const bool updated = sha.update(buffer.data(), amount);
    storage.checkpoint_complete();
    if (!updated) {
      return false;
    }
    consumed += amount;
  }
  return checkpointed_sha256_finish(
             storage, bank, offset, &sha, digest) ==
         SoundStorageIoResult::Ok;
}

std::uint32_t crc32_update(std::uint32_t state,
                           const std::uint8_t* data,
                           std::size_t length) {
  for (std::size_t index = 0; index < length; ++index) {
    state ^= data[index];
    for (std::uint32_t bit = 0; bit < 8U; ++bit) {
      state = (state >> 1U) ^
              (((state & 1U) != 0U) ? 0xEDB88320U : 0U);
    }
  }
  return state;
}

SoundStoreResult checkpointed_memory_crc32(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t offset,
    const std::uint8_t* data,
    std::size_t length,
    std::uint32_t* crc) {
  if (data == nullptr || crc == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  std::uint32_t state = 0xFFFFFFFFU;
  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto amount =
        std::min<std::size_t>(kIoBufferBytes, length - consumed);
    const auto current =
        static_cast<std::uint64_t>(offset) + consumed;
    if (current > std::numeric_limits<std::uint32_t>::max()) {
      return SoundStoreResult::InvalidArgument;
    }
    const auto checkpoint = storage.checkpoint(
        SoundStorageWorkKind::Crc32,
        bank,
        static_cast<std::uint32_t>(current),
        amount);
    if (checkpoint != SoundStorageIoResult::Ok) {
      return map_io(checkpoint);
    }
    state = crc32_update(state, data + consumed, amount);
    storage.checkpoint_complete();
    consumed += amount;
  }
  *crc = state ^ 0xFFFFFFFFU;
  return SoundStoreResult::Ok;
}

SoundStoreResult checkpointed_memory_sha256(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t offset,
    const std::uint8_t* data,
    std::size_t length,
    SoundSha256Digest* digest) {
  if (data == nullptr || digest == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  SoundSha256 sha;
  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto amount =
        std::min<std::size_t>(kIoBufferBytes, length - consumed);
    const auto current =
        static_cast<std::uint64_t>(offset) + consumed;
    if (current > std::numeric_limits<std::uint32_t>::max()) {
      return SoundStoreResult::InvalidArgument;
    }
    const auto checkpoint = storage.checkpoint(
        SoundStorageWorkKind::Sha256,
        bank,
        static_cast<std::uint32_t>(current),
        amount);
    if (checkpoint != SoundStorageIoResult::Ok) {
      return map_io(checkpoint);
    }
    const bool updated = sha.update(data + consumed, amount);
    storage.checkpoint_complete();
    if (!updated) {
      return SoundStoreResult::InvalidArgument;
    }
    consumed += amount;
  }
  return map_io(checkpointed_sha256_finish(
      storage, bank, offset, &sha, digest));
}

bool region_crc32(SoundBankStorage& storage,
                  SoundBankId bank,
                  std::uint32_t offset,
                  std::size_t length,
                  std::uint32_t* crc) {
  if (crc == nullptr) {
    return false;
  }
  std::array<std::uint8_t, kIoBufferBytes> buffer{};
  std::uint32_t state = 0xFFFFFFFFU;
  std::size_t consumed = 0U;
  while (consumed < length) {
    const auto amount = std::min(buffer.size(), length - consumed);
    const auto current = static_cast<std::uint64_t>(offset) + consumed;
    if (current > std::numeric_limits<std::uint32_t>::max() ||
        !read_exact(storage,
                    bank,
                    static_cast<std::uint32_t>(current),
                    buffer.data(),
                    amount)) {
      return false;
    }
    if (storage.checkpoint(
            SoundStorageWorkKind::Crc32,
            bank,
            static_cast<std::uint32_t>(current),
            amount) != SoundStorageIoResult::Ok) {
      return false;
    }
    state = crc32_update(state, buffer.data(), amount);
    storage.checkpoint_complete();
    consumed += amount;
  }
  *crc = state ^ 0xFFFFFFFFU;
  return true;
}

std::size_t payload_block_count(std::uint32_t payload_bytes) {
  return payload_bytes == 0U
             ? 0U
             : (static_cast<std::size_t>(payload_bytes) +
                kSoundPayloadBlockSize - 1U) /
                   kSoundPayloadBlockSize;
}

std::size_t payload_block_length(std::uint32_t payload_bytes,
                                 std::size_t block_index) {
  const auto offset = block_index * kSoundPayloadBlockSize;
  if (offset >= payload_bytes) {
    return 0U;
  }
  return std::min<std::size_t>(
      kSoundPayloadBlockSize,
      static_cast<std::size_t>(payload_bytes) - offset);
}

bool plan_is_valid(const SoundBundlePlan& plan) {
  if (plan.manifest_bytes < 32U ||
      plan.manifest_bytes > kSoundSectorSize ||
      plan.payload_bytes > kSoundPayloadMaxSize ||
      digest_is_zero(plan.manifest_sha256) ||
      digest_is_zero(plan.bundle_sha256)) {
    return false;
  }
  const auto count = payload_block_count(plan.payload_bytes);
  if (count > kSoundPayloadBlockCount) {
    return false;
  }
  for (std::size_t index = count;
       index < plan.payload_block_crc32.size();
       ++index) {
    if (plan.payload_block_crc32[index] != 0U) {
      return false;
    }
  }
  if (plan.base_generation == 0U) {
    return digest_is_zero(plan.base_bundle_sha256);
  }
  return !digest_is_zero(plan.base_bundle_sha256);
}

bool plans_equal(const SoundBundlePlan& left,
                 const SoundBundlePlan& right) {
  return left.base_generation == right.base_generation &&
         left.base_bundle_sha256 == right.base_bundle_sha256 &&
         left.manifest_bytes == right.manifest_bytes &&
         left.payload_bytes == right.payload_bytes &&
         left.manifest_crc32 == right.manifest_crc32 &&
         left.manifest_sha256 == right.manifest_sha256 &&
         left.bundle_sha256 == right.bundle_sha256 &&
         left.payload_block_crc32 == right.payload_block_crc32;
}

SoundStoreResult derive_transaction_id(
    SoundBankStorage& storage,
    const SoundBundlePlan& plan,
    SoundBankId target_bank,
    std::uint64_t generation,
    std::array<std::uint8_t, kSoundTransactionIdBytes>*
        transaction_id) {
  if (transaction_id == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  constexpr std::size_t kTransactionInputBytes =
      kTransactionDomain.size() + 1U + 8U + 8U +
      32U + 4U + 4U + 32U + 32U;
  static_assert(kTransactionInputBytes <= kIoBufferBytes);

  std::array<std::uint8_t, kTransactionInputBytes> input{};
  std::size_t cursor = 0U;
  const auto append = [&input, &cursor](
                          const std::uint8_t* bytes,
                          std::size_t length) {
    std::copy(bytes, bytes + length, input.begin() + cursor);
    cursor += length;
  };
  const auto encoded_bank = static_cast<std::uint8_t>(target_bank);
  std::array<std::uint8_t, 8> encoded_generation{};
  std::array<std::uint8_t, 8> encoded_base_generation{};
  std::array<std::uint8_t, 4> encoded_manifest_bytes{};
  std::array<std::uint8_t, 4> encoded_payload_bytes{};
  write_le64(encoded_generation.data(), generation);
  write_le64(encoded_base_generation.data(), plan.base_generation);
  write_le32(encoded_manifest_bytes.data(), plan.manifest_bytes);
  write_le32(encoded_payload_bytes.data(), plan.payload_bytes);
  append(kTransactionDomain.data(), kTransactionDomain.size());
  append(&encoded_bank, sizeof(encoded_bank));
  append(encoded_generation.data(), encoded_generation.size());
  append(
      encoded_base_generation.data(),
      encoded_base_generation.size());
  append(
      plan.base_bundle_sha256.data(),
      plan.base_bundle_sha256.size());
  append(
      encoded_manifest_bytes.data(),
      encoded_manifest_bytes.size());
  append(
      encoded_payload_bytes.data(),
      encoded_payload_bytes.size());
  append(plan.manifest_sha256.data(), plan.manifest_sha256.size());
  append(plan.bundle_sha256.data(), plan.bundle_sha256.size());
  if (cursor != input.size()) {
    return SoundStoreResult::InvalidArgument;
  }

  const auto checkpoint = storage.checkpoint(
      SoundStorageWorkKind::Sha256,
      target_bank,
      kSoundStagingHeaderOffset,
      input.size());
  if (checkpoint != SoundStorageIoResult::Ok) {
    return map_io(checkpoint);
  }
  SoundSha256 hash;
  const bool accepted = hash.update(input.data(), input.size());
  storage.checkpoint_complete();
  SoundSha256Digest digest{};
  const auto finish_result = checkpointed_sha256_finish(
      storage,
      target_bank,
      kSoundStagingHeaderOffset,
      &hash,
      &digest);
  if (finish_result != SoundStorageIoResult::Ok) {
    return map_io(finish_result);
  }
  transaction_id->fill(0U);
  if (accepted) {
    std::copy(digest.begin(),
              digest.begin() + transaction_id->size(),
              transaction_id->begin());
  }
  if (!transaction_id_valid(*transaction_id) && accepted) {
    std::copy(digest.begin() + transaction_id->size(),
              digest.end(),
              transaction_id->begin());
  }
  if (!transaction_id_valid(*transaction_id)) {
    transaction_id->back() = 1U;
  }
  return accepted ? SoundStoreResult::Ok
                  : SoundStoreResult::InvalidArgument;
}

SoundStoreResult encode_staging_header(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint64_t generation,
    const std::array<std::uint8_t, kSoundTransactionIdBytes>&
        transaction_id,
    const SoundBundlePlan& plan,
    std::array<std::uint8_t, kStagingHeaderBytes>* encoded) {
  encoded->fill(0U);
  std::copy(kStagingMagic.begin(), kStagingMagic.end(), encoded->begin());
  write_le16(encoded->data() + 4U, kStorageVersion);
  write_le16(encoded->data() + 6U, kStagingHeaderBytes);
  (*encoded)[8] = static_cast<std::uint8_t>(bank);
  write_le64(encoded->data() + 16U, generation);
  write_le64(encoded->data() + 24U, plan.base_generation);
  std::copy(transaction_id.begin(),
            transaction_id.end(),
            encoded->begin() + 32U);
  write_le32(encoded->data() + 48U, plan.manifest_bytes);
  write_le32(encoded->data() + 52U, plan.payload_bytes);
  write_le32(encoded->data() + 56U, kSoundPayloadBlockSize);
  write_le16(encoded->data() + 60U,
             static_cast<std::uint16_t>(
                 payload_block_count(plan.payload_bytes)));
  write_le32(encoded->data() + 64U, plan.manifest_crc32);
  std::copy(plan.base_bundle_sha256.begin(),
            plan.base_bundle_sha256.end(),
            encoded->begin() + 72U);
  std::copy(plan.manifest_sha256.begin(),
            plan.manifest_sha256.end(),
            encoded->begin() + 104U);
  std::copy(plan.bundle_sha256.begin(),
            plan.bundle_sha256.end(),
            encoded->begin() + 136U);
  for (std::size_t index = 0;
       index < plan.payload_block_crc32.size();
       ++index) {
    write_le32(encoded->data() + 168U + index * 4U,
               plan.payload_block_crc32[index]);
  }
  std::uint32_t crc = 0U;
  const auto crc_result = checkpointed_memory_crc32(
      storage,
      bank,
      kSoundStagingHeaderOffset,
      encoded->data(),
      kStagingHeaderCrcOffset,
      &crc);
  if (crc_result != SoundStoreResult::Ok) {
    return crc_result;
  }
  write_le32(encoded->data() + kStagingHeaderCrcOffset, crc);
  return SoundStoreResult::Ok;
}

SoundStoreResult decode_staging_header(
    SoundBankStorage& storage,
    const std::array<std::uint8_t, kStagingHeaderBytes>& encoded,
    SoundBankId expected_bank,
    std::uint64_t* generation,
    std::array<std::uint8_t, kSoundTransactionIdBytes>*
        transaction_id,
    SoundBundlePlan* plan) {
  if (generation == nullptr || transaction_id == nullptr ||
      plan == nullptr ||
      !std::equal(kStagingMagic.begin(),
                  kStagingMagic.end(),
                  encoded.begin()) ||
      read_le16(encoded.data() + 4U) != kStorageVersion ||
      read_le16(encoded.data() + 6U) != kStagingHeaderBytes ||
      encoded[8] != static_cast<std::uint8_t>(expected_bank) ||
      !all_zero(encoded.data() + 9U, 7U) ||
      read_le32(encoded.data() + 56U) != kSoundPayloadBlockSize ||
      read_le16(encoded.data() + 62U) != 0U ||
      read_le32(encoded.data() + 68U) != 0U ||
      !all_zero(encoded.data() + 680U, 20U)) {
    return SoundStoreResult::InvalidStaging;
  }
  std::uint32_t observed_crc = 0U;
  const auto crc_result = checkpointed_memory_crc32(
      storage,
      expected_bank,
      kSoundStagingHeaderOffset,
      encoded.data(),
      kStagingHeaderCrcOffset,
      &observed_crc);
  if (crc_result != SoundStoreResult::Ok) {
    return crc_result;
  }
  if (read_le32(encoded.data() + kStagingHeaderCrcOffset) !=
      observed_crc) {
    return SoundStoreResult::InvalidStaging;
  }

  SoundBundlePlan decoded{};
  *generation = read_le64(encoded.data() + 16U);
  decoded.base_generation = read_le64(encoded.data() + 24U);
  std::copy(encoded.begin() + 32U,
            encoded.begin() + 48U,
            transaction_id->begin());
  decoded.manifest_bytes = read_le32(encoded.data() + 48U);
  decoded.payload_bytes = read_le32(encoded.data() + 52U);
  decoded.manifest_crc32 = read_le32(encoded.data() + 64U);
  std::copy(encoded.begin() + 72U,
            encoded.begin() + 104U,
            decoded.base_bundle_sha256.begin());
  std::copy(encoded.begin() + 104U,
            encoded.begin() + 136U,
            decoded.manifest_sha256.begin());
  std::copy(encoded.begin() + 136U,
            encoded.begin() + 168U,
            decoded.bundle_sha256.begin());
  for (std::size_t index = 0;
       index < decoded.payload_block_crc32.size();
       ++index) {
    decoded.payload_block_crc32[index] =
        read_le32(encoded.data() + 168U + index * 4U);
  }
  if (*generation == 0U ||
      !transaction_id_valid(*transaction_id) ||
      !plan_is_valid(decoded) ||
      read_le16(encoded.data() + 60U) !=
          payload_block_count(decoded.payload_bytes)) {
    return SoundStoreResult::InvalidStaging;
  }
  *plan = decoded;
  return SoundStoreResult::Ok;
}

SoundStoreResult encode_commit_body(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint64_t generation,
    const std::array<std::uint8_t, kSoundTransactionIdBytes>&
        transaction_id,
    const SoundBundlePlan& plan,
    std::array<std::uint8_t, kCommitBodyBytes>* body) {
  body->fill(0U);
  write_le16(body->data(), kStorageVersion);
  write_le16(body->data() + 2U, kCommitBodyBytes);
  (*body)[4] = static_cast<std::uint8_t>(bank);
  write_le64(body->data() + 8U, generation);
  write_le64(body->data() + 16U, plan.base_generation);
  std::copy(transaction_id.begin(),
            transaction_id.end(),
            body->begin() + 24U);
  write_le32(body->data() + 40U, plan.manifest_bytes);
  write_le32(body->data() + 44U, plan.payload_bytes);
  std::copy(plan.manifest_sha256.begin(),
            plan.manifest_sha256.end(),
            body->begin() + 48U);
  std::copy(plan.bundle_sha256.begin(),
            plan.bundle_sha256.end(),
            body->begin() + 80U);
  std::uint32_t crc = 0U;
  const auto crc_result = checkpointed_memory_crc32(
      storage,
      bank,
      kSoundCommitOffset,
      body->data(),
      kCommitBodyCrcOffset,
      &crc);
  if (crc_result != SoundStoreResult::Ok) {
    return crc_result;
  }
  write_le32(body->data() + kCommitBodyCrcOffset, crc);
  return SoundStoreResult::Ok;
}

void encode_commit_marker(
    std::uint32_t body_crc,
    std::array<std::uint8_t, kCommitMarkerBytes>* marker) {
  marker->fill(0U);
  for (std::size_t index = 0; index < kCommitMagic.size(); ++index) {
    (*marker)[index] = kCommitMagic[index];
    (*marker)[4U + index] =
        static_cast<std::uint8_t>(~kCommitMagic[index]);
  }
  write_le32(marker->data() + 8U, body_crc);
  write_le32(marker->data() + 12U, ~body_crc);
}

SoundStoreResult decode_commit(
    SoundBankStorage& storage,
    SoundBankId bank,
    SoundBankSnapshot* snapshot) {
  if (snapshot == nullptr || !bank_id_is_valid(bank)) {
    return SoundStoreResult::InvalidArgument;
  }
  std::array<std::uint8_t, kCommitBodyBytes> body{};
  std::array<std::uint8_t, kCommitMarkerBytes> marker{};
  auto read_result = storage.read(
      bank,
      kSoundCommitOffset,
      body.data(),
      body.size());
  if (read_result != SoundStorageIoResult::Ok) {
    return map_io(read_result);
  }
  read_result = storage.read(
      bank,
      kSoundCommitOffset + kCommitMarkerOffset,
      marker.data(),
      marker.size());
  if (read_result != SoundStorageIoResult::Ok) {
    return map_io(read_result);
  }

  const bool body_erased = std::all_of(
      body.begin(),
      body.end(),
      [](std::uint8_t value) { return value == 0xFFU; });
  const bool marker_erased = std::all_of(
      marker.begin(),
      marker.end(),
      [](std::uint8_t value) { return value == 0xFFU; });
  if (body_erased && marker_erased) {
    return SoundStoreResult::FactoryBlank;
  }

  if (read_le16(body.data()) != kStorageVersion ||
      read_le16(body.data() + 2U) != kCommitBodyBytes ||
      body[4] != static_cast<std::uint8_t>(bank) ||
      !all_zero(body.data() + 5U, 3U) ||
      !all_zero(body.data() + 116U, 12U)) {
    return SoundStoreResult::InvalidBank;
  }
  std::uint32_t observed_crc = 0U;
  const auto crc_result = checkpointed_memory_crc32(
      storage,
      bank,
      kSoundCommitOffset,
      body.data(),
      kCommitBodyCrcOffset,
      &observed_crc);
  if (crc_result != SoundStoreResult::Ok) {
    return crc_result;
  }
  if (read_le32(body.data() + kCommitBodyCrcOffset) !=
      observed_crc) {
    return SoundStoreResult::InvalidBank;
  }
  for (std::size_t index = 0; index < kCommitMagic.size(); ++index) {
    if (marker[index] != kCommitMagic[index] ||
        marker[4U + index] !=
            static_cast<std::uint8_t>(~kCommitMagic[index])) {
      return SoundStoreResult::InvalidBank;
    }
  }
  const auto body_crc = read_le32(body.data() + kCommitBodyCrcOffset);
  if (read_le32(marker.data() + 8U) != body_crc ||
      read_le32(marker.data() + 12U) != ~body_crc) {
    return SoundStoreResult::InvalidBank;
  }

  SoundBankSnapshot decoded{};
  decoded.valid = true;
  decoded.bank = bank;
  decoded.generation = read_le64(body.data() + 8U);
  decoded.base_generation = read_le64(body.data() + 16U);
  std::copy(body.begin() + 24U,
            body.begin() + 40U,
            decoded.transaction_id.begin());
  decoded.manifest_bytes = read_le32(body.data() + 40U);
  decoded.payload_bytes = read_le32(body.data() + 44U);
  std::copy(body.begin() + 48U,
            body.begin() + 80U,
            decoded.manifest_sha256.begin());
  std::copy(body.begin() + 80U,
            body.begin() + 112U,
            decoded.bundle_sha256.begin());
  if (decoded.generation == 0U ||
      decoded.base_generation != decoded.generation - 1U ||
      !transaction_id_valid(decoded.transaction_id) ||
      decoded.manifest_bytes < 32U ||
      decoded.manifest_bytes > kSoundSectorSize ||
      decoded.payload_bytes > kSoundPayloadMaxSize ||
      digest_is_zero(decoded.manifest_sha256) ||
      digest_is_zero(decoded.bundle_sha256)) {
    return SoundStoreResult::InvalidBank;
  }
  *snapshot = decoded;
  return SoundStoreResult::Ok;
}

const SoundBankSnapshot* snapshot_for_bank(
    const SoundStoreSelection& selection,
    SoundBankId bank) {
  return bank == SoundBankId::A
             ? &selection.bank_a
             : &selection.bank_b;
}

}  // namespace

namespace {

SoundStoreResult validate_sound_bank_committed_content(
    SoundBankStorage& storage,
    SoundBankId bank,
    SoundBankSnapshot* snapshot) {
  if (snapshot == nullptr || !bank_id_is_valid(bank)) {
    return SoundStoreResult::InvalidArgument;
  }
  *snapshot = {};
  snapshot->bank = bank;
  SoundBankSnapshot decoded{};
  const auto commit_result = decode_commit(storage, bank, &decoded);
  if (commit_result != SoundStoreResult::Ok) {
    return commit_result;
  }

  SoundManifestSummary summary{};
  const auto format = validate_sound_manifest(
      storage,
      bank,
      decoded.manifest_bytes,
      decoded.payload_bytes,
      &summary);
  if (format != SoundFormatResult::Ok) {
    return map_format(format);
  }

  SoundSha256Digest manifest_digest{};
  SoundSha256Digest bundle_digest{};
  const auto digest_result = calculate_sound_bank_digests(
      storage,
      bank,
      decoded.manifest_bytes,
      decoded.payload_bytes,
      &manifest_digest,
      &bundle_digest);
  if (digest_result != SoundFormatResult::Ok) {
    return map_format(digest_result);
  }
  if (!sound_digest_equal(manifest_digest, decoded.manifest_sha256) ||
      !sound_digest_equal(bundle_digest, decoded.bundle_sha256)) {
    return SoundStoreResult::HashMismatch;
  }

  *snapshot = decoded;
  return SoundStoreResult::Ok;
}

SoundStoreResult validate_sound_bank_read_candidate(
    SoundBankStorage& storage,
    const SoundBankSnapshot& committed) {
  if (!committed.valid || !bank_id_is_valid(committed.bank) ||
      committed.manifest_bytes < 32U ||
      committed.manifest_bytes > kSoundSectorSize) {
    return SoundStoreResult::InvalidBank;
  }

  // The commit marker is written only after strict resource/bundle
  // validation. Cold start re-authenticates the small manifest that binds all
  // payload ranges and per-resource SHA-256 values, without serializing an
  // entire <=512 KiB payload through supervisor permit round-trips before the
  // first sound. Stream open and every decoded frame still validate their
  // exact structure and bounds.
  SoundSha256 manifest_hash;
  std::array<std::uint8_t, kIoBufferBytes> buffer{};
  std::size_t consumed = 0U;
  while (consumed < committed.manifest_bytes) {
    const auto amount = std::min<std::size_t>(
        buffer.size(), committed.manifest_bytes - consumed);
    const auto offset = static_cast<std::uint32_t>(
        kSoundManifestOffset + consumed);
    const auto read_result = storage.read(
        committed.bank, offset, buffer.data(), amount);
    if (read_result != SoundStorageIoResult::Ok) {
      return map_io(read_result);
    }
    const auto checkpoint = storage.checkpoint(
        SoundStorageWorkKind::Sha256,
        committed.bank,
        offset,
        amount);
    if (checkpoint != SoundStorageIoResult::Ok) {
      return map_io(checkpoint);
    }
    const bool updated = manifest_hash.update(buffer.data(), amount);
    storage.checkpoint_complete();
    if (!updated) {
      return SoundStoreResult::InvalidArgument;
    }
    consumed += amount;
  }
  SoundSha256Digest observed{};
  const auto finish_result = checkpointed_sha256_finish(
      storage,
      committed.bank,
      kSoundManifestOffset,
      &manifest_hash,
      &observed);
  if (finish_result != SoundStorageIoResult::Ok) {
    return map_io(finish_result);
  }
  return sound_digest_equal(
             observed, committed.manifest_sha256)
             ? SoundStoreResult::Ok
             : SoundStoreResult::HashMismatch;
}

bool bank_validation_failure_is_nonfatal(
    SoundStoreResult result) {
  return result == SoundStoreResult::FactoryBlank ||
         result == SoundStoreResult::InvalidBank ||
         result == SoundStoreResult::InvalidManifest ||
         result == SoundStoreResult::HashMismatch;
}

SoundStoreResult read_selection_disposition(
    const SoundStoreSelection& selection,
    SoundStoreResult bank_a_result,
    SoundStoreResult bank_b_result) {
  if (selection.split_brain) {
    return SoundStoreResult::SplitBrain;
  }
  if (selection.active_valid) {
    return SoundStoreResult::Ok;
  }
  if (bank_a_result == SoundStoreResult::FactoryBlank &&
      bank_b_result == SoundStoreResult::FactoryBlank) {
    return SoundStoreResult::FactoryBlank;
  }
  return SoundStoreResult::Unavailable;
}

}  // namespace

SoundStoreResult validate_sound_bank(SoundBankStorage& storage,
                                     SoundBankId bank,
                                     SoundBankSnapshot* snapshot) {
  const auto content_result =
      validate_sound_bank_committed_content(storage, bank, snapshot);
  if (content_result != SoundStoreResult::Ok) {
    return content_result;
  }

  const SoundBankSnapshot decoded = *snapshot;
  const auto manifest_tail =
      kSoundSectorSize - decoded.manifest_bytes;
  const auto payload_tail =
      kSoundPayloadMaxSize - decoded.payload_bytes;
  const std::array<SoundStoreResult, 4> erased_results{{
      verify_region_erased(
          storage,
          bank,
          kSoundManifestOffset + decoded.manifest_bytes,
          manifest_tail),
      verify_region_erased(
          storage,
          bank,
          kSoundPayloadOffset + decoded.payload_bytes,
          payload_tail),
      verify_region_erased(
          storage,
          bank,
          kSoundReservedOffset,
          kSoundReservedEndOffset - kSoundReservedOffset),
      verify_region_erased(
          storage,
          bank,
          kSoundCommitOffset + kCommitMarkerOffset +
              kCommitMarkerBytes,
          kSoundSectorSize - kCommitMarkerOffset -
              kCommitMarkerBytes),
  }};
  for (const auto erased_result : erased_results) {
    if (erased_result == SoundStoreResult::Unavailable ||
        erased_result == SoundStoreResult::IoError ||
        erased_result == SoundStoreResult::InvalidArgument) {
      return erased_result;
    }
  }
  for (const auto erased_result : erased_results) {
    if (erased_result != SoundStoreResult::Ok) {
      return erased_result;
    }
  }

  *snapshot = decoded;
  return SoundStoreResult::Ok;
}

SoundStoreSelection select_sound_banks(const SoundBankSnapshot& bank_a,
                                       const SoundBankSnapshot& bank_b) {
  SoundStoreSelection selection{};
  selection.bank_a = bank_a;
  selection.bank_b = bank_b;
  if (!bank_a.valid && !bank_b.valid) {
    return selection;
  }
  if (bank_a.valid && !bank_b.valid) {
    selection.active_valid = true;
    selection.active = bank_a;
    return selection;
  }
  if (!bank_a.valid && bank_b.valid) {
    selection.active_valid = true;
    selection.active = bank_b;
    return selection;
  }
  if (bank_a.generation > bank_b.generation) {
    if (bank_a.generation - bank_b.generation != 1U) {
      selection.split_brain = true;
      return selection;
    }
    selection.active_valid = true;
    selection.active = bank_a;
  } else if (bank_b.generation > bank_a.generation) {
    if (bank_b.generation - bank_a.generation != 1U) {
      selection.split_brain = true;
      return selection;
    }
    selection.active_valid = true;
    selection.active = bank_b;
  } else if (sound_digest_equal(
                 bank_a.bundle_sha256, bank_b.bundle_sha256)) {
    selection.active_valid = true;
    selection.active = bank_a;
  } else {
    selection.split_brain = true;
  }
  return selection;
}

SoundAssetStore::SoundAssetStore(SoundBankStorage& storage)
    : storage_(storage) {}

SoundStoreResult SoundAssetStore::scan() {
  if (update_active_) {
    return SoundStoreResult::Busy;
  }
  // A failed rescan must never leave an earlier selection authorized for a
  // later read or erase. Build the next selection locally and publish it only
  // after both banks have been inspected without a storage failure.
  scanned_ = false;
  read_selection_scanned_ = false;
  read_selection_result_ = SoundStoreResult::Unavailable;
  selection_ = {};
  read_selection_ = {};
  SoundBankSnapshot bank_a{};
  SoundBankSnapshot bank_b{};
  bank_a.bank = SoundBankId::A;
  bank_b.bank = SoundBankId::B;
  const auto bank_a_result =
      validate_sound_bank(storage_, SoundBankId::A, &bank_a);
  if (bank_a_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_a_result)) {
    return bank_a_result;
  }
  if (bank_a_result != SoundStoreResult::Ok) {
    bank_a.valid = false;
  }
  const auto bank_b_result =
      validate_sound_bank(storage_, SoundBankId::B, &bank_b);
  if (bank_b_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_b_result)) {
    return bank_b_result;
  }
  if (bank_b_result != SoundStoreResult::Ok) {
    bank_b.valid = false;
  }
  selection_ = select_sound_banks(bank_a, bank_b);
  scanned_ = true;
  read_selection_ = selection_;
  read_selection_scanned_ = true;
  read_selection_result_ = read_selection_disposition(
      read_selection_, bank_a_result, bank_b_result);
  return selection_.split_brain
             ? SoundStoreResult::SplitBrain
             : SoundStoreResult::Ok;
}

const SoundStoreSelection& SoundAssetStore::selection() const {
  return selection_;
}

SoundStoreResult SoundAssetStore::query_current_active(
    SoundBankSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *snapshot = {};

  // This is the App's authoritative reconciliation query, so never answer
  // from cached selection alone. In particular, a commit marker may have
  // reached Flash even when its readback reported an error and left the RAM
  // transaction active.
  SoundBankSnapshot bank_a{};
  SoundBankSnapshot bank_b{};
  bank_a.bank = SoundBankId::A;
  bank_b.bank = SoundBankId::B;
  const auto bank_a_result =
      validate_sound_bank(storage_, SoundBankId::A, &bank_a);
  if (bank_a_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_a_result)) {
    scanned_ = false;
    read_selection_scanned_ = false;
    read_selection_result_ = SoundStoreResult::Unavailable;
    selection_ = {};
    read_selection_ = {};
    return bank_a_result;
  }
  if (bank_a_result != SoundStoreResult::Ok) {
    bank_a.valid = false;
  }
  const auto bank_b_result =
      validate_sound_bank(storage_, SoundBankId::B, &bank_b);
  if (bank_b_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_b_result)) {
    scanned_ = false;
    read_selection_scanned_ = false;
    read_selection_result_ = SoundStoreResult::Unavailable;
    selection_ = {};
    read_selection_ = {};
    return bank_b_result;
  }
  if (bank_b_result != SoundStoreResult::Ok) {
    bank_b.valid = false;
  }

  const auto refreshed = select_sound_banks(bank_a, bank_b);
  if (refreshed.split_brain) {
    scanned_ = false;
    read_selection_scanned_ = false;
    read_selection_result_ = SoundStoreResult::Unavailable;
    selection_ = {};
    read_selection_ = {};
    return SoundStoreResult::SplitBrain;
  }
  selection_ = refreshed;
  scanned_ = true;
  read_selection_ = refreshed;
  read_selection_scanned_ = true;
  read_selection_result_ = read_selection_disposition(
      read_selection_, bank_a_result, bank_b_result);

  if (update_active_ && refreshed.active_valid &&
      refreshed.active.bank == update_bank_ &&
      refreshed.active.generation == update_generation_) {
    if (refreshed.active.transaction_id !=
            update_transaction_id_ ||
        !sound_digest_equal(
            refreshed.active.bundle_sha256,
            update_plan_.bundle_sha256)) {
      return SoundStoreResult::TransactionMismatch;
    }
    static_cast<void>(clear_update_state());
  }

  if (selection_.active_valid) {
    *snapshot = selection_.active;
  }
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::begin_new_update(
    const SoundBundlePlan& plan,
    SoundUpdateIdentity* identity) {
  if (identity == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *identity = {};
  if (!plan_is_valid(plan)) {
    return SoundStoreResult::InvalidArgument;
  }
  if (update_active_) {
    return SoundStoreResult::Busy;
  }
  if (!scanned_) {
    const auto scan_result = scan();
    if (scan_result != SoundStoreResult::Ok) {
      return scan_result;
    }
  }
  if (selection_.split_brain) {
    return SoundStoreResult::SplitBrain;
  }

  if (selection_.active_valid) {
    if (plan.base_generation != selection_.active.generation ||
        !sound_digest_equal(
            plan.base_bundle_sha256,
            selection_.active.bundle_sha256)) {
      return SoundStoreResult::StaleBase;
    }
    if (selection_.active.generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      return SoundStoreResult::GenerationExhausted;
    }
    update_generation_ = selection_.active.generation + 1U;
    update_bank_ = selection_.active.bank == SoundBankId::A
                       ? SoundBankId::B
                       : SoundBankId::A;
  } else {
    if (plan.base_generation != 0U ||
        !digest_is_zero(plan.base_bundle_sha256)) {
      return SoundStoreResult::StaleBase;
    }
    update_generation_ = 1U;
    update_bank_ = SoundBankId::A;
  }
  if (target_is_pinned(update_bank_)) {
    update_generation_ = 0U;
    return SoundStoreResult::BankPinned;
  }

  auto erase_result = storage_.erase(
      update_bank_, kSoundCommitOffset, kSoundSectorSize);
  if (erase_result != SoundStorageIoResult::Ok) {
    // A backend may complete an erase before reporting an error. Discard all
    // cached committed snapshots so a later outcome query must rescan Flash
    // instead of trusting a possibly erased marker.
    scanned_ = false;
    read_selection_scanned_ = false;
    read_selection_result_ = SoundStoreResult::Unavailable;
    selection_ = {};
    read_selection_ = {};
    update_generation_ = 0U;
    return map_io(erase_result);
  }
  auto* erased_snapshot =
      update_bank_ == SoundBankId::A
          ? &selection_.bank_a
          : &selection_.bank_b;
  *erased_snapshot = {};
  erased_snapshot->bank = update_bank_;
  erase_result =
      storage_.erase(update_bank_, 0U, kSoundCommitOffset);
  if (erase_result != SoundStorageIoResult::Ok) {
    update_generation_ = 0U;
    return map_io(erase_result);
  }
  const auto erased_result = verify_region_erased(
      storage_, update_bank_, 0U, kSoundBankSize);
  if (erased_result != SoundStoreResult::Ok) {
    update_generation_ = 0U;
    return erased_result;
  }

  update_plan_ = plan;
  const auto transaction_result = derive_transaction_id(
      storage_,
      update_plan_,
      update_bank_,
      update_generation_,
      &update_transaction_id_);
  if (transaction_result != SoundStoreResult::Ok) {
    update_generation_ = 0U;
    return transaction_result;
  }
  payload_complete_.fill(false);
  manifest_complete_ = false;
  update_active_ = true;

  std::array<std::uint8_t, kStagingHeaderBytes> header{};
  const auto encode_result = encode_staging_header(
      storage_,
      update_bank_,
      update_generation_,
      update_transaction_id_,
      update_plan_,
      &header);
  if (encode_result != SoundStoreResult::Ok) {
    static_cast<void>(clear_update_state());
    return encode_result;
  }
  if (!write_exact(storage_,
                   update_bank_,
                   4U,
                   header.data() + 4U,
                   header.size() - 4U)) {
    clear_update_state();
    return SoundStoreResult::IoError;
  }
  std::array<std::uint8_t, kStagingHeaderBytes> readback{};
  if (!read_exact(storage_,
                  update_bank_,
                  4U,
                  readback.data() + 4U,
                  readback.size() - 4U) ||
      !std::equal(header.begin() + 4U,
                  header.end(),
                  readback.begin() + 4U) ||
      !write_exact(storage_,
                   update_bank_,
                   0U,
                   header.data(),
                   kStagingMagic.size()) ||
      !read_exact(storage_,
                  update_bank_,
                  0U,
                  readback.data(),
                  kStagingMagic.size()) ||
      !std::equal(header.begin(),
                  header.begin() + kStagingMagic.size(),
                  readback.begin())) {
    clear_update_state();
    return SoundStoreResult::IoError;
  }

  identity->generation = update_generation_;
  identity->target_bank = update_bank_;
  identity->transaction_id = update_transaction_id_;
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::begin_or_resume_update(
    const SoundBundlePlan& plan,
    SoundUpdateIdentity* identity) {
  if (identity == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *identity = {};
  if (!plan_is_valid(plan)) {
    return SoundStoreResult::InvalidArgument;
  }

  // The common lost-ACK retry must be a true idempotent read of RAM. In
  // particular, do not revalidate Flash here: the original begin already
  // established the durable identity and exact plan.
  if (update_active_) {
    if (!plans_equal(plan, update_plan_)) {
      return SoundStoreResult::Busy;
    }
    identity->generation = update_generation_;
    identity->target_bank = update_bank_;
    identity->transaction_id = update_transaction_id_;
    return SoundStoreResult::Ok;
  }

  if (!scanned_) {
    const auto scan_result = scan();
    if (scan_result != SoundStoreResult::Ok) {
      return scan_result;
    }
  }
  if (selection_.split_brain) {
    return SoundStoreResult::SplitBrain;
  }

  SoundBankId target_bank = SoundBankId::A;
  std::uint64_t generation = 1U;
  if (selection_.active_valid) {
    if (plan.base_generation != selection_.active.generation ||
        !sound_digest_equal(
            plan.base_bundle_sha256,
            selection_.active.bundle_sha256)) {
      return SoundStoreResult::StaleBase;
    }
    if (selection_.active.generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      return SoundStoreResult::GenerationExhausted;
    }
    generation = selection_.active.generation + 1U;
    target_bank = selection_.active.bank == SoundBankId::A
                      ? SoundBankId::B
                      : SoundBankId::A;
  } else if (plan.base_generation != 0U ||
             !digest_is_zero(plan.base_bundle_sha256)) {
    return SoundStoreResult::StaleBase;
  }

  if (target_is_pinned(target_bank)) {
    return SoundStoreResult::BankPinned;
  }

  std::array<std::uint8_t, kSoundTransactionIdBytes>
      expected_transaction_id{};
  const auto expected_transaction_result = derive_transaction_id(
      storage_,
      plan,
      target_bank,
      generation,
      &expected_transaction_id);
  if (expected_transaction_result != SoundStoreResult::Ok) {
    return expected_transaction_result;
  }
  const auto* committed = snapshot_for_bank(selection_, target_bank);
  if (committed->valid) {
    // A validated older committed bank is normal safe-BEGIN input. It is not
    // an ambiguous staging transaction, so normal generation reclamation is
    // authorized.
    return begin_new_update(plan, identity);
  }

  std::array<std::uint8_t, kStagingHeaderBytes> encoded{};
  const auto read_result = storage_.read(
      target_bank,
      kSoundStagingHeaderOffset,
      encoded.data(),
      encoded.size());
  if (read_result != SoundStorageIoResult::Ok) {
    return map_io(read_result);
  }
  if (std::all_of(
          encoded.begin(),
          encoded.end(),
          [](std::uint8_t value) { return value == 0xFFU; })) {
    return begin_new_update(plan, identity);
  }

  SoundBundlePlan staged_plan{};
  std::uint64_t staged_generation = 0U;
  std::array<std::uint8_t, kSoundTransactionIdBytes>
      staged_transaction_id{};
  const auto decode_result = decode_staging_header(
      storage_,
      encoded,
      target_bank,
      &staged_generation,
      &staged_transaction_id,
      &staged_plan);
  if (decode_result != SoundStoreResult::Ok) {
    if (decode_result != SoundStoreResult::InvalidStaging) {
      return decode_result;
    }
    // Non-erased but invalid state may be the result of a torn prior write.
    // Fail closed and leave it intact for diagnostics/recovery.
    return SoundStoreResult::InvalidStaging;
  }
  std::array<std::uint8_t, kSoundTransactionIdBytes>
      observed_transaction_id{};
  const auto transaction_result = derive_transaction_id(
      storage_,
      staged_plan,
      target_bank,
      staged_generation,
      &observed_transaction_id);
  if (transaction_result != SoundStoreResult::Ok) {
    return transaction_result;
  }
  if (observed_transaction_id != staged_transaction_id) {
    return SoundStoreResult::InvalidStaging;
  }
  if (selection_.active_valid &&
      staged_generation <= selection_.active.generation) {
    // A newer (or same-generation) committed active bank proves this inactive
    // staging transaction can no longer become current. This commonly occurs
    // when reclaiming an old committed bank erased its final marker before an
    // I/O error was reported. Ordinary BEGIN may safely finish that generation
    // reclamation; explicit invalid-staging RECOVER remains forbidden from
    // erasing valid staging.
    return begin_new_update(plan, identity);
  }
  if (staged_generation != generation ||
      staged_transaction_id != expected_transaction_id ||
      !plans_equal(staged_plan, plan)) {
    // A complete but different staging transaction owns the target bank.
    return SoundStoreResult::Busy;
  }

  const auto resume_result =
      load_staging_header(target_bank, expected_transaction_id);
  if (resume_result != SoundStoreResult::Ok) {
    return resume_result;
  }
  identity->generation = update_generation_;
  identity->target_bank = update_bank_;
  identity->transaction_id = update_transaction_id_;
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::resume_update(
    const std::array<std::uint8_t, kSoundTransactionIdBytes>& transaction_id,
    SoundUpdateIdentity* identity) {
  if (identity == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *identity = {};
  if (!transaction_id_valid(transaction_id)) {
    return SoundStoreResult::InvalidArgument;
  }
  if (update_active_) {
    return SoundStoreResult::Busy;
  }
  if (!scanned_) {
    const auto scan_result = scan();
    if (scan_result != SoundStoreResult::Ok) {
      return scan_result;
    }
  }
  if (selection_.split_brain) {
    return SoundStoreResult::SplitBrain;
  }

  const std::array<SoundBankId, 2> candidates{{
      SoundBankId::A, SoundBankId::B}};
  for (const auto candidate : candidates) {
    const auto* committed = snapshot_for_bank(selection_, candidate);
    if (committed->valid || target_is_pinned(candidate)) {
      continue;
    }
    const auto result = load_staging_header(candidate, transaction_id);
    if (result == SoundStoreResult::TransactionMismatch ||
        result == SoundStoreResult::InvalidBank) {
      continue;
    }
    if (result != SoundStoreResult::Ok) {
      return result;
    }
    identity->generation = update_generation_;
    identity->target_bank = update_bank_;
    identity->transaction_id = update_transaction_id_;
    return SoundStoreResult::Ok;
  }
  return SoundStoreResult::TransactionMismatch;
}

SoundStoreResult SoundAssetStore::query_transaction_outcome(
    const std::array<std::uint8_t, kSoundTransactionIdBytes>& transaction_id,
    SoundTransactionOutcome* outcome) {
  if (outcome == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *outcome = {};
  if (!transaction_id_valid(transaction_id)) {
    return SoundStoreResult::InvalidArgument;
  }
  outcome->identity.transaction_id = transaction_id;

  if (update_active_ &&
      update_transaction_id_ == transaction_id) {
    outcome->state = SoundTransactionState::Active;
    outcome->identity.generation = update_generation_;
    outcome->identity.target_bank = update_bank_;
    outcome->manifest_bytes = update_plan_.manifest_bytes;
    outcome->payload_bytes = update_plan_.payload_bytes;
    outcome->bundle_sha256 = update_plan_.bundle_sha256;
    return SoundStoreResult::Ok;
  }

  if (!scanned_) {
    const auto scan_result = scan();
    if (scan_result != SoundStoreResult::Ok &&
        scan_result != SoundStoreResult::SplitBrain) {
      return scan_result;
    }
  }

  const std::array<const SoundBankSnapshot*, 2> committed{{
      &selection_.bank_a,
      &selection_.bank_b,
  }};
  for (const auto* snapshot : committed) {
    if (!snapshot->valid ||
        snapshot->transaction_id != transaction_id) {
      continue;
    }
    outcome->state = SoundTransactionState::Committed;
    outcome->identity.generation = snapshot->generation;
    outcome->identity.target_bank = snapshot->bank;
    outcome->manifest_bytes = snapshot->manifest_bytes;
    outcome->payload_bytes = snapshot->payload_bytes;
    outcome->bundle_sha256 = snapshot->bundle_sha256;
    return SoundStoreResult::Ok;
  }

  outcome->state = SoundTransactionState::Unknown;
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::discard_invalid_staging(
    const SoundBundlePlan& plan) {
  if (!plan_is_valid(plan)) {
    return SoundStoreResult::InvalidArgument;
  }
  if (update_active_) {
    return SoundStoreResult::Busy;
  }
  if (!scanned_) {
    const auto scan_result = scan();
    if (scan_result != SoundStoreResult::Ok) {
      return scan_result;
    }
  }
  if (selection_.split_brain) {
    return SoundStoreResult::SplitBrain;
  }

  SoundBankId target_bank = SoundBankId::A;
  if (selection_.active_valid) {
    if (plan.base_generation != selection_.active.generation ||
        !sound_digest_equal(
            plan.base_bundle_sha256,
            selection_.active.bundle_sha256)) {
      return SoundStoreResult::StaleBase;
    }
    if (selection_.active.generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      return SoundStoreResult::GenerationExhausted;
    }
    target_bank = selection_.active.bank == SoundBankId::A
                      ? SoundBankId::B
                      : SoundBankId::A;
  } else if (plan.base_generation != 0U ||
             !digest_is_zero(plan.base_bundle_sha256)) {
    return SoundStoreResult::StaleBase;
  }

  // Keep the destructive boundary explicit in case selection rules change.
  if (selection_.active_valid &&
      target_bank == selection_.active.bank) {
    return SoundStoreResult::InvalidBank;
  }
  if (target_is_pinned(target_bank)) {
    return SoundStoreResult::BankPinned;
  }
  const auto* target_snapshot =
      snapshot_for_bank(selection_, target_bank);
  if (target_snapshot->valid) {
    // The inactive bank may still be a fully valid committed rollback/history
    // snapshot even when its staging-sector bytes are independently damaged.
    // RECOVER is never a shortcut for normal committed-bank reclamation;
    // BEGIN owns that explicit generation transition.
    return SoundStoreResult::Busy;
  }

  std::array<std::uint8_t, kStagingHeaderBytes> encoded{};
  const auto read_result = storage_.read(
      target_bank,
      kSoundStagingHeaderOffset,
      encoded.data(),
      encoded.size());
  if (read_result != SoundStorageIoResult::Ok) {
    return map_io(read_result);
  }
  if (std::all_of(
          encoded.begin(),
          encoded.end(),
          [](std::uint8_t value) { return value == 0xFFU; })) {
    // The remainder may intentionally retain an older committed snapshot;
    // the ordinary safe BEGIN path owns its normal full-bank reclamation.
    return SoundStoreResult::Ok;
  }

  SoundBundlePlan staged_plan{};
  std::uint64_t staged_generation = 0U;
  std::array<std::uint8_t, kSoundTransactionIdBytes>
      staged_transaction_id{};
  const auto decode_result = decode_staging_header(
      storage_,
      encoded,
      target_bank,
      &staged_generation,
      &staged_transaction_id,
      &staged_plan);
  if (decode_result == SoundStoreResult::Ok) {
    std::array<std::uint8_t, kSoundTransactionIdBytes>
        observed_transaction_id{};
    const auto transaction_result = derive_transaction_id(
        storage_,
        staged_plan,
        target_bank,
        staged_generation,
        &observed_transaction_id);
    if (transaction_result != SoundStoreResult::Ok) {
      return transaction_result;
    }
    if (observed_transaction_id == staged_transaction_id) {
      // A complete self-authenticating transaction is never a cleanup
      // candidate, even when it belongs to a different plan.
      return SoundStoreResult::Busy;
    }
    // Preserve the existing fail-closed recovery outcome: a structurally
    // decoded but non-self-authenticating header is left untouched here.
    return SoundStoreResult::Ok;
  }
  if (decode_result != SoundStoreResult::InvalidStaging) {
    // A priority pause or backend uncertainty is not evidence that staging is
    // corrupt. Never turn it into authorization for destructive recovery.
    return decode_result;
  }

  const auto erase_result =
      storage_.erase(target_bank, 0U, kSoundBankSize);
  if (erase_result != SoundStorageIoResult::Ok) {
    // The backend may have mutated Flash before reporting failure. Force every
    // later operation to establish fresh Flash truth.
    scanned_ = false;
    read_selection_scanned_ = false;
    read_selection_result_ = SoundStoreResult::Unavailable;
    selection_ = {};
    read_selection_ = {};
    return map_io(erase_result);
  }
  const auto verify_result = verify_region_erased(
      storage_, target_bank, 0U, kSoundBankSize);
  if (verify_result != SoundStoreResult::Ok) {
    scanned_ = false;
    read_selection_scanned_ = false;
    read_selection_result_ = SoundStoreResult::Unavailable;
    selection_ = {};
    read_selection_ = {};
    return verify_result;
  }

  auto* erased_snapshot =
      target_bank == SoundBankId::A
          ? &selection_.bank_a
          : &selection_.bank_b;
  *erased_snapshot = {};
  erased_snapshot->bank = target_bank;
  selection_ =
      select_sound_banks(selection_.bank_a, selection_.bank_b);
  scanned_ = true;
  // The repaired staging region does not carry enough information to
  // distinguish a factory-blank device from another no-active-bank state.
  // Leave immutable-read selection uncached so its next consumer inspects
  // both final commit records directly.
  read_selection_ = {};
  read_selection_scanned_ = false;
  read_selection_result_ = SoundStoreResult::Unavailable;
  return selection_.split_brain
             ? SoundStoreResult::SplitBrain
             : SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::write_manifest(
    const std::uint8_t* manifest,
    std::size_t length) {
  if (!update_active_) {
    return SoundStoreResult::TransactionMismatch;
  }
  if (manifest == nullptr ||
      length != update_plan_.manifest_bytes) {
    return SoundStoreResult::InvalidArgument;
  }

  std::uint32_t input_crc = 0U;
  const auto crc_result = checkpointed_memory_crc32(
      storage_,
      update_bank_,
      kSoundManifestOffset,
      manifest,
      length,
      &input_crc);
  if (crc_result != SoundStoreResult::Ok) {
    return crc_result;
  }
  SoundSha256Digest input_digest{};
  const auto sha_result = checkpointed_memory_sha256(
      storage_,
      update_bank_,
      kSoundManifestOffset,
      manifest,
      length,
      &input_digest);
  if (sha_result != SoundStoreResult::Ok) {
    return sha_result;
  }
  if (input_crc != update_plan_.manifest_crc32 ||
      !sound_digest_equal(
          input_digest, update_plan_.manifest_sha256)) {
    return SoundStoreResult::CrcMismatch;
  }

  if (manifest_complete_) {
    const auto verify = verify_manifest();
    if (verify == SoundStoreResult::Ok) {
      return verify;
    }
    if (verify != SoundStoreResult::CrcMismatch &&
        verify != SoundStoreResult::HashMismatch) {
      return verify;
    }
    manifest_complete_ = false;
  }
  const auto erase_result = storage_.erase(
      update_bank_, kSoundManifestOffset, kSoundSectorSize);
  if (erase_result != SoundStorageIoResult::Ok) {
    return map_io(erase_result);
  }
  const auto write_result = storage_.write(
      update_bank_,
      kSoundManifestOffset,
      manifest,
      length);
  if (write_result != SoundStorageIoResult::Ok) {
    return map_io(write_result);
  }
  const auto verify = verify_manifest();
  if (verify != SoundStoreResult::Ok) {
    return verify;
  }
  manifest_complete_ = true;
  return mark_manifest_complete();
}

SoundStoreResult SoundAssetStore::write_payload_block(
    std::size_t block_index,
    const std::uint8_t* data,
    std::size_t length) {
  if (!update_active_) {
    return SoundStoreResult::TransactionMismatch;
  }
  const auto count = payload_block_count(update_plan_.payload_bytes);
  const auto expected_length =
      payload_block_length(update_plan_.payload_bytes, block_index);
  if (block_index >= count || data == nullptr ||
      length != expected_length) {
    return SoundStoreResult::InvalidArgument;
  }
  const auto physical_offset = static_cast<std::uint32_t>(
      kSoundPayloadOffset + block_index * kSoundPayloadBlockSize);
  std::uint32_t input_crc = 0U;
  const auto crc_result = checkpointed_memory_crc32(
      storage_,
      update_bank_,
      physical_offset,
      data,
      length,
      &input_crc);
  if (crc_result != SoundStoreResult::Ok) {
    return crc_result;
  }
  if (input_crc !=
      update_plan_.payload_block_crc32[block_index]) {
    return SoundStoreResult::CrcMismatch;
  }
  if (payload_complete_[block_index]) {
    bool complete = false;
    const auto verify = verify_payload_block(block_index, &complete);
    if (verify != SoundStoreResult::Ok) {
      return verify;
    }
    if (!complete) {
      payload_complete_[block_index] = false;
      return SoundStoreResult::CrcMismatch;
    }
    return SoundStoreResult::Ok;
  }

  auto io_result = storage_.erase(
      update_bank_, physical_offset, kSoundPayloadBlockSize);
  if (io_result != SoundStorageIoResult::Ok) {
    return map_io(io_result);
  }
  io_result =
      storage_.write(update_bank_, physical_offset, data, length);
  if (io_result != SoundStorageIoResult::Ok) {
    return map_io(io_result);
  }
  bool complete = false;
  const auto verify = verify_payload_block(block_index, &complete);
  if (verify != SoundStoreResult::Ok || !complete) {
    return verify == SoundStoreResult::Ok
               ? SoundStoreResult::CrcMismatch
               : verify;
  }
  payload_complete_[block_index] = true;
  return mark_payload_complete(block_index);
}

SoundStoreResult SoundAssetStore::commit_update() {
  if (!update_active_) {
    return SoundStoreResult::TransactionMismatch;
  }
  auto result = verify_manifest();
  if (result != SoundStoreResult::Ok) {
    return result;
  }
  result = verify_all_payload_blocks();
  if (result != SoundStoreResult::Ok) {
    return result;
  }

  SoundManifestSummary summary{};
  const auto format = validate_sound_manifest(
      storage_,
      update_bank_,
      update_plan_.manifest_bytes,
      update_plan_.payload_bytes,
      &summary);
  if (format != SoundFormatResult::Ok) {
    return map_format(format);
  }
  SoundSha256Digest manifest_digest{};
  SoundSha256Digest bundle_digest{};
  const auto digest_result = calculate_sound_bank_digests(
      storage_,
      update_bank_,
      update_plan_.manifest_bytes,
      update_plan_.payload_bytes,
      &manifest_digest,
      &bundle_digest);
  if (digest_result != SoundFormatResult::Ok) {
    return map_format(digest_result);
  }
  if (!sound_digest_equal(
          manifest_digest, update_plan_.manifest_sha256) ||
      !sound_digest_equal(
          bundle_digest, update_plan_.bundle_sha256)) {
    return SoundStoreResult::HashMismatch;
  }

  std::array<std::uint8_t, kCommitBodyBytes> body{};
  const auto encode_result = encode_commit_body(
      storage_,
      update_bank_,
      update_generation_,
      update_transaction_id_,
      update_plan_,
      &body);
  if (encode_result != SoundStoreResult::Ok) {
    return encode_result;
  }
  auto io_result = storage_.write(
      update_bank_, kSoundCommitOffset, body.data(), body.size());
  std::array<std::uint8_t, kCommitBodyBytes> body_readback{};
  if (!read_exact(storage_,
                  update_bank_,
                  kSoundCommitOffset,
                  body_readback.data(),
                  body_readback.size()) ||
      body_readback != body) {
    return io_result == SoundStorageIoResult::Ok
               ? SoundStoreResult::IoError
               : map_io(io_result);
  }

  std::array<std::uint8_t, kCommitMarkerBytes> marker{};
  encode_commit_marker(
      read_le32(body.data() + kCommitBodyCrcOffset), &marker);
  io_result = storage_.write(
      update_bank_,
      kSoundCommitOffset + kCommitMarkerOffset,
      marker.data(),
      marker.size());
  std::array<std::uint8_t, kCommitMarkerBytes> marker_readback{};
  if (!read_exact(storage_,
                  update_bank_,
                  kSoundCommitOffset + kCommitMarkerOffset,
                  marker_readback.data(),
                  marker_readback.size()) ||
      marker_readback != marker) {
    return io_result == SoundStorageIoResult::Ok
               ? SoundStoreResult::IoError
               : map_io(io_result);
  }

  SoundBankSnapshot committed{};
  result = validate_sound_bank(storage_, update_bank_, &committed);
  if (result != SoundStoreResult::Ok ||
      committed.generation != update_generation_ ||
      committed.transaction_id != update_transaction_id_ ||
      !sound_digest_equal(
          committed.bundle_sha256, update_plan_.bundle_sha256)) {
    return result == SoundStoreResult::Ok
               ? SoundStoreResult::HashMismatch
               : result;
  }

  result = refresh_selection_after_commit();
  if (result != SoundStoreResult::Ok) {
    return result;
  }
  return clear_update_state();
}

SoundStoreResult SoundAssetStore::abort_update() {
  if (!update_active_) {
    return SoundStoreResult::Ok;
  }

  // The marker is the point of no return. A backend may fully program it and
  // still report an I/O error (for example, a transport timeout after the
  // Flash operation completed). Never claim to cancel such a transaction:
  // recover the committed generation into the in-memory selection instead.
  SoundBankSnapshot committed{};
  const auto committed_result =
      validate_sound_bank(storage_, update_bank_, &committed);
  if (committed_result == SoundStoreResult::Ok) {
    if (committed.generation != update_generation_ ||
        committed.transaction_id != update_transaction_id_ ||
        !sound_digest_equal(
            committed.bundle_sha256, update_plan_.bundle_sha256)) {
      return SoundStoreResult::TransactionMismatch;
    }
    const auto refresh_result = refresh_selection_after_commit();
    if (refresh_result != SoundStoreResult::Ok) {
      return refresh_result;
    }
    return clear_update_state();
  }
  if (committed_result == SoundStoreResult::Unavailable ||
      committed_result == SoundStoreResult::IoError) {
    return committed_result;
  }

  const auto erase_result = storage_.erase(
      update_bank_, kSoundStagingHeaderOffset, kSoundSectorSize);
  if (erase_result != SoundStorageIoResult::Ok) {
    return map_io(erase_result);
  }
  return clear_update_state();
}

SoundStoreResult SoundAssetStore::acquire_active_read(
    SoundReadLease* lease) {
  if (lease == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *lease = {};
  if (!read_selection_scanned_) {
    const auto result = scan_committed_content();
    if (result != SoundStoreResult::Ok) {
      return result;
    }
  }
  if (read_selection_result_ != SoundStoreResult::Ok) {
    return read_selection_result_;
  }
  if (!read_selection_.active_valid) {
    return read_selection_.split_brain
               ? SoundStoreResult::SplitBrain
               : SoundStoreResult::Unavailable;
  }
  auto free_slot = read_leases_.end();
  for (auto iterator = read_leases_.begin();
       iterator != read_leases_.end();
       ++iterator) {
    if (!iterator->valid) {
      free_slot = iterator;
      break;
    }
  }
  if (free_slot == read_leases_.end()) {
    return SoundStoreResult::Busy;
  }

  std::uint64_t candidate = next_read_lease_id_;
  for (std::size_t attempt = 0;
       attempt <= read_leases_.size();
       ++attempt) {
    if (candidate == 0U) {
      candidate = 1U;
    }
    const bool already_used = std::any_of(
        read_leases_.begin(),
        read_leases_.end(),
        [candidate](const SoundReadLease& existing) {
          return existing.valid && existing.lease_id == candidate;
        });
    if (!already_used) {
      break;
    }
    ++candidate;
  }
  if (candidate == 0U) {
    return SoundStoreResult::Busy;
  }

  SoundReadLease acquired{};
  acquired.valid = true;
  acquired.lease_id = candidate;
  acquired.bank = read_selection_.active.bank;
  acquired.generation = read_selection_.active.generation;
  acquired.bundle_sha256 =
      read_selection_.active.bundle_sha256;
  *free_slot = acquired;
  *lease = acquired;
  next_read_lease_id_ =
      candidate == std::numeric_limits<std::uint64_t>::max()
          ? 1U
          : candidate + 1U;
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::release_read(
    const SoundReadLease& lease) {
  if (!lease.valid) {
    return SoundStoreResult::InvalidArgument;
  }
  for (auto& existing : read_leases_) {
    if (existing.valid &&
        existing.lease_id == lease.lease_id &&
        existing.bank == lease.bank &&
        existing.generation == lease.generation &&
        sound_digest_equal(
            existing.bundle_sha256, lease.bundle_sha256)) {
      existing = {};
      return SoundStoreResult::Ok;
    }
  }
  return SoundStoreResult::TransactionMismatch;
}

bool SoundAssetStore::update_active() const {
  return update_active_;
}

SoundBankId SoundAssetStore::update_bank() const {
  return update_bank_;
}

std::uint64_t SoundAssetStore::update_generation() const {
  return update_generation_;
}

SoundStoreResult SoundAssetStore::update_progress(
    SoundUpdateProgress* progress) const {
  if (progress == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *progress = {};
  if (!update_active_) {
    return SoundStoreResult::TransactionMismatch;
  }

  progress->identity.generation = update_generation_;
  progress->identity.target_bank = update_bank_;
  progress->identity.transaction_id = update_transaction_id_;
  progress->manifest_bytes = update_plan_.manifest_bytes;
  progress->payload_bytes = update_plan_.payload_bytes;
  progress->payload_block_count = static_cast<std::uint16_t>(
      payload_block_count(update_plan_.payload_bytes));
  progress->manifest_complete = manifest_complete_;
  progress->bundle_sha256 = update_plan_.bundle_sha256;
  for (std::size_t index = 0U;
       index < progress->payload_block_count;
       ++index) {
    if (payload_complete_[index]) {
      progress->payload_complete_bitmap[index / 8U] =
          static_cast<std::uint8_t>(
              progress->payload_complete_bitmap[index / 8U] |
              (1U << (index % 8U)));
    }
  }
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::load_staging_header(
    SoundBankId bank,
    const std::array<std::uint8_t, kSoundTransactionIdBytes>& transaction_id) {
  std::array<std::uint8_t, kStagingHeaderBytes> encoded{};
  if (!read_exact(storage_,
                  bank,
                  kSoundStagingHeaderOffset,
                  encoded.data(),
                  encoded.size())) {
    return SoundStoreResult::IoError;
  }
  SoundBundlePlan plan{};
  std::uint64_t generation = 0U;
  std::array<std::uint8_t, kSoundTransactionIdBytes>
      decoded_transaction_id{};
  const auto decode_result = decode_staging_header(
      storage_,
      encoded,
      bank,
      &generation,
      &decoded_transaction_id,
      &plan);
  if (decode_result != SoundStoreResult::Ok) {
    if (decode_result != SoundStoreResult::InvalidStaging) {
      return decode_result;
    }
    return SoundStoreResult::TransactionMismatch;
  }
  std::array<std::uint8_t, kSoundTransactionIdBytes>
      expected_transaction_id{};
  const auto transaction_result = derive_transaction_id(
      storage_,
      plan,
      bank,
      generation,
      &expected_transaction_id);
  if (transaction_result != SoundStoreResult::Ok) {
    return transaction_result;
  }
  if (decoded_transaction_id != transaction_id ||
      expected_transaction_id != decoded_transaction_id) {
    return SoundStoreResult::TransactionMismatch;
  }
  if (selection_.active_valid) {
    if (plan.base_generation != selection_.active.generation ||
        !sound_digest_equal(
            plan.base_bundle_sha256,
            selection_.active.bundle_sha256) ||
        generation != selection_.active.generation + 1U) {
      return SoundStoreResult::StaleBase;
    }
  } else if (plan.base_generation != 0U ||
             !digest_is_zero(plan.base_bundle_sha256) ||
             generation != 1U) {
    return SoundStoreResult::StaleBase;
  }

  update_active_ = true;
  update_bank_ = bank;
  update_generation_ = generation;
  update_plan_ = plan;
  update_transaction_id_ = decoded_transaction_id;
  payload_complete_.fill(false);
  manifest_complete_ = false;

  const auto manifest_result = verify_manifest();
  if (manifest_result == SoundStoreResult::Ok) {
    manifest_complete_ = true;
  } else if (manifest_result == SoundStoreResult::IoError ||
             manifest_result == SoundStoreResult::Unavailable ||
             manifest_result == SoundStoreResult::InvalidArgument) {
    clear_update_state();
    return manifest_result;
  }
  const auto count = payload_block_count(update_plan_.payload_bytes);
  for (std::size_t index = 0; index < count; ++index) {
    bool complete = false;
    const auto verify = verify_payload_block(index, &complete);
    if (verify == SoundStoreResult::IoError) {
      clear_update_state();
      return verify;
    }
    payload_complete_[index] = complete;
  }
  // The journal is advisory only. Resume reconstructs authoritative progress
  // from manifest SHA/CRC and payload block CRC without issuing Flash writes;
  // a later reboot performs the same verification again.
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::verify_manifest() {
  std::uint32_t crc = 0U;
  SoundSha256Digest digest{};
  if (!region_crc32(storage_,
                    update_bank_,
                    kSoundManifestOffset,
                    update_plan_.manifest_bytes,
                    &crc) ||
      !region_sha256(storage_,
                     update_bank_,
                     kSoundManifestOffset,
                     update_plan_.manifest_bytes,
                     &digest)) {
    return SoundStoreResult::IoError;
  }
  if (crc != update_plan_.manifest_crc32) {
    return SoundStoreResult::CrcMismatch;
  }
  return sound_digest_equal(
             digest, update_plan_.manifest_sha256)
             ? SoundStoreResult::Ok
             : SoundStoreResult::HashMismatch;
}

SoundStoreResult SoundAssetStore::verify_payload_block(
    std::size_t block_index,
    bool* complete) {
  if (complete == nullptr) {
    return SoundStoreResult::InvalidArgument;
  }
  *complete = false;
  const auto length =
      payload_block_length(update_plan_.payload_bytes, block_index);
  if (length == 0U) {
    return SoundStoreResult::InvalidArgument;
  }
  const auto offset = static_cast<std::uint32_t>(
      kSoundPayloadOffset + block_index * kSoundPayloadBlockSize);
  std::uint32_t crc = 0U;
  if (!region_crc32(
          storage_, update_bank_, offset, length, &crc)) {
    return SoundStoreResult::IoError;
  }
  *complete =
      crc == update_plan_.payload_block_crc32[block_index];
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::verify_all_payload_blocks() {
  const auto count = payload_block_count(update_plan_.payload_bytes);
  for (std::size_t index = 0; index < count; ++index) {
    bool complete = false;
    const auto result = verify_payload_block(index, &complete);
    if (result != SoundStoreResult::Ok) {
      return result;
    }
    payload_complete_[index] = complete;
    if (!complete) {
      return SoundStoreResult::Incomplete;
    }
  }
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::mark_manifest_complete() {
  const std::uint8_t complete = 0U;
  return map_io(storage_.write(
      update_bank_,
      kSoundJournalOffset + kJournalManifestByte,
      &complete,
      sizeof(complete)));
}

SoundStoreResult SoundAssetStore::mark_payload_complete(
    std::size_t block_index) {
  if (block_index >= kSoundPayloadBlockCount) {
    return SoundStoreResult::InvalidArgument;
  }
  const auto journal_offset = static_cast<std::uint32_t>(
      kSoundJournalOffset + kJournalPayloadBitmap +
      block_index / 8U);
  std::uint8_t current = 0xFFU;
  auto result = storage_.read(
      update_bank_, journal_offset, &current, sizeof(current));
  if (result != SoundStorageIoResult::Ok) {
    return map_io(result);
  }
  const auto updated = static_cast<std::uint8_t>(
      current & ~(1U << (block_index % 8U)));
  result = storage_.write(
      update_bank_, journal_offset, &updated, sizeof(updated));
  return map_io(result);
}

SoundStoreResult SoundAssetStore::clear_update_state() {
  update_active_ = false;
  update_bank_ = SoundBankId::A;
  update_generation_ = 0U;
  update_plan_ = {};
  update_transaction_id_ = {};
  payload_complete_.fill(false);
  manifest_complete_ = false;
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::refresh_selection_after_commit() {
  SoundBankSnapshot bank_a{};
  SoundBankSnapshot bank_b{};
  bank_a.bank = SoundBankId::A;
  bank_b.bank = SoundBankId::B;
  const auto bank_a_result =
      validate_sound_bank(storage_, SoundBankId::A, &bank_a);
  if (bank_a_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_a_result)) {
    return bank_a_result;
  }
  if (bank_a_result != SoundStoreResult::Ok) {
    bank_a.valid = false;
  }
  const auto bank_b_result =
      validate_sound_bank(storage_, SoundBankId::B, &bank_b);
  if (bank_b_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_b_result)) {
    return bank_b_result;
  }
  if (bank_b_result != SoundStoreResult::Ok) {
    bank_b.valid = false;
  }
  const auto refreshed = select_sound_banks(bank_a, bank_b);
  if (!refreshed.active_valid || refreshed.split_brain ||
      refreshed.active.bank != update_bank_ ||
      refreshed.active.generation != update_generation_) {
    return refreshed.split_brain
               ? SoundStoreResult::SplitBrain
               : SoundStoreResult::InvalidBank;
  }
  selection_ = refreshed;
  scanned_ = true;
  read_selection_ = refreshed;
  read_selection_scanned_ = true;
  read_selection_result_ = SoundStoreResult::Ok;
  return SoundStoreResult::Ok;
}

SoundStoreResult SoundAssetStore::scan_committed_content() {
  if (update_active_) {
    return SoundStoreResult::Busy;
  }

  // This selection can authorize immutable reads only. It neither reads nor
  // modifies `selection_`/`scanned_`, so no update/erase/query path can
  // consume it as durable truth (and a fast-read failure cannot destroy an
  // independently established strict snapshot).
  read_selection_scanned_ = false;
  read_selection_result_ = SoundStoreResult::Unavailable;
  read_selection_ = {};
  SoundBankSnapshot bank_a{};
  SoundBankSnapshot bank_b{};
  bank_a.bank = SoundBankId::A;
  bank_b.bank = SoundBankId::B;
  const auto bank_a_result =
      decode_commit(storage_, SoundBankId::A, &bank_a);
  if (bank_a_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_a_result)) {
    return bank_a_result;
  }
  if (bank_a_result != SoundStoreResult::Ok) {
    bank_a.valid = false;
  }
  const auto bank_b_result =
      decode_commit(storage_, SoundBankId::B, &bank_b);
  if (bank_b_result != SoundStoreResult::Ok &&
      !bank_validation_failure_is_nonfatal(bank_b_result)) {
    return bank_b_result;
  }
  if (bank_b_result != SoundStoreResult::Ok) {
    bank_b.valid = false;
  }
  if (bank_a_result == SoundStoreResult::FactoryBlank &&
      bank_b_result == SoundStoreResult::FactoryBlank) {
    read_selection_scanned_ = true;
    read_selection_result_ = SoundStoreResult::FactoryBlank;
    return SoundStoreResult::FactoryBlank;
  }

  auto candidates = select_sound_banks(bank_a, bank_b);
  auto validate_candidate =
      [this](SoundBankSnapshot* candidate) {
        if (candidate == nullptr || !candidate->valid) {
          return SoundStoreResult::InvalidBank;
        }
        const auto result = validate_sound_bank_read_candidate(
            storage_, *candidate);
        if (result != SoundStoreResult::Ok &&
            bank_validation_failure_is_nonfatal(result)) {
          candidate->valid = false;
        }
        return result;
      };

  if (candidates.split_brain) {
    // A damaged manifest can make two otherwise plausible commit records look
    // split. Authenticate both only on this exceptional path, then arbitrate
    // again; backend uncertainty remains fatal and can never cause fallback.
    const auto a_result = validate_candidate(&bank_a);
    if (a_result != SoundStoreResult::Ok &&
        !bank_validation_failure_is_nonfatal(a_result)) {
      return a_result;
    }
    const auto b_result = validate_candidate(&bank_b);
    if (b_result != SoundStoreResult::Ok &&
        !bank_validation_failure_is_nonfatal(b_result)) {
      return b_result;
    }
    candidates = select_sound_banks(bank_a, bank_b);
  } else if (candidates.active_valid) {
    auto* active =
        candidates.active.bank == SoundBankId::A
            ? &bank_a
            : &bank_b;
    const auto active_result = validate_candidate(active);
    if (active_result != SoundStoreResult::Ok &&
        !bank_validation_failure_is_nonfatal(active_result)) {
      return active_result;
    }
    if (!active->valid) {
      // Corruption in the preferred manifest safely falls back to the other
      // commit only after that manifest also authenticates.
      auto* fallback =
          active == &bank_a ? &bank_b : &bank_a;
      if (fallback->valid) {
        const auto fallback_result = validate_candidate(fallback);
        if (fallback_result != SoundStoreResult::Ok &&
            !bank_validation_failure_is_nonfatal(fallback_result)) {
          return fallback_result;
        }
      }
      candidates = select_sound_banks(bank_a, bank_b);
    } else {
      // Do not publish an unauthenticated historical bank as valid read
      // history. Strict scans remain the only source of complete A/B truth.
      auto* historical =
          active == &bank_a ? &bank_b : &bank_a;
      historical->valid = false;
      candidates = select_sound_banks(bank_a, bank_b);
    }
  }

  read_selection_ = candidates;
  read_selection_scanned_ = true;
  read_selection_result_ = read_selection_.split_brain
                               ? SoundStoreResult::SplitBrain
                               : read_selection_.active_valid
                                     ? SoundStoreResult::Ok
                                     : SoundStoreResult::Unavailable;
  return read_selection_result_;
}

bool SoundAssetStore::target_is_pinned(SoundBankId bank) const {
  return std::any_of(
      read_leases_.begin(),
      read_leases_.end(),
      [bank](const SoundReadLease& lease) {
        return lease.valid && lease.bank == bank;
      });
}

}  // namespace easy_input::speaker_assets
