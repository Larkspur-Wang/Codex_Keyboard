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

ai_keyboard::FeedbackColor task_activity_color(std::uint8_t running_tasks) {
  switch (std::min<std::uint8_t>(running_tasks, 4U)) {
    case 0U:
      return {0U, 24U, 5U};
    case 1U:
      return {38U, 30U, 0U};
    case 2U:
      return {46U, 16U, 0U};
    case 3U:
      return {26U, 0U, 40U};
    default:
      return {48U, 0U, 0U};
  }
}

std::array<ai_keyboard::FeedbackColor, 5> mailbox_frame_for_slots(
    const std::array<std::uint8_t, 4>& coverage_by_slot,
    std::uint8_t running_tasks) {
  std::array<ai_keyboard::FeedbackColor, 5> frame{};
  // D1/frame 0 is the physical rightmost LED; D5/frame 4 is leftmost.
  frame[0U] = task_activity_color(running_tasks);
  for (std::size_t index = 0U; index < coverage_by_slot.size(); ++index) {
    frame[4U - index] = mailbox_color_for_coverage(coverage_by_slot[index]);
  }
  return frame;
}

}  // namespace easy_codex
