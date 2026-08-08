#include "keyboard/config_status.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "keyboard/config_receiver.h"

namespace ai_keyboard {
namespace {

constexpr std::size_t kStatusStringMaxLen = 48;
constexpr std::size_t kCompactStatusStringMaxLen = 12;
constexpr std::size_t kFirmwareStringMaxLen = 40;
constexpr std::size_t kAudioCaptureStringMaxLen = 24;

std::string escape_json_string(const std::string& value) {
  std::ostringstream out;
  for (const auto ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "\\u"
              << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
              << static_cast<unsigned>(static_cast<unsigned char>(ch))
              << std::dec << std::nouppercase;
        } else {
          out << ch;
        }
        break;
    }
  }
  return out.str();
}

std::string clipped_status_string(const std::string& value,
                                  std::size_t max_len = kStatusStringMaxLen) {
  if (value.size() <= max_len) {
    return value;
  }
  return value.substr(0, max_len);
}

std::string escape_json_string_bounded(const std::string& value,
                                       std::size_t max_encoded_len) {
  std::string escaped;
  escaped.reserve(std::min(value.size(), max_encoded_len));
  std::size_t index = 0;
  while (index < value.size()) {
    const auto ch = value[index];
    const auto byte = static_cast<unsigned char>(ch);
    std::string token;
    std::size_t consumed = 1;
    if (byte <= 0x7F) {
      switch (ch) {
        case '"':
          token = "\\\"";
          break;
        case '\\':
          token = "\\\\";
          break;
        case '\b':
          token = "\\b";
          break;
        case '\f':
          token = "\\f";
          break;
        case '\n':
          token = "\\n";
          break;
        case '\r':
          token = "\\r";
          break;
        case '\t':
          token = "\\t";
          break;
        default:
          if (byte < 0x20) {
            std::ostringstream encoded;
            encoded << "\\u" << std::hex << std::uppercase << std::setw(4)
                    << std::setfill('0') << static_cast<unsigned>(byte);
            token = encoded.str();
          } else {
            token.assign(1, ch);
          }
          break;
      }
    } else {
      std::size_t sequence_len = 0;
      if (byte >= 0xC2 && byte <= 0xDF) {
        sequence_len = 2;
      } else if (byte >= 0xE0 && byte <= 0xEF) {
        sequence_len = 3;
      } else if (byte >= 0xF0 && byte <= 0xF4) {
        sequence_len = 4;
      }

      bool valid = sequence_len != 0 && index + sequence_len <= value.size();
      for (std::size_t offset = 1; valid && offset < sequence_len; ++offset) {
        const auto continuation = static_cast<unsigned char>(value[index + offset]);
        valid = (continuation & 0xC0) == 0x80;
      }
      if (valid) {
        const auto second = static_cast<unsigned char>(value[index + 1]);
        valid = !((byte == 0xE0 && second < 0xA0) ||
                  (byte == 0xED && second > 0x9F) ||
                  (byte == 0xF0 && second < 0x90) ||
                  (byte == 0xF4 && second > 0x8F));
      }
      if (valid) {
        token.assign(value, index, sequence_len);
        consumed = sequence_len;
      } else {
        token = "\\uFFFD";
      }
    }
    if (escaped.size() + token.size() > max_encoded_len) {
      break;
    }
    escaped += token;
    index += consumed;
  }
  return escaped;
}

void append_object_separator(std::ostringstream& out, bool* first) {
  if (*first) {
    *first = false;
  } else {
    out << ",";
  }
}

void append_string_field(std::ostringstream& out,
                         bool* first,
                         const char* name,
                         const std::string& value,
                         std::size_t max_len = kStatusStringMaxLen) {
  if (value.empty()) {
    return;
  }
  append_object_separator(out, first);
  out << "\"" << name << "\":\""
      << escape_json_string(clipped_status_string(value, max_len)) << "\"";
}

void append_bounded_string_field(std::ostringstream& out,
                                 bool* first,
                                 const char* name,
                                 const std::string& value,
                                 std::size_t max_encoded_len,
                                 const char* fallback = nullptr) {
  const std::string selected = value.empty() && fallback != nullptr ? fallback : value;
  if (selected.empty() && fallback == nullptr) {
    return;
  }
  append_object_separator(out, first);
  out << "\"" << name << "\":\""
      << escape_json_string_bounded(selected, max_encoded_len) << "\"";
}

void append_bool_field(std::ostringstream& out, bool* first, const char* name, bool value) {
  append_object_separator(out, first);
  out << "\"" << name << "\":" << (value ? "true" : "false");
}

void append_uint_field(std::ostringstream& out,
                       bool* first,
                       const char* name,
                       std::uint32_t value) {
  append_object_separator(out, first);
  out << "\"" << name << "\":" << value;
}

void append_uint64_field(std::ostringstream& out,
                         bool* first,
                         const char* name,
                         std::uint64_t value) {
  append_object_separator(out, first);
  out << "\"" << name << "\":" << value;
}

void append_int_field(std::ostringstream& out, bool* first, const char* name, int value) {
  append_object_separator(out, first);
  out << "\"" << name << "\":" << value;
}

void append_sync_core(std::ostringstream& out,
                      bool* first,
                      const ConfigStatusSnapshot& snapshot,
                      bool include_usb_management_capability = true,
                      bool include_legacy_capabilities = true) {
  append_bounded_string_field(
      out, first, "firmware", snapshot.firmware, kFirmwareStringMaxLen, "unknown");
  append_bounded_string_field(
      out, first, "phase", snapshot.phase, kCompactStatusStringMaxLen, "status");
  append_bounded_string_field(
      out, first, "status", snapshot.status, kCompactStatusStringMaxLen, "unknown");
  append_bounded_string_field(out,
                              first,
                              "target_platform",
                              snapshot.target_platform,
                              kCompactStatusStringMaxLen,
                              "macos");
  append_object_separator(out, first);
  out << "\"capabilities\":{";
  if (include_legacy_capabilities) {
    out << "\"semantic_actions\":"
        << (snapshot.semantic_actions ? "true" : "false")
        << ",\"offline_platform_switch\":"
        << (snapshot.offline_platform_switch ? "true" : "false")
        << ",";
  }
  out << "\"config_max_bytes\":" << kConfigMaxJsonLen;
  if (include_usb_management_capability) {
    out << ",\"usb_management_v1\":"
        << (snapshot.usb_management_v1 ? "true" : "false");
  }
  out << "}";
  append_bool_field(out, first, "saved", snapshot.saved);
}

void append_battery_summary(std::ostringstream& out,
                            bool* first,
                            const ConfigStatusSnapshot& snapshot,
                            bool include_detail) {
  append_uint_field(out, first, "battery_mv", snapshot.battery_mv);
  append_uint_field(out, first, "battery_percent", snapshot.battery_percent);
  if (!include_detail || snapshot.battery.state.empty()) {
    return;
  }
  append_uint_field(out, first, "battery_raw_mv", snapshot.battery.raw_mv);
  append_bounded_string_field(out,
                              first,
                              "battery_state",
                              snapshot.battery.state,
                              kCompactStatusStringMaxLen);
  append_uint_field(out, first, "battery_sample_age_ms", snapshot.battery.sample_age_ms);
  append_bool_field(out, first, "battery_full_anchor", snapshot.battery.full_anchor_ready);
}

void append_compact_power(std::ostringstream& out,
                          bool* first,
                          const PowerDiagnosticsSnapshot& power) {
  if (power.mode.empty()) {
    return;
  }
  append_object_separator(out, first);
  out << "\"power\":{";
  bool first_power = true;
  append_bool_field(out, &first_power, "compact", true);
  append_bounded_string_field(
      out, &first_power, "mode", power.mode, kCompactStatusStringMaxLen, "unknown");
  append_bounded_string_field(out,
                              &first_power,
                              "last_wake",
                              power.last_wake,
                              kCompactStatusStringMaxLen,
                              "unknown");
  out << "}";
}

void append_compact_diagnostics(std::ostringstream& out,
                                bool* first,
                                const BoardDiagnosticsSnapshot& diagnostics,
                                bool include_details) {
  if (diagnostics.board.empty()) {
    return;
  }
  append_object_separator(out, first);
  out << "\"diag\":{";
  bool first_diag = true;
  append_bool_field(out, &first_diag, "compact", true);
  append_bounded_string_field(
      out, &first_diag, "board", diagnostics.board, kCompactStatusStringMaxLen);
  if (!include_details) {
    out << "}";
    return;
  }
  append_bounded_string_field(
      out, &first_diag, "keys", diagnostics.keys, kCompactStatusStringMaxLen);
  append_bounded_string_field(
      out, &first_diag, "enc", diagnostics.encoder, kCompactStatusStringMaxLen);
  append_bounded_string_field(
      out, &first_diag, "last", diagnostics.last_input, 24);
  append_uint_field(out, &first_diag, "in_drop", diagnostics.input_drops);
  append_uint_field(out, &first_diag, "enc_drop", diagnostics.encoder_drops);
  out << "}";
}

void append_compact_audio(std::ostringstream& out,
                          bool* first,
                          const AudioStatusSnapshot& audio,
                          bool include_last_error) {
  if (audio.source.empty() && !audio.enabled) {
    return;
  }
  append_object_separator(out, first);
  out << "\"audio\":{";
  bool first_audio = true;
  append_bool_field(out, &first_audio, "compact", true);
  append_bool_field(out, &first_audio, "enabled", audio.enabled);
  append_bounded_string_field(
      out, &first_audio, "source", audio.source, kCompactStatusStringMaxLen, "unknown");
  append_bounded_string_field(out,
                              &first_audio,
                              "microphone_source",
                              audio.microphone_source,
                              kCompactStatusStringMaxLen);
  append_bounded_string_field(
      out, &first_audio, "capture", audio.capture, kAudioCaptureStringMaxLen);
  append_bounded_string_field(out,
                              &first_audio,
                              "control_state",
                              audio.control_state,
                              40);
  append_bounded_string_field(out, &first_audio, "host", audio.host, 24);
  if (audio.port > 0) {
    append_int_field(out, &first_audio, "port", audio.port);
  }
  if (include_last_error) {
    append_bounded_string_field(out, &first_audio, "last_error", audio.last_error, 32);
  }
  out << "}";
}

void append_compact_speaker_boot(
    std::ostringstream& out,
    bool* first,
    const SpeakerProbeSnapshot* speaker) {
  if (speaker == nullptr || !speaker->present) {
    return;
  }
  append_object_separator(out, first);
  // The normal battery/GATT view only needs enough immutable evidence to
  // locate a failed boot playback stage. Full metrics remain in spk_probe.
  // This core fits inside the battery view's reserved 120-byte BLE budget even
  // with worst-case 32-bit values.
  out << "\"spk\":{"
      << "\"v\":" << static_cast<unsigned>(speaker->version)
      << ",\"g\":" << speaker->generation
      << ",\"st\":" << static_cast<unsigned>(speaker->stage)
      << ",\"r\":" << static_cast<unsigned>(speaker->result)
      << ",\"e\":" << static_cast<unsigned>(speaker->error)
      << ",\"x\":" << speaker->raw_error
      << "}";
}

std::string build_compact_status_json(const ConfigStatusSnapshot& snapshot,
                                      bool include_fingerprint,
                                      bool include_battery_detail,
                                      bool include_audio,
                                      bool include_power,
                                      bool include_diagnostics,
                                      bool include_audio_last_error = true,
                                      bool include_fingerprint_details = true,
                                      bool include_usb_management_capability = true,
                                      bool include_legacy_capabilities = true,
                                      bool include_diagnostic_details = true,
                                      bool include_speaker_boot = false) {
  std::ostringstream out;
  out << "{\"schema\":\"ai_keyboard.config_status.v1\"";
  bool first = false;
  append_sync_core(
      out,
      &first,
      snapshot,
      include_usb_management_capability,
      include_legacy_capabilities);
  if (include_fingerprint) {
    append_uint_field(out, &first, "bytes", snapshot.bytes);
    append_uint_field(out, &first, "crc16", snapshot.crc16);
    if (include_fingerprint_details) {
      append_bounded_string_field(
          out, &first, "ptt_hotkey", snapshot.ptt_hotkey, 32, "");
      append_bounded_string_field(
          out, &first, "edit_ptt_hotkey", snapshot.edit_ptt_hotkey, 32, "");
      append_bounded_string_field(
          out, &first, "hotkey_mode", snapshot.hotkey_mode, kCompactStatusStringMaxLen, "");
    }
  }
  append_battery_summary(out, &first, snapshot, include_battery_detail);
  if (include_audio) {
    append_compact_audio(out, &first, snapshot.audio, include_audio_last_error);
  }
  if (include_power) {
    append_compact_power(out, &first, snapshot.power);
  }
  if (include_diagnostics) {
    append_compact_diagnostics(
        out, &first, snapshot.diagnostics, include_diagnostic_details);
  }
  if (include_speaker_boot) {
    append_compact_speaker_boot(out, &first, snapshot.speaker);
  }
  out << "}";
  return out.str();
}

std::string build_battery_status_json(const ConfigStatusSnapshot& snapshot) {
  return build_compact_status_json(snapshot,
                                   true,
                                   true,
                                   false,
                                   true,
                                   false,
                                   true,
                                   false,
                                   false,
                                   true,
                                   true,
                                   true);
}

std::string build_speaker_probe_status_json(
    const ConfigStatusSnapshot& snapshot) {
  const auto& speaker = *snapshot.speaker;
  std::ostringstream out;
  out << "{\"schema\":\"ai_keyboard.config_status.v1\"";
  bool first = false;
  // The probe view is diagnostic and versioned. Keep every synchronization
  // fact, but omit legacy static booleans and hotkey detail so worst-case
  // 32-bit metrics remain within the shared USB/BLE 512-byte envelope.
  append_bounded_string_field(
      out, &first, "firmware", snapshot.firmware, 36, "unknown");
  append_bounded_string_field(
      out, &first, "phase", "spk_probe", kCompactStatusStringMaxLen);
  append_bounded_string_field(
      out, &first, "status", "probe", kCompactStatusStringMaxLen);
  append_bounded_string_field(out,
                              &first,
                              "target_platform",
                              snapshot.target_platform,
                              kCompactStatusStringMaxLen,
                              "macos");
  append_object_separator(out, &first);
  out << "\"capabilities\":{\"config_max_bytes\":" << kConfigMaxJsonLen
      << "}";
  append_bool_field(out, &first, "saved", snapshot.saved);
  append_uint_field(out, &first, "bytes", snapshot.bytes);
  append_uint_field(out, &first, "crc16", snapshot.crc16);

  append_object_separator(out, &first);
  out << "\"spk\":{"
      << "\"v\":" << static_cast<unsigned>(speaker.version)
      << ",\"g\":" << speaker.generation
      << ",\"st\":"
      << static_cast<unsigned>(speaker.stage)
      << ",\"r\":"
      << static_cast<unsigned>(speaker.result)
      << ",\"e\":"
      << static_cast<unsigned>(speaker.error)
      << ",\"x\":" << speaker.raw_error
      << ",\"mg\":" << speaker.microphone_generation
      << ",\"fu\":" << speaker.first_submit_us
      << ",\"du\":" << speaker.decode_total_us
      << ",\"mu\":" << speaker.decode_max_us
      << ",\"n\":" << speaker.decoded_frames
      << ",\"p\":" << speaker.decoded_pcm_bytes
      << ",\"sw\":" << speaker.stack_high_water_bytes
      << ",\"h0\":" << speaker.heap_begin_free
      << ",\"h1\":" << speaker.heap_terminal_free
      << ",\"hl\":" << speaker.heap_largest_block
      << ",\"hm\":" << speaker.heap_minimum_free
      << ",\"pk\":" << speaker.decoded_abs_peak
      << ",\"rm\":" << speaker.decoded_rms_permille
      << "}}";
  return out.str();
}

}  // namespace

static std::string build_full_status_json(const ConfigStatusSnapshot& snapshot) {
  std::ostringstream out;
  out << "{\"schema\":\"ai_keyboard.config_status.v1\""
      << ",\"firmware\":\"" << escape_json_string(snapshot.firmware) << "\""
      << ",\"phase\":\"" << escape_json_string(snapshot.phase) << "\""
      << ",\"status\":\"" << escape_json_string(snapshot.status) << "\""
      << ",\"bytes\":" << snapshot.bytes
      << ",\"crc16\":" << snapshot.crc16
      << ",\"ptt_hotkey\":\"" << escape_json_string(snapshot.ptt_hotkey) << "\""
      << ",\"edit_ptt_hotkey\":\"" << escape_json_string(snapshot.edit_ptt_hotkey) << "\""
      << ",\"hotkey_mode\":\"" << escape_json_string(snapshot.hotkey_mode) << "\""
      << ",\"target_platform\":\"" << escape_json_string(snapshot.target_platform) << "\""
      << ",\"capabilities\":{\"semantic_actions\":"
      << (snapshot.semantic_actions ? "true" : "false")
      << ",\"offline_platform_switch\":"
      << (snapshot.offline_platform_switch ? "true" : "false")
      << ",\"config_max_bytes\":" << kConfigMaxJsonLen
      << ",\"usb_management_v1\":"
      << (snapshot.usb_management_v1 ? "true" : "false") << "}"
      << ",\"saved\":" << (snapshot.saved ? "true" : "false")
      << ",\"battery_mv\":" << snapshot.battery_mv
      << ",\"battery_percent\":" << static_cast<unsigned>(snapshot.battery_percent);
  if (!snapshot.battery.state.empty()) {
    out << ",\"battery_raw_mv\":" << snapshot.battery.raw_mv
        << ",\"battery_state\":\"" << escape_json_string(snapshot.battery.state) << "\""
        << ",\"battery_sample_age_ms\":" << snapshot.battery.sample_age_ms
        << ",\"battery_full_anchor\":"
        << (snapshot.battery.full_anchor_ready ? "true" : "false");
  }
  const bool has_audio_status = !snapshot.audio.source.empty() || snapshot.audio.enabled;
  if (has_audio_status) {
    bool first_audio = true;
    out << ",\"audio\":{";
    append_bool_field(out, &first_audio, "enabled", snapshot.audio.enabled);
    append_string_field(out, &first_audio, "source", snapshot.audio.source);
    append_string_field(out, &first_audio, "microphone_source", snapshot.audio.microphone_source);
    append_string_field(out, &first_audio, "capture", snapshot.audio.capture);
    append_string_field(out, &first_audio, "host", snapshot.audio.host);
    if (snapshot.audio.port > 0) {
      append_int_field(out, &first_audio, "port", snapshot.audio.port);
    }
    append_uint_field(out, &first_audio, "sent_packets", snapshot.audio.sent_packets);
    append_uint_field(out, &first_audio, "sent_bytes", snapshot.audio.sent_bytes);
    append_uint_field(out, &first_audio, "last_rms_milli", snapshot.audio.last_rms_milli);
    append_uint_field(out, &first_audio, "peak_rms_milli", snapshot.audio.peak_rms_milli);
    append_uint_field(out, &first_audio, "send_errors", snapshot.audio.send_errors);
    append_uint_field(out, &first_audio, "read_errors", snapshot.audio.read_errors);
    append_uint_field(out, &first_audio, "recovery_count", snapshot.audio.recovery_count);
    append_uint_field(out, &first_audio, "session_generation", snapshot.audio.session_generation);
    append_uint64_field(out, &first_audio, "session_id", snapshot.audio.session_id);
    append_string_field(out, &first_audio, "stream_phase", snapshot.audio.stream_phase);
    append_string_field(out, &first_audio, "stop_reason", snapshot.audio.stop_reason);
    append_string_field(out, &first_audio, "control_state", snapshot.audio.control_state);
    append_string_field(out, &first_audio, "last_error", snapshot.audio.last_error);
    out << "}";
  }
  if (!snapshot.power.mode.empty()) {
    bool first_power = true;
    out << ",\"power\":{";
    append_string_field(out, &first_power, "mode", snapshot.power.mode);
    append_uint_field(out, &first_power, "poll_ms", snapshot.power.poll_ms);
    append_uint_field(out, &first_power, "idle_ms", snapshot.power.idle_ms);
    append_uint_field(out, &first_power, "deep_entries", snapshot.power.deep_entries);
    append_uint_field(out, &first_power, "deep_ms", snapshot.power.deep_ms);
    append_uint_field(out, &first_power, "last_enter_ms", snapshot.power.last_enter_ms);
    append_uint_field(out, &first_power, "last_exit_ms", snapshot.power.last_exit_ms);
    append_string_field(out, &first_power, "last_wake", snapshot.power.last_wake);
    append_uint_field(out, &first_power, "wake_edges", snapshot.power.wake_edges);
    append_bool_field(out, &first_power, "usb", snapshot.power.usb);
    if (snapshot.power.cycle_seq != 0) {
      append_uint_field(out, &first_power, "cycle_seq", snapshot.power.cycle_seq);
      append_uint_field(out, &first_power, "cycle_idle_ms", snapshot.power.cycle_idle_ms);
      append_uint_field(out, &first_power, "cycle_deep_ms", snapshot.power.cycle_deep_ms);
      append_uint_field(out, &first_power, "cycle_flags", snapshot.power.cycle_flags);
      append_string_field(out, &first_power, "cycle_wake", snapshot.power.cycle_wake);
    }
    out << "}";
  }
  if (!has_audio_status && !snapshot.diagnostics.board.empty()) {
    bool first_diag = true;
    out << ",\"diag\":{";
    append_string_field(out, &first_diag, "board", snapshot.diagnostics.board);
    append_string_field(out, &first_diag, "keys", snapshot.diagnostics.keys);
    append_string_field(out, &first_diag, "enc", snapshot.diagnostics.encoder);
    append_string_field(out, &first_diag, "last", snapshot.diagnostics.last_input);
    append_uint_field(out, &first_diag, "last_age_ms", snapshot.diagnostics.last_input_age_ms);
    append_int_field(out, &first_diag, "wake", snapshot.diagnostics.key_wake);
    append_int_field(out, &first_diag, "vin", snapshot.diagnostics.external_power);
    append_int_field(out, &first_diag, "chrg", snapshot.diagnostics.charge);
    append_int_field(out, &first_diag, "pwr_gpio", snapshot.diagnostics.pwr_gpio);
    append_int_field(out, &first_diag, "pwr_level", snapshot.diagnostics.pwr_level);
    append_int_field(out, &first_diag, "pwr_active", snapshot.diagnostics.pwr_active);
    append_int_field(out, &first_diag, "led_gpio", snapshot.diagnostics.led_gpio);
    append_uint_field(out, &first_diag, "in_edge", snapshot.diagnostics.input_edges);
    append_uint_field(out, &first_diag, "in_drop", snapshot.diagnostics.input_drops);
    append_uint_field(out, &first_diag, "in_evt", snapshot.diagnostics.input_events);
    append_uint_field(out, &first_diag, "in_filter", snapshot.diagnostics.input_filtered);
    append_uint_field(out, &first_diag, "enc_edge", snapshot.diagnostics.encoder_edges);
    append_uint_field(out, &first_diag, "enc_step", snapshot.diagnostics.encoder_steps);
    append_uint_field(out, &first_diag, "enc_invalid", snapshot.diagnostics.encoder_invalid);
    append_uint_field(out, &first_diag, "enc_partial", snapshot.diagnostics.encoder_partial);
    append_uint_field(out, &first_diag, "enc_drop", snapshot.diagnostics.encoder_drops);
    out << "}";
  }
  out << "}";
  return out.str();
}

std::string build_config_status_json(const ConfigStatusSnapshot& snapshot) {
  if (snapshot.phase == "spk_probe" && snapshot.speaker != nullptr &&
      snapshot.speaker->present) {
    return build_speaker_probe_status_json(snapshot);
  }

  if (snapshot.phase == "battery") {
    const auto compact = build_battery_status_json(snapshot);
    if (compact.size() <=
        kConfigStatusGattSafeLen - kConfigStatusBatteryBleReserveLen) {
      return compact;
    }

    // The strings above are bounded, so this is only an adversarial-data guard.
    // Keep the compact wake/mode evidence before verbose battery details when
    // the reserved BLE budget is exhausted by counter-width extremes.
    const auto without_battery_detail = build_compact_status_json(snapshot,
                                                                   true,
                                                                   false,
                                                                   false,
                                                                   true,
                                                                   false,
                                                                   true,
                                                                   false,
                                                                   false,
                                                                   true,
                                                                   true,
                                                                   true);
    if (without_battery_detail.size() <=
        kConfigStatusGattSafeLen - kConfigStatusBatteryBleReserveLen) {
      return without_battery_detail;
    }

    // Speaker boot evidence and compact power detail are diagnostic overlays.
    // Try keeping the speaker core without power before falling back to the
    // mandatory management contract.
    const auto without_power = build_compact_status_json(snapshot,
                                                          true,
                                                          false,
                                                          false,
                                                          false,
                                                          false,
                                                          true,
                                                          false,
                                                          false,
                                                          true,
                                                          true,
                                                          true);
    if (without_power.size() <=
        kConfigStatusGattSafeLen - kConfigStatusBatteryBleReserveLen) {
      return without_power;
    }

    // A battery refresh replaces the one published GATT status snapshot. It
    // therefore must remain a self-contained management snapshot even when
    // optional telemetry does not fit. The App needs both explicit legacy
    // capability booleans to distinguish a current firmware from an old or
    // incomplete response; never trade those fields for power/speaker detail.
    // With bounded firmware/phase/status/platform strings this core, config
    // fingerprint and battery summary are guaranteed to fit in the 392-byte
    // base budget reserved ahead of the BLE connection overlay.
    return build_compact_status_json(snapshot,
                                     true,
                                     false,
                                     false,
                                     false,
                                     false,
                                     true,
                                     false,
                                     false,
                                     true,
                                     true,
                                     false);
  }

  const bool has_audio_status = !snapshot.audio.source.empty() || snapshot.audio.enabled;
  if (!has_audio_status && !snapshot.diagnostics.board.empty()) {
    const auto compact = build_compact_status_json(snapshot,
                                                    false,
                                                    false,
                                                    false,
                                                    true,
                                                    true);
    if (compact.size() <= kConfigStatusGattSafeLen) {
      return compact;
    }
    const auto minimal_diagnostics = build_compact_status_json(snapshot,
                                                               false,
                                                               false,
                                                               false,
                                                               true,
                                                               true,
                                                               true,
                                                               true,
                                                               true,
                                                               true,
                                                               false);
    if (minimal_diagnostics.size() <= kConfigStatusGattSafeLen) {
      return minimal_diagnostics;
    }
    return build_compact_status_json(snapshot,
                                     false,
                                     false,
                                     false,
                                     false,
                                     false);
  }

  const auto full = build_full_status_json(snapshot);
  if (full.size() <= kConfigStatusGattSafeLen) {
    return full;
  }

  const bool preserve_fingerprint = snapshot.phase == "boot" && snapshot.bytes > 0;
  const auto compact = build_compact_status_json(snapshot,
                                                  preserve_fingerprint,
                                                  false,
                                                  true,
                                                  true,
                                                  false);
  if (compact.size() <= kConfigStatusGattSafeLen) {
    return compact;
  }

  if (!snapshot.audio.control_state.empty()) {
    const auto minimal_audio_with_error =
        build_compact_status_json(snapshot,
                                  preserve_fingerprint,
                                  false,
                                  true,
                                  false,
                                  false,
                                  true,
                                  false,
                                  false,
                                  false);
    if (minimal_audio_with_error.size() <= kConfigStatusGattSafeLen) {
      return minimal_audio_with_error;
    }
  }

  const auto without_audio_error = build_compact_status_json(snapshot,
                                                              preserve_fingerprint,
                                                              false,
                                                              true,
                                                              true,
                                                              false,
                                                              false);
  if (without_audio_error.size() <= kConfigStatusGattSafeLen) {
    return without_audio_error;
  }

  // Runtime audio evidence is more useful than static legacy capabilities or
  // power detail when the shared 512-byte envelope is tight. In particular,
  // control_state carries the mic_ctrl loop/heartbeat counters, which lets a
  // HID/GATT status read distinguish a task that is waiting from one that has
  // stopped progressing. Do not drop that evidence before trying a compact
  // audio-only view.
  if (!snapshot.audio.control_state.empty()) {
    const auto minimal_audio = build_compact_status_json(snapshot,
                                                         preserve_fingerprint,
                                                         false,
                                                         true,
                                                         false,
                                                         false,
                                                         false,
                                                         false,
                                                         false,
                                                         false);
    if (minimal_audio.size() <= kConfigStatusGattSafeLen) {
      return minimal_audio;
    }
  }

  if (preserve_fingerprint) {
    return build_compact_status_json(snapshot,
                                     true,
                                     false,
                                     false,
                                     false,
                                     false);
  }

  return build_compact_status_json(snapshot,
                                   false,
                                   false,
                                   false,
                                   false,
                                   false);
}

std::string build_config_confirmation_status_json(const ConfigStatusSnapshot& snapshot) {
  return build_compact_status_json(snapshot,
                                   true,
                                   false,
                                   false,
                                   false,
                                   false);
}

}  // namespace ai_keyboard
