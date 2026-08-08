#include "speaker_assets/factory_boot_sound.h"

#include <cstddef>
#include <cstdint>

namespace easy_input::speaker_assets {
namespace {

extern const std::uint8_t kFactoryBootSoundStart[]
    asm("_binary_waytoagi_eiad_start");
extern const std::uint8_t kFactoryBootSoundEnd[]
    asm("_binary_waytoagi_eiad_end");

}  // namespace

FactoryBootSound factory_boot_sound() {
  return {
      kFactoryBootSoundStart,
      static_cast<std::size_t>(
          kFactoryBootSoundEnd - kFactoryBootSoundStart),
  };
}

}  // namespace easy_input::speaker_assets
