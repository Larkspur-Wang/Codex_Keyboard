#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace easy_input::speaker_assets {

using SoundSha256Digest = std::array<std::uint8_t, 32>;

class SoundSha256 {
 public:
  SoundSha256();

  void reset();
  bool update(const std::uint8_t* data, std::size_t length);

  // Finalization is idempotent. Call reset() before adding more input.
  SoundSha256Digest finish();

 private:
  void transform(const std::uint8_t* block);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  SoundSha256Digest digest_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffered_bytes_ = 0;
  bool finished_ = false;
};

std::uint32_t sound_crc32_iso_hdlc(const std::uint8_t* data,
                                   std::size_t length);

bool sound_digest_equal(const SoundSha256Digest& first,
                        const SoundSha256Digest& second);

}  // namespace easy_input::speaker_assets
