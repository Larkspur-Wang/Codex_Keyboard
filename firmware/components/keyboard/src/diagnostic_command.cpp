#include "keyboard/diagnostic_command.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ai_keyboard {
namespace {

std::string uppercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

bool parse_input_token(const std::string& token, InputId* input) {
  const auto value = uppercase(token);
  if (value == "KEY1") {
    *input = InputId::Key1;
    return true;
  }
  if (value == "KEY2") {
    *input = InputId::Key2;
    return true;
  }
  if (value == "KEY3") {
    *input = InputId::Key3;
    return true;
  }
  if (value == "KEY4") {
    *input = InputId::Key4;
    return true;
  }
  if (value == "KEY5") {
    *input = InputId::Key5;
    return true;
  }
  if (value == "KEY6") {
    *input = InputId::Key6;
    return true;
  }
  if (value == "KEY7") {
    *input = InputId::Key7;
    return true;
  }
  if (value == "KEY8") {
    *input = InputId::Key8;
    return true;
  }
  if (value == "KEY9" || value == "ENC_PRESS" || value == "ENCODER_PRESS") {
    *input = InputId::EncoderPress;
    return true;
  }
  if (value == "ENC_LEFT" || value == "ENCODER_LEFT") {
    *input = InputId::EncoderLeft;
    return true;
  }
  if (value == "ENC_RIGHT" || value == "ENCODER_RIGHT") {
    *input = InputId::EncoderRight;
    return true;
  }
  return false;
}

bool parse_kind_token(const std::string& token, DiagnosticCommandKind* kind) {
  const auto value = uppercase(token);
  if (value == "PRESS" || value == "DOWN") {
    *kind = DiagnosticCommandKind::InputPressed;
    return true;
  }
  if (value == "RELEASE" || value == "UP") {
    *kind = DiagnosticCommandKind::InputReleased;
    return true;
  }
  if (value == "TAP") {
    *kind = DiagnosticCommandKind::InputTapped;
    return true;
  }
  return false;
}

bool parse_input_test_token(const std::string& token, DiagnosticCommandKind* kind) {
  const auto value = uppercase(token);
  if (value == "START" || value == "RESET") {
    *kind = DiagnosticCommandKind::InputTestStart;
    return true;
  }
  if (value == "STATUS") {
    *kind = DiagnosticCommandKind::InputTestStatus;
    return true;
  }
  if (value == "STOP") {
    *kind = DiagnosticCommandKind::InputTestStop;
    return true;
  }
  return false;
}

}  // namespace

DiagnosticCommand parse_diagnostic_command(const std::string& line) {
  std::istringstream stream(line);
  std::string prefix;
  std::string input_token;
  std::string kind_token;
  std::string extra;
  if (!(stream >> prefix >> input_token)) {
    return {};
  }
  if (uppercase(prefix) != "DIAG") {
    return {};
  }
  if (uppercase(input_token) == "STATUS") {
    if (stream >> extra) {
      return {};
    }
    return {true, DiagnosticCommandKind::Status, InputId::Count};
  }
  if (uppercase(input_token) == "INPUTS" || uppercase(input_token) == "PINS") {
    if (stream >> extra) {
      return {};
    }
    return {true, DiagnosticCommandKind::Inputs, InputId::Count};
  }
  const auto upper_input_token = uppercase(input_token);
  if (upper_input_token == "INPUT-TEST" || upper_input_token == "INPUT_TEST" ||
      upper_input_token == "PHYSICAL-TEST" || upper_input_token == "PHYSICAL_TEST") {
    if (!(stream >> kind_token)) {
      return {};
    }
    if (stream >> extra) {
      return {};
    }
    DiagnosticCommandKind kind = DiagnosticCommandKind::None;
    if (!parse_input_test_token(kind_token, &kind)) {
      return {};
    }
    return {true, kind, InputId::Count};
  }
  if (!(stream >> kind_token)) {
    return {};
  }
  if (stream >> extra) {
    return {};
  }

  DiagnosticCommand command;
  command.valid = parse_input_token(input_token, &command.input) &&
                  parse_kind_token(kind_token, &command.kind);
  if (!command.valid) {
    return {};
  }
  return command;
}

}  // namespace ai_keyboard
