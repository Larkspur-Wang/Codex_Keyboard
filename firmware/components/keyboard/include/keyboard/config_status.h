#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "keyboard/speaker_probe_status.h"

namespace ai_keyboard {

inline constexpr std::size_t kConfigStatusGattSafeLen = 512;
inline constexpr std::size_t kConfigStatusBatteryBleReserveLen = 120;

struct PowerDiagnosticsSnapshot {
  std::string mode;
  std::uint32_t poll_ms = 0;
  std::uint32_t idle_ms = 0;
  std::uint32_t deep_entries = 0;
  std::uint32_t deep_ms = 0;
  std::uint32_t last_enter_ms = 0;
  std::uint32_t last_exit_ms = 0;
  std::string last_wake;
  std::uint32_t wake_edges = 0;
  bool usb = false;
  std::uint32_t cycle_seq = 0;
  std::uint32_t cycle_idle_ms = 0;
  std::uint32_t cycle_deep_ms = 0;
  std::uint32_t cycle_flags = 0;
  std::string cycle_wake;
};

struct BoardDiagnosticsSnapshot {
  std::string board;
  std::string keys;
  std::string encoder;
  std::string pinmap;
  std::string raw_gpio;
  std::string last_input;
  std::uint32_t last_input_age_ms = 0;
  int key_wake = -1;
  int external_power = -1;
  int charge = -1;
  int pwr_gpio = -1;
  int pwr_level = -1;
  int pwr_active = -1;
  int led_gpio = -1;
  std::uint32_t input_edges = 0;
  std::uint32_t input_drops = 0;
  std::uint32_t input_events = 0;
  std::uint32_t input_filtered = 0;
  std::uint32_t encoder_edges = 0;
  std::uint32_t encoder_steps = 0;
  std::uint32_t encoder_invalid = 0;
  std::uint32_t encoder_partial = 0;
  std::uint32_t encoder_drops = 0;
};

struct AudioStatusSnapshot {
  bool enabled = false;
  std::string source;
  std::string microphone_source;
  std::string transport;
  std::string capture;
  std::string host;
  int port = 0;
  std::string mic_channel;
  std::string speaker_channel;
  std::uint32_t sent_packets = 0;
  std::uint32_t sent_bytes = 0;
  std::uint32_t last_rms_milli = 0;
  std::uint32_t peak_rms_milli = 0;
  std::uint32_t send_errors = 0;
  std::uint32_t read_errors = 0;
  std::uint32_t recovery_count = 0;
  std::uint32_t session_generation = 0;
  std::uint64_t session_id = 0;
  std::string stream_phase;
  std::string stop_reason;
  std::string control_state;
  std::string last_error;
  std::string stream_host;
  int stream_port = 0;
};

struct BatteryStatusSnapshot {
  std::uint16_t raw_mv = 0;
  std::string state;
  std::uint32_t sample_age_ms = 0;
  bool full_anchor_ready = false;
};

struct ConfigStatusSnapshot {
  std::string firmware;
  std::string phase;
  std::string status;
  std::size_t bytes = 0;
  std::uint16_t crc16 = 0;
  std::string ptt_hotkey;
  std::string edit_ptt_hotkey;
  std::string hotkey_mode;
  bool saved = false;
  std::uint16_t battery_mv = 0;
  std::uint8_t battery_percent = 0;
  AudioStatusSnapshot audio;
  PowerDiagnosticsSnapshot power;
  BoardDiagnosticsSnapshot diagnostics;
  BatteryStatusSnapshot battery;
  std::string target_platform = "macos";
  bool semantic_actions = true;
  bool offline_platform_switch = true;
  bool usb_management_v1 = true;
  // Synchronously consumed optional diagnostic view. The normal production
  // status stack frame stays pointer-sized and does not carry the probe POD.
  const SpeakerProbeSnapshot* speaker = nullptr;
};

std::string build_config_status_json(const ConfigStatusSnapshot& snapshot);
std::string build_config_confirmation_status_json(const ConfigStatusSnapshot& snapshot);

}  // namespace ai_keyboard
