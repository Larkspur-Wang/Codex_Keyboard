#pragma once

#include <cstddef>
#include <cstdint>

namespace easy_input::speaker_assets {

struct FactoryBootSound {
  const std::uint8_t* encoded = nullptr;
  std::size_t encoded_bytes = 0U;
};

// Returns the immutable WaytoAGI EIAD v1 fallback embedded in the app image.
// It is never written into sound_a/sound_b and is used only when the Store
// proves that no sound preference has ever committed.
[[nodiscard]] FactoryBootSound factory_boot_sound();

}  // namespace easy_input::speaker_assets
