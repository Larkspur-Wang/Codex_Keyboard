#include <cassert>

#include "keyboard/input_test.h"

using ai_keyboard::InputId;

void start_resets_progress_and_lists_every_input_missing() {
  ai_keyboard::PhysicalInputTest test;
  test.start();

  const auto progress = test.progress();
  assert(progress.active);
  assert(progress.observed_count == 0);
  assert(progress.expected_count == 11);
  assert(!progress.complete);
  assert(progress.missing_csv.find("KEY1") != std::string::npos);
  assert(progress.missing_csv.find("ENC_PRESS") != std::string::npos);
}

void duplicate_observations_do_not_increment_count() {
  ai_keyboard::PhysicalInputTest test;
  test.start();

  assert(test.observe(InputId::Key1));
  assert(!test.observe(InputId::Key1));

  const auto progress = test.progress();
  assert(progress.observed_count == 1);
  assert(progress.observed_csv == "KEY1");
}

void all_keys_and_encoder_inputs_complete_the_test() {
  ai_keyboard::PhysicalInputTest test;
  test.start();

  test.observe(InputId::Key1);
  test.observe(InputId::Key2);
  test.observe(InputId::Key3);
  test.observe(InputId::Key4);
  test.observe(InputId::Key5);
  test.observe(InputId::Key6);
  test.observe(InputId::Key7);
  test.observe(InputId::Key8);
  test.observe(InputId::EncoderLeft);
  test.observe(InputId::EncoderRight);
  test.observe(InputId::EncoderPress);

  const auto progress = test.progress();
  assert(progress.observed_count == progress.expected_count);
  assert(progress.complete);
  assert(progress.missing_csv == "none");
}

void inactive_test_ignores_observations() {
  ai_keyboard::PhysicalInputTest test;

  assert(!test.observe(InputId::Key1));
  assert(!test.progress().active);
  assert(test.progress().observed_count == 0);
}

int main() {
  start_resets_progress_and_lists_every_input_missing();
  duplicate_observations_do_not_increment_count();
  all_keys_and_encoder_inputs_complete_the_test();
  inactive_test_ignores_observations();
  return 0;
}
