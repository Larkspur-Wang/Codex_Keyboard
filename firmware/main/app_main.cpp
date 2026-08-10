#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "keyboard/agent_status.h"
#include "keyboard/audio_stub.h"
#include "keyboard/board_pins.h"
#include "keyboard/battery_estimator.h"
#include "keyboard/config_payload.h"
#include "keyboard/config_receiver.h"
#include "keyboard/config_status.h"
#include "keyboard/config_state.h"
#include "keyboard/codex_slot_state.h"
#include "keyboard/encoder.h"
#include "keyboard/held_keyboard_state.h"
#include "keyboard/hid_report_queue.h"
#include "keyboard/keyboard_snapshot_delivery.h"
#include "keyboard/keymap.h"
#include "keyboard/power_cycle.h"
#include "keyboard/power_policy.h"
#include "keyboard/platform_selection.h"
#include "keyboard/transport_routing.h"
#include "platform/battery_adc.h"
#include "platform/ble_hid.h"
#include "platform/gpio_keys.h"
#include "platform/keyboard_audio.h"
#include "platform/led_strip_status.h"
#include "platform/nvs_store.h"
#include "platform/peripheral_power.h"
#include "platform/usb_hid.h"
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
#include "platform/speaker_output.h"
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
#include "keyboard/speaker_service_startup.h"
#include "platform/codex_lan_playback.h"
#include "platform/speaker_assets_supervisor.h"
#include "speaker_assets/factory_boot_sound.h"
#include "speaker_assets/volume_prompt.h"
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC)
#include "speaker_assets/diagnostic_link_anchor.h"
#endif
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "easy_input";
constexpr const char* kFirmwareName = "EasyInput AI";
#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
constexpr const char* kFirmwareVersion =
    "0.4.40-idf-v2-spk-ima-probe";
#elif defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
constexpr const char* kFirmwareVersion =
    "0.4.40-idf-v2-spk-opus-observe";
#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
constexpr const char* kFirmwareVersion =
    "0.4.40-idf-v2-spk-boot-probe";
#elif defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
constexpr const char* kFirmwareVersion =
    "0.6.2-easy-codex-board-volume";
#else
constexpr const char* kFirmwareVersion = "0.4.40-idf-v2-audio-pool";
#endif
constexpr std::uint32_t kUsbDisconnectConfirmMs = 25;
constexpr std::uint32_t kActivePollIntervalMs =
    ai_keyboard::kDefaultPowerPolicy.active_poll_ms;
constexpr std::uint32_t kIdleConnectedPollIntervalMs =
    ai_keyboard::kDefaultPowerPolicy.idle_connected_poll_ms;
constexpr std::uint32_t kDeepIdleConnectedPollIntervalMs =
    ai_keyboard::kDefaultPowerPolicy.deep_idle_connected_poll_ms;
constexpr std::uint32_t kDeepIdleUnverifiedWakePollIntervalMs =
    ai_keyboard::kDefaultPowerPolicy.deep_idle_unverified_poll_ms;
constexpr std::uint32_t kIdleUsbPollIntervalMs =
    ai_keyboard::kDefaultPowerPolicy.idle_usb_poll_ms;
constexpr std::uint32_t kIdleBatteryPollIntervalMs =
    ai_keyboard::kDefaultPowerPolicy.idle_battery_poll_ms;
constexpr std::uint32_t kIdleAfterMs = ai_keyboard::kDefaultPowerPolicy.idle_after_ms;
constexpr std::uint32_t kDeepIdleAfterMs =
    ai_keyboard::kDefaultPowerPolicy.deep_idle_after_ms;
// 电池供电且满足全部安全门槛时,超长空闲进入 deep sleep(微安级待机);
// 任意主键/旋钮按压经 KEY_WAKE 唤醒,代价是唤醒后需重启+蓝牙重连(~2-4s)。
constexpr std::uint32_t kDeepSleepAfterMs =
    ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms;
constexpr std::uint32_t kControlledLightSleepNapMs = 30;
constexpr std::uint32_t kControlledLightSleepErrorBackoffMs = 5000;
constexpr std::uint32_t kActivePowerLogIntervalMs = 60000;
constexpr std::uint32_t kIdlePowerLogIntervalMs = 300000;
constexpr std::uint32_t kActiveHeartbeatLogIntervalMs = 60000;
constexpr std::uint32_t kIdleHeartbeatLogIntervalMs = 600000;
constexpr std::uint32_t kEncoderConfigModeHoldMs = 3000;
constexpr std::uint32_t kPlatformSelectionModeTimeoutMs = 10000;
constexpr std::uint32_t kEncoderLedFeedbackMinIntervalMs = 45;
constexpr std::uint32_t kSpeakerVolumePersistDelayMs = 1000;
constexpr std::uint16_t kBleActiveConnIntervalMin = 12;
constexpr std::uint16_t kBleActiveConnIntervalMax = 36;
constexpr std::uint16_t kBleActiveConnLatency = 0;
constexpr std::uint16_t kBleIdleConnIntervalMin = 48;
constexpr std::uint16_t kBleIdleConnIntervalMax = 96;
constexpr std::uint16_t kBleIdleConnLatency = 4;
constexpr std::uint16_t kBleDeepIdleConnIntervalMin = 96;
constexpr std::uint16_t kBleDeepIdleConnIntervalMax = 160;
constexpr std::uint16_t kBleDeepIdleConnLatency = 8;
constexpr std::uint32_t kV2BringupLedPowerProbeMs = 220;
constexpr std::uint32_t kV2BringupLedSelfTestMs = 450;
constexpr std::uint32_t kSpeakerAssetsInputQuietMs = 30;
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
constexpr std::uint32_t kSpeakerAssetsRetryMs = 1000;
#endif
constexpr std::uint32_t kRetainedPowerCycleMagic = 0x50435943;
constexpr std::uint16_t kRetainedPowerCycleVersion = 1;

struct RetainedPowerCycle {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint32_t sequence;
  std::uint32_t idle_ms;
  std::uint32_t deep_idle_ms;
  std::uint8_t stage_flags;
  std::uint8_t wake_reason;
  std::uint16_t reserved_tail;
};

RTC_DATA_ATTR RetainedPowerCycle g_retained_power_cycle;

int encoder_step_count(int encoder_step) {
  const int magnitude = encoder_step < 0 ? -encoder_step : encoder_step;
  return magnitude == 0 ? 1 : magnitude;
}

using PowerMode = ai_keyboard::PowerPolicyMode;
using ChargeState = ai_keyboard::BatteryPowerState;

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
enum class SpeakerStartupPhase : std::uint8_t {
  StartLocal,
  ResolveBoot,
  BeginOutput,
  WaitPlayback,
  ShutdownOutput,
  ReleaseLease,
  WaitLeaseIdle,
  Ready,
};
#endif

struct AppContext {
  easy_codex::CodexSlotState codex_slots;
  ai_keyboard::ConfigState config_state;
  easy_input::GpioInputScanner inputs;
  easy_input::PeripheralPowerController peripheral_power;
  easy_input::StatusLedStrip leds;
  easy_input::NvsConfigStore config_store;
  easy_input::BatteryAdc battery;
  ai_keyboard::BatteryEstimator battery_estimator;
  easy_input::BleHidTransport ble;
  easy_input::UsbHidTransport usb;
  ai_keyboard::UsbPhysicalPresenceMonitor usb_physical_presence{
      kUsbDisconnectConfirmMs};
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  ai_keyboard::AudioIoArbiter audio_io_arbiter;
#endif
  easy_input::KeyboardAudioLink audio;
  bool audio_ready = false;
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  easy_input::SpeakerOutput speaker;
#endif
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  bool speaker_probe_pending = false;
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  easy_input::SpeakerAssetsSupervisor speaker_assets;
  easy_input::CodexLanPlayback codex_playback;
  easy_input::speaker_assets::SoundReadLease boot_sound_lease;
  easy_input::speaker_assets::SoundResolvedAsset boot_sound_asset;
  SpeakerStartupPhase speaker_startup_phase =
      SpeakerStartupPhase::StartLocal;
  bool speaker_boot_resolution_started = false;
  bool speaker_boot_skip_requested = false;
  bool speaker_factory_boot_sound = false;
  bool speaker_skip_boot_after_deep_sleep = false;
  bool speaker_wifi_unavailable_logged = false;
  std::uint32_t speaker_local_retry_after_ms = 0U;
  std::uint32_t speaker_wifi_retry_after_ms = 0U;
#endif
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  bool speaker_power_hold_active = false;
#endif
  ai_keyboard::EncoderScrollAxis encoder_scroll_axis = ai_keyboard::EncoderScrollAxis::Vertical;
  std::uint16_t battery_mv = 0;
  std::uint16_t battery_raw_mv = 0;
  std::uint8_t battery_percent = 0;
  std::uint32_t battery_sample_ms = 0;
  bool battery_sample_valid = false;
  std::uint32_t last_power_log_ms = 0;
  std::uint32_t last_heartbeat_log_ms = 0;
  std::uint32_t last_activity_ms = 0;
  bool last_usb_mounted = false;
  bool last_ble_connected = false;
  bool led_status_initialized = false;
  PowerMode logged_power_mode = PowerMode::Active;
  PowerMode tracked_power_mode = PowerMode::Active;
  std::uint32_t deep_idle_entries = 0;
  std::uint32_t deep_idle_total_ms = 0;
  std::uint32_t deep_idle_enter_ms = 0;
  std::uint32_t last_deep_idle_enter_ms = 0;
  std::uint32_t last_deep_idle_exit_ms = 0;
  const char* last_wake_reason = "boot";
  ai_keyboard::PowerCycleSnapshot latest_power_cycle;
  bool key_wake_verified = false;
  bool controlled_light_sleep_available = false;
  bool audio_power_hold_active = false;
  std::uint32_t light_sleep_entries = 0;
  std::uint32_t light_sleep_total_ms = 0;
  std::uint32_t last_light_sleep_ms = 0;
  std::uint32_t last_light_sleep_error_ms = 0;
  const char* last_light_sleep_wake = "boot";
  const char* light_sleep_block_reason = "boot";
  bool encoder_press_pending = false;
  bool encoder_press_config_triggered = false;
  std::uint32_t encoder_press_down_ms = 0;
  ai_keyboard::PlatformSelectionController platform_selection;
  std::uint32_t last_encoder_led_feedback_ms = 0;
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  std::uint8_t speaker_volume_level = ai_keyboard::kSpeakerVolumeDefault;
  bool speaker_volume_dirty = false;
  std::uint32_t speaker_volume_changed_ms = 0;
#endif
  std::uint32_t hid_event_sequence = 0;
  ai_keyboard::HeldKeyboardState held_keyboard;
  ai_keyboard::KeyboardSnapshotDelivery keyboard_delivery;
  ai_keyboard::KeyboardTransportLatch keyboard_transport;
  std::array<ai_keyboard::KeyboardTransportLatch,
             ai_keyboard::kKeyboardStateSourceCount>
      bridged_hotkey_transports;
  std::array<ai_keyboard::BridgedHotkeyDelivery,
             ai_keyboard::kKeyboardStateSourceCount>
      bridged_hotkey_deliveries;
  bool transport_usb_mounted = false;
  std::uint32_t usb_transport_epoch = 0;
  bool transport_ble_connected = false;
  std::uint32_t ble_transport_epoch = 0;
  bool codex_route_connected = false;
  std::uint32_t codex_route_generation = 0;
  bool input_led_feedback_pending = false;
  ai_keyboard::InputId pending_input_led = ai_keyboard::InputId::Key1;
  ai_keyboard::InputPhase pending_input_led_phase = ai_keyboard::InputPhase::Released;
  std::uint32_t pending_input_led_ms = 0;
  ai_keyboard::AgentStatusCommand last_agent_status_command{};
  bool last_agent_status_valid = false;
  std::uint32_t current_poll_interval_ms = kActivePollIntervalMs;
  std::string last_input = "none";
  std::uint32_t last_input_ms = 0;
  TaskHandle_t platform_task = nullptr;
  std::atomic<bool> remote_activity_pending{false};
  std::atomic<bool> status_refresh_pending{false};
};

void handle_input_event(const easy_input::InputEvent& event, void* context);

std::uint32_t millis() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

TickType_t delay_ticks(std::uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks == 0 ? 1 : ticks;
}

const char* input_name(ai_keyboard::InputId input) {
  switch (input) {
    case ai_keyboard::InputId::Key1:
      return "KEY1";
    case ai_keyboard::InputId::Key2:
      return "KEY2";
    case ai_keyboard::InputId::Key3:
      return "KEY3";
    case ai_keyboard::InputId::Key4:
      return "KEY4";
    case ai_keyboard::InputId::Key5:
      return "KEY5";
    case ai_keyboard::InputId::Key6:
      return "KEY6";
    case ai_keyboard::InputId::Key7:
      return "KEY7";
    case ai_keyboard::InputId::Key8:
      return "KEY8";
    case ai_keyboard::InputId::EncoderLeft:
      return "ENC_LEFT";
    case ai_keyboard::InputId::EncoderRight:
      return "ENC_RIGHT";
    case ai_keyboard::InputId::EncoderPress:
      return "ENC_PRESS";
    case ai_keyboard::InputId::Count:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* phase_name(ai_keyboard::InputPhase phase) {
  return phase == ai_keyboard::InputPhase::Pressed ? "pressed" : "released";
}

bool is_codex_slot_input(ai_keyboard::InputId input) {
  return input >= ai_keyboard::InputId::Key1 &&
         input <= ai_keyboard::InputId::Key8;
}

void handle_codex_slot_transition(AppContext* app,
                                  const easy_codex::DeviceTransition& transition) {
  for (std::size_t index = 0; index < transition.count; ++index) {
    const auto& action = transition.actions[index];
    switch (action.kind) {
      case easy_codex::DeviceActionKind::PttStarted:
      {
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
        app->codex_playback.preempt({});
#endif
        const auto session_id =
            easy_codex::encode_capture_session_identity({
                action.slot,
                action.capture_generation,
                action.connection_generation,
            });
        if (session_id == 0) {
          ESP_LOGE(kTag,
                   "codex ptt.start rejected invalid identity slot=%u capture_generation=%lu connection_generation=%lu",
                   static_cast<unsigned>(action.slot),
                   static_cast<unsigned long>(action.capture_generation),
                   static_cast<unsigned long>(action.connection_generation));
          break;
        }
        app->audio.prepare_for_audio_trigger();
        app->audio.start_stream("codex_slot", session_id);
        app->audio.request_heartbeat_refresh();
        ESP_LOGI(kTag,
                 "codex ptt.start slot=%u capture_generation=%lu connection_generation=%lu",
                 static_cast<unsigned>(action.slot),
                 static_cast<unsigned long>(action.capture_generation),
                 static_cast<unsigned long>(action.connection_generation));
        break;
      }
      case easy_codex::DeviceActionKind::PttEnded:
      {
        const auto session_id =
            easy_codex::encode_capture_session_identity({
                action.slot,
                action.capture_generation,
                action.connection_generation,
            });
        if (session_id != 0) {
          app->audio.stop_stream(session_id);
        }
        app->audio.request_heartbeat_refresh();
        ESP_LOGI(kTag,
                 "codex ptt.end slot=%u capture_generation=%lu connection_generation=%lu",
                 static_cast<unsigned>(action.slot),
                 static_cast<unsigned long>(action.capture_generation),
                 static_cast<unsigned long>(action.connection_generation));
        break;
      }
      case easy_codex::DeviceActionKind::PlayRequested:
        app->audio.request_heartbeat_refresh();
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
        if (!app->codex_playback.request(
                action.slot,
                action.request_generation,
                action.connection_generation)) {
          ESP_LOGW(kTag,
                   "codex play.request unavailable slot=%u request_generation=%lu",
                   static_cast<unsigned>(action.slot),
                   static_cast<unsigned long>(action.request_generation));
        }
#endif
        ESP_LOGI(kTag,
                 "codex play.request slot=%u request_generation=%lu connection_generation=%lu",
                 static_cast<unsigned>(action.slot),
                 static_cast<unsigned long>(action.request_generation),
                 static_cast<unsigned long>(action.connection_generation));
        break;
      case easy_codex::DeviceActionKind::PlaybackPreempted:
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
        app->codex_playback.preempt(action.playback);
#endif
        ESP_LOGI(kTag,
                 "codex play.preempted slot=%u summary_generation=%llu lease=%llu",
                 static_cast<unsigned>(action.slot),
                 static_cast<unsigned long long>(
                     action.playback.summary_generation),
                 static_cast<unsigned long long>(action.playback.lease));
        break;
      case easy_codex::DeviceActionKind::RejectedBusy:
        ESP_LOGW(kTag,
                 "codex input rejected while busy slot=%u",
                 static_cast<unsigned>(action.slot));
        break;
      case easy_codex::DeviceActionKind::PlaybackFinished:
      case easy_codex::DeviceActionKind::None:
        break;
    }
  }
}

const char* ptt_mode_name(ai_keyboard::PttMode mode) {
  switch (mode) {
    case ai_keyboard::PttMode::Hold:
      return "hold";
    case ai_keyboard::PttMode::Toggle:
      return "toggle";
  }
  return "toggle";
}

int input_gpio(ai_keyboard::InputId input) {
  switch (input) {
    case ai_keyboard::InputId::Key1:
    case ai_keyboard::InputId::Key2:
    case ai_keyboard::InputId::Key3:
    case ai_keyboard::InputId::Key4:
    case ai_keyboard::InputId::Key5:
    case ai_keyboard::InputId::Key6:
    case ai_keyboard::InputId::Key7:
    case ai_keyboard::InputId::Key8: {
      const auto index = static_cast<std::size_t>(input);
      return index < ai_keyboard::kKeyPins.size()
                 ? static_cast<int>(ai_keyboard::kKeyPins[index].gpio)
                 : -1;
    }
    case ai_keyboard::InputId::EncoderLeft:
      return static_cast<int>(ai_keyboard::kEncoderPinA);
    case ai_keyboard::InputId::EncoderRight:
      return static_cast<int>(ai_keyboard::kEncoderPinB);
    case ai_keyboard::InputId::EncoderPress:
      return static_cast<int>(ai_keyboard::kEncoderPressPin);
    case ai_keyboard::InputId::Count:
      return -1;
  }
  return -1;
}

const char* parse_status_name(ai_keyboard::ConfigParseStatus status) {
  switch (status) {
    case ai_keyboard::ConfigParseStatus::Ok:
      return "ok";
    case ai_keyboard::ConfigParseStatus::InvalidJson:
      return "invalid_json";
    case ai_keyboard::ConfigParseStatus::InvalidSchema:
      return "invalid_schema";
    case ai_keyboard::ConfigParseStatus::UnsupportedAudio:
      return "unsupported_audio";
    case ai_keyboard::ConfigParseStatus::MissingProfile:
      return "missing_profile";
    case ai_keyboard::ConfigParseStatus::MissingBinding:
      return "missing_binding";
    case ai_keyboard::ConfigParseStatus::UnknownAction:
      return "unknown_action";
    case ai_keyboard::ConfigParseStatus::FixedTextTooLarge:
      return "fixed_text_too_large";
  }
  return "unknown";
}

void configure_power_management() {
#if CONFIG_PM_ENABLE
  esp_pm_config_t config = {};
  config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  config.min_freq_mhz = CONFIG_XTAL_FREQ;
  config.light_sleep_enable = false;

  const esp_err_t err = esp_pm_configure(&config);
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "power management enabled max=%dMHz min=%dMHz light_sleep=%s",
             config.max_freq_mhz,
             config.min_freq_mhz,
             config.light_sleep_enable ? "true" : "false");
  } else {
    ESP_LOGW(kTag, "power management unavailable: %s", esp_err_to_name(err));
  }
#else
  ESP_LOGW(kTag, "power management disabled in sdkconfig");
#endif
}

void finish_deep_idle(AppContext* app, std::uint32_t now_ms, const char* reason) {
  if (app->tracked_power_mode != PowerMode::DeepIdle || app->deep_idle_enter_ms == 0) {
    return;
  }
  app->deep_idle_total_ms += now_ms - app->deep_idle_enter_ms;
  app->last_deep_idle_exit_ms = now_ms;
  app->deep_idle_enter_ms = 0;
  app->last_wake_reason = reason == nullptr ? "unknown" : reason;
}

std::uint32_t next_power_cycle_sequence(const AppContext* app) {
  const auto next = app->latest_power_cycle.sequence + 1;
  return next == 0 ? 1 : next;
}

void record_completed_power_cycle(AppContext* app,
                                  std::uint32_t now_ms,
                                  const char* reason,
                                  bool reached_deep_sleep = false) {
  const auto snapshot = ai_keyboard::build_power_cycle_snapshot(
      next_power_cycle_sequence(app),
      now_ms - app->last_activity_ms,
      ai_keyboard::power_cycle_wake_reason(reason),
      reached_deep_sleep);
  if (!snapshot.valid()) {
    return;
  }
  app->latest_power_cycle = snapshot;
  ESP_LOGI(kTag,
           "power cycle seq=%lu flags=%u idle_ms=%lu deep_ms=%lu wake=%s",
           static_cast<unsigned long>(snapshot.sequence),
           static_cast<unsigned>(snapshot.stage_flags),
           static_cast<unsigned long>(snapshot.idle_ms),
           static_cast<unsigned long>(snapshot.deep_idle_ms),
           ai_keyboard::power_cycle_wake_reason_name(snapshot.wake_reason));
}

void retain_power_cycle_for_deep_sleep(const ai_keyboard::PowerCycleSnapshot& snapshot) {
  g_retained_power_cycle = {
      kRetainedPowerCycleMagic,
      kRetainedPowerCycleVersion,
      0,
      snapshot.sequence,
      snapshot.idle_ms,
      snapshot.deep_idle_ms,
      snapshot.stage_flags,
      static_cast<std::uint8_t>(snapshot.wake_reason),
      0,
  };
}

void restore_retained_power_cycle(AppContext* app, esp_sleep_wakeup_cause_t wake_cause) {
  const bool retained = g_retained_power_cycle.magic == kRetainedPowerCycleMagic &&
                        g_retained_power_cycle.version == kRetainedPowerCycleVersion &&
                        g_retained_power_cycle.sequence != 0;
  if (!retained || wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    g_retained_power_cycle.magic = 0;
    return;
  }

  auto wake_reason = ai_keyboard::PowerCycleWakeReason::Other;
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    wake_reason = ai_keyboard::PowerCycleWakeReason::DeepSleepKey;
  } else if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
    wake_reason = ai_keyboard::PowerCycleWakeReason::Timer;
  }
  app->latest_power_cycle = {
      g_retained_power_cycle.sequence,
      g_retained_power_cycle.idle_ms,
      g_retained_power_cycle.deep_idle_ms,
      g_retained_power_cycle.stage_flags,
      wake_reason,
  };
  g_retained_power_cycle.magic = 0;
}

void track_power_mode(AppContext* app, PowerMode mode, std::uint32_t now_ms) {
  if (mode == app->tracked_power_mode) {
    return;
  }

  if (app->tracked_power_mode == PowerMode::DeepIdle) {
    finish_deep_idle(app, now_ms, "mode");
  }

  if (mode == PowerMode::DeepIdle) {
    ++app->deep_idle_entries;
    app->deep_idle_enter_ms = now_ms;
    app->last_deep_idle_enter_ms = now_ms;
  }
  app->tracked_power_mode = mode;
}

std::uint32_t deep_idle_total_ms(const AppContext* app, std::uint32_t now_ms) {
  auto total = app->deep_idle_total_ms;
  if (app->tracked_power_mode == PowerMode::DeepIdle && app->deep_idle_enter_ms != 0) {
    total += now_ms - app->deep_idle_enter_ms;
  }
  return total;
}

void mark_activity(AppContext* app, std::uint32_t now_ms, const char* reason = "input") {
  record_completed_power_cycle(app, now_ms, reason);
  finish_deep_idle(app, now_ms, reason);
  app->tracked_power_mode = PowerMode::Active;
  app->last_activity_ms = now_ms;
  app->audio.cancel_wifi_release_for_device_activity();
  if (app->logged_power_mode != PowerMode::Active) {
    ESP_LOGI(kTag,
             "power active wake previous_mode=%s",
             app->logged_power_mode == PowerMode::DeepIdle ? "deep_idle" : "idle");
    app->logged_power_mode = PowerMode::Active;
  }
}

void signal_remote_activity(void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app == nullptr) {
    return;
  }
  app->remote_activity_pending.store(true, std::memory_order_release);
  if (app->platform_task != nullptr) {
    xTaskNotifyGive(app->platform_task);
  }
}

void signal_status_read(void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app == nullptr) {
    return;
  }
  app->status_refresh_pending.store(true, std::memory_order_release);
  if (app->platform_task != nullptr) {
    xTaskNotifyGive(app->platform_task);
  }
}

void signal_usb_status_request(void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app != nullptr && app->platform_task != nullptr) {
    xTaskNotifyGive(app->platform_task);
  }
}

void consume_remote_activity(AppContext* app, std::uint32_t now_ms) {
  if (app->remote_activity_pending.exchange(false, std::memory_order_acq_rel)) {
    mark_activity(app, now_ms, "wifi_audio_control");
  }
}

bool power_mode_is_idle(PowerMode mode) {
  return mode != PowerMode::Active;
}

const char* power_mode_name(PowerMode mode) {
  return ai_keyboard::power_policy_mode_name(mode);
}

bool external_power_status_active(const AppContext* app);
bool usb_vbus_status_present();

ai_keyboard::PowerPolicyInputs base_power_policy_inputs(const AppContext* app,
                                                        std::uint32_t now_ms) {
  ai_keyboard::PowerPolicyInputs inputs;
  inputs.now_ms = now_ms;
  inputs.last_activity_ms = app->last_activity_ms;
  inputs.external_power = external_power_status_active(app);
  const bool usb_disconnect_sample_pending =
      app->usb_physical_presence.disconnect_pending() ||
      (app->usb_physical_presence.present() && !inputs.external_power);
  inputs.input_active =
      app->inputs.any_input_active() || app->keyboard_delivery.pending() ||
      usb_disconnect_sample_pending;
  inputs.ble_connected = app->ble.connected();
  inputs.usb_mounted = app->usb.mounted();
  inputs.key_wake_verified = app->key_wake_verified;
  return inputs;
}

PowerMode power_mode_for(const AppContext* app, std::uint32_t now_ms) {
  return ai_keyboard::evaluate_power_policy(base_power_policy_inputs(app, now_ms)).mode;
}

ai_keyboard::PowerPolicyInputs power_policy_inputs(AppContext* app,
                                                   std::uint32_t now_ms);

void sync_ble_connection_power_profile(AppContext* app, PowerMode mode) {
  const bool input_delivery_active =
      app->inputs.any_input_active() || app->keyboard_delivery.pending() ||
      app->ble.input_delivery_pending();
  auto profile = easy_input::BleHidTransport::ConnectionPowerProfile::Active;
  if (app->ble.connected() && !external_power_status_active(app) &&
      !input_delivery_active) {
    if (mode == PowerMode::DeepIdle) {
      profile = easy_input::BleHidTransport::ConnectionPowerProfile::DeepIdle;
    } else if (mode == PowerMode::Idle) {
      profile = easy_input::BleHidTransport::ConnectionPowerProfile::Idle;
    }
  }
  app->ble.set_connection_power_profile(profile);
}

bool external_power_status_active(const AppContext* app) {
  (void)app;
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    return usb_vbus_status_present();
  }

  if constexpr (ai_keyboard::kChargeStatusPin >= 0) {
    return gpio_get_level(static_cast<gpio_num_t>(ai_keyboard::kChargeStatusPin)) ==
           ai_keyboard::kChargeStatusChargingLevel;
  }

  return false;
}

bool usb_vbus_status_present() {
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    return gpio_get_level(
               static_cast<gpio_num_t>(
                   ai_keyboard::kExternalPowerSensePin)) ==
           ai_keyboard::kExternalPowerSenseActiveLevel;
  }
  return false;
}

void sync_usb_physical_presence(AppContext* app, std::uint32_t now_ms) {
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    if (!app->usb_physical_presence.update(
            usb_vbus_status_present(), now_ms)) {
      return;
    }
    const bool present = app->usb_physical_presence.present();
    app->usb.observe_physical_presence(present);
    ESP_LOGI(kTag,
             "USB physical VBUS stable present=%u",
             present ? 1U : 0U);
  }
}

const char* charge_state_name(ChargeState state) {
  return ai_keyboard::battery_power_state_name(state);
}

ChargeState charge_state_for(const AppContext* app) {
  if (!external_power_status_active(app)) {
    return ChargeState::Battery;
  }

  if constexpr (ai_keyboard::kChargeStatusPin >= 0) {
    const int chrg_level =
        gpio_get_level(static_cast<gpio_num_t>(ai_keyboard::kChargeStatusPin));
    return ai_keyboard::battery_power_state_from_signals(
        true, chrg_level, ai_keyboard::kChargeStatusChargingLevel);
  }

  return ChargeState::UsbUnknown;
}

void configure_board_status_inputs() {
  std::uint64_t mask = 0;
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    mask |= 1ULL << ai_keyboard::kExternalPowerSensePin;
  }
  if constexpr (ai_keyboard::kChargeStatusPin >= 0) {
    mask |= 1ULL << ai_keyboard::kChargeStatusPin;
  }
  if constexpr (ai_keyboard::kKeyWakePin >= 0) {
    mask |= 1ULL << ai_keyboard::kKeyWakePin;
  }
  if (mask == 0) {
    return;
  }

  gpio_config_t config = {};
  config.pin_bit_mask = mask;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "board status input config failed: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(kTag,
           "configured board status inputs pullup=on external_power=GPIO%d charge=GPIO%d key_wake=GPIO%d",
           static_cast<int>(ai_keyboard::kExternalPowerSensePin),
           static_cast<int>(ai_keyboard::kChargeStatusPin),
           static_cast<int>(ai_keyboard::kKeyWakePin));
}

bool configure_light_sleep_wakeup() {
  if constexpr (ai_keyboard::kKeyWakePin < 0) {
    ESP_LOGI(kTag, "controlled light sleep disabled: board has no KEY_WAKE");
    return false;
  } else {
    const std::uint64_t wake_mask = 1ULL << ai_keyboard::kKeyWakePin;
    const esp_err_t err =
        esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
      ESP_LOGW(kTag,
               "controlled light sleep wake config failed key_wake=GPIO%d: %s",
               static_cast<int>(ai_keyboard::kKeyWakePin),
               esp_err_to_name(err));
      return false;
    }
    // 自动浅睡的按键唤醒同样由上面的 EXT1(ANY_LOW)承担——EXT1 对
    // light/deep sleep 都有效。不要用 gpio_wakeup_enable:它会把
    // gpio_keys 配好的 KEY_WAKE 边沿中断覆盖成电平触发,按键期间
    // 中断风暴饿死主循环(0.4.3 按键全失效的事故根因)。
    ESP_LOGI(kTag,
             "controlled light sleep wake configured key_wake=GPIO%d nap_ms=%lu",
             static_cast<int>(ai_keyboard::kKeyWakePin),
             static_cast<unsigned long>(kControlledLightSleepNapMs));
    return true;
  }
}

int read_optional_gpio(std::int8_t gpio) {
  if (gpio < 0) {
    return -1;
  }
  return gpio_get_level(static_cast<gpio_num_t>(gpio));
}

const char* light_sleep_wakeup_cause_name(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "key_wake";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "gpio";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "undefined";
    default:
      return "other";
  }
}

bool key_wake_line_asserted() {
  if constexpr (ai_keyboard::kKeyWakePin < 0) {
    return false;
  } else {
    return read_optional_gpio(ai_keyboard::kKeyWakePin) == 0;
  }
}

int read_gpio(std::uint8_t gpio) {
  return gpio_get_level(static_cast<gpio_num_t>(gpio));
}

std::string raw_key_levels() {
  std::string levels;
  levels.reserve(ai_keyboard::kKeyPins.size());
  for (const auto& key : ai_keyboard::kKeyPins) {
    const int level = read_gpio(key.gpio);
    levels.push_back(level == 0 ? '0' : '1');
  }
  return levels;
}

std::string raw_encoder_levels() {
  std::string levels;
  levels.reserve(3);
  levels.push_back(read_gpio(ai_keyboard::kEncoderPinA) == 0 ? '0' : '1');
  levels.push_back(read_gpio(ai_keyboard::kEncoderPinB) == 0 ? '0' : '1');
  levels.push_back(read_gpio(ai_keyboard::kEncoderPressPin) == 0 ? '0' : '1');
  return levels;
}

std::string pinmap_summary() {
  std::string summary;
  summary.reserve(96);
  for (std::size_t index = 0; index < ai_keyboard::kKeyPins.size(); ++index) {
    if (!summary.empty()) {
      summary += ",";
    }
    summary += "K" + std::to_string(index + 1) + "=" +
               std::to_string(ai_keyboard::kKeyPins[index].gpio);
  }
  summary += ",E=" + std::to_string(ai_keyboard::kEncoderPinA) + "/" +
             std::to_string(ai_keyboard::kEncoderPinB) + "/" +
             std::to_string(ai_keyboard::kEncoderPressPin);
  summary += ",W=" + std::to_string(static_cast<int>(ai_keyboard::kKeyWakePin));
  summary += ",P=" + std::to_string(static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin));
  summary += ",L=" + std::to_string(ai_keyboard::kWs2812Pin);
  summary += ",MIC=" + std::to_string(static_cast<int>(ai_keyboard::kMicI2sBclkPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kMicI2sWsPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kMicI2sDataInPin));
  summary += ",SPK=" + std::to_string(static_cast<int>(ai_keyboard::kSpkI2sBclkPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kSpkI2sWsPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kSpkI2sDataOutPin));
  return summary;
}

std::string raw_diagnostic_gpio_levels() {
  std::string levels;
  levels.reserve(64);
  for (const auto& key : ai_keyboard::kKeyPins) {
    if (!levels.empty()) {
      levels += ",";
    }
    const auto gpio = key.gpio;
    levels += std::to_string(gpio) + ":" +
              std::to_string(gpio_get_level(static_cast<gpio_num_t>(gpio)));
  }
  return levels;
}

ai_keyboard::BoardDiagnosticsSnapshot board_diagnostics(const AppContext* app,
                                                         std::uint32_t now_ms) {
  const auto last_input_age =
      app->last_input_ms == 0 ? 0 : static_cast<std::uint32_t>(now_ms - app->last_input_ms);
  const auto input = app->inputs.diagnostics();
  return {
      ai_keyboard::kBoardName,
      raw_key_levels(),
      raw_encoder_levels(),
      pinmap_summary(),
      raw_diagnostic_gpio_levels(),
      app->last_input,
      last_input_age,
      read_optional_gpio(ai_keyboard::kKeyWakePin),
      read_optional_gpio(ai_keyboard::kExternalPowerSensePin),
      read_optional_gpio(ai_keyboard::kChargeStatusPin),
      static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
      read_optional_gpio(ai_keyboard::kPeripheralPowerEnablePin),
      static_cast<int>(ai_keyboard::kPeripheralPowerEnableActiveLevel),
      static_cast<int>(ai_keyboard::kWs2812Pin),
      input.raw_edges,
      input.edge_queue_drops,
      input.emitted_events,
      input.filtered_transitions,
      input.encoder_edges,
      input.encoder_steps,
      input.encoder_invalid_transitions,
      input.encoder_partial_resets,
      input.encoder_queue_drops,
  };
}

ai_keyboard::PowerDiagnosticsSnapshot power_diagnostics(AppContext* app,
                                                         std::uint32_t now_ms) {
  const auto decision = ai_keyboard::evaluate_power_policy(power_policy_inputs(app, now_ms));
  return {
      power_mode_name(decision.mode),
      decision.poll_ms,
      now_ms - app->last_activity_ms,
      app->deep_idle_entries,
      deep_idle_total_ms(app, now_ms),
      app->last_deep_idle_enter_ms,
      app->last_deep_idle_exit_ms,
      app->last_wake_reason,
      app->inputs.wake_edge_count(),
      app->usb.mounted(),
      app->latest_power_cycle.sequence,
      app->latest_power_cycle.idle_ms,
      app->latest_power_cycle.deep_idle_ms,
      app->latest_power_cycle.stage_flags,
      ai_keyboard::power_cycle_wake_reason_name(app->latest_power_cycle.wake_reason),
  };
}

std::uint16_t config_json_crc16(const std::string& json) {
  return ai_keyboard::crc16_ccitt(reinterpret_cast<const std::uint8_t*>(json.data()),
                                  json.size());
}

std::string publish_config_status(AppContext* app,
                                  const char* phase,
                                  const char* status,
                                  std::size_t bytes,
                                  std::uint16_t crc16,
                                  bool saved,
                                  bool force_confirmation_view = false,
                                  const ai_keyboard::SpeakerProbeSnapshot*
                                      speaker_probe = nullptr) {
  const bool diagnostic_status = phase != nullptr && std::string_view(phase) == "diag";
  const bool battery_status = phase != nullptr && std::string_view(phase) == "battery";
  const bool speaker_status =
      phase != nullptr && std::string_view(phase) == "spk_probe";
  const bool config_confirmation = force_confirmation_view ||
      (phase != nullptr &&
       (std::string_view(phase) == "push" || std::string_view(phase) == "platform"));
  const auto now_ms = millis();
  ai_keyboard::AudioStatusSnapshot audio_status;
  if (!diagnostic_status && !battery_status && !speaker_status &&
      !config_confirmation) {
    const auto audio_diagnostics = app->audio.diagnostics();
    audio_status = {
        app->config_state.audio_enabled(),
        ai_keyboard::kAudioTransport,
        "keyboard",
        ai_keyboard::kAudioTransport,
        app->audio.capture_status(),
        app->config_state.audio_host(),
        app->config_state.audio_port(),
        ai_keyboard::kAudioMicrophoneChannel,
        ai_keyboard::kAudioSpeakerChannel,
        audio_diagnostics.sent_packets,
        audio_diagnostics.sent_bytes,
        audio_diagnostics.last_rms_milli,
        audio_diagnostics.peak_rms_milli,
        audio_diagnostics.send_errors,
        audio_diagnostics.read_errors,
        audio_diagnostics.recovery_count,
        audio_diagnostics.session_generation,
        audio_diagnostics.session_id,
        audio_diagnostics.stream_phase,
        audio_diagnostics.stop_reason,
        audio_diagnostics.control_state,
        audio_diagnostics.last_error,
        audio_diagnostics.stream_host,
        audio_diagnostics.stream_port,
    };
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  // Product playback already records a fixed, allocation-free boot probe.
  // Include its compact terminal core in ordinary battery/GATT refreshes so a
  // silent boot can be diagnosed later without serial or diagnostic firmware.
  const auto product_speaker_probe = app->speaker.probe_snapshot();
  if (speaker_probe == nullptr && product_speaker_probe.present) {
    speaker_probe = &product_speaker_probe;
  }
#endif
  ai_keyboard::ConfigStatusSnapshot snapshot{
      kFirmwareVersion,
      phase == nullptr ? "" : phase,
      status == nullptr ? "unknown" : status,
      bytes,
      crc16,
      app->config_state.ptt_hotkey(),
      app->config_state.edit_ptt_hotkey(),
      ptt_mode_name(app->config_state.ptt_mode()),
      saved,
      app->battery_mv,
      app->battery_percent,
      audio_status,
      diagnostic_status || battery_status ? power_diagnostics(app, now_ms)
                                          : ai_keyboard::PowerDiagnosticsSnapshot{},
      diagnostic_status ? board_diagnostics(app, now_ms)
                        : ai_keyboard::BoardDiagnosticsSnapshot{},
      battery_status && app->battery_sample_valid
          ? ai_keyboard::BatteryStatusSnapshot{
                app->battery_raw_mv,
                charge_state_name(charge_state_for(app)),
                now_ms - app->battery_sample_ms,
                app->battery_estimator.full_anchor_mv() > 0,
            }
          : ai_keyboard::BatteryStatusSnapshot{},
      ai_keyboard::host_platform_name(app->config_state.target_platform()),
      true,
      true,
      true,
  };
  snapshot.speaker = speaker_probe;
  auto status_json = config_confirmation
      ? ai_keyboard::build_config_confirmation_status_json(snapshot)
      : ai_keyboard::build_config_status_json(snapshot);
  if (status_json.size() > ai_keyboard::kConfigStatusGattSafeLen) {
    ESP_LOGE(kTag,
             "CONFIG status builder exceeded GATT budget phase=%s bytes=%u; using core confirmation view",
             phase == nullptr ? "" : phase,
             static_cast<unsigned>(status_json.size()));
    status_json = ai_keyboard::build_config_confirmation_status_json(snapshot);
  }
  if (status_json.size() > ai_keyboard::kConfigStatusGattSafeLen) {
    ESP_LOGE(kTag,
             "CONFIG status suppressed after bounded fallback phase=%s bytes=%u",
             phase == nullptr ? "" : phase,
             static_cast<unsigned>(status_json.size()));
    return {};
  }
  app->ble.publish_status_json(status_json);
  return status_json;
}

void publish_config_status_for_json(AppContext* app,
                                    const char* phase,
                                    const char* status,
                                    const std::string& json,
                                    bool saved) {
  publish_config_status(app, phase, status, json.size(), config_json_crc16(json), saved);
}

void publish_config_status_without_payload(AppContext* app,
                                           const char* phase,
                                           const char* status,
                                           bool saved) {
  publish_config_status(app, phase, status, 0, 0, saved);
}

bool decode_speaker_sync_key(
    const std::string& encoded,
    std::array<std::uint8_t, 32>* decoded) {
  if (decoded == nullptr || encoded.size() != 64U) {
    return false;
  }
  auto hex_nibble = [](char value, std::uint8_t* nibble) {
    if (nibble == nullptr) {
      return false;
    }
    if (value >= '0' && value <= '9') {
      *nibble = static_cast<std::uint8_t>(value - '0');
      return true;
    }
    if (value >= 'a' && value <= 'f') {
      *nibble = static_cast<std::uint8_t>(
          value - 'a' + 10);
      return true;
    }
    if (value >= 'A' && value <= 'F') {
      *nibble = static_cast<std::uint8_t>(
          value - 'A' + 10);
      return true;
    }
    return false;
  };
  decoded->fill(0U);
  for (std::size_t index = 0U; index < decoded->size();
       ++index) {
    std::uint8_t high = 0U;
    std::uint8_t low = 0U;
    if (!hex_nibble(encoded[index * 2U], &high) ||
        !hex_nibble(encoded[index * 2U + 1U], &low)) {
      decoded->fill(0U);
      return false;
    }
    (*decoded)[index] = static_cast<std::uint8_t>(
        (high << 4U) | low);
  }
  return std::any_of(
      decoded->begin(),
      decoded->end(),
      [](std::uint8_t value) { return value != 0U; });
}

void sync_keyboard_audio_config(AppContext* app, const char* reason) {
  easy_input::KeyboardAudioConfig config;
  const auto port = app->config_state.audio_port();
  // 麦克风来源是 App 的本地会话策略。固件只根据已配置的 Wi-Fi 音频
  // 端点决定硬件能力，避免旧 audio_enabled 状态把控制面永久关闭。
  config.enabled = app->config_state.audio_enabled();
  config.wifi_ssid = app->config_state.wifi_ssid();
  config.wifi_password = app->config_state.wifi_password();
  config.host = app->config_state.audio_host();
  config.port = static_cast<std::uint16_t>(std::clamp(port, 1, 65535));
  config.speaker_sync_key_epoch =
      app->config_state.speaker_sync_key_epoch();
  config.speaker_sync_key_valid =
      config.speaker_sync_key_epoch != 0U &&
      decode_speaker_sync_key(
          app->config_state.speaker_sync_key(),
          &config.speaker_sync_key);
  app->audio.configure(config);
  ESP_LOGI(kTag,
           "audio sync reason=%s enabled=%d transport=%s mic=keyboard host=%s port=%u capture=%s",
           reason == nullptr ? "" : reason,
           config.enabled ? 1 : 0,
           ai_keyboard::kAudioTransport,
           config.host.empty() ? "(empty)" : config.host.c_str(),
           static_cast<unsigned>(config.port),
           app->audio.capture_status().c_str());
}

enum class PttAudioTrigger {
  None,
  Voice,
  Edit,
};

bool hotkey_matches_configured_ptt(const std::string& configured_hotkey,
                                   const std::string& action_hotkey) {
  return !configured_hotkey.empty() && configured_hotkey == action_hotkey;
}

PttAudioTrigger ptt_audio_trigger_for_action(const AppContext* app,
                                             const ai_keyboard::Action& action) {
  if (action.kind == ai_keyboard::ActionKind::VoicePttHold) {
    return PttAudioTrigger::Voice;
  }
  if (action.kind == ai_keyboard::ActionKind::EditPttHold) {
    return PttAudioTrigger::Edit;
  }

  if (app != nullptr && action.kind == ai_keyboard::ActionKind::Hotkey) {
    if (hotkey_matches_configured_ptt(app->config_state.ptt_hotkey(), action.hotkey)) {
      return PttAudioTrigger::Voice;
    }
    if (hotkey_matches_configured_ptt(app->config_state.edit_ptt_hotkey(), action.hotkey)) {
      return PttAudioTrigger::Edit;
    }
  }

  return PttAudioTrigger::None;
}

void handle_ptt_keyboard_audio(AppContext* app,
                               const ai_keyboard::Action& action,
                               ai_keyboard::InputPhase phase,
                               const char* source) {
  const auto trigger = ptt_audio_trigger_for_action(app, action);
  if (trigger == PttAudioTrigger::None) {
    return;
  }

  ESP_LOGD(kTag,
           "audio ptt event forwarded source=%s phase=%s trigger=%s capture=%s",
           source == nullptr ? "" : source,
           phase_name(phase),
           trigger == PttAudioTrigger::Edit ? "edit" : "voice",
           app->audio.capture_status().c_str());
  if (phase == ai_keyboard::InputPhase::Pressed) {
    app->audio.prepare_for_audio_trigger();
  }
}

const char* keyboard_transport_name(ai_keyboard::KeyboardTransportOwner owner) {
  switch (owner) {
    case ai_keyboard::KeyboardTransportOwner::None:
      return "none";
    case ai_keyboard::KeyboardTransportOwner::Usb:
      return "usb";
    case ai_keyboard::KeyboardTransportOwner::Ble:
      return "ble";
    case ai_keyboard::KeyboardTransportOwner::Suppressed:
      return "suppressed";
  }
  return "unknown";
}

void reconcile_keyboard_transport_lifetimes(AppContext* app) {
  app->ble.refresh_connection_identity();

  // TinyUSB callbacks own the USB lifetime generation. Reading the generation
  // here detects an unmount/remount pair even when both callbacks happened
  // between two main-loop iterations.
  const auto usb_epoch = app->usb.connection_epoch();
  app->transport_usb_mounted = usb_epoch != 0;
  app->usb_transport_epoch = usb_epoch;

  const auto ble_epoch = app->ble.connection_epoch();
  app->transport_ble_connected =
      app->ble.connected() && ble_epoch != 0;
  app->ble_transport_epoch =
      app->transport_ble_connected ? ble_epoch : 0;

  const bool keyboard_invalidated =
      app->keyboard_transport.observe_transport_state(
          app->transport_usb_mounted,
          app->usb_transport_epoch,
          app->transport_ble_connected,
          app->ble_transport_epoch);
  if (keyboard_invalidated) {
    const auto held = app->held_keyboard.current();
    // The old endpoint can no longer consume a pending transition. Establish
    // the current physical state as the delivery baseline so no stale key-down
    // is replayed into the fresh endpoint. If everything is already released,
    // the next chord may select the fresh endpoint immediately.
    app->keyboard_delivery.reset(held);
    if (held.empty()) {
      app->keyboard_transport.commit_snapshot(true);
    }
    ESP_LOGW(kTag,
             "keyboard transport lifetime changed; held=%u next chord gated=%u",
             static_cast<unsigned>(app->held_keyboard.active_source_count()),
             held.empty() ? 0U : 1U);
  }

  for (std::size_t index = 0;
       index < app->bridged_hotkey_transports.size();
       ++index) {
    auto& transport = app->bridged_hotkey_transports[index];
    const bool invalidated = transport.observe_transport_state(
        app->transport_usb_mounted,
        app->usb_transport_epoch,
        app->transport_ble_connected,
        app->ble_transport_epoch);
    if (!invalidated) {
      continue;
    }
    auto& delivery = app->bridged_hotkey_deliveries[index];
    delivery.reset_to_desired();
    if (!delivery.desired_pressed()) {
      transport.commit_snapshot(true);
    }
  }
}

void reconcile_codex_route_lifetime(AppContext* app) {
  const auto snapshot = app->audio.wifi_service_snapshot();
  const bool connected = snapshot.configured && snapshot.connected &&
                         !snapshot.disconnect_pending &&
                         snapshot.host_ipv4_valid && snapshot.generation != 0;
  const auto generation = connected ? snapshot.generation : 0U;
  if (connected == app->codex_route_connected &&
      generation == app->codex_route_generation) {
    return;
  }

  if (app->codex_route_generation != 0) {
    handle_codex_slot_transition(
        app, app->codex_slots.disconnect(app->codex_route_generation));
  }
  app->codex_route_connected = connected;
  app->codex_route_generation = generation;
  ESP_LOGI(kTag,
           "codex route connected=%u generation=%lu configured=%u pending=%u",
           connected ? 1U : 0U,
           static_cast<unsigned long>(generation),
           snapshot.configured ? 1U : 0U,
           snapshot.disconnect_pending ? 1U : 0U);
}

bool send_keyboard_snapshot(AppContext* app,
                            const ai_keyboard::HidKeyboardSnapshot& snapshot,
                            ai_keyboard::HidReportClass report_class) {
  const auto owner = app->keyboard_transport.select_for_snapshot(
      snapshot.empty(),
      app->transport_usb_mounted,
      app->usb_transport_epoch,
      app->transport_ble_connected,
      app->ble_transport_epoch);
  bool accepted = false;
  switch (owner) {
    case ai_keyboard::KeyboardTransportOwner::Usb: {
      accepted = app->usb.queue_keyboard_report_for_epoch(
          snapshot.modifier,
          snapshot.keycodes,
          snapshot.apple_fn,
          report_class,
          app->keyboard_transport.owner_epoch());
      app->usb.poll_pending_reports();
      break;
    }
    case ai_keyboard::KeyboardTransportOwner::Ble: {
      const auto expected_owner = app->ble.connection_identity();
      if (!expected_owner.valid() ||
          expected_owner.generation != app->keyboard_transport.owner_epoch()) {
        return false;
      }
      accepted = app->ble.send_keyboard_report_for_owner(
          snapshot.modifier,
          snapshot.keycodes,
          snapshot.apple_fn,
          report_class,
          expected_owner);
      break;
    }
    case ai_keyboard::KeyboardTransportOwner::None:
    case ai_keyboard::KeyboardTransportOwner::Suppressed:
      // A chord that started without a host, or whose owner disconnected, is
      // deliberately suppressed until physical release. Treat suppression as
      // handled so it cannot block a later chord.
      accepted = true;
      break;
  }
  return accepted;
}

bool flush_pending_keyboard_snapshot(AppContext* app) {
  // Sample endpoint lifetimes before taking a pending snapshot. Reconciliation
  // may deliberately cancel a state queued for a disconnected host.
  reconcile_keyboard_transport_lifetimes(app);
  const auto pending = app->keyboard_delivery.pending_snapshot();
  if (!pending.valid()) {
    // select_for_snapshot() may have provisionally latched an endpoint before
    // a queue accepted the first down edge. If that edge and its matching up
    // coalesced under bounded overload, release the empty chord latch here so
    // the next physical press can select a live endpoint.
    if (app->keyboard_delivery.desired().empty()) {
      app->keyboard_transport.commit_snapshot(true);
    }
    return true;
  }
  if (!send_keyboard_snapshot(app, pending.snapshot, pending.report_class)) {
    return false;
  }
  if (!app->keyboard_delivery.mark_accepted(pending.generation)) {
    ESP_LOGE(kTag,
             "HID snapshot acceptance raced with desired state generation=%lu",
             static_cast<unsigned long>(pending.generation));
    return false;
  }
  app->keyboard_transport.commit_snapshot(pending.snapshot.empty());
  return true;
}

bool dispatch_held_keyboard_event(AppContext* app,
                                  ai_keyboard::InputId source,
                                  const ai_keyboard::FirmwareEvent& event) {
  ai_keyboard::HeldKeyboardUpdate update;
  if (event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown) {
    const auto report = ai_keyboard::hid_report_for_hotkey(event.value);
    update = app->held_keyboard.press(source, report);
  } else {
    update = app->held_keyboard.release(source);
  }

  if (!update.accepted()) {
    ESP_LOGW(kTag,
             "HID state rejected source=%s kind=%s value=%s status=%u",
             input_name(source),
             event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown ? "down" : "up",
             event.value.c_str(),
             static_cast<unsigned>(update.status));
    return false;
  }
  if (!update.state_changed || !update.report_changed) {
    return true;
  }

  app->keyboard_delivery.set_desired(update.snapshot);
  const bool accepted = flush_pending_keyboard_snapshot(app);
  if (!accepted) {
    ESP_LOGD(kTag,
             "HID snapshot not accepted source=%s owner=%s empty=%u held=%u",
             input_name(source),
             keyboard_transport_name(app->keyboard_transport.owner()),
             update.snapshot.empty() ? 1U : 0U,
             static_cast<unsigned>(app->held_keyboard.active_source_count()));
  }
  return accepted;
}

bool flush_pending_bridged_hotkey_event(AppContext* app,
                                        ai_keyboard::InputId source) {
  const auto source_index = static_cast<std::size_t>(source);
  if (source_index >= app->bridged_hotkey_transports.size()) {
    return false;
  }
  auto& transport = app->bridged_hotkey_transports[source_index];
  auto& delivery = app->bridged_hotkey_deliveries[source_index];
  const auto pending = delivery.pending_transition();
  if (!pending.valid) {
    // A complete short press may occur while the transport queue is full. If
    // its down edge never entered the queue, down+up safely coalesce to a
    // no-op and the provisional endpoint latch can be released immediately.
    if (!delivery.desired_pressed()) {
      transport.commit_snapshot(true);
    }
    return true;
  }

  const bool released = !pending.pressed;
  const auto owner = transport.select_for_snapshot(
      released,
      app->transport_usb_mounted,
      app->usb_transport_epoch,
      app->transport_ble_connected,
      app->ble_transport_epoch);
  const ai_keyboard::FirmwareEvent event{
      pending.pressed ? ai_keyboard::FirmwareEventKind::HidKeyDown
                      : ai_keyboard::FirmwareEventKind::HidKeyUp,
      pending.hotkey,
      true,
  };
  bool accepted = false;
  switch (owner) {
    case ai_keyboard::KeyboardTransportOwner::Usb:
      accepted = app->usb.send_firmware_event_for_epoch(
          input_name(source), event, transport.owner_epoch());
      break;
    case ai_keyboard::KeyboardTransportOwner::Ble: {
      const auto expected_owner = app->ble.connection_identity();
      if (!expected_owner.valid() ||
          expected_owner.generation != transport.owner_epoch()) {
        return false;
      }
      accepted = app->ble.send_firmware_event_for_owner(
          input_name(source), event, expected_owner);
      break;
    }
    case ai_keyboard::KeyboardTransportOwner::None:
    case ai_keyboard::KeyboardTransportOwner::Suppressed:
      accepted = true;
      break;
  }
  if (accepted && delivery.mark_accepted(pending)) {
    transport.commit_snapshot(released);
    return true;
  }
  return false;
}

void flush_pending_bridged_hotkey_events(AppContext* app) {
  for (std::size_t index = 0;
       index < app->bridged_hotkey_deliveries.size();
       ++index) {
    flush_pending_bridged_hotkey_event(
        app, static_cast<ai_keyboard::InputId>(index));
  }
}

bool dispatch_bridged_hotkey_event(AppContext* app,
                                   ai_keyboard::InputId source,
                                   const ai_keyboard::FirmwareEvent& event) {
  reconcile_keyboard_transport_lifetimes(app);
  const auto source_index = static_cast<std::size_t>(source);
  if (source_index >= app->bridged_hotkey_deliveries.size()) {
    return false;
  }
  const bool pressed =
      event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown;
  app->bridged_hotkey_deliveries[source_index].set_desired(
      pressed, event.value);
  return flush_pending_bridged_hotkey_event(app, source);
}

void dispatch_firmware_event(AppContext* app,
                             ai_keyboard::InputId source_input,
                             const ai_keyboard::FirmwareEvent& event) {
  if (event.kind == ai_keyboard::FirmwareEventKind::None) {
    return;
  }

  const char* source = input_name(source_input);
  const auto sequence = ++app->hid_event_sequence;
  if (event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown ||
      event.kind == ai_keyboard::FirmwareEventKind::HidKeyUp) {
    const auto report = ai_keyboard::hid_report_for_hotkey(event.value);
    const bool app_bridge =
        event.bridge_app_hotkey && !report.valid;
    const bool accepted =
        app_bridge
            ? dispatch_bridged_hotkey_event(app, source_input, event)
            : dispatch_held_keyboard_event(app, source_input, event);
    if (!accepted) {
      ESP_LOGD(kTag,
               "HID dispatch seq=%lu source=%s kind=%s delivery rejected",
               static_cast<unsigned long>(sequence),
               source,
               event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown ? "down" : "up");
    }
    return;
  }

  const auto route = ai_keyboard::route_for_firmware_event(event, app->ble.connected());
  const char* kind = "unknown";
  switch (event.kind) {
    case ai_keyboard::FirmwareEventKind::None:
      kind = "none";
      break;
    case ai_keyboard::FirmwareEventKind::HidKeyDown:
      kind = "down";
      break;
    case ai_keyboard::FirmwareEventKind::HidKeyUp:
      kind = "up";
      break;
    case ai_keyboard::FirmwareEventKind::HidTap:
      kind = "tap";
      break;
    case ai_keyboard::FirmwareEventKind::FixedText:
      kind = "text";
      break;
    case ai_keyboard::FirmwareEventKind::AppCommand:
      kind = "app";
      break;
  }
  ESP_LOGD(kTag,
           "HID dispatch seq=%lu source=%s kind=%s value=%s route=%s usb=%u ble=%u",
           static_cast<unsigned long>(sequence),
           source,
           kind,
           event.value.c_str(),
           route == ai_keyboard::FirmwareTransportRoute::BleFirst ? "ble_first" : "usb_first",
           app->usb.ready() ? 1U : 0U,
           app->ble.connected() ? 1U : 0U);
  if (route == ai_keyboard::FirmwareTransportRoute::BleFirst) {
    if (!app->ble.send_firmware_event(source, event)) {
      ESP_LOGW(kTag, "HID dispatch seq=%lu BLE delivery failed", static_cast<unsigned long>(sequence));
    }
    return;
  }

  const auto usb_epoch = app->usb.connection_epoch();
  if (usb_epoch != 0) {
    if (!app->usb.send_firmware_event_for_epoch(source, event, usb_epoch)) {
      // USB ownership is fixed at physical-event arrival. Queue pressure or a
      // concurrent remount must not migrate the same event to BLE.
      ESP_LOGW(kTag,
               "HID dispatch seq=%lu USB owner rejected report; "
               "BLE fallback suppressed",
               static_cast<unsigned long>(sequence));
    }
    return;
  }
  if (!app->ble.send_firmware_event(source, event)) {
    ESP_LOGW(kTag,
             "HID dispatch seq=%lu no transport accepted report",
             static_cast<unsigned long>(sequence));
  }
}

void dispatch_encoder_press_click(AppContext* app) {
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (!app->speaker.ready()) {
    if (app->speaker_startup_phase != SpeakerStartupPhase::Ready ||
        app->speaker.begin(app->platform_task, &app->audio_io_arbiter) != ESP_OK) {
      ESP_LOGW(kTag, "ENC_PRESS volume announcement speaker unavailable");
      return;
    }
  }
  const auto prompt = easy_input::speaker_assets::volume_prompt(
      app->speaker_volume_level);
  if (prompt.encoded == nullptr || prompt.encoded_bytes == 0U ||
      !app->speaker.request_embedded_asset(
          prompt.encoded, prompt.encoded_bytes)) {
    ESP_LOGW(kTag,
             "ENC_PRESS volume announcement unavailable level=%u busy=%u",
             static_cast<unsigned>(app->speaker_volume_level),
             app->speaker.busy() ? 1U : 0U);
    return;
  }
  mark_activity(app, millis(), "speaker_volume_announcement");
  ESP_LOGI(kTag,
           "ENC_PRESS volume announcement level=%u percent=%u",
           static_cast<unsigned>(app->speaker_volume_level),
           static_cast<unsigned>(app->speaker_volume_level) * 10U);
#else
  ESP_LOGI(kTag, "ENC_PRESS volume announcement unavailable in this build");
#endif
}

void release_keyboard_reports(AppContext* app) {
  app->held_keyboard.clear();
  // Preserve the delivery state that the transport has already accepted. A
  // zero snapshot remains pending until USB/BLE accepts it; queue pressure
  // must never turn platform/config switching into a one-shot best effort.
  app->keyboard_delivery.set_desired({});
  flush_pending_keyboard_snapshot(app);
  for (std::size_t index = 0;
       index < app->bridged_hotkey_deliveries.size();
       ++index) {
    app->bridged_hotkey_deliveries[index].set_desired(false, {});
    flush_pending_bridged_hotkey_event(
        app, static_cast<ai_keyboard::InputId>(index));
  }
}

bool save_selected_host_platform(AppContext* app,
                                 ai_keyboard::HostPlatform platform,
                                 std::uint32_t now_ms) {
  release_keyboard_reports(app);
  esp_err_t save_err = ESP_OK;
  const bool saved = app->config_store.save_host_platform(platform, &save_err);
  if (saved) {
    app->config_state.set_target_platform(platform);
  }
  app->leds.show_status_event(saved
      ? (platform == ai_keyboard::HostPlatform::Windows
             ? easy_input::StatusLedEvent::PlatformWindows
             : easy_input::StatusLedEvent::PlatformMacOS)
      : easy_input::StatusLedEvent::SaveFailed, now_ms);
  ESP_LOGI(kTag, "HOST platform=%s saved=%u", ai_keyboard::host_platform_name(platform), saved ? 1U : 0U);
  return saved;
}

void handle_platform_selection_result(
    AppContext* app,
    const ai_keyboard::PlatformSelectionResult& result,
    std::uint32_t now_ms) {
  using Outcome = ai_keyboard::PlatformSelectionOutcome;
  if (result.outcome == Outcome::MacOS || result.outcome == Outcome::Windows) {
    const auto platform = result.outcome == Outcome::Windows
                              ? ai_keyboard::HostPlatform::Windows
                              : ai_keyboard::HostPlatform::MacOS;
    const bool saved = save_selected_host_platform(app, platform, now_ms);
    publish_config_status_without_payload(
        app, "platform", saved ? "ok" : "save_failed", saved);
    ESP_LOGI(kTag,
             "HOST platform selected from config mode platform=%s saved=%u",
             ai_keyboard::host_platform_name(platform),
             saved ? 1U : 0U);
    return;
  }
  if (result.outcome == Outcome::Conflict) {
    app->leds.show_status_event(easy_input::StatusLedEvent::SaveFailed, now_ms);
    publish_config_status_without_payload(app, "platform", "conflict", false);
    ESP_LOGW(kTag, "HOST platform selection rejected: KEY1 and KEY2 conflict");
    return;
  }
  if (result.outcome == Outcome::TimedOut) {
    ESP_LOGI(kTag, "HOST platform selection mode timed out");
  } else if (result.outcome == Outcome::Cancelled) {
    ESP_LOGI(kTag, "HOST platform selection mode cancelled by another key");
  }
}

bool has_pending_encoder_work(const AppContext* app) {
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  return app->speaker_volume_dirty;
#else
  static_cast<void>(app);
  return false;
#endif
}

ai_keyboard::PowerPolicyInputs power_policy_inputs(AppContext* app,
                                                   std::uint32_t now_ms) {
  auto inputs = base_power_policy_inputs(app, now_ms);
  inputs.config_window_active = app->ble.config_window_active();
  inputs.encoder_press_pending = app->encoder_press_pending;
  inputs.wheel_report_pending = has_pending_encoder_work(app);
  inputs.audio_streaming = app->audio.streaming();
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  inputs.audio_streaming =
      inputs.audio_streaming ||
      app->audio_io_arbiter.microphone_generation() != 0;
  inputs.speaker_playback_active = app->speaker.sleep_blocked();
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  inputs.speaker_playback_active =
      inputs.speaker_playback_active ||
      app->speaker_assets.transfer_active() ||
      app->codex_playback.sleep_blocked();
#endif
#endif
  inputs.wifi_active = app->audio.wifi_active_or_streaming();
  inputs.wake_source_configured = app->controlled_light_sleep_available;
  inputs.key_wake_asserted = key_wake_line_asserted();
  inputs.light_sleep_backoff =
      app->last_light_sleep_error_ms != 0 &&
      now_ms - app->last_light_sleep_error_ms < kControlledLightSleepErrorBackoffMs;
  return inputs;
}

std::uint32_t poll_interval_ms(AppContext* app, std::uint32_t now_ms) {
  return ai_keyboard::evaluate_power_policy(power_policy_inputs(app, now_ms)).poll_ms;
}

bool controlled_light_sleep_allowed(AppContext* app,
                                    PowerMode mode,
                                    std::uint32_t now_ms,
                                    const char** reason) {
  const auto decision = ai_keyboard::evaluate_power_policy(power_policy_inputs(app, now_ms));
  const char* block = decision.mode == mode ? decision.light_sleep_block : "mode_changed";
  if (reason != nullptr) {
    *reason = block;
  }
  return decision.mode == mode && decision.light_sleep_allowed;
}

bool try_controlled_light_sleep(AppContext* app, PowerMode mode, std::uint32_t now_ms) {
  const char* reason = "unknown";
  if (!controlled_light_sleep_allowed(app, mode, now_ms, &reason)) {
    app->light_sleep_block_reason = reason;
    return false;
  }
  app->light_sleep_block_reason = "ok";

  const esp_err_t timer_err =
      esp_sleep_enable_timer_wakeup(static_cast<std::uint64_t>(kControlledLightSleepNapMs) *
                                    1000ULL);
  if (timer_err != ESP_OK) {
    app->last_light_sleep_error_ms = now_ms;
    ESP_LOGW(kTag, "controlled light sleep timer wake config failed: %s",
             esp_err_to_name(timer_err));
    return false;
  }

  const std::int64_t started_us = esp_timer_get_time();
  const esp_err_t sleep_err = esp_light_sleep_start();
  const auto after_ms = millis();
  if (sleep_err != ESP_OK) {
    app->last_light_sleep_error_ms = after_ms;
    ESP_LOGW(kTag, "controlled light sleep start failed: %s", esp_err_to_name(sleep_err));
    return false;
  }

  const std::int64_t slept_us = esp_timer_get_time() - started_us;
  const auto slept_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(1, slept_us / 1000));
  ++app->light_sleep_entries;
  app->light_sleep_total_ms += slept_ms;
  app->last_light_sleep_ms = after_ms;

  const auto cause = esp_sleep_get_wakeup_cause();
  app->last_light_sleep_wake = light_sleep_wakeup_cause_name(cause);
  const bool input_wake = cause == ESP_SLEEP_WAKEUP_EXT1 ||
                          cause == ESP_SLEEP_WAKEUP_GPIO || key_wake_line_asserted();
  if (input_wake) {
    app->key_wake_verified = true;
    mark_activity(app, after_ms, "light_sleep_key_wake");
    const auto recovered_mask = app->inputs.recover_pressed_after_light_sleep(
        after_ms, handle_input_event, app);
    if (recovered_mask == 0) {
      ESP_LOGW(kTag,
               "light sleep input wake had no pressed GPIO to recover cause=%s",
               app->last_light_sleep_wake);
    } else {
      ESP_LOGI(kTag,
               "light sleep recovered pressed inputs mask=0x%03lx cause=%s",
               static_cast<unsigned long>(recovered_mask),
               app->last_light_sleep_wake);
    }
  }
  return true;
}

// 深睡准入:比 light sleep 更严——30 分钟无活动,且不要求 BLE 已连
// (未连接时挂机耗电更冤,深睡收益更大;唤醒后重新广播即可重连)。
bool deep_sleep_allowed(AppContext* app, std::uint32_t now_ms, const char** reason) {
  const auto decision = ai_keyboard::evaluate_power_policy(power_policy_inputs(app, now_ms));
  if (reason != nullptr) {
    *reason = decision.deep_sleep_block;
  }
  return decision.deep_sleep_allowed;
}

bool try_begin_audio_deep_sleep_quiesce(AppContext* app) {
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  return app->audio_io_arbiter.try_begin_deep_sleep_quiesce();
#else
  static_cast<void>(app);
  return true;
#endif
}

void cancel_audio_deep_sleep_quiesce(AppContext* app) {
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (!app->audio_io_arbiter.cancel_deep_sleep_quiesce()) {
    ESP_LOGE(kTag, "deep sleep audio admission gate cancel failed");
  }
#else
  static_cast<void>(app);
#endif
}

void maybe_enter_deep_sleep(AppContext* app, std::uint32_t now_ms) {
  const auto decision = ai_keyboard::evaluate_power_policy(power_policy_inputs(app, now_ms));
  if (decision.wifi_release_required) {
    app->audio.request_wifi_release_for_deep_sleep();
    return;
  }
  if (!decision.deep_sleep_allowed) {
    // 深睡门槛已到但出现 USB、配置窗口或输入等新阻塞条件时,
    // 取消此前可能已经发出的释放请求并恢复控制通道。
    if (now_ms - app->last_activity_ms >= kDeepSleepAfterMs) {
      app->audio.cancel_wifi_release_for_device_activity();
    }
    return;
  }

  ESP_LOGI(kTag,
           "entering deep sleep idle_ms=%lu battery=%umV wake=KEY_WAKE(GPIO%d) any key",
           static_cast<unsigned long>(now_ms - app->last_activity_ms),
           static_cast<unsigned>(app->battery_mv),
           static_cast<int>(ai_keyboard::kKeyWakePin));

  if constexpr (ai_keyboard::kKeyWakePin >= 0) {
    // 深睡时数字域上拉断电,显式启用 RTC 域上拉,防 KEY_WAKE 悬空误唤醒。
    const esp_err_t pullup_err =
        rtc_gpio_pullup_en(static_cast<gpio_num_t>(ai_keyboard::kKeyWakePin));
    if (pullup_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep KEY_WAKE pull-up failed: %s",
               esp_err_to_name(pullup_err));
      return;
    }
    const esp_err_t pulldown_err =
        rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(ai_keyboard::kKeyWakePin));
    if (pulldown_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep KEY_WAKE pull-down disable failed: %s",
               esp_err_to_name(pulldown_err));
      return;
    }
    // 清掉 30ms nap 的定时唤醒源,避免深睡被立即唤醒。
    const esp_err_t timer_err =
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if (timer_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep timer wake disable failed: %s",
               esp_err_to_name(timer_err));
      return;
    }
    const esp_err_t wake_err = esp_sleep_enable_ext1_wakeup_io(
        1ULL << ai_keyboard::kKeyWakePin,
        ESP_EXT1_WAKEUP_ANY_LOW);
    if (wake_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep KEY_WAKE config failed: %s",
               esp_err_to_name(wake_err));
      return;
    }

    // Atomically close new microphone/speaker admission before the final
    // policy snapshots. An asynchronous Wi-Fi start may otherwise arrive
    // after a snapshot and race the GPIO8 shutdown sequence.
    if (!try_begin_audio_deep_sleep_quiesce(app)) {
      ESP_LOGI(kTag, "deep sleep cancelled: audio transition/owner active");
      return;
    }

    // Wake-source setup and logging may take time. Re-evaluate every blocker
    // only after the audio admission gate is closed. Also honor GPIO/encoder
    // edges latched by an ISR after this main-loop iteration polled input.
    const auto final_decision = ai_keyboard::evaluate_power_policy(
        power_policy_inputs(app, millis()));
    if (!final_decision.deep_sleep_allowed || app->inputs.activity_pending()) {
      ESP_LOGI(kTag,
               "deep sleep cancelled during final gate reason=%s input_pending=%u",
               final_decision.deep_sleep_block,
               app->inputs.activity_pending() ? 1U : 0U);
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    const esp_err_t led_err = app->leds.prepare_for_deep_sleep();
    if (led_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep LED quiesce failed: %s",
               esp_err_to_name(led_err));
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    // The black frame takes a bounded RMT transaction. Recheck every mutable
    // non-audio blocker once more immediately before command pins are
    // destructively detached and the rail is driven low.
    const auto commit_decision = ai_keyboard::evaluate_power_policy(
        power_policy_inputs(app, millis()));
    if (!commit_decision.deep_sleep_allowed ||
        app->inputs.activity_pending()) {
      ESP_LOGI(kTag,
               "deep sleep cancelled at commit gate reason=%s input_pending=%u",
               commit_decision.deep_sleep_block,
               app->inputs.activity_pending() ? 1U : 0U);
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    record_completed_power_cycle(app, now_ms, "unknown", true);
    retain_power_cycle_for_deep_sleep(app->latest_power_cycle);
    const esp_err_t power_err =
        app->peripheral_power.prepare_for_deep_sleep();
    if (power_err != ESP_OK) {
      g_retained_power_cycle.magic = 0;
      ESP_LOGE(kTag,
               "deep sleep shared rail shutdown failed: %s",
               esp_err_to_name(power_err));
      if (power_err == ESP_ERR_INVALID_STATE) {
        cancel_audio_deep_sleep_quiesce(app);
        return;
      }
      // Once command-pin reconfiguration starts there is no safe generic
      // rollback for live RMT/I2S routing. Cold restart restores the complete
      // awake initialization contract instead of continuing half-configured.
      ESP_LOGE(kTag, "restarting after partial peripheral shutdown failure");
      esp_restart();
      return;
    }
    esp_deep_sleep_start();
  }
}

void show_encoder_volume_feedback(AppContext* app,
                                  std::int8_t direction,
                                  std::uint32_t now_ms) {
  if (direction == 0 || app == nullptr) {
    return;
  }
  if (app->last_encoder_led_feedback_ms != 0 &&
      now_ms - app->last_encoder_led_feedback_ms < kEncoderLedFeedbackMinIntervalMs) {
    return;
  }
  app->last_encoder_led_feedback_ms = now_ms;
  app->leds.show_scroll_event(direction, 0, now_ms);
}

void dispatch_encoder_rotation(AppContext* app, const easy_input::InputEvent& event) {
  if (event.encoder_step == 0) {
    return;
  }
  const bool clockwise = event.encoder_step < 0;
  const auto now_ms = millis();
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  const auto previous = app->speaker_volume_level;
  const auto adjusted =
      ai_keyboard::adjust_speaker_volume_for_wired_encoder_step(
          previous, event.encoder_step);
  if (adjusted == previous) {
    return;
  }
  app->speaker_volume_level = adjusted;
  app->speaker.set_volume_level(adjusted);
  app->speaker_volume_dirty = true;
  app->speaker_volume_changed_ms = now_ms;
  ESP_LOGI(kTag,
           "ENC_BOARD_VOLUME direction=%s steps=%d level=%u percent=%u",
           clockwise ? "clockwise_up" : "counter_clockwise_down",
           encoder_step_count(event.encoder_step),
           static_cast<unsigned>(adjusted),
           static_cast<unsigned>(adjusted) * 10U);
  show_encoder_volume_feedback(app, clockwise ? 1 : -1, now_ms);
#else
  static_cast<void>(app);
  static_cast<void>(clockwise);
  static_cast<void>(now_ms);
#endif
}

void persist_pending_speaker_volume(AppContext* app, std::uint32_t now_ms) {
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (!app->speaker_volume_dirty ||
      now_ms - app->speaker_volume_changed_ms < kSpeakerVolumePersistDelayMs) {
    return;
  }
  esp_err_t error = ESP_OK;
  if (!app->config_store.save_speaker_volume(
          app->speaker_volume_level, &error)) {
    ESP_LOGW(kTag,
             "speaker volume save failed level=%u: %s",
             static_cast<unsigned>(app->speaker_volume_level),
             esp_err_to_name(error));
    app->speaker_volume_changed_ms = now_ms;
    return;
  }
  app->speaker_volume_dirty = false;
  ESP_LOGI(kTag,
           "speaker volume saved level=%u percent=%u",
           static_cast<unsigned>(app->speaker_volume_level),
           static_cast<unsigned>(app->speaker_volume_level) * 10U);
#else
  static_cast<void>(app);
  static_cast<void>(now_ms);
#endif
}

void sync_encoder_scroll_axis(AppContext* app) {
  const auto axis = app->config_state.encoder_scroll().axis;
  app->encoder_scroll_axis = axis == ai_keyboard::EncoderScrollAxis::Horizontal
                                 ? ai_keyboard::EncoderScrollAxis::Horizontal
                                 : ai_keyboard::EncoderScrollAxis::Vertical;
}

void check_encoder_press_config_hold(AppContext* app, std::uint32_t now_ms) {
  if (!app->encoder_press_pending || app->encoder_press_config_triggered) {
    return;
  }
  if (!app->inputs.low_active_pressed(ai_keyboard::kEncoderPressPin)) {
    return;
  }
  if (now_ms - app->encoder_press_down_ms < kEncoderConfigModeHoldMs) {
    return;
  }

  app->encoder_press_config_triggered = true;
  app->platform_selection.arm(now_ms, kPlatformSelectionModeTimeoutMs);
  mark_activity(app, now_ms, "config");
  app->ble.open_config_window("encoder_long_press");
  app->leds.show_status_event(easy_input::StatusLedEvent::ConfigMode, now_ms);
  ESP_LOGI(kTag,
           "CONFIG mode opened by encoder long press hold_ms=%lu",
           static_cast<unsigned long>(now_ms - app->encoder_press_down_ms));
  // 状态特征只回吐最后一次发布的缓存;长按进配置模式时发布一份实时快照,
  // 让 App 在无串口环境下读到音频/控制通道与省电门控的完整诊断。
  const char* deep_sleep_block = "unknown";
  deep_sleep_allowed(app, now_ms, &deep_sleep_block);
  std::array<char, 120> diag_status{};
  std::snprintf(diag_status.data(),
                diag_status.size(),
                "%s ds=%s ls=%s m=%s up=%lus vin=%d chrg=%d",
                app->audio.capture_status().c_str(),
                deep_sleep_block,
                app->light_sleep_block_reason,
                power_mode_name(app->tracked_power_mode),
                static_cast<unsigned long>(now_ms / 1000U),
                read_optional_gpio(ai_keyboard::kExternalPowerSensePin),
                read_optional_gpio(ai_keyboard::kChargeStatusPin));
  publish_config_status(app, "diag", diag_status.data(), 0, 0, true);
}

void sync_led_status(AppContext* app, std::uint32_t now_ms) {
  const bool usb_mounted = app->usb.mounted();
  if (!app->led_status_initialized || usb_mounted != app->last_usb_mounted) {
    if (app->led_status_initialized || usb_mounted) {
      app->leds.show_status_event(usb_mounted ? easy_input::StatusLedEvent::UsbConnected
                                              : easy_input::StatusLedEvent::UsbDisconnected,
                                  now_ms);
    }
    app->last_usb_mounted = usb_mounted;
  }

  const bool ble_connected = app->ble.connected();
  if (!app->led_status_initialized || ble_connected != app->last_ble_connected) {
    if (app->led_status_initialized || ble_connected) {
      app->leds.show_status_event(ble_connected ? easy_input::StatusLedEvent::BleConnected
                                                : easy_input::StatusLedEvent::BleDisconnected,
                                  now_ms);
    }
    app->last_ble_connected = ble_connected;
  }
  app->led_status_initialized = true;
}

void sync_audio_power_hold(AppContext* app) {
  // GPIO8 now stays high for the whole awake lifecycle. The per-feature hold
  // remains a readiness/diagnostic fact for the audio arbiter; releasing it
  // cannot switch off the shared rail before whole-device deep sleep.
  bool audio_active = app->audio.streaming();
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  const auto microphone_generation =
      app->audio_io_arbiter.microphone_generation();
  audio_active = audio_active || microphone_generation != 0;
#endif
  if (app->audio_power_hold_active != audio_active) {
    const esp_err_t power_err =
        app->peripheral_power.set_audio_power_hold(audio_active);
    if (power_err != ESP_OK) {
      ESP_LOGE(kTag,
               "audio power activity update failed: %s",
               esp_err_to_name(power_err));
      return;
    }
    app->audio_power_hold_active = audio_active;
  }
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (audio_active && microphone_generation != 0 &&
      app->audio_power_hold_active) {
    app->audio_io_arbiter.mark_microphone_power_ready(
        microphone_generation);
  }
#endif
}

#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
void service_speaker(AppContext* app) {
  const bool playback_allowed =
      !app->audio.streaming() &&
      !app->audio_io_arbiter.microphone_requested();
  app->speaker.poll(playback_allowed);

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (app->speaker_startup_phase ==
      SpeakerStartupPhase::StartLocal) {
    const auto now_ms = millis();
    const bool retry_due =
        app->speaker_local_retry_after_ms == 0U ||
        static_cast<std::int32_t>(
            now_ms - app->speaker_local_retry_after_ms) >= 0;
    if (retry_due) {
      const auto local_result =
          app->speaker_assets.begin_local(
              &app->usb, app->platform_task);
      if (local_result == ESP_OK) {
        app->speaker_local_retry_after_ms = 0U;
        app->speaker_startup_phase =
            app->speaker_skip_boot_after_deep_sleep
                ? SpeakerStartupPhase::WaitLeaseIdle
                : SpeakerStartupPhase::ResolveBoot;
        ESP_LOGI(kTag, "local speaker asset service started");
      } else {
        app->speaker_local_retry_after_ms =
            now_ms + kSpeakerAssetsRetryMs;
        ESP_LOGW(
            kTag,
            "local speaker asset service deferred: %s",
            esp_err_to_name(local_result));
      }
    }
  }

  ai_keyboard::SpeakerServiceStartupInputs startup_inputs{
      app->speaker_assets.ready(),
      app->audio_ready,
      app->speaker_assets.wifi_ready(),
  };
  const auto startup_action =
      ai_keyboard::evaluate_speaker_service_startup(
          startup_inputs);
  if (startup_action ==
      ai_keyboard::SpeakerServiceStartupAction::StartWifi) {
    const auto now_ms = millis();
    const bool retry_due =
        app->speaker_wifi_retry_after_ms == 0U ||
        static_cast<std::int32_t>(
            now_ms - app->speaker_wifi_retry_after_ms) >= 0;
    if (retry_due) {
      const auto wifi_result =
          app->speaker_assets.start_wifi(&app->audio);
      if (wifi_result == ESP_OK) {
        app->speaker_wifi_retry_after_ms = 0U;
        app->audio.request_heartbeat_refresh();
        ESP_LOGI(
            kTag,
            "speaker asset Wi-Fi service started before optional Boot playback");
      } else {
        app->speaker_wifi_retry_after_ms =
            now_ms + kSpeakerAssetsRetryMs;
        ESP_LOGW(
            kTag,
            "speaker asset Wi-Fi service deferred before Boot playback: %s",
            esp_err_to_name(wifi_result));
      }
    }
  } else if (
      startup_action ==
          ai_keyboard::SpeakerServiceStartupAction::WifiUnavailable &&
      !app->speaker_wifi_unavailable_logged) {
    app->speaker_wifi_unavailable_logged = true;
    ESP_LOGW(
        kTag,
        "Wi-Fi sound sync unavailable because keyboard audio did not initialize; USB sound sync and local Boot playback remain available");
  }
  startup_inputs = {
      app->speaker_assets.ready(),
      app->audio_ready,
      app->speaker_assets.wifi_ready(),
  };
  if (!ai_keyboard::speaker_boot_pipeline_allowed(startup_inputs)) {
    // The required Wi-Fi service owns startup allocation priority. In
    // particular, do not create I2S/decoder/worker resources while its
    // carrier allocation is waiting for a contiguous internal-heap block.
    return;
  }

  if (!playback_allowed &&
      app->speaker_startup_phase ==
          SpeakerStartupPhase::ResolveBoot) {
    app->speaker_boot_skip_requested = true;
  }

  if (app->speaker_startup_phase ==
      SpeakerStartupPhase::ResolveBoot) {
    // A microphone request that arrives before Boot resolution starts wins
    // immediately. Do not start a Store lease merely to play a late startup
    // sound after the user's recording has finished.
    if (app->speaker_boot_skip_requested &&
        !app->speaker_boot_resolution_started) {
      app->speaker_startup_phase =
          SpeakerStartupPhase::WaitLeaseIdle;
      ESP_LOGI(kTag, "Boot sound skipped for microphone priority");
    } else {
      const auto boot_result =
          app->speaker_assets.take_boot_playback(
              &app->boot_sound_lease,
              &app->boot_sound_asset);
      app->speaker_boot_resolution_started = true;
      if (boot_result ==
          easy_input::SpeakerAssetsBootPlaybackResult::Ready) {
        app->speaker_factory_boot_sound = false;
        app->speaker_startup_phase =
            app->speaker_boot_skip_requested || !playback_allowed
                ? SpeakerStartupPhase::ReleaseLease
                : SpeakerStartupPhase::BeginOutput;
      } else if (
          boot_result ==
          easy_input::SpeakerAssetsBootPlaybackResult::FactoryDefault) {
        app->speaker_factory_boot_sound = true;
        app->speaker_startup_phase =
            app->speaker_boot_skip_requested || !playback_allowed
                ? SpeakerStartupPhase::ReleaseLease
                : SpeakerStartupPhase::BeginOutput;
      } else if (
          boot_result ==
          easy_input::SpeakerAssetsBootPlaybackResult::Unavailable) {
        app->speaker_startup_phase =
            SpeakerStartupPhase::WaitLeaseIdle;
        ESP_LOGI(
            kTag,
            "Boot sound is disabled or unavailable; startup stays silent");
      }
    }
  }

  if (app->speaker_startup_phase ==
      SpeakerStartupPhase::BeginOutput) {
    if (!playback_allowed) {
      app->speaker_boot_skip_requested = true;
      app->speaker_startup_phase =
          SpeakerStartupPhase::ReleaseLease;
    } else {
      app->speaker.mark_boot_pending(
          app->audio_io_arbiter.microphone_generation());
      const auto begin_result =
          app->speaker.begin(
              app->platform_task, &app->audio_io_arbiter);
      if (begin_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Boot speaker unavailable: %s",
            esp_err_to_name(begin_result));
        app->speaker_startup_phase =
            SpeakerStartupPhase::ReleaseLease;
      } else {
        bool requested = false;
        if (app->speaker_factory_boot_sound) {
          const auto factory_sound =
              easy_input::speaker_assets::factory_boot_sound();
          requested = app->speaker.request_embedded_asset(
              factory_sound.encoded,
              factory_sound.encoded_bytes);
        } else {
          requested = app->speaker.request_asset(
              app->speaker_assets.playback_storage(),
              app->boot_sound_lease,
              app->boot_sound_asset);
        }
        if (!requested) {
          ESP_LOGW(kTag, "boot asset playback request rejected");
          app->speaker_startup_phase =
              SpeakerStartupPhase::ShutdownOutput;
        } else {
          app->speaker_startup_phase =
              SpeakerStartupPhase::WaitPlayback;
          mark_activity(
              app,
              millis(),
              app->speaker_factory_boot_sound
                  ? "speaker_boot_factory"
                  : "speaker_boot_asset");
        }
      }
    }
  }
#endif

#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  if (app->speaker_probe_pending && playback_allowed) {
    app->speaker_probe_pending = false;
    if (!app->speaker.request_diagnostic_tone()) {
      ESP_LOGW(kTag, "speaker diagnostic request rejected");
    } else {
      mark_activity(app, millis(), "speaker_probe");
    }
  }
#endif

  // Establish the microphone lease before releasing the speaker lease. The
  // audio worker also waits for this power-ready generation, so a concurrent
  // mic start can never overlap or race a GPIO8 rail handoff.
  sync_audio_power_hold(app);
  const bool power_required = app->speaker.power_lease_required();
  if (app->speaker_power_hold_active != power_required) {
    const esp_err_t power_err =
        app->peripheral_power.set_speaker_power_hold(power_required);
    if (power_err != ESP_OK) {
      ESP_LOGE(kTag,
               "speaker power activity update failed: %s",
               esp_err_to_name(power_err));
      return;
    }
    app->speaker_power_hold_active = power_required;
  }
  if (power_required && app->peripheral_power.ready()) {
    app->speaker.notify_power_ready();
  }
  const bool handoff_complete =
      app->speaker.complete_power_handoff();
  if (!handoff_complete) {
    ESP_LOGW(kTag, "speaker audio ownership handoff deferred");
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::WaitPlayback &&
      handoff_complete && !app->speaker.busy()) {
    app->speaker_startup_phase =
        SpeakerStartupPhase::ReleaseLease;
  }

  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::ShutdownOutput &&
      handoff_complete && !app->speaker.busy()) {
    if (!app->speaker.request_shutdown()) {
      ESP_LOGW(kTag, "speaker startup resource shutdown deferred");
    } else if (app->speaker.shutdown_complete()) {
      app->speaker_startup_phase =
          SpeakerStartupPhase::ReleaseLease;
    }
  }

  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::ReleaseLease) {
    if (!app->boot_sound_lease.valid) {
      app->speaker_startup_phase =
          SpeakerStartupPhase::WaitLeaseIdle;
    } else if (
        app->speaker_assets.queue_playback_lease_release(
            app->boot_sound_lease)) {
      app->boot_sound_lease = {};
      app->boot_sound_asset = {};
      app->speaker_startup_phase =
          SpeakerStartupPhase::WaitLeaseIdle;
    } else {
      ESP_LOGW(kTag, "boot sound read lease release was not queued");
    }
  }

  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::WaitLeaseIdle &&
      app->speaker_assets.boot_idle()) {
    app->speaker_startup_phase =
        SpeakerStartupPhase::Ready;
  }
#endif
}
#endif

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
bool speaker_asset_resource_steps_allowed(
    const AppContext* app,
    std::uint32_t now_ms) {
  if (app == nullptr || app->audio.streaming() ||
      app->audio_io_arbiter.microphone_requested() ||
      app->speaker.busy() || app->codex_playback.active() ||
      app->inputs.any_input_active() ||
      !app->held_keyboard.empty() ||
      app->keyboard_delivery.pending() ||
      app->ble.input_delivery_pending() ||
      has_pending_encoder_work(app) ||
      (app->last_input_ms != 0U &&
       now_ms - app->last_input_ms <
           kSpeakerAssetsInputQuietMs)) {
    return false;
  }
  for (const auto& delivery : app->bridged_hotkey_deliveries) {
    if (delivery.pending()) {
      return false;
    }
  }
  return true;
}
#endif

void flush_input_led_feedback(AppContext* app) {
  if (!app->input_led_feedback_pending) {
    return;
  }
  const auto input = app->pending_input_led;
  const auto phase = app->pending_input_led_phase;
  const auto queued_ms = app->pending_input_led_ms;
  app->input_led_feedback_pending = false;
  app->leds.show_input_event(input, phase, queued_ms);
}

void handle_input_event(const easy_input::InputEvent& event, void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app == nullptr) {
    return;
  }

  const auto now = millis();
  mark_activity(app, now, "input");
  const auto* name = input_name(event.input);
  app->last_input = std::string(name) + ":" + phase_name(event.phase);
  const int gpio = input_gpio(event.input);
  if (gpio >= 0) {
    app->last_input += ":gpio=" + std::to_string(gpio);
  }
  if (event.encoder_step != 0) {
    app->last_input += ":step=" + std::to_string(event.encoder_step);
  }
  app->last_input_ms = now;
  const bool encoder_turn = event.input == ai_keyboard::InputId::EncoderLeft ||
                            event.input == ai_keyboard::InputId::EncoderRight;
  if (!encoder_turn &&
      ai_keyboard::feedback_for_input_event(event.input, event.phase).active) {
    // Coalesce visual feedback until after HID state has been queued. RMT
    // rendering is intentionally outside the input edge hot path. An inactive
    // release feedback must not erase a press queued in the same poll cycle.
    app->pending_input_led = event.input;
    app->pending_input_led_phase = event.phase;
    app->pending_input_led_ms = now;
    app->input_led_feedback_pending = true;
  }

  if (event.encoder_step != 0) {
    ESP_LOGD(kTag, "%s step=%d", name, event.encoder_step);
  } else {
    ESP_LOGD(kTag, "%s %s", name, phase_name(event.phase));
  }

  if (encoder_turn) {
    if (app->platform_selection.active()) {
      ESP_LOGI(kTag, "%s consumed while awaiting platform selection", name);
      return;
    }
    dispatch_encoder_rotation(app, event);
    return;
  }

  if (event.input == ai_keyboard::InputId::EncoderPress) {
    if (event.phase == ai_keyboard::InputPhase::Pressed) {
      app->encoder_press_pending = true;
      app->encoder_press_config_triggered = false;
      app->encoder_press_down_ms = now;
      return;
    }

    const bool should_dispatch_click =
        app->encoder_press_pending && !app->encoder_press_config_triggered;
    app->encoder_press_pending = false;
    app->encoder_press_config_triggered = false;
    app->encoder_press_down_ms = 0;
    if (should_dispatch_click) {
      dispatch_encoder_press_click(app);
    } else {
      ESP_LOGI(kTag, "ENC_PRESS release ignored after config mode long press");
    }
    return;
  }
  const auto platform_result =
      app->platform_selection.handle_event(event.input, event.phase, now);
  handle_platform_selection_result(app, platform_result, now);
  if (platform_result.consumed) {
    app->input_led_feedback_pending = false;
    ESP_LOGI(kTag, "%s %s consumed by platform selection",
             name,
             phase_name(event.phase));
    return;
  }

  if (is_codex_slot_input(event.input)) {
    handle_codex_slot_transition(
        app,
        app->codex_slots.handle_input(
            event.input, event.phase, app->codex_route_generation));
    return;
  }

  const auto& action = app->config_state.keymap().action_for(event.input);
  const auto firmware_event =
      ai_keyboard::event_for_action(action,
                                    event.phase,
                                    app->config_state.ptt_hotkey(),
                                    app->config_state.edit_ptt_hotkey(),
                                    app->config_state.target_platform());
  handle_ptt_keyboard_audio(app, action, event.phase, name);
  dispatch_firmware_event(app, event.input, firmware_event);
}

void load_stored_config(AppContext* app) {
  ai_keyboard::HostPlatform stored_platform = ai_keyboard::HostPlatform::MacOS;
  if (!app->config_store.load_host_platform(&stored_platform)) {
    stored_platform = ai_keyboard::HostPlatform::MacOS;
  }
  app->config_state.set_target_platform(stored_platform);
  std::string stored_json;
  esp_err_t err = ESP_OK;
  if (!app->config_store.load_config(&stored_json, &err)) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGI(kTag, "CONFIG load skipped: no stored config");
      publish_config_status_without_payload(app, "boot", "no_stored_config", false);
    } else {
      ESP_LOGW(kTag, "CONFIG load skipped: %s", esp_err_to_name(err));
      publish_config_status_without_payload(app, "boot", "nvs_load_failed", false);
    }
    return;
  }

  const auto status = app->config_state.apply_json(stored_json);
  if (status == ai_keyboard::ConfigParseStatus::Ok) {
    app->config_state.set_target_platform(stored_platform);
    sync_encoder_scroll_axis(app);
    sync_keyboard_audio_config(app, "boot");
  }
  ESP_LOGI(kTag,
           "CONFIG load status=%s bytes=%u ptt_hotkey=%s edit_ptt_hotkey=%s",
           parse_status_name(status),
           static_cast<unsigned>(stored_json.size()),
           app->config_state.ptt_hotkey().c_str(),
           app->config_state.edit_ptt_hotkey().c_str());
  publish_config_status_for_json(app,
                                 "boot",
                                 parse_status_name(status),
                                 stored_json,
                                 status == ai_keyboard::ConfigParseStatus::Ok);
}

// B 协议(罗技 HID++ 式):配置/音频控制经 HID 送达后,从 0x11 输入报文
// 回传 bytes/crc16 指纹;App 匹配指纹即确认送达,蓝牙下不再依赖 GATT 读回。
enum class ConfigIngressTransport : std::uint8_t {
  Usb,
  Ble,
  Wifi,
};

void send_hid_config_ack(AppContext* app,
                         std::uint8_t phase_code,
                         bool ok,
                         const std::string& json,
                         bool saved,
                         ConfigIngressTransport origin_transport,
                         std::uint32_t usb_origin_epoch,
                         ai_keyboard::BleOwnerToken ble_origin_owner) {
  const auto bytes = static_cast<std::uint16_t>(
      std::min<std::size_t>(json.size(), UINT16_MAX));
  const auto crc16 = config_json_crc16(json);
  if (origin_transport == ConfigIngressTransport::Usb) {
    if (!app->usb.send_config_ack_for_epoch(
            phase_code, ok, bytes, crc16, saved, usb_origin_epoch)) {
      ESP_LOGW(kTag,
               "CONFIG ACK USB origin expired/rejected phase=%u bytes=%u epoch=%lu",
               static_cast<unsigned>(phase_code),
               static_cast<unsigned>(bytes),
               static_cast<unsigned long>(usb_origin_epoch));
    }
    return;
  }
  if (origin_transport == ConfigIngressTransport::Ble) {
    if (!app->ble.send_config_ack_for_owner(
            phase_code, ok, bytes, crc16, saved, ble_origin_owner)) {
      ESP_LOGW(kTag,
               "CONFIG ACK BLE origin expired/unavailable phase=%u bytes=%u "
               "owner=%u/%lu",
               static_cast<unsigned>(phase_code),
               static_cast<unsigned>(bytes),
               static_cast<unsigned>(ble_origin_owner.conn_handle),
               static_cast<unsigned long>(ble_origin_owner.generation));
    }
    return;
  }
  // Wi-Fi config has no HID request origin. Its own control flow observes the
  // published status; never leak that ACK to an unrelated ambient USB/BLE host.
  ESP_LOGD(kTag,
           "CONFIG ACK skipped for Wi-Fi origin phase=%u bytes=%u",
           static_cast<unsigned>(phase_code),
           static_cast<unsigned>(bytes));
}

void apply_pending_config(AppContext* app) {
  std::string json;
  const char* source = "usb";
  ConfigIngressTransport origin_transport = ConfigIngressTransport::Usb;
  std::uint32_t usb_origin_epoch = 0;
  ai_keyboard::BleOwnerToken ble_origin_owner{};
  if (!app->usb.take_pending_config(&json, &usb_origin_epoch)) {
    source = "ble";
    origin_transport = ConfigIngressTransport::Ble;
    if (!app->ble.take_pending_config(&json, &ble_origin_owner)) {
      source = "wifi";
      origin_transport = ConfigIngressTransport::Wifi;
      if (!app->audio.take_pending_config(&json)) {
        return;
      }
    }
  }

  if (json.empty()) {
    return;
  }
  mark_activity(app, millis(), source);

  auto candidate_state = app->config_state;
  const auto status = candidate_state.apply_json(json);
  if (status != ai_keyboard::ConfigParseStatus::Ok) {
    ESP_LOGW(kTag,
             "CONFIG %s push rejected status=%s bytes=%u",
             source,
             parse_status_name(status),
             static_cast<unsigned>(json.size()));
    publish_config_status_for_json(app, "push", parse_status_name(status), json, false);
    send_hid_config_ack(
        app,
        1,
        false,
        json,
        false,
        origin_transport,
        usb_origin_epoch,
        ble_origin_owner);
    return;
  }

  esp_err_t save_err = ESP_OK;
  const auto& applied_json = candidate_state.last_applied_json();
  const bool saved = app->config_store.save_config_and_host_platform(
      applied_json, candidate_state.target_platform(), &save_err);
  const bool platform_changed =
      candidate_state.target_platform() != app->config_state.target_platform();
  if (saved) {
    if (platform_changed) {
      release_keyboard_reports(app);
    }
    app->config_state = candidate_state;
    sync_encoder_scroll_axis(app);
    sync_keyboard_audio_config(app, source);
  }
  ESP_LOGI(kTag,
           "CONFIG %s push status=ok bytes=%u save=%s ptt_hotkey=%s edit_ptt_hotkey=%s",
           source,
           static_cast<unsigned>(applied_json.size()),
            saved ? "ok" : esp_err_to_name(save_err),
           candidate_state.ptt_hotkey().c_str(),
           candidate_state.edit_ptt_hotkey().c_str());
  publish_config_status_for_json(app,
                                 "push",
                                 saved ? "ok" : "save_failed",
                                 applied_json,
                                 saved);
  // 回执按"收到的原始 payload"计算指纹,App 端对照的是自己发出的字节。
  send_hid_config_ack(
      app,
      1,
      true,
      json,
      saved,
      origin_transport,
      usb_origin_epoch,
      ble_origin_owner);
}

void apply_pending_agent_status(AppContext* app, std::uint32_t now_ms) {
  ai_keyboard::AgentStatusCommand selected{};
  bool pending = false;
  const char* source = "none";

  ai_keyboard::AgentStatusCommand ble_command{};
  if (app->ble.take_pending_agent_status(&ble_command)) {
    selected = ble_command;
    pending = true;
    source = "ble";
  }

  ai_keyboard::AgentStatusCommand usb_command{};
  if (app->usb.take_pending_agent_status(&usb_command)) {
    if (!pending ||
        ai_keyboard::agent_status_sequence_is_newer(usb_command.sequence,
                                                    selected.sequence)) {
      selected = usb_command;
      source = "usb";
    }
    pending = true;
  }

  if (!pending) {
    return;
  }

  // Only suppress an exact repeat. Sequence values restart with the desktop
  // process, so treating them as a permanent monotonic counter would discard
  // valid commands after an App restart.
  if (app->last_agent_status_valid &&
      ai_keyboard::agent_status_commands_equal(
          selected, app->last_agent_status_command)) {
    ESP_LOGD(kTag,
             "agent status duplicate transport=%s seq=%lu source=%08lx",
             source,
             static_cast<unsigned long>(selected.sequence),
             static_cast<unsigned long>(selected.source_hash));
    return;
  }

  app->last_agent_status_valid = true;
  app->last_agent_status_command = selected;
  app->leds.set_agent_status(selected, now_ms);
  ESP_LOGI(kTag,
           "agent status transport=%s state=%u seq=%lu ttl_ms=%lu source=%08lx",
           source,
           static_cast<unsigned>(selected.state),
           static_cast<unsigned long>(selected.sequence),
           static_cast<unsigned long>(selected.ttl_ms),
           static_cast<unsigned long>(selected.source_hash));
}

void apply_pending_mailbox_status(AppContext* app, std::uint32_t now_ms) {
  easy_codex::MailboxWireStatus status{};
  if (!app->audio.take_pending_mailbox_status(&status)) {
    return;
  }
  app->leds.set_mailbox_status(
      status.unread_slots, status.coverage_by_slot, status.running_tasks, now_ms);
  ESP_LOGI(kTag,
           "mailbox status slots=0x%02x coverage=%u/%u/%u/%u running=%u heartbeat=%lu",
           static_cast<unsigned>(status.unread_slots),
           static_cast<unsigned>(status.coverage_by_slot[0]),
           static_cast<unsigned>(status.coverage_by_slot[1]),
           static_cast<unsigned>(status.coverage_by_slot[2]),
           static_cast<unsigned>(status.coverage_by_slot[3]),
           static_cast<unsigned>(status.running_tasks),
           static_cast<unsigned long>(status.heartbeat_sequence));
}

bool sync_power_sample(AppContext* app, std::uint32_t now_ms, bool idle, bool force) {
  if (app->audio.streaming()) {
    return false;
  }
  const auto interval = idle ? kIdlePowerLogIntervalMs : kActivePowerLogIntervalMs;
  if (!force && app->last_power_log_ms != 0 && now_ms - app->last_power_log_ms < interval) {
    return false;
  }
  app->last_power_log_ms = now_ms;

  const auto sample = app->battery.read();
  if (sample.error != ESP_OK) {
    ESP_LOGW(kTag, "power read failed: %s", esp_err_to_name(sample.error));
    return false;
  }

  const int sen_vin = read_optional_gpio(ai_keyboard::kExternalPowerSensePin);
  const int sen_chrg = read_optional_gpio(ai_keyboard::kChargeStatusPin);
  const auto charge_state = charge_state_for(app);
  const auto estimate = app->battery_estimator.update(sample.rail_mv, charge_state, now_ms);
  if (!sample.calibrated || !estimate.valid) {
    ESP_LOGW(kTag,
             "power sample ignored calibrated=%d measured_mv=%d",
             sample.calibrated ? 1 : 0,
             sample.rail_mv);
    return false;
  }

  if (estimate.full_anchor_updated) {
    esp_err_t save_error = ESP_OK;
    if (!app->config_store.save_battery_full_anchor_mv(estimate.full_anchor_mv, &save_error)) {
      ESP_LOGW(kTag,
               "battery full anchor save failed measured_mv=%d: %s",
               estimate.full_anchor_mv,
               esp_err_to_name(save_error));
    }
  }

  ESP_LOGI(kTag,
           "power raw=%d sense_mv=%d measured_mv=%d corrected_mv=%d battery=%u%% adc_calibrated=%s full_anchor=%d interval_ms=%lu sen_vin=%d sen_chrg=%d charge=%s",
           sample.raw,
           sample.sense_mv,
           sample.rail_mv,
           estimate.corrected_mv,
           static_cast<unsigned>(estimate.percent),
           sample.calibrated ? "true" : "false",
           estimate.full_anchor_mv,
           static_cast<unsigned long>(interval),
           sen_vin,
           sen_chrg,
           charge_state_name(charge_state));
  app->battery_raw_mv = static_cast<std::uint16_t>(
      std::clamp(sample.rail_mv, 0, static_cast<int>(UINT16_MAX)));
  app->battery_mv = static_cast<std::uint16_t>(
      std::clamp(estimate.corrected_mv, 0, static_cast<int>(UINT16_MAX)));
  app->battery_percent = estimate.percent;
  app->battery_sample_ms = now_ms;
  app->battery_sample_valid = true;
  app->ble.update_battery_level(app->battery_percent);
  return true;
}

void process_pending_status_refresh(AppContext* app, std::uint32_t now_ms) {
  ai_keyboard::StatusHidRequest usb_request{};
  std::uint32_t usb_request_epoch = 0;
  const bool usb_requested =
      app->usb.take_pending_status_request(&usb_request, &usb_request_epoch);
  const bool ble_requested =
      app->status_refresh_pending.exchange(false, std::memory_order_acq_rel);
  if (!usb_requested && !ble_requested) {
    return;
  }

  mark_activity(app, now_ms, usb_requested ? "usb_status_read" : "status_read");
  const bool fresh_requested =
      ble_requested ||
      (usb_requested &&
       (usb_request.flags & ai_keyboard::kStatusRequestFlagFresh) != 0);
  [[maybe_unused]] const bool sampled = fresh_requested &&
      sync_power_sample(app, now_ms, false, true);
  // Status reads on every management transport describe the same persisted
  // configuration fact. Runtime battery/connection fields may change, but
  // they must never erase the config bytes/CRC used for sync confirmation.
  const auto& applied_json = app->config_state.last_applied_json();
  const auto applied_crc =
      applied_json.empty() ? 0 : config_json_crc16(applied_json);

#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  // Capture one immutable probe generation and build one wire object for both
  // transports. A simultaneous BLE refresh must not replace it with a battery
  // view, and USB confirmation must not drop its diagnostic fields.
  const auto speaker_probe = app->speaker.probe_snapshot();
  const auto speaker_status_json = publish_config_status(
      app,
      "spk_probe",
      ai_keyboard::speaker_probe_result_name(speaker_probe.result),
      applied_json.size(),
      applied_crc,
      true,
      false,
      &speaker_probe);
  if (usb_requested &&
      (speaker_status_json.empty() ||
       !app->usb.queue_status_response_for_epoch(
           usb_request.request_id,
           speaker_status_json,
           usb_request_epoch))) {
    ESP_LOGW(kTag,
             "USB speaker probe status unavailable request=%08lx bytes=%u",
             static_cast<unsigned long>(usb_request.request_id),
             static_cast<unsigned>(speaker_status_json.size()));
  }
  return;
#else
  if (usb_requested) {
    // A USB status response doubles as recovery after a lost config ACK, so
    // its fingerprint must describe the currently applied/persisted config,
    // not the 0x13 request itself. An empty config is the valid factory case.
    const auto status_json = publish_config_status(
        app,
        "status",
        sampled ? "fresh" : "cached",
        applied_json.size(),
        applied_crc,
        true,
        true);
    if (status_json.empty() ||
        !app->usb.queue_status_response_for_epoch(
            usb_request.request_id, status_json, usb_request_epoch)) {
      ESP_LOGW(kTag,
               "USB STATUS response unavailable request=%08lx bytes=%u",
               static_cast<unsigned long>(usb_request.request_id),
               static_cast<unsigned>(status_json.size()));
    }
  }

  // GATT keeps detailed battery/power metadata, but carries the same stable
  // config fingerprint as USB so repeated reads and App reactivation can
  // revalidate device sync without relying on a stale push acknowledgement.
  if (ble_requested) {
    publish_config_status(app,
                          "battery",
                          sampled ? "fresh" : "cached",
                          applied_json.size(),
                          applied_crc,
                          true);
  }
#endif
}

void log_power_mode_transition(AppContext* app, PowerMode mode, std::uint32_t now_ms) {
  if (mode != app->logged_power_mode) {
    ESP_LOGI(kTag,
             "power mode=%s idle_elapsed_ms=%lu poll_ms=%lu external_power=%d charge=%s key_wake_verified=%d",
             power_mode_name(mode),
             static_cast<unsigned long>(now_ms - app->last_activity_ms),
             static_cast<unsigned long>(app->current_poll_interval_ms),
             external_power_status_active(app) ? 1 : 0,
             charge_state_name(charge_state_for(app)),
             app->key_wake_verified ? 1 : 0);
    app->logged_power_mode = mode;
  }
}

void log_heartbeat(AppContext* app, std::uint32_t now_ms, PowerMode mode) {
  const auto interval = power_mode_is_idle(mode) ? kIdleHeartbeatLogIntervalMs
                                                : kActiveHeartbeatLogIntervalMs;
  if (now_ms - app->last_heartbeat_log_ms < interval) {
    return;
  }
  app->last_heartbeat_log_ms = now_ms;
  ESP_LOGI(kTag,
           "heartbeat ms=%lu firmware=%s keys=%u leds=%u audio=%s power_mode=%s poll_ms=%lu light_sleep_entries=%lu light_sleep_ms=%lu last_light_sleep=%s block=%s",
           static_cast<unsigned long>(now_ms),
           kFirmwareVersion,
           static_cast<unsigned>(ai_keyboard::kKeyPins.size()),
           static_cast<unsigned>(ai_keyboard::kWs2812Count),
           ai_keyboard::kAudioHardwareAvailable ? "available" : "unavailable",
           power_mode_name(mode),
           static_cast<unsigned long>(app->current_poll_interval_ms),
           static_cast<unsigned long>(app->light_sleep_entries),
           static_cast<unsigned long>(app->light_sleep_total_ms),
           app->last_light_sleep_wake,
           app->light_sleep_block_reason);
}

void log_boot_summary() {
  ESP_LOGI(kTag, "%s firmware %s", kFirmwareName, kFirmwareVersion);
  ESP_LOGI(kTag,
           "board=%s target=esp32s3 keys=%u encoder=(GPIO%u,GPIO%u,GPIO%u) leds=%u led_gpio=GPIO%u key_wake=GPIO%d battery=(en GPIO%d adc GPIO%d)",
           ai_keyboard::kBoardName,
           static_cast<unsigned>(ai_keyboard::kKeyPins.size()),
           static_cast<unsigned>(ai_keyboard::kEncoderPinA),
           static_cast<unsigned>(ai_keyboard::kEncoderPinB),
           static_cast<unsigned>(ai_keyboard::kEncoderPressPin),
           static_cast<unsigned>(ai_keyboard::kWs2812Count),
           static_cast<unsigned>(ai_keyboard::kWs2812Pin),
           static_cast<int>(ai_keyboard::kKeyWakePin),
           static_cast<int>(ai_keyboard::kBatterySenseEnablePin),
           static_cast<int>(ai_keyboard::kBatterySenseAdcPin));
  ESP_LOGI(kTag,
           "v2 power pins sen_vin=GPIO%d active=%d sen_chrg=GPIO%d key_wake=GPIO%d pwr_en=GPIO%d active=%d",
           static_cast<int>(ai_keyboard::kExternalPowerSensePin),
           static_cast<int>(ai_keyboard::kExternalPowerSenseActiveLevel),
           static_cast<int>(ai_keyboard::kChargeStatusPin),
           static_cast<int>(ai_keyboard::kKeyWakePin),
           static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
           static_cast<int>(ai_keyboard::kPeripheralPowerEnableActiveLevel));
  ESP_LOGI(kTag,
           "v2 audio pins mic_i2s=(bclk GPIO%d ws GPIO%d din GPIO%d right) spk_i2s=(bclk GPIO%d ws GPIO%d dout GPIO%d left)",
           static_cast<int>(ai_keyboard::kMicI2sBclkPin),
           static_cast<int>(ai_keyboard::kMicI2sWsPin),
           static_cast<int>(ai_keyboard::kMicI2sDataInPin),
           static_cast<int>(ai_keyboard::kSpkI2sBclkPin),
           static_cast<int>(ai_keyboard::kSpkI2sWsPin),
           static_cast<int>(ai_keyboard::kSpkI2sDataOutPin));
  ESP_LOGI(kTag, "%s", ai_keyboard::kAudioUnavailableReason);
  ESP_LOGI(kTag,
           "power policy active_poll=%lums idle_after=%lums deep_idle_after=%lums idle_connected_poll=%lums deep_idle_connected_poll=%lums deep_idle_unverified_wake_poll=%lums idle_battery_poll=%lums idle_usb_poll=%lums active_battery_log=%lums idle_battery_log=%lums",
           static_cast<unsigned long>(kActivePollIntervalMs),
           static_cast<unsigned long>(kIdleAfterMs),
           static_cast<unsigned long>(kDeepIdleAfterMs),
           static_cast<unsigned long>(kIdleConnectedPollIntervalMs),
           static_cast<unsigned long>(kDeepIdleConnectedPollIntervalMs),
           static_cast<unsigned long>(kDeepIdleUnverifiedWakePollIntervalMs),
           static_cast<unsigned long>(kIdleBatteryPollIntervalMs),
           static_cast<unsigned long>(kIdleUsbPollIntervalMs),
           static_cast<unsigned long>(kActivePowerLogIntervalMs),
           static_cast<unsigned long>(kIdlePowerLogIntervalMs));
  ESP_LOGI(kTag,
           "controlled light sleep policy enabled_in_deep_idle=true nap_ms=%lums error_backoff_ms=%lums automatic_light_sleep=false",
           static_cast<unsigned long>(kControlledLightSleepNapMs),
           static_cast<unsigned long>(kControlledLightSleepErrorBackoffMs));
  ESP_LOGI(kTag,
           "BLE connection power active_interval=%u-%u active_latency=%u idle_interval=%u-%u idle_latency=%u deep_idle_interval=%u-%u deep_idle_latency=%u",
           static_cast<unsigned>(kBleActiveConnIntervalMin),
           static_cast<unsigned>(kBleActiveConnIntervalMax),
           static_cast<unsigned>(kBleActiveConnLatency),
           static_cast<unsigned>(kBleIdleConnIntervalMin),
           static_cast<unsigned>(kBleIdleConnIntervalMax),
           static_cast<unsigned>(kBleIdleConnLatency),
           static_cast<unsigned>(kBleDeepIdleConnIntervalMin),
           static_cast<unsigned>(kBleDeepIdleConnIntervalMax),
           static_cast<unsigned>(kBleDeepIdleConnLatency));
}

void wait_for_next_poll(AppContext* app) {
  ulTaskNotifyTake(pdTRUE, delay_ticks(app->current_poll_interval_ms));
}

void run_v2_led_bringup_probe(AppContext* app) {
  if constexpr (ai_keyboard::kPeripheralPowerEnablePin >= 0) {
    if (!external_power_status_active(app)) {
      ESP_LOGI(kTag, "V2 LED probe skipped on battery power");
      return;
    }

    // V2 uses one WS2812 data chain. Walk each pixel first, then light all five,
    // so bring-up can verify both single-pixel and multi-pixel control.
    constexpr std::array<easy_input::Rgb, ai_keyboard::kWs2812Count> kProbeColors{{
        {34, 0, 0},
        {28, 10, 0},
        {0, 28, 0},
        {0, 0, 34},
        {24, 24, 24},
    }};
    for (std::size_t index = 0; index < kProbeColors.size(); ++index) {
      ESP_LOGI(kTag, "V2 LED probe pixel=%u GPIO%u",
               static_cast<unsigned>(index),
               static_cast<unsigned>(ai_keyboard::kWs2812Pin));
      app->leds.show_pixel(index, kProbeColors[index]);
      vTaskDelay(delay_ticks(kV2BringupLedPowerProbeMs));
    }
    app->leds.show_raw_color({18, 18, 18});
    vTaskDelay(delay_ticks(kV2BringupLedPowerProbeMs));
    app->leds.show_status_event(easy_input::StatusLedEvent::ConfigMode, millis());
    vTaskDelay(delay_ticks(kV2BringupLedSelfTestMs));
    app->leds.update(millis());
  }
}

}  // namespace

extern "C" void app_main(void) {
#if defined(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC)
  // Link-only diagnostic: returns a retained table address and performs no
  // sound-bank read, erase or write. The volatile sink keeps this root
  // observable even if a future diagnostic build enables whole-program LTO.
  const void* volatile speaker_assets_link_anchor =
      easy_input_speaker_assets_diagnostic_link_anchor();
  static_cast<void>(speaker_assets_link_anchor);
#endif
  static AppContext app;
  app.platform_task = xTaskGetCurrentTaskHandle();

  log_boot_summary();
  const auto wake_cause = esp_sleep_get_wakeup_cause();
  restore_retained_power_cycle(&app, wake_cause);
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    // 深睡被 KEY_WAKE 唤醒:既是日志证据,也直接预置唤醒线已验证,
    // 本次开机无需再等真实边沿即可使用 750ms 深空闲轮询与再次深睡。
    app.key_wake_verified = true;
    app.last_wake_reason = "deep_sleep_key_wake";
    ESP_LOGI(kTag, "boot from deep sleep via KEY_WAKE");
  }
  configure_power_management();
  configure_board_status_inputs();
  ESP_ERROR_CHECK(app.peripheral_power.begin_awake());
  app.last_activity_ms = millis();
  ESP_ERROR_CHECK(easy_input::initialize_nvs_storage());
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  esp_err_t speaker_volume_error = ESP_OK;
  if (!app.config_store.load_speaker_volume(
          &app.speaker_volume_level, &speaker_volume_error)) {
    app.speaker_volume_level = ai_keyboard::kSpeakerVolumeDefault;
    if (speaker_volume_error != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(kTag,
               "speaker volume load failed; using default=%u: %s",
               static_cast<unsigned>(app.speaker_volume_level),
               esp_err_to_name(speaker_volume_error));
    }
  }
  app.speaker.set_volume_level(app.speaker_volume_level);
  ESP_LOGI(kTag,
           "speaker volume loaded level=%u percent=%u",
           static_cast<unsigned>(app.speaker_volume_level),
           static_cast<unsigned>(app.speaker_volume_level) * 10U);
#endif
  app.inputs.set_notify_task(xTaskGetCurrentTaskHandle());
  ESP_ERROR_CHECK(app.inputs.begin(millis()));
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  app.audio.set_audio_io_arbiter(&app.audio_io_arbiter);
#endif
  const esp_err_t audio_err = app.audio.begin();
  app.codex_slots.seed_capture_counter(esp_random());
  app.audio_ready = audio_err == ESP_OK;
  if (audio_err != ESP_OK) {
    ESP_LOGW(kTag, "Keyboard audio link unavailable: %s", esp_err_to_name(audio_err));
  } else {
    app.audio.set_activity_callback(signal_remote_activity, &app);
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  const auto codex_playback_err = app.codex_playback.begin(
      &app.audio,
      &app.speaker,
      &app.audio_io_arbiter,
      &app.codex_slots,
      app.platform_task);
  if (codex_playback_err != ESP_OK) {
    ESP_LOGW(kTag,
             "Codex LAN playback unavailable: %s",
             esp_err_to_name(codex_playback_err));
  }
#endif
  app.ble.set_status_read_callback(signal_status_read, &app);
  ESP_ERROR_CHECK(app.ble.begin());
  app.usb.set_status_request_callback(signal_usb_status_request, &app);
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    app.usb_physical_presence.reset(
        usb_vbus_status_present(), millis());
  }
  const esp_err_t usb_err = app.usb.begin();
  if (usb_err != ESP_OK) {
    ESP_LOGW(kTag, "USB HID unavailable: %s", esp_err_to_name(usb_err));
  }
  publish_config_status_without_payload(&app, "boot", "initializing", false);
  app.controlled_light_sleep_available = configure_light_sleep_wakeup();
  ESP_ERROR_CHECK(app.leds.begin());
  run_v2_led_bringup_probe(&app);
  ESP_ERROR_CHECK(app.battery.begin());
  std::int32_t stored_battery_full_mv = 0;
  esp_err_t battery_anchor_error = ESP_OK;
  if (app.config_store.load_battery_full_anchor_mv(&stored_battery_full_mv,
                                                   &battery_anchor_error)) {
    if (!app.battery_estimator.set_full_anchor_mv(stored_battery_full_mv)) {
      ESP_LOGW(kTag,
               "ignored invalid stored battery full anchor measured_mv=%ld",
               static_cast<long>(stored_battery_full_mv));
    }
  } else if (battery_anchor_error != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(kTag,
             "battery full anchor load failed: %s",
             esp_err_to_name(battery_anchor_error));
  }
  sync_power_sample(&app, millis(), false, true);
  load_stored_config(&app);
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  // Keep the microphone's frozen resource pool first. The platform loop then
  // starts local Store/USB and the required Wi-Fi sound service before the
  // optional Boot playback pipeline. Playback can be skipped or fail without
  // suppressing the listener advertised through the signed heartbeat.
  app.speaker_skip_boot_after_deep_sleep =
      wake_cause == ESP_SLEEP_WAKEUP_EXT1;
  if (app.speaker_skip_boot_after_deep_sleep) {
    ESP_LOGI(kTag, "speaker boot sound skipped after deep-sleep key wake");
  }
#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  app.speaker.mark_boot_pending(
      app.audio_io_arbiter.microphone_generation());
  const esp_err_t speaker_err =
      app.speaker.begin(app.platform_task, &app.audio_io_arbiter);
  if (speaker_err != ESP_OK) {
    ESP_LOGW(kTag,
             "speaker diagnostic unavailable: %s",
             esp_err_to_name(speaker_err));
  }
  if (speaker_err == ESP_OK) {
    if (wake_cause != ESP_SLEEP_WAKEUP_EXT1) {
      // The boot sound is a local device capability. Once the speaker is ready,
      // request it for cold/restart boots regardless of USB/VBUS, BLE, or App
      // state. A key wake from our 30-minute deep sleep is not a power-on event.
      app.speaker_probe_pending = true;
    } else {
      ESP_LOGI(kTag, "speaker boot sound skipped after deep-sleep key wake");
    }
  }
#endif
  sync_led_status(&app, millis());

  ESP_LOGI(kTag, "platform loop started; BLE HID + USB HID + S3C config enabled");
  while (true) {
    const auto now = millis();
    consume_remote_activity(&app, now);
    sync_usb_physical_presence(&app, now);
    if (app.inputs.take_wake_edge_count() > 0) {
      app.key_wake_verified = true;
      mark_activity(&app, now, "key_wake");
      app.ble.prepare_for_input_delivery();
    }
    if (app.inputs.take_input_edge_count() > 0) {
      mark_activity(&app, now, "input_edge");
      app.ble.prepare_for_input_delivery();
    }
    PowerMode mode = power_mode_for(&app, now);
    app.current_poll_interval_ms = poll_interval_ms(&app, now);
    track_power_mode(&app, mode, now);
    log_power_mode_transition(&app, mode, now);
    apply_pending_config(&app);
    apply_pending_mailbox_status(&app, millis());
    apply_pending_agent_status(&app, millis());
    reconcile_keyboard_transport_lifetimes(&app);
    reconcile_codex_route_lifetime(&app);
    // BLE profile preparation above may take long enough for both edges of a
    // short click to arrive. Poll with a fresh timestamp so queued ISR
    // snapshots and the settling sample always stay in chronological order.
    app.inputs.poll(millis(), handle_input_event, &app);
    persist_pending_speaker_volume(&app, millis());
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
    service_speaker(&app);
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
    app.codex_playback.poll();
    const auto resource_now = millis();
    app.speaker_assets.poll(
        resource_now,
        speaker_asset_resource_steps_allowed(
            &app, resource_now));
#endif
    app.usb.poll_pending_reports();
    app.ble.poll_input_delivery(millis());
    // Polling may have freed a bounded transport slot. Retry the coalesced
    // latest full keyboard state and every stateful App hotkey transition once
    // per loop, outside the input callback.
    flush_pending_keyboard_snapshot(&app);
    flush_pending_bridged_hotkey_events(&app);
    flush_input_led_feedback(&app);
    // Apply config above and deliver physical input first. A status request
    // arriving with the final config chunk must observe the new saved state.
    process_pending_status_refresh(&app, millis());
    check_encoder_press_config_hold(&app, millis());
    const auto platform_selection_now = millis();
    handle_platform_selection_result(
        &app,
        app.platform_selection.update(platform_selection_now),
        platform_selection_now);
    sync_led_status(&app, millis());
    sync_audio_power_hold(&app);
    mode = power_mode_for(&app, millis());
    app.leds.update(now);
    sync_power_sample(&app, now, power_mode_is_idle(mode), false);
    app.current_poll_interval_ms = poll_interval_ms(&app, millis());
    track_power_mode(&app, mode, millis());
    sync_ble_connection_power_profile(&app, mode);
    log_heartbeat(&app, now, mode);
    maybe_enter_deep_sleep(&app, millis());
    if (!try_controlled_light_sleep(&app, mode, millis())) {
      wait_for_next_poll(&app);
    }
  }
}
