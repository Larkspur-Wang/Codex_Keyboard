#pragma once

#include <string>

#include "keyboard/keymap.h"

namespace ai_keyboard {

enum class DiagnosticCommandKind {
  None,
  Status,
  Inputs,
  InputTestStart,
  InputTestStatus,
  InputTestStop,
  InputPressed,
  InputReleased,
  InputTapped,
};

struct DiagnosticCommand {
  bool valid = false;
  DiagnosticCommandKind kind = DiagnosticCommandKind::None;
  InputId input = InputId::Count;
};

DiagnosticCommand parse_diagnostic_command(const std::string& line);

}  // namespace ai_keyboard
