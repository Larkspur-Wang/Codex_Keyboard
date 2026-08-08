#pragma once

#include <cstdint>

#include "speaker_assets/sound_asset_crypto.h"
#include "speaker_assets/sound_asset_store.h"

namespace easy_input::speaker_assets {

enum class SoundFormatResult : std::uint8_t {
  Ok,
  InvalidArgument,
  IoError,
  InvalidManifest,
  InvalidResource,
  InvalidMapping,
  HashMismatch,
};

struct SoundManifestSummary {
  std::uint32_t manifest_bytes = 0;
  std::uint32_t payload_bytes = 0;
  std::uint16_t resource_count = 0;
  std::uint16_t mapping_count = 0;
};

[[nodiscard]] SoundFormatResult validate_sound_manifest(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t manifest_bytes,
    std::uint32_t payload_bytes,
    SoundManifestSummary* summary);

[[nodiscard]] SoundFormatResult calculate_sound_bank_digests(
    SoundBankStorage& storage,
    SoundBankId bank,
    std::uint32_t manifest_bytes,
    std::uint32_t payload_bytes,
    SoundSha256Digest* manifest_digest,
    SoundSha256Digest* bundle_digest);

}  // namespace easy_input::speaker_assets
