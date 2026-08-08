#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"
#include "speaker_assets/sound_asset_store.h"

namespace easy_input::speaker_assets {

class EspSoundBankStorage final : public SoundBankStorage {
 public:
  EspSoundBankStorage() = default;

  [[nodiscard]] SoundStorageIoResult open();
  [[nodiscard]] bool is_open() const;

  SoundStorageIoResult read(SoundBankId bank,
                            std::uint32_t offset,
                            std::uint8_t* output,
                            std::size_t length) override;
  SoundStorageIoResult write(SoundBankId bank,
                             std::uint32_t offset,
                             const std::uint8_t* data,
                             std::size_t length) override;
  SoundStorageIoResult erase(SoundBankId bank,
                             std::uint32_t offset,
                             std::size_t length) override;

 private:
  static constexpr esp_partition_type_t kPartitionType =
      static_cast<esp_partition_type_t>(0x40);
  static constexpr esp_partition_subtype_t kBankASubtype =
      static_cast<esp_partition_subtype_t>(0x00);
  static constexpr esp_partition_subtype_t kBankBSubtype =
      static_cast<esp_partition_subtype_t>(0x01);
  static constexpr std::uint32_t kBankAAddress = 0x310000;
  static constexpr std::uint32_t kBankBAddress = 0x3A0000;
  static constexpr std::uint32_t kBankSize = kSoundBankSize;
  static constexpr std::size_t kEraseBlockSize = kSoundSectorSize;

  [[nodiscard]] const esp_partition_t* partition_for(
      SoundBankId bank) const;
  [[nodiscard]] static bool descriptor_matches(
      const esp_partition_t* partition,
      const char* expected_label,
      esp_partition_subtype_t expected_subtype,
      std::uint32_t expected_address);
  [[nodiscard]] static bool range_is_valid(const esp_partition_t* partition,
                                           std::uint32_t offset,
                                           std::size_t length);

  const esp_partition_t* bank_a_ = nullptr;
  const esp_partition_t* bank_b_ = nullptr;
};

}  // namespace easy_input::speaker_assets
