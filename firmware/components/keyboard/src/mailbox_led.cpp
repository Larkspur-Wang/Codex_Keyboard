#include "keyboard/mailbox_led.h"

#include <algorithm>

namespace easy_codex {

ai_keyboard::FeedbackColor mailbox_color_for_coverage(
    std::uint32_t coverage_count) {
  if (coverage_count == 0U) {
    return {};
  }
  const auto bounded = std::min<std::uint32_t>(coverage_count, 16U);
  const auto green = static_cast<std::uint8_t>(2U + bounded * 3U);
  return {0U, green, static_cast<std::uint8_t>(1U + bounded / 4U)};
}

std::array<ai_keyboard::FeedbackColor, 5> mailbox_frame_for_slots(
    const std::array<std::uint8_t, 4>& coverage_by_slot) {
  std::array<ai_keyboard::FeedbackColor, 5> frame{};
  for (std::size_t index = 0U; index < coverage_by_slot.size(); ++index) {
    frame[4U - index] = mailbox_color_for_coverage(coverage_by_slot[index]);
  }
  return frame;
}

}  // namespace easy_codex
