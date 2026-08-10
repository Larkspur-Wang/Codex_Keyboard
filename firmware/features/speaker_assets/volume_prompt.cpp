#include "speaker_assets/volume_prompt.h"

namespace easy_input::speaker_assets {
namespace {

#define DECLARE_VOLUME_PROMPT(percent)                                      \
  extern const std::uint8_t kVolume##percent##Start[]                      \
      asm("_binary_volume_" #percent "_eiad_start");                      \
  extern const std::uint8_t kVolume##percent##End[]                        \
      asm("_binary_volume_" #percent "_eiad_end")

DECLARE_VOLUME_PROMPT(10);
DECLARE_VOLUME_PROMPT(20);
DECLARE_VOLUME_PROMPT(30);
DECLARE_VOLUME_PROMPT(40);
DECLARE_VOLUME_PROMPT(50);
DECLARE_VOLUME_PROMPT(60);
DECLARE_VOLUME_PROMPT(70);
DECLARE_VOLUME_PROMPT(80);
DECLARE_VOLUME_PROMPT(90);
DECLARE_VOLUME_PROMPT(100);

#undef DECLARE_VOLUME_PROMPT

EmbeddedVolumePrompt prompt(const std::uint8_t* begin,
                            const std::uint8_t* end) {
  return {begin, static_cast<std::size_t>(end - begin)};
}

}  // namespace

EmbeddedVolumePrompt volume_prompt(std::uint8_t level) {
  switch (level) {
    case 1: return prompt(kVolume10Start, kVolume10End);
    case 2: return prompt(kVolume20Start, kVolume20End);
    case 3: return prompt(kVolume30Start, kVolume30End);
    case 4: return prompt(kVolume40Start, kVolume40End);
    case 5: return prompt(kVolume50Start, kVolume50End);
    case 6: return prompt(kVolume60Start, kVolume60End);
    case 7: return prompt(kVolume70Start, kVolume70End);
    case 8: return prompt(kVolume80Start, kVolume80End);
    case 9: return prompt(kVolume90Start, kVolume90End);
    case 10: return prompt(kVolume100Start, kVolume100End);
    default: return {};
  }
}

}  // namespace easy_input::speaker_assets
