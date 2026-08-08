#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/sound_asset_store.h"

namespace easy_input::speaker_assets {

inline constexpr std::uint32_t kSoundAssetSampleRate = 48000U;
inline constexpr std::uint16_t kSoundAssetFrameSamples = 480U;
inline constexpr std::size_t kSoundAssetFrameHeaderBytes = 6U;
inline constexpr std::size_t kSoundAssetMaximumFramePayloadBytes = 240U;
inline constexpr std::size_t kSoundAssetMaximumEncodedFrameBytes =
    kSoundAssetFrameHeaderBytes + kSoundAssetMaximumFramePayloadBytes;

enum class SoundAssetTrigger : std::uint8_t {
  Boot = 1U,
  Key = 2U,
  EncoderLeft = 3U,
  EncoderRight = 4U,
  EncoderPress = 5U,
};

enum class SoundAssetReadResult : std::uint8_t {
  Ok,
  End,
  InvalidArgument,
  InvalidLease,
  NotMapped,
  IoError,
  InvalidManifest,
  InvalidResource,
  OutputTooSmall,
  NotReady,
};

// Immutable address resolved from a manifest that was already validated by
// SoundAssetStore. The lease identity is copied into the result so a stream
// cannot accidentally combine an address from one committed generation with
// a different active read lease.
struct SoundResolvedAsset {
  bool valid = false;
  std::uint64_t lease_id = 0U;
  SoundBankId bank = SoundBankId::A;
  std::uint64_t generation = 0U;
  SoundSha256Digest bundle_sha256{};
  SoundSha256Digest resource_sha256{};
  std::uint16_t resource_index = 0U;
  std::uint32_t payload_offset = 0U;
  std::uint32_t encoded_bank_offset = 0U;
  std::uint32_t encoded_bytes = 0U;
  std::uint32_t decoded_samples = 0U;
  std::uint16_t frame_count = 0U;
};

// The caller must hold lease until playback ends and then release it through
// SoundAssetStore. That lease pins the committed bank, so this hot path only
// parses the already-validated manifest and never re-hashes the entire bundle.
[[nodiscard]] SoundAssetReadResult resolve_sound_asset(
    SoundBankStorage& storage,
    const SoundReadLease& lease,
    SoundAssetTrigger trigger,
    std::uint8_t trigger_index,
    SoundResolvedAsset* asset);

// Allocation-free EIAD v1 stream reader. At most one encoded 10 ms frame is
// held in RAM (246 bytes), and decoded PCM is written directly into the
// caller-owned 480-sample buffer.
class SoundAssetStreamDecoder {
 public:
  [[nodiscard]] SoundAssetReadResult open(
      SoundBankStorage& storage,
      const SoundReadLease& lease,
      const SoundResolvedAsset& asset);
  // Opens one immutable EIAD v1 resource embedded in the application image.
  // The bytes remain caller-owned and must outlive the decoder. This path
  // never creates a synthetic bank lease or copies the complete resource.
  [[nodiscard]] SoundAssetReadResult open_embedded(
      const std::uint8_t* encoded,
      std::size_t encoded_bytes);
  [[nodiscard]] SoundAssetReadResult reset();
  [[nodiscard]] SoundAssetReadResult decode_next(
      std::int16_t* output,
      std::size_t output_capacity_samples,
      std::size_t* output_samples);
  // Releases the decoder's borrowed storage/asset identity before the
  // corresponding SoundReadLease may be returned to the Store owner.
  void close();

  [[nodiscard]] bool ready() const;
  [[nodiscard]] std::uint16_t next_frame_index() const;
  [[nodiscard]] std::uint32_t decoded_samples() const;
  [[nodiscard]] const SoundResolvedAsset& asset() const;

 private:
  [[nodiscard]] SoundAssetReadResult read_encoded(
      std::uint32_t offset,
      std::uint8_t* destination,
      std::size_t length);

  SoundBankStorage* storage_ = nullptr;
  const std::uint8_t* embedded_data_ = nullptr;
  SoundResolvedAsset asset_{};
  std::uint32_t next_encoded_offset_ = 0U;
  std::uint32_t decoded_samples_ = 0U;
  std::uint16_t next_frame_index_ = 0U;
  bool ready_ = false;
  std::array<std::uint8_t, kSoundAssetMaximumEncodedFrameBytes>
      encoded_frame_{};
};

static_assert(kSoundAssetMaximumEncodedFrameBytes <= 256U);
static_assert(sizeof(SoundAssetStreamDecoder) <= 400U);

}  // namespace easy_input::speaker_assets
