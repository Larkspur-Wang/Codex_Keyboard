#include "keyboard/input_test.h"

#include <array>

namespace ai_keyboard {
namespace {

struct InputTestItem {
  InputId input;
  const char* name;
};

constexpr std::array<InputTestItem, 11> kInputTestItems{{
    {InputId::Key1, "KEY1"},
    {InputId::Key2, "KEY2"},
    {InputId::Key3, "KEY3"},
    {InputId::Key4, "KEY4"},
    {InputId::Key5, "KEY5"},
    {InputId::Key6, "KEY6"},
    {InputId::Key7, "KEY7"},
    {InputId::Key8, "KEY8"},
    {InputId::EncoderLeft, "ENC_LEFT"},
    {InputId::EncoderRight, "ENC_RIGHT"},
    {InputId::EncoderPress, "ENC_PRESS"},
}};

std::uint16_t input_bit(InputId input) {
  for (std::size_t index = 0; index < kInputTestItems.size(); ++index) {
    if (kInputTestItems[index].input == input) {
      return static_cast<std::uint16_t>(1U << index);
    }
  }
  return 0;
}

std::string join_inputs(std::uint16_t mask, bool missing) {
  std::string result;
  for (std::size_t index = 0; index < kInputTestItems.size(); ++index) {
    const bool observed = (mask & (1U << index)) != 0;
    if (observed == missing) {
      continue;
    }
    if (!result.empty()) {
      result += ",";
    }
    result += kInputTestItems[index].name;
  }
  return result.empty() ? "none" : result;
}

std::uint8_t count_observed(std::uint16_t mask) {
  std::uint8_t count = 0;
  for (std::size_t index = 0; index < kInputTestItems.size(); ++index) {
    if ((mask & (1U << index)) != 0) {
      ++count;
    }
  }
  return count;
}

}  // namespace

void PhysicalInputTest::start() {
  active_ = true;
  observed_mask_ = 0;
}

void PhysicalInputTest::stop() {
  active_ = false;
}

bool PhysicalInputTest::active() const {
  return active_;
}

bool PhysicalInputTest::observe(InputId input) {
  if (!active_) {
    return false;
  }
  const auto bit = input_bit(input);
  if (bit == 0 || (observed_mask_ & bit) != 0) {
    return false;
  }
  observed_mask_ |= bit;
  return true;
}

InputTestProgress PhysicalInputTest::progress() const {
  InputTestProgress progress;
  progress.active = active_;
  progress.observed_mask = observed_mask_;
  progress.observed_count = count_observed(observed_mask_);
  progress.expected_count = static_cast<std::uint8_t>(kInputTestItems.size());
  progress.complete = progress.observed_count == progress.expected_count;
  progress.observed_csv = join_inputs(observed_mask_, false);
  progress.missing_csv = join_inputs(observed_mask_, true);
  return progress;
}

const char* physical_input_test_name(InputId input) {
  for (const auto& item : kInputTestItems) {
    if (item.input == input) {
      return item.name;
    }
  }
  return "UNKNOWN";
}

}  // namespace ai_keyboard
