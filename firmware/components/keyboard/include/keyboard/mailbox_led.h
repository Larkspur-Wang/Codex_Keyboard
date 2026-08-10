#pragma once

#include <array>
#include <cstdint>

#include "keyboard/input_feedback.h"

namespace easy_codex {

ai_keyboard::FeedbackColor mailbox_color_for_coverage(
    std::uint32_t coverage_count);

ai_keyboard::FeedbackColor task_activity_color(std::uint8_t running_tasks);

std::array<ai_keyboard::FeedbackColor, 5> mailbox_frame_for_slots(
    const std::array<std::uint8_t, 4>& coverage_by_slot,
    std::uint8_t running_tasks);

}  // namespace easy_codex
