#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace easy_input::ima_adpcm_assets {

namespace detail {

constexpr std::uint8_t base64_value(char value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<std::uint8_t>(value - 'A');
  }
  if (value >= 'a' && value <= 'z') {
    return static_cast<std::uint8_t>(value - 'a' + 26);
  }
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0' + 52);
  }
  return value == '+' ? 62U : 63U;
}

template <std::size_t OutputSize, std::size_t EncodedSize>
constexpr auto decode_base64(const char (&encoded)[EncodedSize]) {
  static_assert((EncodedSize - 1U) % 4U == 0U);
  std::array<std::uint8_t, OutputSize> output{};
  std::size_t output_index = 0;
  for (std::size_t offset = 0;
       offset + 3U < EncodedSize - 1U;
       offset += 4U) {
    const auto first = base64_value(encoded[offset]);
    const auto second = base64_value(encoded[offset + 1U]);
    output[output_index++] = static_cast<std::uint8_t>(
        (first << 2U) | (second >> 4U));
    if (encoded[offset + 2U] != '=') {
      const auto third = base64_value(encoded[offset + 2U]);
      output[output_index++] = static_cast<std::uint8_t>(
          (second << 4U) | (third >> 2U));
      if (encoded[offset + 3U] != '=') {
        const auto fourth = base64_value(encoded[offset + 3U]);
        output[output_index++] = static_cast<std::uint8_t>(
            (third << 6U) | fourth);
      }
    }
  }
  return output;
}

inline constexpr char kEasyInputBootProbeEiadBase64[] =
    "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";

}  // namespace detail

inline constexpr auto kEasyInputBootProbeEiad =
    detail::decode_base64<6872U>(detail::kEasyInputBootProbeEiadBase64);
static_assert(kEasyInputBootProbeEiad.size() == 6872U);

}  // namespace easy_input::ima_adpcm_assets
