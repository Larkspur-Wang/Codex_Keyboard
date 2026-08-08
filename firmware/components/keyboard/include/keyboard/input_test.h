#pragma once

#include <cstdint>
#include <string>

#include "keyboard/keymap.h"

namespace ai_keyboard {

struct InputTestProgress {
  bool active = false;
  std::uint16_t observed_mask = 0;
  std::uint8_t observed_count = 0;
  std::uint8_t expected_count = 0;
  bool complete = false;
  std::string observed_csv;
  std::string missing_csv;
};

class PhysicalInputTest {
 public:
  void start();
  void stop();
  bool active() const;
  bool observe(InputId input);
  InputTestProgress progress() const;

 private:
  bool active_ = false;
  std::uint16_t observed_mask_ = 0;
};

const char* physical_input_test_name(InputId input);

}  // namespace ai_keyboard
