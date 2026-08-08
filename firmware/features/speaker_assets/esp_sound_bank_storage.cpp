#include "speaker_assets/esp_sound_bank_storage.h"

#include <cstring>

#include "esp_err.h"

namespace easy_input::speaker_assets {
namespace {

SoundStorageIoResult map_esp_result(esp_err_t result) {
  switch (result) {
    case ESP_OK:
      return SoundStorageIoResult::Ok;
    case ESP_ERR_INVALID_ARG:
      return SoundStorageIoResult::InvalidArgument;
    case ESP_ERR_INVALID_SIZE:
      return SoundStorageIoResult::OutOfBounds;
    case ESP_ERR_NOT_ALLOWED:
      return SoundStorageIoResult::Unavailable;
    default:
      return SoundStorageIoResult::IoError;
  }
}

bool bank_id_is_valid(SoundBankId bank) {
  return bank == SoundBankId::A || bank == SoundBankId::B;
}

}  // namespace

SoundStorageIoResult EspSoundBankStorage::open() {
  bank_a_ = nullptr;
  bank_b_ = nullptr;

  const auto* bank_a = esp_partition_find_first(
      kPartitionType, kBankASubtype, "sound_a");
  const auto* bank_b = esp_partition_find_first(
      kPartitionType, kBankBSubtype, "sound_b");

  if (!descriptor_matches(
          bank_a, "sound_a", kBankASubtype, kBankAAddress) ||
      !descriptor_matches(
          bank_b, "sound_b", kBankBSubtype, kBankBAddress)) {
    return SoundStorageIoResult::Unavailable;
  }

  bank_a_ = bank_a;
  bank_b_ = bank_b;
  return SoundStorageIoResult::Ok;
}

bool EspSoundBankStorage::is_open() const {
  return bank_a_ != nullptr && bank_b_ != nullptr;
}

SoundStorageIoResult EspSoundBankStorage::read(SoundBankId bank,
                                               std::uint32_t offset,
                                               std::uint8_t* output,
                                               std::size_t length) {
  if (!bank_id_is_valid(bank)) {
    return SoundStorageIoResult::InvalidArgument;
  }

  const auto* partition = partition_for(bank);
  if (partition == nullptr) {
    return SoundStorageIoResult::Unavailable;
  }
  if (output == nullptr && length != 0U) {
    return SoundStorageIoResult::InvalidArgument;
  }
  if (!range_is_valid(partition, offset, length)) {
    return SoundStorageIoResult::OutOfBounds;
  }
  if (length == 0U) {
    return SoundStorageIoResult::Ok;
  }

  return map_esp_result(
      esp_partition_read(partition, offset, output, length));
}

SoundStorageIoResult EspSoundBankStorage::write(SoundBankId bank,
                                                std::uint32_t offset,
                                                const std::uint8_t* data,
                                                std::size_t length) {
  if (!bank_id_is_valid(bank)) {
    return SoundStorageIoResult::InvalidArgument;
  }

  const auto* partition = partition_for(bank);
  if (partition == nullptr) {
    return SoundStorageIoResult::Unavailable;
  }
  if (data == nullptr && length != 0U) {
    return SoundStorageIoResult::InvalidArgument;
  }
  if (!range_is_valid(partition, offset, length)) {
    return SoundStorageIoResult::OutOfBounds;
  }
  if (length == 0U) {
    return SoundStorageIoResult::Ok;
  }

  return map_esp_result(
      esp_partition_write(partition, offset, data, length));
}

SoundStorageIoResult EspSoundBankStorage::erase(SoundBankId bank,
                                                std::uint32_t offset,
                                                std::size_t length) {
  if (!bank_id_is_valid(bank)) {
    return SoundStorageIoResult::InvalidArgument;
  }

  const auto* partition = partition_for(bank);
  if (partition == nullptr) {
    return SoundStorageIoResult::Unavailable;
  }
  if (!range_is_valid(partition, offset, length)) {
    return SoundStorageIoResult::OutOfBounds;
  }
  if ((offset % kEraseBlockSize) != 0U ||
      (length % kEraseBlockSize) != 0U) {
    return SoundStorageIoResult::NotAligned;
  }
  if (length == 0U) {
    return SoundStorageIoResult::Ok;
  }

  return map_esp_result(
      esp_partition_erase_range(partition, offset, length));
}

const esp_partition_t* EspSoundBankStorage::partition_for(
    SoundBankId bank) const {
  switch (bank) {
    case SoundBankId::A:
      return bank_a_;
    case SoundBankId::B:
      return bank_b_;
    default:
      return nullptr;
  }
}

bool EspSoundBankStorage::descriptor_matches(
    const esp_partition_t* partition,
    const char* expected_label,
    esp_partition_subtype_t expected_subtype,
    std::uint32_t expected_address) {
  return partition != nullptr &&
         partition->type == kPartitionType &&
         partition->subtype == expected_subtype &&
         partition->address == expected_address &&
         partition->size == kBankSize &&
         partition->erase_size == kEraseBlockSize &&
         !partition->encrypted &&
         !partition->readonly &&
         std::strcmp(partition->label, expected_label) == 0;
}

bool EspSoundBankStorage::range_is_valid(
    const esp_partition_t* partition,
    std::uint32_t offset,
    std::size_t length) {
  if (partition == nullptr || offset > partition->size) {
    return false;
  }
  return length <=
         static_cast<std::size_t>(partition->size - offset);
}

}  // namespace easy_input::speaker_assets
