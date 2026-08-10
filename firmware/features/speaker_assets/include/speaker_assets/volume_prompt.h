#pragma once

#include <cstddef>
#include <cstdint>

namespace easy_input::speaker_assets {

struct EmbeddedVolumePrompt {
  const std::uint8_t* encoded = nullptr;
  std::size_t encoded_bytes = 0U;
};

[[nodiscard]] EmbeddedVolumePrompt volume_prompt(std::uint8_t level);

}  // namespace easy_input::speaker_assets
