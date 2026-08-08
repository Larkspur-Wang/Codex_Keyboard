#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

#include "keyboard/config_status.h"

namespace {

static_assert(ai_keyboard::kConfigStatusGattSafeLen <= 512);

bool is_valid_utf8(const std::string& value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (lead <= 0x7F) {
      ++index;
      continue;
    }

    std::size_t length = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      length = 3;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      length = 4;
    } else {
      return false;
    }
    if (index + length > value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
    }
    const auto second = static_cast<unsigned char>(value[index + 1]);
    if ((lead == 0xE0 && second < 0xA0) ||
        (lead == 0xED && second > 0x9F) ||
        (lead == 0xF0 && second < 0x90) ||
        (lead == 0xF4 && second > 0x8F)) {
      return false;
    }
    index += length;
  }
  return true;
}

void assert_sync_core(const std::string& json,
                      const std::string& firmware,
                      const std::string& target_platform,
                      bool saved,
                      bool usb_management = true,
                      bool legacy_capabilities = true) {
  assert(json.find(R"("firmware":")" + firmware + R"(")") != std::string::npos);
  assert(json.find(R"("target_platform":")" + target_platform + R"(")") !=
         std::string::npos);
  const auto expected_capabilities = !legacy_capabilities
      ? R"("capabilities":{"config_max_bytes":2048})"
      : (usb_management
             ? R"("capabilities":{"semantic_actions":true,"offline_platform_switch":true,"config_max_bytes":2048,"usb_management_v1":true})"
             : R"("capabilities":{"semantic_actions":true,"offline_platform_switch":true,"config_max_bytes":2048})");
  assert(json.find(expected_capabilities) != std::string::npos);
  assert(json.find(saved ? R"("saved":true)" : R"("saved":false)") !=
         std::string::npos);
}

}  // namespace

void builds_compact_status_json() {
  const auto json = ai_keyboard::build_config_status_json({
      "0.1.test",
      "push",
      "ok",
      581,
      0x29B1,
      "F12",
      "F14",
      "toggle",
      true,
      3710,
      33,
      {
          true,
          "wifi_udp",
          "keyboard",
          "wifi_udp",
          "pending",
          "192.168.1.55",
          17333,
          "right",
          "left",
      },
      {},
      {
          "v2",
          "11101111",
          "101",
          "K1=2,K2=47,K3=38,K4=41,K5=1,K6=6,K7=7,K8=48,E=17/16/18,W=21,P=8,L=12",
          "2:1,47:1,38:1,41:1,1:1,6:1,7:1,48:1",
          "KEY5:pressed:gpio=1",
          25,
          1,
          0,
          1,
          8,
          1,
          1,
          12,
      },
  });

  assert(json.find(R"("schema":"ai_keyboard.config_status.v1")") != std::string::npos);
  assert(json.find(R"("firmware":"0.1.test")") != std::string::npos);
  assert(json.find(R"("phase":"push")") != std::string::npos);
  assert(json.find(R"("status":"ok")") != std::string::npos);
  assert(json.find(R"("saved":true)") != std::string::npos);
  assert(json.find(R"("battery_mv":3710)") != std::string::npos);
  assert(json.find(R"("battery_percent":33)") != std::string::npos);
  assert(json.find(R"("audio":{"compact":true,"enabled":true)") !=
         std::string::npos);
  assert(json.find(R"("source":"wifi_udp")") != std::string::npos);
  assert(json.find(R"("microphone_source":"keyboard")") != std::string::npos);
  assert(json.find(R"("capture":"pending")") != std::string::npos);
  assert(json.find(R"("host":"192.168.1.55")") != std::string::npos);
  assert(json.find(R"("port":17333)") != std::string::npos);
  assert(json.find(R"("sent_packets")") == std::string::npos);
  assert(json.find(R"("last_rms_milli")") == std::string::npos);
  assert(json.find(R"("power")") == std::string::npos);
  assert(json.find(R"("diag")") == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void includes_compact_board_diagnostics_without_audio_status() {
  const auto json = ai_keyboard::build_config_status_json({
      "0.1.test",
      "inputs",
      "ok",
      0,
      0,
      "F12",
      "F14",
      "toggle",
      true,
      3710,
      33,
      {},
      {
          "deep_idle",
          750,
          305000,
          2,
          600000,
          300000,
          900000,
          "key_wake",
          12,
          false,
          7,
          150000,
          45000,
          3,
          "light_key",
      },
      {
          "v2",
          "11101111",
          "101",
          "K1=2,K2=47,K3=38,K4=41,K5=1,K6=6,K7=7,K8=48,E=17/16/18,W=21,P=8,L=12",
          "2:1,47:1,38:1,41:1,1:1,6:1,7:1,48:1",
          "KEY5:pressed:gpio=1",
          25,
          1,
          0,
          1,
          8,
          1,
          1,
          12,
          101,
          2,
          48,
          7,
          240,
          56,
          3,
          4,
          1,
      },
  });

  assert(json.find(R"("diag":{"compact":true,"board":"v2")") != std::string::npos);
  assert(json.find(R"("keys":"11101111")") != std::string::npos);
  assert(json.find(R"("enc":"101")") != std::string::npos);
  assert(json.find(R"("last":"KEY5:pressed:gpio=1")") != std::string::npos);
  assert(json.find(R"("power":{"compact":true,"mode":"deep_idle")") !=
         std::string::npos);
  assert(json.find(R"("poll_ms")") == std::string::npos);
  assert(json.find(R"("last_wake":"key_wake")") != std::string::npos);
  assert(json.find(R"("wake_edges")") == std::string::npos);
  assert(json.find(R"("cycle_seq")") == std::string::npos);
  assert(json.find(R"("in_edge")") == std::string::npos);
  assert(json.find(R"("in_drop":2)") != std::string::npos);
  assert(json.find(R"("in_evt")") == std::string::npos);
  assert(json.find(R"("in_filter")") == std::string::npos);
  assert(json.find(R"("enc_edge")") == std::string::npos);
  assert(json.find(R"("enc_step")") == std::string::npos);
  assert(json.find(R"("enc_invalid")") == std::string::npos);
  assert(json.find(R"("enc_partial")") == std::string::npos);
  assert(json.find(R"("enc_drop":1)") != std::string::npos);
  assert(json.find(R"("map")") == std::string::npos);
  assert(json.find(R"("raw")") == std::string::npos);
  assert_sync_core(json, "0.1.test", "macos", true);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void long_press_diagnostic_status_is_self_contained_and_gatt_safe() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.30-idf-v2-readable-status";
  snapshot.phase = "diag";
  snapshot.status =
      "wifi=ready ds=config_window ls=config_window m=active up=4294967s vin=1 chrg=1";
  snapshot.ptt_hotkey = "Ctrl+Shift+Space";
  snapshot.edit_ptt_hotkey = "Ctrl+Shift+E";
  snapshot.hotkey_mode = "toggle";
  snapshot.saved = true;
  snapshot.battery_mv = 3508;
  snapshot.battery_percent = 5;
  snapshot.target_platform = "windows";
  snapshot.power = {
      "deep_idle",
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      "encoder_long_press",
      std::numeric_limits<std::uint32_t>::max(),
      false,
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      "status_read",
  };
  snapshot.diagnostics = {
      "v2",
      "11101111",
      "101",
      "K1=2,K2=47,K3=38,K4=41,K5=1,K6=6,K7=7,K8=48,E=17/16/18,W=21,P=8,L=12",
      "2:1,47:1,38:1,41:1,1:1,6:1,7:1,48:1",
      "KEY2:pressed:gpio=47",
      std::numeric_limits<std::uint32_t>::max(),
      1,
      1,
      0,
      8,
      1,
      1,
      12,
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max(),
  };

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert_sync_core(json, snapshot.firmware, "windows", true);
  assert(json.find(R"("phase":"diag")") != std::string::npos);
  assert(json.find(R"("battery_mv":3508)") != std::string::npos);
  assert(json.find(R"("power":{"compact":true)") != std::string::npos);
  assert(json.find(R"("diag":{"compact":true,"board":"v2")") != std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void clips_long_audio_error_and_stays_gatt_safe() {
  ai_keyboard::ConfigStatusSnapshot snapshot{
      "0.3.0-idf-v2-audio-session",
      "push",
      "ok",
      834,
      0x1234,
      "RightMeta",
      "AltGr",
      "toggle",
      true,
      4048,
      85,
      {
          true,
          "wifi_udp",
          "keyboard",
          "udp",
          "wifi_failed",
          "192.168.31.80",
          17333,
          "i2s_right",
          "i2s_left",
          123456,
          987654,
          275,
          683,
          2,
          3,
          4,
          9,
          42,
          "recovering",
          "",
          "ready",
          std::string(240, 'x'),
          "192.168.31.80",
          17333,
      },
  };
  const auto json = ai_keyboard::build_config_status_json(snapshot);

  assert_sync_core(json, snapshot.firmware, "macos", true, false, false);
  assert(json.find(R"("last_error":")") != std::string::npos);
  assert(json.find(std::string(120, 'x')) == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void preserves_app_visible_audio_capture_states_in_compact_status() {
  const char* capture_states[] = {
      "mic_streaming",
      "mic_restarting",
      "mic_task_failed",
  };
  for (const auto* capture : capture_states) {
    ai_keyboard::ConfigStatusSnapshot snapshot;
    snapshot.firmware = "0.4.30-idf-v2-readable-status";
    snapshot.phase = "boot";
    snapshot.status = "ok";
    snapshot.saved = true;
    snapshot.battery_mv = 3508;
    snapshot.battery_percent = 5;
    snapshot.target_platform = "windows";
    snapshot.audio.enabled = true;
    snapshot.audio.source = "wifi_udp";
    snapshot.audio.microphone_source = "keyboard";
    snapshot.audio.capture = capture;
    snapshot.audio.host = "192.168.31.80";
    snapshot.audio.port = 17333;
    snapshot.audio.sent_packets = std::numeric_limits<std::uint32_t>::max();
    snapshot.audio.sent_bytes = std::numeric_limits<std::uint32_t>::max();
    snapshot.audio.last_error = std::string(240, 'x');

    const auto json = ai_keyboard::build_config_status_json(snapshot);
    assert_sync_core(json, snapshot.firmware, "windows", true);
    assert(json.find(R"("audio":{"compact":true)") != std::string::npos);
    assert(json.find(std::string(R"("capture":")") + capture + R"(")") !=
           std::string::npos);
    assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
  }
}

void oversized_runtime_status_preserves_audio_control_progress() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.40-idf-v2-speaker-assets";
  snapshot.phase = "status";
  snapshot.status = std::string(64, 's');
  snapshot.saved = true;
  snapshot.battery_mv = 4179;
  snapshot.battery_percent = 98;
  snapshot.target_platform = "windows";
  snapshot.audio.enabled = true;
  snapshot.audio.source = "wifi_udp";
  snapshot.audio.microphone_source = "keyboard";
  snapshot.audio.capture = "mic_stopped";
  snapshot.audio.host = "192.168.31.80";
  snapshot.audio.port = 17333;
  snapshot.audio.control_state = "ready loops=4294967295 hb=4294967295";
  snapshot.audio.last_error = std::string(240, 'x');
  snapshot.audio.sent_packets = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.sent_bytes = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.last_rms_milli = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.peak_rms_milli = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.send_errors = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.read_errors = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.recovery_count = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.session_generation = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.session_id = std::numeric_limits<std::uint64_t>::max();
  snapshot.audio.stream_phase = std::string(64, 'p');
  snapshot.audio.stop_reason = std::string(64, 'r');
  snapshot.audio.stream_host = "192.168.31.80";
  snapshot.audio.stream_port = 17333;
  snapshot.power.mode = std::string(64, 'm');
  snapshot.power.last_wake = std::string(64, 'w');

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert_sync_core(json, snapshot.firmware, "windows", true, false, false);
  assert(json.find(R"("audio":{"compact":true)") != std::string::npos);
  assert(json.find(
             R"("control_state":"ready loops=4294967295 hb=4294967295")") !=
         std::string::npos);
  assert(json.find(R"("power")") == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void stored_boot_status_keeps_config_fingerprint_when_full_status_is_too_large() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.30-idf-v2-readable-status";
  snapshot.phase = "boot";
  snapshot.status = "ok";
  snapshot.bytes = 1024;
  snapshot.crc16 = 0xA55A;
  snapshot.ptt_hotkey = "Ctrl+Shift+Space";
  snapshot.edit_ptt_hotkey = "Ctrl+Shift+E";
  snapshot.hotkey_mode = "toggle";
  snapshot.saved = true;
  snapshot.battery_mv = 3508;
  snapshot.battery_percent = 5;
  snapshot.target_platform = "windows";
  snapshot.audio.enabled = true;
  snapshot.audio.source = "wifi_udp";
  snapshot.audio.microphone_source = "keyboard";
  snapshot.audio.capture = "mic_streaming";
  snapshot.audio.host = "192.168.31.80";
  snapshot.audio.port = 17333;
  snapshot.audio.sent_packets = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.sent_bytes = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.last_error = std::string(240, 'x');

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert_sync_core(json, snapshot.firmware, "windows", true);
  assert(json.find(R"("phase":"boot")") != std::string::npos);
  assert(json.find(R"("bytes":1024)") != std::string::npos);
  assert(json.find(R"("crc16":42330)") != std::string::npos);
  // At the 512-byte BLE wire budget, an adversarially long audio error may
  // force the compact fallback to drop audio details. The persisted config
  // fingerprint and sync core remain the non-negotiable recovery contract.
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void compact_status_clipping_preserves_utf8_codepoint_boundaries() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "固件版本固件版本固件版本固件版本";
  snapshot.phase = "boot";
  snapshot.status = "状态状态状态状态状态状态状态状态";
  snapshot.saved = true;
  snapshot.target_platform = "windows";
  snapshot.audio.enabled = true;
  snapshot.audio.source = "wifi_udp";
  snapshot.audio.capture = "mic_streaming";
  snapshot.audio.last_error = std::string(240, 'x');

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
  assert(is_valid_utf8(json));
}

void config_confirmation_omits_diagnostics_and_is_gatt_safe() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.24-idf-v2-host-platform";
  snapshot.phase = "push";
  snapshot.status = "ok";
  snapshot.bytes = 979;
  snapshot.crc16 = 0xA55A;
  snapshot.ptt_hotkey = "Ctrl+Shift+Space";
  snapshot.edit_ptt_hotkey = "Ctrl+Shift+E";
  snapshot.hotkey_mode = "toggle";
  snapshot.saved = true;
  snapshot.battery_mv = 3470;
  snapshot.battery_percent = 4;
  snapshot.target_platform = "windows";
  snapshot.audio.enabled = true;
  snapshot.audio.source = "wifi_udp";
  snapshot.audio.last_error = std::string(240, 'x');
  snapshot.power.mode = "active";
  snapshot.diagnostics.board = "v2";

  const auto json = ai_keyboard::build_config_confirmation_status_json(snapshot);
  assert_sync_core(json, snapshot.firmware, "windows", true);
  assert(json.find(R"("bytes":979)") != std::string::npos);
  assert(json.find(R"("crc16":42330)") != std::string::npos);
  assert(json.find(R"("target_platform":"windows")") != std::string::npos);
  assert(json.find(R"("saved":true)") != std::string::npos);
  assert(json.find(R"("audio")") == std::string::npos);
  assert(json.find(R"("power")") == std::string::npos);
  assert(json.find(R"("diag")") == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void includes_fresh_battery_metadata() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.12-idf-v2-battery-estimate";
  snapshot.phase = "battery";
  snapshot.status = "fresh";
  snapshot.bytes = 1052;
  snapshot.crc16 = 65424;
  snapshot.ptt_hotkey = "RightMeta";
  snapshot.edit_ptt_hotkey = "AltGr";
  snapshot.hotkey_mode = "toggle";
  snapshot.saved = true;
  snapshot.battery_mv = 3790;
  snapshot.battery_percent = 50;
  snapshot.target_platform = "windows";
  snapshot.power = {
      "active",
      5,
      0,
      2,
      600000,
      300000,
      900000,
      "status_read",
      12,
      false,
      8,
      150000,
      420000,
      3,
      "status",
  };
  snapshot.battery = {3710, "battery", 24, true};

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert_sync_core(json, snapshot.firmware, "windows", true, false);
  assert(json.find(R"("battery_mv":3790)") != std::string::npos);
  assert(json.find(R"("bytes":1052)") != std::string::npos);
  assert(json.find(R"("crc16":65424)") != std::string::npos);
  assert(json.find(R"("ptt_hotkey")") == std::string::npos);
  assert(json.find(R"("audio")") == std::string::npos);
  assert(json.find(R"("power":{"compact":true,"mode":"active")") != std::string::npos);
  assert(json.find(R"("poll_ms")") == std::string::npos);
  assert(json.find(R"("idle_ms")") == std::string::npos);
  assert(json.find(R"("deep_entries")") == std::string::npos);
  assert(json.find(R"("deep_ms")") == std::string::npos);
  assert(json.find(R"("last_enter_ms")") == std::string::npos);
  assert(json.find(R"("wake_edges")") == std::string::npos);
  assert(json.find(R"("usb")") == std::string::npos);
  assert(json.find(R"("cycle_seq")") == std::string::npos);
  assert(json.find(R"("cycle_wake")") == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen -
                            ai_keyboard::kConfigStatusBatteryBleReserveLen);
}

void battery_status_reserves_space_for_ble_at_counter_limits() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.30-idf-v2-readable-status";
  snapshot.phase = "battery";
  snapshot.status = std::string(64, 's');
  snapshot.bytes = std::numeric_limits<std::uint16_t>::max();
  snapshot.crc16 = std::numeric_limits<std::uint16_t>::max();
  snapshot.battery_mv = std::numeric_limits<std::uint16_t>::max();
  snapshot.battery_percent = std::numeric_limits<std::uint8_t>::max();
  snapshot.saved = true;
  snapshot.target_platform = "windows";
  snapshot.battery = {
      std::numeric_limits<std::uint16_t>::max(),
      std::string(64, 'b'),
      std::numeric_limits<std::uint32_t>::max(),
      true,
  };
  snapshot.power.mode = std::string(64, 'm');
  snapshot.power.last_wake = std::string(64, 'w');
  snapshot.power.cycle_seq = std::numeric_limits<std::uint32_t>::max();
  snapshot.power.cycle_idle_ms = std::numeric_limits<std::uint32_t>::max();
  snapshot.power.cycle_deep_ms = std::numeric_limits<std::uint32_t>::max();
  snapshot.power.cycle_flags = std::numeric_limits<std::uint32_t>::max();
  snapshot.power.cycle_wake = std::string(64, 'c');

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert_sync_core(json, snapshot.firmware, "windows", true, false);
  assert(json.find(R"("bytes":65535)") != std::string::npos);
  assert(json.find(R"("crc16":65535)") != std::string::npos);
  assert(json.find(R"("battery_mv":65535)") != std::string::npos);
  assert(json.find(R"("power")") == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen -
                            ai_keyboard::kConfigStatusBatteryBleReserveLen);
}

void escapes_status_strings() {
  const auto json = ai_keyboard::build_config_status_json({
      "0.1.\"test\"",
      "boot",
      "bad\\status",
      0,
      0,
      "F12",
      "F14",
      "hold",
      false,
  });

  assert(json.find(R"("firmware":"0.1.\"test\"")") != std::string::npos);
  assert(json.find(R"("status":"bad\\status")") != std::string::npos);
  assert(json.find(R"("saved":false)") != std::string::npos);
  assert(json.find(R"("power")") == std::string::npos);
}

void legacy_status_omits_optional_speaker_probe() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.40";
  snapshot.phase = "status";
  snapshot.status = "cached";
  snapshot.saved = true;

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert(json.find(R"("spk":)") == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

void battery_status_carries_compact_boot_speaker_evidence() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.40-idf-v2-root-fix";
  snapshot.phase = "battery";
  snapshot.status = "fresh";
  snapshot.bytes = 1999;
  snapshot.crc16 = 44143;
  snapshot.saved = true;
  snapshot.battery_mv = 4049;
  snapshot.battery_percent = 85;
  snapshot.target_platform = "macos";
  snapshot.power.mode = "active";
  snapshot.power.last_wake = "boot";

  ai_keyboard::SpeakerProbeSnapshot speaker;
  speaker.present = true;
  speaker.version = ai_keyboard::kSpeakerProbeStatusVersion;
  speaker.generation = std::numeric_limits<std::uint32_t>::max();
  speaker.stage = ai_keyboard::SpeakerProbeStage::TaskAlloc;
  speaker.result = ai_keyboard::SpeakerProbeResult::Failed;
  speaker.error = ai_keyboard::SpeakerProbeError::TaskAlloc;
  speaker.raw_error = std::numeric_limits<std::int32_t>::min();
  speaker.heap_begin_free = std::numeric_limits<std::uint32_t>::max();
  speaker.heap_largest_block = std::numeric_limits<std::uint32_t>::max();
  snapshot.speaker = &speaker;

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert_sync_core(json, snapshot.firmware, "macos", true, false);
  assert(json.find(R"("bytes":1999)") != std::string::npos);
  assert(json.find(R"("crc16":44143)") != std::string::npos);
  assert(json.find(
             R"("spk":{"v":1,"g":4294967295,"st":6,"r":5,"e":7,"x":-2147483648})") !=
         std::string::npos);
  assert(json.size() <=
         ai_keyboard::kConfigStatusGattSafeLen -
             ai_keyboard::kConfigStatusBatteryBleReserveLen);
}

void speaker_probe_worst_case_is_versioned_complete_and_gatt_safe() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = std::string(40, 'f');
  snapshot.phase = "spk_probe";
  snapshot.status = "cancelled";
  snapshot.bytes = std::numeric_limits<std::uint16_t>::max();
  snapshot.crc16 = std::numeric_limits<std::uint16_t>::max();
  snapshot.saved = true;
  snapshot.target_platform = "windows";
  ai_keyboard::SpeakerProbeSnapshot speaker;
  speaker.present = true;
  speaker.version = ai_keyboard::kSpeakerProbeStatusVersion;
  speaker.stage = ai_keyboard::SpeakerProbeStage::Done;
  speaker.result = ai_keyboard::SpeakerProbeResult::Cancelled;
  speaker.error = ai_keyboard::SpeakerProbeError::Unknown;
  speaker.generation = std::numeric_limits<std::uint32_t>::max();
  speaker.raw_error = std::numeric_limits<std::int32_t>::min();
  speaker.microphone_generation =
      std::numeric_limits<std::uint32_t>::max();
  speaker.first_submit_us =
      std::numeric_limits<std::uint32_t>::max();
  speaker.decode_total_us =
      std::numeric_limits<std::uint32_t>::max();
  speaker.decode_max_us =
      std::numeric_limits<std::uint32_t>::max();
  speaker.decoded_frames =
      std::numeric_limits<std::uint32_t>::max();
  speaker.decoded_pcm_bytes =
      std::numeric_limits<std::uint32_t>::max();
  speaker.stack_high_water_bytes =
      std::numeric_limits<std::uint32_t>::max();
  speaker.heap_begin_free =
      std::numeric_limits<std::uint32_t>::max();
  speaker.heap_terminal_free =
      std::numeric_limits<std::uint32_t>::max();
  speaker.heap_largest_block =
      std::numeric_limits<std::uint32_t>::max();
  speaker.heap_minimum_free =
      std::numeric_limits<std::uint32_t>::max();
  speaker.decoded_abs_peak =
      std::numeric_limits<std::uint32_t>::max();
  speaker.decoded_rms_permille =
      std::numeric_limits<std::uint32_t>::max();
  snapshot.speaker = &speaker;

  const auto json = ai_keyboard::build_config_status_json(snapshot);
  assert_sync_core(
      json, std::string(36, 'f'), "windows", true, false, false);
  assert(json.find(R"("phase":"spk_probe")") != std::string::npos);
  assert(json.find(R"("status":"probe")") != std::string::npos);
  assert(json.find(R"("bytes":65535)") != std::string::npos);
  assert(json.find(R"("crc16":65535)") != std::string::npos);
  assert(json.find(
             R"("spk":{"v":1,"g":4294967295,"st":21,"r":6,"e":19,"x":-2147483648)") !=
         std::string::npos);
  const char* required_metrics[] = {
      R"("mg":4294967295)",
      R"("fu":4294967295)",
      R"("du":4294967295)",
      R"("mu":4294967295)",
      R"("n":4294967295)",
      R"("p":4294967295)",
      R"("sw":4294967295)",
      R"("h0":4294967295)",
      R"("h1":4294967295)",
      R"("hl":4294967295)",
      R"("hm":4294967295)",
      R"("pk":4294967295)",
      R"("rm":4294967295)",
  };
  for (const auto* metric : required_metrics) {
    assert(json.find(metric) != std::string::npos);
  }
  assert(json.find(R"("ptt_hotkey")") == std::string::npos);
  assert(json.find(R"("semantic_actions")") == std::string::npos);
  assert(json.size() <= ai_keyboard::kConfigStatusGattSafeLen);
}

int main() {
  builds_compact_status_json();
  includes_compact_board_diagnostics_without_audio_status();
  long_press_diagnostic_status_is_self_contained_and_gatt_safe();
  clips_long_audio_error_and_stays_gatt_safe();
  preserves_app_visible_audio_capture_states_in_compact_status();
  oversized_runtime_status_preserves_audio_control_progress();
  stored_boot_status_keeps_config_fingerprint_when_full_status_is_too_large();
  compact_status_clipping_preserves_utf8_codepoint_boundaries();
  config_confirmation_omits_diagnostics_and_is_gatt_safe();
  includes_fresh_battery_metadata();
  battery_status_reserves_space_for_ble_at_counter_limits();
  escapes_status_strings();
  legacy_status_omits_optional_speaker_probe();
  battery_status_carries_compact_boot_speaker_evidence();
  speaker_probe_worst_case_is_versioned_complete_and_gatt_safe();
  return 0;
}
