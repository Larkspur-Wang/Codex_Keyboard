#include <cassert>

#include "keyboard/diagnostic_command.h"

using ai_keyboard::DiagnosticCommandKind;
using ai_keyboard::InputId;

void parses_key_press_and_release_commands() {
  const auto press = ai_keyboard::parse_diagnostic_command("diag KEY1 press");
  assert(press.valid);
  assert(press.kind == DiagnosticCommandKind::InputPressed);
  assert(press.input == InputId::Key1);

  const auto release = ai_keyboard::parse_diagnostic_command("diag KEY1 release");
  assert(release.valid);
  assert(release.kind == DiagnosticCommandKind::InputReleased);
  assert(release.input == InputId::Key1);
}

void parses_key_and_encoder_tap_commands() {
  const auto key = ai_keyboard::parse_diagnostic_command("diag KEY6 tap");
  assert(key.valid);
  assert(key.kind == DiagnosticCommandKind::InputTapped);
  assert(key.input == InputId::Key6);

  const auto encoder = ai_keyboard::parse_diagnostic_command("diag ENC_LEFT tap");
  assert(encoder.valid);
  assert(encoder.kind == DiagnosticCommandKind::InputTapped);
  assert(encoder.input == InputId::EncoderLeft);
}

void key9_aliases_encoder_press() {
  const auto command = ai_keyboard::parse_diagnostic_command("diag KEY9 press");
  assert(command.valid);
  assert(command.kind == DiagnosticCommandKind::InputPressed);
  assert(command.input == InputId::EncoderPress);
}

void parses_status_command() {
  const auto command = ai_keyboard::parse_diagnostic_command("diag status");
  assert(command.valid);
  assert(command.kind == DiagnosticCommandKind::Status);
  assert(command.input == InputId::Count);
}

void parses_inputs_command() {
  const auto command = ai_keyboard::parse_diagnostic_command("diag inputs");
  assert(command.valid);
  assert(command.kind == DiagnosticCommandKind::Inputs);
  assert(command.input == InputId::Count);
}

void parses_physical_input_test_commands() {
  const auto start = ai_keyboard::parse_diagnostic_command("diag input-test start");
  assert(start.valid);
  assert(start.kind == DiagnosticCommandKind::InputTestStart);
  assert(start.input == InputId::Count);

  const auto status = ai_keyboard::parse_diagnostic_command("diag input-test status");
  assert(status.valid);
  assert(status.kind == DiagnosticCommandKind::InputTestStatus);
  assert(status.input == InputId::Count);

  const auto stop = ai_keyboard::parse_diagnostic_command("diag input-test stop");
  assert(stop.valid);
  assert(stop.kind == DiagnosticCommandKind::InputTestStop);
  assert(stop.input == InputId::Count);
}

void rejects_unknown_or_non_diag_commands() {
  assert(!ai_keyboard::parse_diagnostic_command("KEY1 press").valid);
  assert(!ai_keyboard::parse_diagnostic_command("diag KEY10 press").valid);
  assert(!ai_keyboard::parse_diagnostic_command("diag KEY1 hold").valid);
  assert(!ai_keyboard::parse_diagnostic_command("diag inputs extra").valid);
  assert(!ai_keyboard::parse_diagnostic_command("diag input-test maybe").valid);
}

int main() {
  parses_key_press_and_release_commands();
  parses_key_and_encoder_tap_commands();
  key9_aliases_encoder_press();
  parses_status_command();
  parses_inputs_command();
  parses_physical_input_test_commands();
  rejects_unknown_or_non_diag_commands();
  return 0;
}
