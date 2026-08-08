#include "platform/keyboard_audio.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unistd.h>

#include "driver/i2s_std.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "keyboard/audio_control_wire.h"
#include "keyboard/audio_packet_wire.h"
#include "keyboard/board_pins.h"
#include "keyboard/config_receiver.h"
#include "keyboard/power_policy.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"

namespace easy_input {
namespace {

constexpr const char* kTag = "keyboard_audio";
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiDisconnectedBit = BIT1;
constexpr std::uint32_t kWifiConnectTimeoutMs = 12000;
constexpr std::uint32_t kAudioRuntimeMaxStreamMs = 90000;
constexpr std::uint32_t kWifiServiceGenerationMax = 0x000FFFFFU;
constexpr std::uint32_t kAudioFrameMs = 20;
constexpr std::uint32_t kAudioSampleRate = 16000;
// Keep the microphone and speaker on explicit, disjoint ESP32-S3
// controllers. I2S_NUM_AUTO would make the assignment depend on which lazy
// path allocates first and could make microphone availability change in a
// speaker-enabled build.
constexpr i2s_port_t kMicI2sController = I2S_NUM_0;
constexpr std::size_t kAudioSamplesPerFrame = kAudioSampleRate * kAudioFrameMs / 1000;
constexpr std::size_t kAudioFrameBytes = kAudioSamplesPerFrame * sizeof(std::int16_t);
constexpr std::uint32_t kAudioMicReadTimeoutMs = 80;
constexpr std::uint32_t kAudioMicReadRetryDelayMs = 10;
constexpr std::uint32_t kAudioMicReadRecoveryThreshold = 2;
constexpr std::uint32_t kAudioRecoveryRetryDelayMs = 80;
constexpr std::uint32_t kAudioMaxConsecutiveRecoveryFailures = 3;
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
constexpr std::uint32_t kSpeakerDrainWaitTimeoutMs = 750;
constexpr std::uint32_t kSpeakerDrainPollMs = 5;
#endif
constexpr std::size_t kAudioPacketMaxBytes =
    ai_keyboard::kAudioPacketHeaderBytes + kAudioFrameBytes +
    ai_keyboard::kAudioPacketAuthTagBytes;
// 采集和网络发送分属两个任务。队列覆盖约 1.28s 音频，短暂重连不会阻塞
// I2S；更长的中断会显式表现为序号缺口，由 Host 丢弃整次 capture。
constexpr std::size_t kAudioCaptureQueueFrames = 64;
constexpr std::uint32_t kAudioStreamTaskStack = 6144;
constexpr std::uint32_t kAudioCaptureTaskStack = 4096;
constexpr std::uint32_t kControlHeartbeatIntervalMs = 2000;
constexpr std::uint32_t kControlRecvTimeoutMs = 300;
constexpr std::uint32_t kControlConfigPollMs = 500;
constexpr std::uint32_t kControlWifiRetryMs = 10000;
constexpr std::uint32_t kControlAudioTriggerWifiRetryMs = 1000;
constexpr std::uint32_t kControlResolveRetryMs = 2000;
// 音频网络省电(录音/租约永远豁免):
// 网络活跃(<2min): 心跳 2s + WIFI_PS_MIN_MODEM;
// 网络空闲(>=2min): 心跳 4s + WIFI_PS_MAX_MODEM。该间隔必须小于 App
// 冷启动控制地址发现窗口(5s),保证 App 重启后无需先切换电脑麦克风即可发起录音;
// enabled 是由完整 Wi-Fi 音频端点推导出的有效硬件能力,不读取旧
// audio_enabled 持久位,也不跟随 App 本地麦克风来源切换;
// 能力启用时清醒期间保留 Wi-Fi 控制面,仅显式禁用能力或 deep sleep 前释放。
constexpr std::uint32_t kControlIdleHeartbeatIntervalMs = 4000;
// App 录音期间每 ~1s 续租;超过租约窗口没有 start/keepalive 就自动停流,
// 防止 stop 丢失后固件继续发无人消费的音频。仅对 Wi-Fi 控制启动的会话生效。
// 窗口只是 stop 丢失的兜底(另有 max_duration 硬上限),放宽到 15s 以容忍
// BLE 共存/射频拥挤下短暂的下行丢包,不再中途误停正常录音。
constexpr std::uint32_t kAudioSessionLeaseMs = 15000;

struct CapturedAudioFrame {
  std::array<std::uint8_t, kAudioFrameBytes> pcm{};
  std::uint32_t capture_sequence = 0;
  std::uint32_t capture_timestamp_ms = 0;
  std::uint16_t payload_bytes = 0;
  std::uint16_t rms_milli = 0;
};
static_assert(sizeof(CapturedAudioFrame) == 652);
constexpr std::size_t kAudioCaptureQueueBytes =
    kAudioCaptureQueueFrames * sizeof(CapturedAudioFrame);

// 串行化 esp_wifi set_config/start/connect 操作段(控制任务与音频任务
// 可能并发冷启动重连);不罩连接等待循环,避免长时间持锁。
struct WifiOpGuard {
  explicit WifiOpGuard(SemaphoreHandle_t handle) : handle_(handle) {
    if (handle_ != nullptr) {
      xSemaphoreTake(handle_, portMAX_DELAY);
    }
  }
  ~WifiOpGuard() {
    if (handle_ != nullptr) {
      xSemaphoreGive(handle_);
    }
  }
  WifiOpGuard(const WifiOpGuard&) = delete;
  WifiOpGuard& operator=(const WifiOpGuard&) = delete;

 private:
  SemaphoreHandle_t handle_;
};

// I2S channel creation, enable/disable, and recovery can be requested by
// configuration and stream tasks. Keep those lifecycle transitions serial.
struct MicOpGuard {
  explicit MicOpGuard(SemaphoreHandle_t handle) : handle_(handle) {
    if (handle_ != nullptr) {
      xSemaphoreTake(handle_, portMAX_DELAY);
    }
  }
  ~MicOpGuard() {
    if (handle_ != nullptr) {
      xSemaphoreGive(handle_);
    }
  }
  MicOpGuard(const MicOpGuard&) = delete;
  MicOpGuard& operator=(const MicOpGuard&) = delete;

 private:
  SemaphoreHandle_t handle_;
};

TickType_t delay_ticks(std::uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks == 0 ? 1 : ticks;
}

void copy_wifi_field(std::uint8_t* destination, std::size_t destination_size, const std::string& source) {
  if (destination_size == 0) {
    return;
  }
  std::memset(destination, 0, destination_size);
  const auto len = std::min(destination_size - 1, source.size());
  std::memcpy(destination, source.data(), len);
}

std::int16_t sample32_to_pcm16(std::int32_t sample) {
  const auto shifted = sample >> 16;
  if (shifted > std::numeric_limits<std::int16_t>::max()) {
    return std::numeric_limits<std::int16_t>::max();
  }
  if (shifted < std::numeric_limits<std::int16_t>::min()) {
    return std::numeric_limits<std::int16_t>::min();
  }
  return static_cast<std::int16_t>(shifted);
}

void write_pcm16_le(std::uint8_t* frame, std::size_t index, std::int16_t sample) {
  const auto offset = index * sizeof(std::int16_t);
  frame[offset] = static_cast<std::uint8_t>(sample & 0xFF);
  frame[offset + 1] = static_cast<std::uint8_t>((sample >> 8) & 0xFF);
}

std::uint32_t pcm16_rms_milli(const std::uint8_t* frame, std::size_t frame_bytes) {
  if (frame == nullptr || frame_bytes < sizeof(std::int16_t)) {
    return 0;
  }

  const auto samples = frame_bytes / sizeof(std::int16_t);
  std::uint64_t square_sum = 0;
  for (std::size_t index = 0; index < samples; ++index) {
    const auto offset = index * sizeof(std::int16_t);
    const auto sample = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(frame[offset]) |
        (static_cast<std::uint16_t>(frame[offset + 1]) << 8));
    const auto sample32 = static_cast<std::int32_t>(sample);
    square_sum += static_cast<std::uint64_t>(sample32 * sample32);
  }

  const auto rms = std::sqrt(static_cast<double>(square_sum) / static_cast<double>(samples));
  const auto milli = static_cast<std::uint32_t>(
      std::lround((rms * 1000.0) / static_cast<double>(std::numeric_limits<std::int16_t>::max())));
  return std::min<std::uint32_t>(1000, milli);
}

}  // namespace

esp_err_t KeyboardAudioLink::begin() {
  if (init_state_ == InitState::Ready) {
    return ESP_OK;
  }
  if (init_state_ == InitState::Failed) {
    return init_error_ == ESP_OK ? ESP_FAIL : init_error_;
  }
  if (init_state_ == InitState::Initializing) {
    return ESP_ERR_INVALID_STATE;
  }
  init_state_ = InitState::Initializing;
  init_error_ = ESP_FAIL;

  // Initialization is a one-shot boot transaction. Some ESP-IDF resources
  // below (default netif and registered event handlers) are process-lifetime
  // objects, so a partial failure must be latched instead of being retried by
  // configure()/preconnect and duplicating those resources under memory
  // pressure. A reboot is the explicit recovery boundary.
  const auto fail_initialization =
      [this](esp_err_t error, const char* status) -> esp_err_t {
    const auto latched_error = error == ESP_OK ? ESP_FAIL : error;
    lock();
    init_error_ = latched_error;
    init_state_ = InitState::Failed;
    if (status != nullptr) {
      capture_status_ = status;
    }
    unlock();
    return latched_error;
  };
  const auto release_audio_pool = [this]() {
    if (capture_worker_task_ != nullptr) {
      vTaskDelete(capture_worker_task_);
      capture_worker_task_ = nullptr;
    }
    if (stream_worker_task_ != nullptr) {
      vTaskDelete(stream_worker_task_);
      stream_worker_task_ = nullptr;
    }
    if (capture_done_ != nullptr) {
      vSemaphoreDelete(capture_done_);
      capture_done_ = nullptr;
    }
    if (capture_queue_ != nullptr) {
      vQueueDelete(capture_queue_);
      capture_queue_ = nullptr;
    }
    if (capture_queue_storage_ != nullptr) {
      heap_caps_free(capture_queue_storage_);
      capture_queue_storage_ = nullptr;
    }
    if (capture_queue_control_ != nullptr) {
      heap_caps_free(capture_queue_control_);
      capture_queue_control_ = nullptr;
    }
  };

  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
      ESP_LOGW(kTag, "failed to create audio mutex");
      return fail_initialization(ESP_ERR_NO_MEM, "aud_init");
    }
  }
  if (wifi_op_mutex_ == nullptr) {
    wifi_op_mutex_ = xSemaphoreCreateMutex();
    if (wifi_op_mutex_ == nullptr) {
      ESP_LOGW(kTag, "failed to create wifi op mutex");
      return fail_initialization(ESP_ERR_NO_MEM, "aud_init");
    }
  }
  if (mic_op_mutex_ == nullptr) {
    mic_op_mutex_ = xSemaphoreCreateMutex();
    if (mic_op_mutex_ == nullptr) {
      ESP_LOGW(kTag, "failed to create microphone op mutex");
      return fail_initialization(ESP_ERR_NO_MEM, "aud_init");
    }
  }

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "esp_netif_init failed: %s", esp_err_to_name(err));
    return fail_initialization(err, "aud_init");
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
    return fail_initialization(err, "aud_init");
  }

  wifi_events_ = xEventGroupCreate();
  if (wifi_events_ == nullptr) {
    ESP_LOGW(kTag, "failed to create Wi-Fi event group");
    return fail_initialization(ESP_ERR_NO_MEM, "aud_init");
  }
  xEventGroupSetBits(wifi_events_, kWifiDisconnectedBit);

  wifi_netif_ = esp_netif_create_default_wifi_sta();
  if (wifi_netif_ == nullptr) {
    ESP_LOGW(kTag, "failed to create Wi-Fi STA netif");
    return fail_initialization(ESP_FAIL, "aud_init");
  }

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init_config);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return fail_initialization(err, "aud_init");
  }
  wifi_service_generation_ = esp_random() & kWifiServiceGenerationMax;
  if (wifi_service_generation_ == 0U) {
    wifi_service_generation_ = 1U;
  }

  err = esp_event_handler_instance_register(WIFI_EVENT,
                                            ESP_EVENT_ANY_ID,
                                            &KeyboardAudioLink::event_handler,
                                            this,
                                            nullptr);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "Wi-Fi event handler registration failed: %s", esp_err_to_name(err));
    return fail_initialization(err, "aud_init");
  }
  err = esp_event_handler_instance_register(IP_EVENT,
                                            IP_EVENT_STA_GOT_IP,
                                            &KeyboardAudioLink::event_handler,
                                            this,
                                            nullptr);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "IP event handler registration failed: %s", esp_err_to_name(err));
    return fail_initialization(err, "aud_init");
  }

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
    return fail_initialization(err, "aud_init");
  }

  // The 64-frame PCM backlog is a bulk, non-DMA buffer. V2 has an explicit
  // 8 MB in-package PSRAM domain for this purpose; keeping the ~42 KiB queue
  // in internal SRAM starves I2S DMA, LwIP receive buffers and NimBLE once the
  // speaker Store/Wi-Fi services coexist. Require PSRAM instead of silently
  // falling back and moving the failure to a later subsystem.
  const bool psram_ready = esp_psram_is_initialized();
  const auto psram_size =
      psram_ready ? esp_psram_get_size() : 0U;
  const auto psram_free =
      psram_ready
          ? heap_caps_get_free_size(
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
          : 0U;
  if (!psram_ready || psram_size < kAudioCaptureQueueBytes ||
      psram_free < kAudioCaptureQueueBytes) {
    ESP_LOGW(
        kTag,
        "audio PSRAM domain unavailable total=%u free=%u required=%u",
        static_cast<unsigned>(psram_size),
        static_cast<unsigned>(psram_free),
        static_cast<unsigned>(kAudioCaptureQueueBytes));
    note_audio_resource_error(
        "aud_psram", "pool_psram_unavailable");
    return fail_initialization(ESP_ERR_NO_MEM, "aud_psram");
  }
  capture_queue_storage_ =
      static_cast<std::uint8_t*>(
          heap_caps_malloc(
              kAudioCaptureQueueBytes,
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  capture_queue_control_ =
      static_cast<StaticQueue_t*>(
          heap_caps_calloc(
              1U,
              sizeof(StaticQueue_t),
              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (capture_queue_storage_ == nullptr ||
      capture_queue_control_ == nullptr) {
    const char* failure_status =
        capture_queue_storage_ == nullptr ? "aud_psram"
                                          : "aud_pool_q";
    note_audio_resource_error(
        failure_status,
        capture_queue_storage_ == nullptr
            ? "pool_psram_storage_create"
            : "pool_internal_queue_control_create");
    release_audio_pool();
    return fail_initialization(ESP_ERR_NO_MEM, failure_status);
  }
  capture_queue_ = xQueueCreateStatic(
      kAudioCaptureQueueFrames,
      sizeof(CapturedAudioFrame),
      capture_queue_storage_,
      capture_queue_control_);
  if (capture_queue_ == nullptr) {
    note_audio_resource_error(
        "aud_psram", "pool_psram_queue_create");
    release_audio_pool();
    return fail_initialization(ESP_ERR_NO_MEM, "aud_psram");
  }
  capture_done_ = xSemaphoreCreateBinary();
  if (capture_done_ == nullptr) {
    note_audio_resource_error("aud_pool_sig", "pool_capture_signal_create");
    release_audio_pool();
    return fail_initialization(ESP_ERR_NO_MEM, "aud_pool_sig");
  }

  const auto stream_task_status = xTaskCreate(&KeyboardAudioLink::task_entry,
                                               "mic_udp",
                                               kAudioStreamTaskStack,
                                               this,
                                               tskIDLE_PRIORITY + 2,
                                               &stream_worker_task_);
  if (stream_task_status != pdPASS) {
    stream_worker_task_ = nullptr;
    note_audio_resource_error("aud_pool_tx", "pool_stream_task_create");
    release_audio_pool();
    return fail_initialization(ESP_ERR_NO_MEM, "aud_pool_tx");
  }

  const auto capture_task_status = xTaskCreate(&KeyboardAudioLink::capture_task_entry,
                                                "mic_capture",
                                                kAudioCaptureTaskStack,
                                                this,
                                                tskIDLE_PRIORITY + 3,
                                                &capture_worker_task_);
  if (capture_task_status != pdPASS) {
    capture_worker_task_ = nullptr;
    note_audio_resource_error("aud_pool_cap", "pool_capture_task_create");
    release_audio_pool();
    return fail_initialization(ESP_ERR_NO_MEM, "aud_pool_cap");
  }

  if (control_task_ == nullptr) {
    // getaddrinfo/esp_wifi/ESP_LOG 调用链吃栈,对照 mic_udp 任务(6144)再留余量。
    const auto task_status = xTaskCreate(&KeyboardAudioLink::control_task_entry,
                                         "mic_ctrl",
                                         8192,
                                         this,
                                         tskIDLE_PRIORITY + 1,
                                         &control_task_);
    if (task_status != pdPASS) {
      control_task_ = nullptr;
      note_audio_resource_error("aud_ctrl", "pool_control_task_create");
      release_audio_pool();
      return fail_initialization(ESP_ERR_NO_MEM, "aud_ctrl");
    }
  }

  mark_status("disabled");
  lock();
  init_error_ = ESP_OK;
  init_state_ = InitState::Ready;
  last_network_activity_tick_ = xTaskGetTickCount();
  unlock();
  constexpr std::uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  ESP_LOGI(kTag,
           "audio resource pool ready frames=%u queue_bytes=%u queue_domain=psram psram_total=%u psram_free=%u stream_stack=%u capture_stack=%u free=%u largest=%u minimum=%u",
           static_cast<unsigned>(kAudioCaptureQueueFrames),
           static_cast<unsigned>(kAudioCaptureQueueBytes),
           static_cast<unsigned>(psram_size),
           static_cast<unsigned>(
               heap_caps_get_free_size(
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(kAudioStreamTaskStack),
           static_cast<unsigned>(kAudioCaptureTaskStack),
           static_cast<unsigned>(heap_caps_get_free_size(kInternalHeapCaps)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalHeapCaps)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(kInternalHeapCaps)));
  return ESP_OK;
}

void KeyboardAudioLink::configure(const KeyboardAudioConfig& config) {
  bool should_preconnect = false;
  bool should_prepare_microphone = false;
  {
    // 配置更新和 deep-sleep disconnect 必须共用 wifi_op -> state 的锁序。
    // 否则断连任务可能在新配置保存后才写回 released 状态,把新端点再次置离线。
    WifiOpGuard wifi_guard(wifi_op_mutex_);
    lock();
    const bool endpoint_changed = config_.wifi_ssid != config.wifi_ssid ||
                                  config_.host != config.host ||
                                  config_.port != config.port;
    const bool speaker_service_changed =
        config_.speaker_sync_key_valid !=
            config.speaker_sync_key_valid ||
        config_.speaker_sync_key_epoch !=
            config.speaker_sync_key_epoch ||
        config_.speaker_sync_key != config.speaker_sync_key;
    const auto previous_ssid = config_.wifi_ssid;
    const auto previous_host = config_.host;
    const auto previous_port = config_.port;
    const bool reconnect_after_release =
        wifi_released_for_deep_sleep_ || wifi_disconnect_pending_;
    config_ = config;
    if (endpoint_changed || speaker_service_changed) {
      wifi_service_host_ipv4_ = 0U;
      wifi_service_host_ipv4_valid_ = false;
      wifi_service_lease_ = {};
      bump_wifi_service_generation_locked();
    }
    // 配置本身属于音频网络活动。必须先保存新配置再清除深睡释放状态,
    // 否则取消释放可能先用旧 SSID/host 触发一次无效重连。
    last_network_activity_tick_ = xTaskGetTickCount();
    deep_sleep_release_requested_ = false;
    deep_sleep_release_requested_tick_ = 0;
    wifi_released_for_deep_sleep_ = false;
    if (!config.enabled) {
      wifi_reconnect_requested_ = false;
      wifi_reconnect_requested_tick_ = 0;
      session_lifecycle_.request_stop(0, "audio_disabled");
      capture_status_ = "disabled";
      reset_diagnostics_locked();
      diagnostics_.stream_phase =
          ai_keyboard::audio_session_phase_name(session_lifecycle_.phase());
      diagnostics_.stop_reason = session_lifecycle_.stop_reason();
    } else {
      // disconnect 的完成态只由真实 WIFI_EVENT 驱动。配置若恰好落在异步
      // 断连窗口内,先记下重连意图;事件到达后控制任务会立即用新配置恢复。
      if (reconnect_after_release) {
        wifi_reconnect_requested_ = true;
        wifi_reconnect_requested_tick_ = xTaskGetTickCount();
      }
      note_stream_target_locked(config);
      if (endpoint_changed) {
        reset_diagnostics_locked();
        note_stream_target_locked(config);
        if (session_lifecycle_.active()) {
          session_lifecycle_.request_stop(0, "endpoint_changed");
          capture_status_ = "mic_restarting";
          ESP_LOGI(kTag,
                   "audio config endpoint changed while streaming old=%s/%s:%u new=%s/%s:%u",
                   previous_ssid.empty() ? "(empty)" : previous_ssid.c_str(),
                   previous_host.empty() ? "(empty)" : previous_host.c_str(),
                   static_cast<unsigned>(previous_port),
                   config.wifi_ssid.empty() ? "(empty)" : config.wifi_ssid.c_str(),
                   config.host.empty() ? "(empty)" : config.host.c_str(),
                   static_cast<unsigned>(config.port));
        }
      }
      if (init_state_ == InitState::Ready &&
          !session_lifecycle_.active() &&
          capture_status_ != "mic_restarting") {
        capture_status_ = "mic_preparing";
        should_preconnect = true;
        should_prepare_microphone = true;
      }
    }
    unlock();
  }

  if (!config.enabled) {
    stop_all_streams();
  } else {
    if (should_prepare_microphone) {
      const esp_err_t mic_err = prepare_microphone_channel();
      if (mic_err != ESP_OK) {
        ESP_LOGW(kTag, "keyboard mic pre-initialization failed: %s", esp_err_to_name(mic_err));
        note_audio_error("mic_prepare_failed", esp_err_to_name(mic_err), true, false);
      } else {
        lock();
        if (capture_status_ == "mic_preparing") {
          capture_status_ = "mic_ready";
        }
        unlock();
      }
    }
    if (should_preconnect) {
      preconnect_wifi("config");
    }
  }

  ESP_LOGI(kTag,
           "audio config enabled=%d ssid=%s host=%s port=%u",
           config.enabled ? 1 : 0,
           config.wifi_ssid.empty() ? "(empty)" : config.wifi_ssid.c_str(),
           config.host.empty() ? "(empty)" : config.host.c_str(),
           static_cast<unsigned>(config.port));
}

void KeyboardAudioLink::set_activity_callback(ActivityCallback callback, void* context) {
  lock();
  activity_callback_ = callback;
  activity_callback_context_ = context;
  unlock();
}

void KeyboardAudioLink::set_heartbeat_extension_callback(
    HeartbeatExtensionCallback callback,
    void* context) {
  lock();
  heartbeat_extension_callback_ = callback;
  heartbeat_extension_context_ = context;
  unlock();
  if (callback != nullptr) {
    request_heartbeat_refresh();
  }
}

void KeyboardAudioLink::request_heartbeat_refresh() {
  TaskHandle_t control_task = nullptr;
  lock();
  ++control_heartbeat_request_generation_;
  control_task = control_task_;
  unlock();
  if (control_task != nullptr) {
    xTaskNotifyGive(control_task);
  }
}

#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
void KeyboardAudioLink::set_audio_io_arbiter(
    ai_keyboard::AudioIoArbiter* arbiter) {
  audio_io_arbiter_ = arbiter;
}
#endif

void KeyboardAudioLink::start_stream(const char* reason, std::uint64_t session_id) {
  note_network_activity();
  const auto config = config_snapshot();
  if (!config.enabled) {
    return;
  }
  if (session_id == 0) {
    mark_status("missing_session");
    ESP_LOGW(kTag, "keyboard mic start rejected without session reason=%s",
             reason == nullptr ? "" : reason);
    return;
  }
  if (config.wifi_ssid.empty() || config.host.empty() || config.port == 0) {
    mark_status("config_incomplete");
    return;
  }
  if (init_state_ != InitState::Ready || capture_queue_ == nullptr ||
      capture_done_ == nullptr || stream_worker_task_ == nullptr ||
      capture_worker_task_ == nullptr) {
    note_audio_resource_error("aud_pool_off", "pool_unavailable_at_start");
    return;
  }

  bool wait_for_restart = false;
  lock();
  auto start_result = session_lifecycle_.request_start(session_id);
  if (start_result == ai_keyboard::AudioSessionStartResult::AlreadyActive) {
    if (session_lifecycle_.phase() == ai_keyboard::AudioSessionPhase::Streaming) {
      capture_status_ = "mic_streaming";
    }
    diagnostics_.session_id = session_lifecycle_.session_id();
    diagnostics_.session_generation = session_lifecycle_.generation();
    diagnostics_.stream_phase =
        ai_keyboard::audio_session_phase_name(session_lifecycle_.phase());
    ESP_LOGI(kTag,
             "keyboard mic session already active reason=%s session=%llu phase=%s",
             reason == nullptr ? "" : reason,
             static_cast<unsigned long long>(session_id),
             diagnostics_.stream_phase.c_str());
    unlock();
    return;
  }
  if (start_result == ai_keyboard::AudioSessionStartResult::NeedsStop) {
    const auto replaced_session = session_lifecycle_.session_id();
    if (session_lifecycle_.request_stop(0, "session_replaced")) {
      capture_status_ = "mic_stopping";
      diagnostics_.stream_phase = "stopping";
      ESP_LOGI(kTag,
               "keyboard mic replacing session old=%llu new=%llu reason=%s",
               static_cast<unsigned long long>(replaced_session),
               static_cast<unsigned long long>(session_id),
               reason == nullptr ? "" : reason);
      wait_for_restart = true;
    }
  }
  unlock();

  if (wait_for_restart && !wait_for_task_stop(2500)) {
    ESP_LOGW(kTag,
             "keyboard mic session restart timed out reason=%s session=%llu active_session=%llu",
             reason == nullptr ? "" : reason,
             static_cast<unsigned long long>(session_id),
             static_cast<unsigned long long>(active_session_id()));
    lock();
    capture_status_ = "mic_restart_timeout";
    diagnostics_.last_error = "session_restart_timeout";
    unlock();
    return;
  }

  lock();
  if (wait_for_restart) {
    start_result = session_lifecycle_.request_start(session_id);
  }
  if (start_result != ai_keyboard::AudioSessionStartResult::Started) {
    diagnostics_.last_error = "session_start_transition_failed";
    capture_status_ = "aud_session";
    unlock();
    return;
  }
  const auto generation = session_lifecycle_.generation();
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (audio_io_arbiter_ != nullptr &&
      !audio_io_arbiter_->request_microphone(generation)) {
    session_lifecycle_.finish(generation, "deep_sleep_quiescing");
    capture_status_ = "mic_sleeping";
    diagnostics_.session_id = session_id;
    diagnostics_.session_generation = generation;
    diagnostics_.stream_phase = "idle";
    diagnostics_.stop_reason = "deep_sleep_quiescing";
    unlock();
    ESP_LOGI(kTag,
             "keyboard mic start rejected during deep-sleep quiesce session=%llu generation=%lu",
             static_cast<unsigned long long>(session_id),
             static_cast<unsigned long>(generation));
    return;
  }
#endif
  lease_armed_ = false;
  capture_status_ = "mic_starting";
  task_owner_generation_ = generation;
  diagnostics_.session_id = session_id;
  diagnostics_.session_generation = generation;
  diagnostics_.stream_phase = "starting";
  diagnostics_.stop_reason.clear();
  stream_job_generation_ = generation;
  stream_job_session_id_ = session_id;
  task_ = stream_worker_task_;
  unlock();

  xTaskNotifyGive(stream_worker_task_);

  ESP_LOGI(kTag,
           "keyboard mic UDP worker dispatched reason=%s session=%llu generation=%lu",
           reason == nullptr ? "" : reason,
           static_cast<unsigned long long>(session_id),
           static_cast<unsigned long>(generation));
}

void KeyboardAudioLink::stop_stream(std::uint64_t session_id) {
  note_network_activity();
  lock();
  const auto active_session = session_lifecycle_.session_id();
  if (session_id != 0 && active_session != 0 && active_session != session_id) {
    ESP_LOGI(kTag,
             "keyboard mic stop ignored for stale session=%llu active_session=%llu",
             static_cast<unsigned long long>(session_id),
             static_cast<unsigned long long>(active_session));
    unlock();
    return;
  }
  session_lifecycle_.request_stop(session_id, "client_stop");
  lease_armed_ = false;
  if (task_ == nullptr && capture_status_ != "disabled") {
    capture_status_ = config_.enabled ? "mic_stopped" : "disabled";
  }
  diagnostics_.stream_phase =
      ai_keyboard::audio_session_phase_name(session_lifecycle_.phase());
  diagnostics_.stop_reason = session_lifecycle_.stop_reason();
  ESP_LOGI(kTag,
           "keyboard mic stop requested session=%llu task=%p",
           static_cast<unsigned long long>(session_id),
           task_);
  unlock();
}

void KeyboardAudioLink::stop_all_streams() {
  lock();
  session_lifecycle_.request_stop(0, "stop_all");
  lease_armed_ = false;
  if (task_ == nullptr && capture_status_ != "disabled") {
    capture_status_ = config_.enabled ? "mic_stopped" : "disabled";
  }
  diagnostics_.stream_phase =
      ai_keyboard::audio_session_phase_name(session_lifecycle_.phase());
  diagnostics_.stop_reason = session_lifecycle_.stop_reason();
  unlock();
}

std::string KeyboardAudioLink::capture_status() const {
  lock();
  auto status = capture_status_;
  unlock();
  return status;
}

KeyboardAudioDiagnostics KeyboardAudioLink::diagnostics() const {
  lock();
  auto diagnostics = diagnostics_;
  unlock();
  return diagnostics;
}

bool KeyboardAudioLink::streaming() const {
  lock();
  const bool active = task_ != nullptr;
  unlock();
  return active;
}

void KeyboardAudioLink::task_entry(void* arg) {
  auto* link = static_cast<KeyboardAudioLink*>(arg);
  if (link == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    link->lock();
    const auto generation = link->stream_job_generation_;
    const auto session_id = link->stream_job_session_id_;
    const bool owns_job =
        link->task_ == link->stream_worker_task_ &&
        link->task_owner_generation_ == generation && generation != 0 &&
        session_id != 0;
    link->unlock();
    if (owns_job) {
      link->run_audio_stream(generation, session_id);
    }
  }
}

void KeyboardAudioLink::capture_task_entry(void* arg) {
  auto* link = static_cast<KeyboardAudioLink*>(arg);
  if (link == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    link->lock();
    const auto generation = link->capture_job_generation_;
    const auto frame_queue = link->capture_queue_;
    const bool owns_job =
        link->task_ == link->stream_worker_task_ &&
        link->task_owner_generation_ == generation && generation != 0 &&
        frame_queue != nullptr;
    link->unlock();
    if (owns_job) {
      link->run_audio_capture(generation, frame_queue);
    } else if (generation != 0 && link->capture_done_ != nullptr) {
      // Never leave the sender blocked if a Stop raced with capture dispatch.
      link->lock();
      link->capture_completed_generation_ = generation;
      link->unlock();
      xSemaphoreGive(link->capture_done_);
    }
  }
}

void KeyboardAudioLink::control_task_entry(void* arg) {
  auto* link = static_cast<KeyboardAudioLink*>(arg);
  if (link != nullptr) {
    link->run_control_channel();
  }
  vTaskDelete(nullptr);
}

void KeyboardAudioLink::run_control_channel() {
  int sock = -1;
  sockaddr_in dest = {};
  bool dest_valid = false;
  std::string dest_host;
  std::uint16_t dest_port = 0;
  std::uint32_t heartbeat_seq = 0;
  std::uint32_t loop_count = 0;
  TickType_t last_heartbeat_tick = 0;
  std::uint32_t handled_heartbeat_request_generation = 0;
  // 首次重连留给开机路径的 preconnect,10s 后才由控制任务兜底重连,
  // 避免开机窗口内两个任务并发调用 esp_wifi 配置/连接。
  TickType_t last_wifi_attempt_tick = xTaskGetTickCount();
  TickType_t last_resolve_attempt_tick = 0;

  const auto close_control_socket = [&sock, &dest_valid]() {
    if (sock >= 0) {
      close(sock);
      sock = -1;
    }
    dest_valid = false;
  };

  // 控制任务把当前阶段写进诊断 last_error("ctrl:" 前缀),
  // 供 App 经 HID/GATT 状态读取在无串口环境下排障。
  const auto note_control_state = [this, &loop_count, &heartbeat_seq](const char* stage) {
    lock();
    diagnostics_.control_state = std::string(stage) +
                                 " loops=" + std::to_string(loop_count) +
                                 " hb=" + std::to_string(heartbeat_seq);
    unlock();
  };
  const auto wait_for_control_work = [](std::uint32_t timeout_ms) {
    // The generation is the source of truth; the task notification only cuts
    // short retry/config waits and may safely coalesce multiple refreshes.
    ulTaskNotifyTake(pdTRUE, delay_ticks(timeout_ms));
  };

  note_control_state("task_started");

  wifi_ps_type_t ps_applied = WIFI_PS_MIN_MODEM;
  bool ps_known = false;
  std::uint32_t observed_wifi_ingress_power_generation =
      wifi_ingress_power_generation_.load(
          std::memory_order_acquire);

  for (;;) {
    loop_count += 1;
    const auto ingress_power_generation =
        wifi_ingress_power_generation_.load(
            std::memory_order_acquire);
    if (ingress_power_generation !=
        observed_wifi_ingress_power_generation) {
      ps_known = false;
      observed_wifi_ingress_power_generation =
          ingress_power_generation;
    }
    const auto config = config_snapshot();
    const bool config_ready = config.enabled && !config.wifi_ssid.empty() &&
                              !config.host.empty() && config.port != 0;
    if (!config_ready) {
      close_control_socket();
      const bool wifi_stopped = stop_wifi_for_inactive_audio();
      if (wifi_stopped) {
        ps_known = false;
      }
      note_control_state(wifi_stopped ? "wifi_off_audio_disabled" : "waiting_config");
      wait_for_control_work(kControlConfigPollMs);
      continue;
    }

    lock();
    const bool session_active =
        session_lifecycle_.active() || lease_armed_ ||
        wifi_service_lease_.valid();
    const auto last_network_activity = last_network_activity_tick_;
    const bool release_requested = deep_sleep_release_requested_;
    const auto release_requested_tick = deep_sleep_release_requested_tick_;
    const bool released_for_deep_sleep = wifi_released_for_deep_sleep_;
    const bool disconnect_pending = wifi_disconnect_pending_;
    unlock();
    const auto now_tick = xTaskGetTickCount();
    const std::uint32_t idle_elapsed_ms =
        session_active ? 0
                       : static_cast<std::uint32_t>((now_tick - last_network_activity) *
                                                    portTICK_PERIOD_MS);
    const auto wifi_power_stage =
        ai_keyboard::evaluate_wifi_power_stage(
            idle_elapsed_ms, session_active, release_requested);
    const bool deep_release = wifi_power_stage == ai_keyboard::WifiPowerStage::Released;
    const std::uint32_t release_request_elapsed_ms =
        release_requested
            ? static_cast<std::uint32_t>((now_tick - release_requested_tick) *
                                         portTICK_PERIOD_MS)
            : 0;
    const bool release_quiesced =
        ai_keyboard::wifi_release_ready_for_deep_sleep(
            release_request_elapsed_ms, session_active, release_requested);
    const bool idle_throttled =
        wifi_power_stage == ai_keyboard::WifiPowerStage::Throttled;

    if (disconnect_pending) {
      close_control_socket();
      ps_known = false;
      note_control_state("wifi_disconnect_pending");
      wait_for_control_work(kControlConfigPollMs);
      continue;
    }

    if ((xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) == 0) {
      close_control_socket();
      ps_known = false;
      if ((deep_release && release_quiesced) || released_for_deep_sleep) {
        bool stay_released = false;
        {
          WifiOpGuard wifi_guard(wifi_op_mutex_);
          lock();
          const bool current_session_active =
              session_lifecycle_.active() || lease_armed_ ||
              wifi_service_lease_.valid();
          const auto current_request_elapsed_ms =
              deep_sleep_release_requested_
                  ? static_cast<std::uint32_t>(
                        (xTaskGetTickCount() - deep_sleep_release_requested_tick_) *
                        portTICK_PERIOD_MS)
                  : 0;
          const bool current_release_quiesced =
              ai_keyboard::wifi_release_ready_for_deep_sleep(
                  current_request_elapsed_ms,
                  current_session_active,
                  deep_sleep_release_requested_);
          stay_released = wifi_released_for_deep_sleep_ || current_release_quiesced;
          if (current_release_quiesced) {
            wifi_released_for_deep_sleep_ = true;
          }
          unlock();
        }
        if (stay_released) {
          note_control_state("wifi_released_for_deep_sleep");
          wait_for_control_work(kControlConfigPollMs);
          continue;
        }
      }
      lock();
      const bool stream_active = session_lifecycle_.active();
      const bool reconnect_requested = wifi_reconnect_requested_;
      const auto reconnect_requested_tick = wifi_reconnect_requested_tick_;
      unlock();
      const bool activity_since_attempt =
          last_wifi_attempt_tick != 0 &&
          static_cast<std::int32_t>(last_network_activity - last_wifi_attempt_tick) > 0;
      // 物理 PTT 只在 App 启动窗口内提升重连频率。超过 12s 后自动回到
      // 常规 10s 节奏,避免路由器不可用时一次按键造成持续高频耗电。
      const bool urgent_reconnect =
          reconnect_requested &&
          now_tick - reconnect_requested_tick < pdMS_TO_TICKS(kWifiConnectTimeoutMs);
      const auto retry_interval_ms = urgent_reconnect
                                         ? kControlAudioTriggerWifiRetryMs
                                         : kControlWifiRetryMs;
      if (!stream_active &&
          (last_wifi_attempt_tick == 0 || activity_since_attempt ||
           now_tick - last_wifi_attempt_tick >= pdMS_TO_TICKS(retry_interval_ms))) {
        last_wifi_attempt_tick = now_tick;
        prepare_wifi_connection(config, "control", false, false);
      }
      note_control_state("waiting_wifi");
      wait_for_control_work(kControlConfigPollMs);
      continue;
    }

    // 录音会话期间彻底关掉 modem sleep:省电档下行接收依赖 DTIM 唤醒窗,
    // 叠加 BLE 共存时 App 的续租包可能连续丢 5s+,租约会被误判过期而中途
    // 停流(上行音频不受影响,故障只出现在下行)。会话结束立即恢复省电。
    {
      // Speaker-service admission also changes Wi-Fi PS under this operation
      // lock. Re-read the lease after acquiring it so a stale idle decision
      // can never overwrite WIFI_PS_NONE after EISR admission.
      WifiOpGuard wifi_guard(wifi_op_mutex_);
      lock();
      const bool current_session_active =
          session_lifecycle_.active() || lease_armed_ ||
          wifi_service_lease_.valid();
      unlock();
      const wifi_ps_type_t ps_desired =
          current_session_active    ? WIFI_PS_NONE
          : idle_throttled ? WIFI_PS_MAX_MODEM
                           : WIFI_PS_MIN_MODEM;
      if (!ps_known || ps_desired != ps_applied) {
        const esp_err_t ps_err = esp_wifi_set_ps(ps_desired);
        if (ps_err == ESP_OK) {
          ps_applied = ps_desired;
          ps_known = true;
        }
      }
    }

    if (sock < 0) {
      sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
      if (sock < 0) {
        ESP_LOGW(kTag, "audio control socket failed errno=%d", errno);
        note_control_state("socket_failed");
        if (deep_release && release_quiesced) {
          ps_known = false;
          begin_wifi_release_for_deep_sleep();
        }
        wait_for_control_work(kControlResolveRetryMs);
        continue;
      }
      sockaddr_in local = {};
      local.sin_family = AF_INET;
      local.sin_addr.s_addr = htonl(INADDR_ANY);
      local.sin_port = 0;
      if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ESP_LOGW(kTag, "audio control bind failed errno=%d", errno);
        note_control_state("bind_failed");
        close_control_socket();
        if (deep_release && release_quiesced) {
          ps_known = false;
          begin_wifi_release_for_deep_sleep();
        }
        wait_for_control_work(kControlResolveRetryMs);
        continue;
      }
      timeval receive_timeout = {};
      receive_timeout.tv_sec = 0;
      receive_timeout.tv_usec = static_cast<suseconds_t>(kControlRecvTimeoutMs) * 1000;
      if (setsockopt(sock,
                     SOL_SOCKET,
                     SO_RCVTIMEO,
                     &receive_timeout,
                     sizeof(receive_timeout)) != 0) {
        const int timeout_errno = errno;
        ESP_LOGW(kTag,
                 "audio control receive timeout unavailable errno=%d",
                 timeout_errno);
        note_control_state("recv_timeout_failed");
        close_control_socket();
        wait_for_control_work(kControlResolveRetryMs);
        continue;
      }
      ESP_LOGI(kTag, "audio control channel ready host=%s port=%u",
               config.host.c_str(), static_cast<unsigned>(config.port));
    }

    if (!dest_valid || dest_host != config.host || dest_port != config.port) {
      const auto now = xTaskGetTickCount();
      if (last_resolve_attempt_tick != 0 &&
          now - last_resolve_attempt_tick < pdMS_TO_TICKS(kControlResolveRetryMs)) {
        if (deep_release && release_quiesced) {
          close_control_socket();
          ps_known = false;
          begin_wifi_release_for_deep_sleep();
        }
        wait_for_control_work(kControlConfigPollMs);
        continue;
      }
      last_resolve_attempt_tick = now;
      addrinfo hints = {};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;
      addrinfo* result = nullptr;
      const auto port_text = std::to_string(config.port);
      const int gai = getaddrinfo(config.host.c_str(), port_text.c_str(), &hints, &result);
      // lwIP 的 ai_addrlen 按 sockaddr_storage 上报,不能拿它和 sockaddr_in 比大小;
      // 校验地址族后按 sockaddr_in 定长拷贝。
      if (gai != 0 || result == nullptr || result->ai_addr == nullptr ||
          result->ai_addr->sa_family != AF_INET) {
        if (result != nullptr) {
          freeaddrinfo(result);
        }
        ESP_LOGW(kTag, "audio control resolve failed host=%s gai=%d", config.host.c_str(), gai);
        note_control_state("resolve_failed");
        close_control_socket();
        if (deep_release && release_quiesced) {
          ps_known = false;
          begin_wifi_release_for_deep_sleep();
        }
        continue;
      }
      std::memcpy(&dest, result->ai_addr, sizeof(dest));
      freeaddrinfo(result);
      dest_valid = true;
      dest_host = config.host;
      dest_port = config.port;
      lock();
      if (config_.host == config.host &&
          config_.port == config.port &&
          (!wifi_service_host_ipv4_valid_ ||
           wifi_service_host_ipv4_ !=
               dest.sin_addr.s_addr)) {
        wifi_service_host_ipv4_ = dest.sin_addr.s_addr;
        wifi_service_host_ipv4_valid_ = true;
        wifi_service_lease_ = {};
        bump_wifi_service_generation_locked();
      }
      unlock();
    }

    const auto heartbeat_interval_ms =
        idle_throttled ? kControlIdleHeartbeatIntervalMs : kControlHeartbeatIntervalMs;
    const auto now = xTaskGetTickCount();
    std::uint32_t heartbeat_request_generation = 0;
    lock();
    heartbeat_request_generation = control_heartbeat_request_generation_;
    unlock();
    const bool heartbeat_requested =
        heartbeat_request_generation != handled_heartbeat_request_generation;
    if (heartbeat_requested || last_heartbeat_tick == 0 ||
        now - last_heartbeat_tick >= pdMS_TO_TICKS(heartbeat_interval_ms)) {
      std::array<std::uint8_t,
                 ai_keyboard::kExtendedHeartbeatPacketBytes>
          heartbeat{};
      ai_keyboard::HeartbeatFlags flags;
      lock();
      flags.streaming = session_lifecycle_.active() &&
                        session_lifecycle_.phase() != ai_keyboard::AudioSessionPhase::Stopping;
      unlock();
      flags.audio_ready = true;
      std::size_t heartbeat_length =
          ai_keyboard::encode_heartbeat(
              heartbeat.data(),
              flags,
              active_session_id(),
              heartbeat_seq++);
      HeartbeatExtensionCallback extension_callback = nullptr;
      void* extension_context = nullptr;
      lock();
      extension_callback = heartbeat_extension_callback_;
      extension_context = heartbeat_extension_context_;
      unlock();
      if (extension_callback != nullptr) {
        const auto extended_length = extension_callback(
            extension_context,
            heartbeat.data(),
            heartbeat_length,
            heartbeat.size());
        if (extended_length == heartbeat.size()) {
          heartbeat_length = extended_length;
        }
      }
      const int sent = sendto(sock,
                              heartbeat.data(),
                              heartbeat_length,
                              0,
                              reinterpret_cast<sockaddr*>(&dest),
                              sizeof(dest));
      if (sent < 0) {
        ESP_LOGW(kTag, "audio control heartbeat send failed errno=%d", errno);
        note_control_state("heartbeat_send_failed");
        close_control_socket();
        if (deep_release && release_quiesced) {
          ps_known = false;
          begin_wifi_release_for_deep_sleep();
        }
        continue;
      }
      last_heartbeat_tick = now;
      handled_heartbeat_request_generation = heartbeat_request_generation;
      note_control_state(heartbeat_requested
                             ? "ready_ptt"
                             : (idle_throttled ? "ready_throttled" : "ready"));
    }

    // 多留 1 字节用于识别并拒绝超过统一配置上限的 UDP 数据报；
    // EICC 控制报文本身只使用前 36 字节。
    std::array<std::uint8_t, ai_keyboard::kConfigMaxJsonLen + 1> receive_buffer{};
    sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    const int received = recvfrom(sock,
                                  receive_buffer.data(),
                                  receive_buffer.size(),
                                  0,
                                  reinterpret_cast<sockaddr*>(&from),
                                  &from_len);
    if (received <= 0) {
      // 释放动作必须发生在一次完整接收等待之后。请求后的 5s 静默期覆盖
      // App 冷启动/重试窗口;任何 PTT 或配置包都会先由下方路径消费并取消请求。
      if (deep_release && release_quiesced && !released_for_deep_sleep) {
        close_control_socket();
        ps_known = false;
        begin_wifi_release_for_deep_sleep();
      }
      continue;
    }
    if (static_cast<std::size_t>(received) > ai_keyboard::kConfigMaxJsonLen) {
      ESP_LOGW(kTag,
               "audio control config/control datagram too large bytes=%d limit=%u",
               received,
               static_cast<unsigned>(ai_keyboard::kConfigMaxJsonLen));
      continue;
    }
    const auto command =
        ai_keyboard::parse_audio_control(receive_buffer.data(), static_cast<std::size_t>(received));
    if (!command.has_value()) {
      // 配置 JSON 直达通道:来源必须是已配置的上位机;交给主循环走
      // 与 BLE/USB 完全相同的应用+存盘+HID 回执路径。
      if (received > 2 && receive_buffer[0] == '{' &&
          from.sin_addr.s_addr == dest.sin_addr.s_addr) {
        lock();
        pending_config_json_.assign(reinterpret_cast<const char*>(receive_buffer.data()),
                                    static_cast<std::size_t>(received));
        unlock();
        note_remote_activity();
        ESP_LOGI(kTag, "audio control received config json bytes=%d", received);
      }
      continue;
    }

    auto status = ai_keyboard::AudioControlAckStatus::Ok;
    if (from.sin_addr.s_addr != dest.sin_addr.s_addr) {
      status = ai_keyboard::AudioControlAckStatus::Unauthorized;
    } else if (command->session_id == 0) {
      status = ai_keyboard::AudioControlAckStatus::BadRequest;
    } else {
      // 合法控制命令视作用户活动:立即回到活跃级(心跳 2s / PS_MIN)。
      note_remote_activity();
      switch (command->action) {
        case ai_keyboard::AudioControlAction::Start:
          start_stream("wifi_control", command->session_id);
          if (active_session_id() == command->session_id) {
            arm_session_lease(command->session_id);
          } else {
            status = ai_keyboard::AudioControlAckStatus::Unavailable;
          }
          break;
        case ai_keyboard::AudioControlAction::Stop:
          stop_stream(command->session_id);
          break;
        case ai_keyboard::AudioControlAction::Keepalive:
          if (!arm_session_lease(command->session_id)) {
            status = ai_keyboard::AudioControlAckStatus::Unavailable;
          }
          break;
      }
    }

    std::array<std::uint8_t, ai_keyboard::kControlAckPacketBytes> ack{};
    ai_keyboard::encode_control_ack(ack.data(),
                                    command->action,
                                    status,
                                    command->session_id,
                                    command->sequence);
    sendto(sock,
           ack.data(),
           ack.size(),
           0,
           reinterpret_cast<sockaddr*>(&from),
           from_len);
    if (command->action != ai_keyboard::AudioControlAction::Keepalive) {
      ESP_LOGI(kTag,
               "AUDIO_CTRL wifi %s session=%llu seq=%lu status=%u capture=%s",
               command->action == ai_keyboard::AudioControlAction::Start ? "start" : "stop",
               static_cast<unsigned long long>(command->session_id),
               static_cast<unsigned long>(command->sequence),
               static_cast<unsigned>(status),
               capture_status().c_str());
    }
  }
}

void KeyboardAudioLink::request_wifi_release_for_deep_sleep() {
  lock();
  const bool can_release = config_.enabled && !session_lifecycle_.active() &&
                           !lease_armed_ &&
                           !wifi_service_lease_.valid();
  if (can_release && !deep_sleep_release_requested_) {
    deep_sleep_release_requested_ = true;
    deep_sleep_release_requested_tick_ = xTaskGetTickCount();
  }
  unlock();
}

void KeyboardAudioLink::cancel_wifi_release_for_device_activity() {
  bool release_active = false;
  lock();
  release_active = deep_sleep_release_requested_ || wifi_released_for_deep_sleep_;
  unlock();
  if (!release_active) {
    return;
  }

  bool should_reconnect = false;
  {
    // 与断连路径共享同一把 Wi-Fi 操作锁,消除“取消深睡”和 disconnect
    // 交错导致设备已唤醒但控制通道仍离线的竞态。
    WifiOpGuard wifi_guard(wifi_op_mutex_);
    lock();
    should_reconnect = (wifi_released_for_deep_sleep_ || wifi_disconnect_pending_) &&
                       config_.enabled &&
                       !config_.wifi_ssid.empty() && !config_.host.empty() &&
                       config_.port != 0;
    deep_sleep_release_requested_ = false;
    deep_sleep_release_requested_tick_ = 0;
    wifi_released_for_deep_sleep_ = false;
    if (should_reconnect) {
      wifi_reconnect_requested_ = true;
      wifi_reconnect_requested_tick_ = xTaskGetTickCount();
    }
    unlock();
  }
}

void KeyboardAudioLink::prepare_for_audio_trigger() {
  note_network_activity();
  const bool connected = wifi_events_ != nullptr &&
                         (xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) != 0;
  bool config_ready = false;
  lock();
  config_ready = config_.enabled && !config_.wifi_ssid.empty() &&
                 !config_.host.empty() && config_.port != 0;
  if (config_ready) {
    // 物理 PTT 是一次确定性的控制面唤醒信号。无论 Wi-Fi 当前是否已连接，
    // 都请求控制任务尽快发送新心跳，让刚重启或刚切回前台的 App 学到路由。
    if (!connected || wifi_disconnect_pending_ || wifi_released_for_deep_sleep_) {
      wifi_reconnect_requested_ = true;
      wifi_reconnect_requested_tick_ = xTaskGetTickCount();
    }
  }
  unlock();
  if (config_ready) {
    request_heartbeat_refresh();
  }
}

void KeyboardAudioLink::note_network_activity() {
  bool release_active = false;
  lock();
  last_network_activity_tick_ = xTaskGetTickCount();
  release_active = deep_sleep_release_requested_ || wifi_released_for_deep_sleep_;
  unlock();
  if (release_active) {
    cancel_wifi_release_for_device_activity();
  }
}

void KeyboardAudioLink::note_remote_activity() {
  note_network_activity();
  ActivityCallback callback = nullptr;
  void* context = nullptr;
  lock();
  callback = activity_callback_;
  context = activity_callback_context_;
  unlock();
  if (callback != nullptr) {
    callback(context);
  }
}

bool KeyboardAudioLink::stop_wifi_for_inactive_audio() {
  WifiOpGuard wifi_guard(wifi_op_mutex_);

  lock();
  const bool can_stop = !config_.enabled && task_ == nullptr &&
                        !session_lifecycle_.active() && !lease_armed_ &&
                        !wifi_service_lease_.valid();
  if (can_stop) {
    wifi_reconnect_requested_ = false;
    wifi_reconnect_requested_tick_ = 0;
    deep_sleep_release_requested_ = false;
    deep_sleep_release_requested_tick_ = 0;
    wifi_released_for_deep_sleep_ = false;
    wifi_disconnect_pending_ = false;
  }
  unlock();

  if (!can_stop) {
    return false;
  }
  if (!wifi_started_) {
    xEventGroupClearBits(wifi_events_, kWifiConnectedBit);
    xEventGroupSetBits(wifi_events_, kWifiDisconnectedBit);
    return true;
  }

  const esp_err_t err = esp_wifi_stop();
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "esp_wifi_stop failed for inactive audio: %s", esp_err_to_name(err));
    return false;
  }

  wifi_started_ = false;
  xEventGroupClearBits(wifi_events_, kWifiConnectedBit);
  xEventGroupSetBits(wifi_events_, kWifiDisconnectedBit);
  ESP_LOGI(kTag, "audio disabled; Wi-Fi driver stopped");
  return true;
}

bool KeyboardAudioLink::begin_wifi_release_for_deep_sleep() {
  WifiOpGuard wifi_guard(wifi_op_mutex_);
  const auto now = xTaskGetTickCount();
  bool already_disconnected = false;
  std::uint32_t release_elapsed_ms = 0;

  lock();
  const bool session_active = session_lifecycle_.active() || lease_armed_ ||
                              wifi_service_lease_.valid();
  release_elapsed_ms = deep_sleep_release_requested_
                           ? static_cast<std::uint32_t>(
                                 (now - deep_sleep_release_requested_tick_) *
                                 portTICK_PERIOD_MS)
                           : 0;
  const bool release_ready = ai_keyboard::wifi_release_ready_for_deep_sleep(
      release_elapsed_ms, session_active, deep_sleep_release_requested_);
  if (!release_ready || wifi_disconnect_pending_) {
    unlock();
    return false;
  }

  already_disconnected =
      (xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) == 0;
  if (already_disconnected) {
    wifi_released_for_deep_sleep_ = true;
  } else {
    wifi_disconnect_pending_ = true;
  }
  wifi_reconnect_requested_ = false;
  wifi_reconnect_requested_tick_ = 0;
  unlock();

  if (already_disconnected) {
    mark_control_state("wifi_released_for_deep_sleep");
    return true;
  }

  xEventGroupClearBits(wifi_events_, kWifiDisconnectedBit);
  const esp_err_t err = esp_wifi_disconnect();
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "audio control requested Wi-Fi release after deep-sleep quiescence_ms=%lu",
             static_cast<unsigned long>(release_elapsed_ms));
    mark_control_state("wifi_disconnect_pending");
    return true;
  }

  lock();
  wifi_disconnect_pending_ = false;
  if (err == ESP_ERR_WIFI_NOT_CONNECT) {
    wifi_released_for_deep_sleep_ = true;
  }
  unlock();
  if (err == ESP_ERR_WIFI_NOT_CONNECT) {
    xEventGroupClearBits(wifi_events_, kWifiConnectedBit);
    xEventGroupSetBits(wifi_events_, kWifiDisconnectedBit);
    mark_control_state("wifi_released_for_deep_sleep");
    return true;
  }

  ESP_LOGW(kTag,
           "audio control deep-sleep Wi-Fi release failed: %s",
           esp_err_to_name(err));
  return false;
}

bool KeyboardAudioLink::take_pending_config(std::string* json) {
  if (json == nullptr) {
    return false;
  }
  lock();
  if (pending_config_json_.empty()) {
    unlock();
    return false;
  }
  *json = std::move(pending_config_json_);
  pending_config_json_.clear();
  unlock();
  return true;
}

bool KeyboardAudioLink::wifi_active_or_streaming() const {
  const bool wifi_connected =
      wifi_events_ != nullptr &&
      (xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) != 0;
  lock();
  const bool streaming = session_lifecycle_.active() ||
                         wifi_service_lease_.valid();
  unlock();
  return wifi_connected || streaming;
}

KeyboardWifiServiceSnapshot
KeyboardAudioLink::wifi_service_snapshot() const {
  KeyboardWifiServiceSnapshot snapshot{};
  const bool connected =
      wifi_events_ != nullptr &&
      (xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) != 0;
  lock();
  snapshot.configured =
      config_.enabled && !config_.wifi_ssid.empty() &&
      !config_.host.empty() && config_.port != 0U;
  snapshot.connected = connected;
  snapshot.disconnect_pending = wifi_disconnect_pending_;
  snapshot.host_ipv4_valid = wifi_service_host_ipv4_valid_;
  snapshot.host_ipv4 = wifi_service_host_ipv4_;
  snapshot.host_port = config_.port;
  snapshot.generation = wifi_service_generation_;
  snapshot.speaker_sync_key = config_.speaker_sync_key;
  snapshot.speaker_sync_key_epoch =
      config_.speaker_sync_key_epoch;
  snapshot.speaker_sync_key_valid =
      config_.speaker_sync_key_valid;
  unlock();
  return snapshot;
}

bool KeyboardAudioLink::acquire_wifi_service_lease(
    std::uint32_t expected_generation,
    KeyboardWifiServiceLease* lease) {
  if (lease == nullptr || expected_generation == 0U) {
    return false;
  }
  *lease = {};
  note_network_activity();
  WifiOpGuard wifi_guard(wifi_op_mutex_);
  const bool connected =
      wifi_events_ != nullptr &&
      (xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) != 0;
  lock();
  const bool ready =
      connected && !wifi_disconnect_pending_ &&
      wifi_service_generation_ == expected_generation &&
      wifi_service_host_ipv4_valid_ &&
      config_.speaker_sync_key_valid &&
      config_.speaker_sync_key_epoch != 0U &&
      !wifi_service_lease_.valid() &&
      next_wifi_service_lease_id_ != 0U;
  if (ready) {
    wifi_service_lease_ = {
        next_wifi_service_lease_id_,
        wifi_service_generation_,
    };
    next_wifi_service_lease_id_ =
        next_wifi_service_lease_id_ ==
                std::numeric_limits<std::uint32_t>::max()
            ? 0U
            : next_wifi_service_lease_id_ + 1U;
    *lease = wifi_service_lease_;
  }
  unlock();
  if (!ready) {
    return false;
  }

  // EISR Ok is a data-plane admission promise, not just an in-memory lease.
  // Make downlink ingress active synchronously before the caller can publish
  // that promise. The same operation lock is used by the control task's idle
  // PS transition, so an older idle decision cannot race this write.
  const esp_err_t ingress_err =
      esp_wifi_set_ps(WIFI_PS_NONE);
  if (ingress_err != ESP_OK) {
    lock();
    if (wifi_service_lease_.lease_id == lease->lease_id &&
        wifi_service_lease_.generation == lease->generation) {
      wifi_service_lease_ = {};
    }
    unlock();
    ESP_LOGW(
        kTag,
        "wifi service ingress activation failed: %s",
        esp_err_to_name(ingress_err));
    *lease = {};
    return false;
  }

  wifi_ingress_power_generation_.fetch_add(
      1U, std::memory_order_acq_rel);
  const bool still_connected =
      wifi_events_ != nullptr &&
      (xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) != 0;
  lock();
  const bool exact =
      still_connected && !wifi_disconnect_pending_ &&
      wifi_service_generation_ == expected_generation &&
      wifi_service_lease_.lease_id == lease->lease_id &&
      wifi_service_lease_.generation == lease->generation;
  if (!exact &&
      wifi_service_lease_.lease_id == lease->lease_id &&
      wifi_service_lease_.generation == lease->generation) {
    wifi_service_lease_ = {};
  }
  unlock();
  if (!exact) {
    *lease = {};
    if (control_task_ != nullptr) {
      xTaskNotifyGive(control_task_);
    }
    return false;
  }
  ESP_LOGI(
      kTag,
      "wifi service ingress active mode=none generation=%lu lease=%lu",
      static_cast<unsigned long>(lease->generation),
      static_cast<unsigned long>(lease->lease_id));
  return true;
}

bool KeyboardAudioLink::release_wifi_service_lease(
    const KeyboardWifiServiceLease& lease) {
  if (!lease.valid()) {
    return false;
  }
  WifiOpGuard wifi_guard(wifi_op_mutex_);
  lock();
  const bool exact =
      wifi_service_lease_.lease_id == lease.lease_id &&
      wifi_service_lease_.generation == lease.generation;
  if (exact) {
    wifi_service_lease_ = {};
  }
  unlock();
  if (exact && control_task_ != nullptr) {
    // Restore the current idle PS policy promptly instead of waiting for the
    // UDP receive timeout to expire.
    xTaskNotifyGive(control_task_);
  }
  return exact;
}

void KeyboardAudioLink::note_wifi_service_activity() {
  note_remote_activity();
}

void KeyboardAudioLink::bump_wifi_service_generation_locked() {
  wifi_service_generation_ =
      wifi_service_generation_ ==
              kWifiServiceGenerationMax
          ? 1U
          : wifi_service_generation_ + 1U;
}

bool KeyboardAudioLink::arm_session_lease(std::uint64_t session_id) {
  lock();
  const bool matched = session_id != 0 && session_lifecycle_.session_id() == session_id &&
                       session_lifecycle_.phase() != ai_keyboard::AudioSessionPhase::Stopping;
  if (matched) {
    lease_armed_ = true;
    lease_deadline_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(kAudioSessionLeaseMs);
  }
  unlock();
  return matched;
}

bool KeyboardAudioLink::session_lease_expired() const {
  lock();
  const bool expired = lease_armed_ &&
                       static_cast<std::int32_t>(xTaskGetTickCount() - lease_deadline_tick_) >= 0;
  unlock();
  return expired;
}

void KeyboardAudioLink::event_handler(void* arg,
                                      esp_event_base_t event_base,
                                      std::int32_t event_id,
                                      void* event_data) {
  (void)event_data;
  auto* link = static_cast<KeyboardAudioLink*>(arg);
  if (link == nullptr || link->wifi_events_ == nullptr) {
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    bool should_reconnect = false;
    link->lock();
    const bool coordinated_release = link->wifi_disconnect_pending_ &&
                                     link->deep_sleep_release_requested_;
    link->wifi_disconnect_pending_ = false;
    link->wifi_service_host_ipv4_ = 0U;
    link->wifi_service_host_ipv4_valid_ = false;
    link->wifi_service_lease_ = {};
    link->bump_wifi_service_generation_locked();
    if (coordinated_release) {
      link->wifi_released_for_deep_sleep_ = true;
    } else {
      // An unexpected AP/link drop during microphone or speaker traffic must
      // enter the existing 12-second urgent reconnect window immediately.
      // Otherwise the ordinary 10-second idle retry races the App's carrier
      // discovery window and a present keyboard is reported as missing.
      should_reconnect =
          link->config_.enabled &&
          !link->config_.wifi_ssid.empty() &&
          !link->config_.host.empty() &&
          link->config_.port != 0;
      if (should_reconnect) {
        link->wifi_reconnect_requested_ = true;
        link->wifi_reconnect_requested_tick_ =
            xTaskGetTickCount();
        link->last_network_activity_tick_ =
            link->wifi_reconnect_requested_tick_;
      }
    }
    link->unlock();
    xEventGroupClearBits(link->wifi_events_, kWifiConnectedBit);
    xEventGroupSetBits(link->wifi_events_, kWifiDisconnectedBit);
    link->mark_control_state(coordinated_release ? "wifi_released_for_deep_sleep"
                                                 : "wifi_disconnected");
    if (should_reconnect) {
      // Wake the control task now; do not wait for its normal polling cadence.
      link->request_heartbeat_refresh();
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    link->lock();
    link->wifi_disconnect_pending_ = false;
    link->wifi_reconnect_requested_ = false;
    link->wifi_reconnect_requested_tick_ = 0;
    link->wifi_service_host_ipv4_ = 0U;
    link->wifi_service_host_ipv4_valid_ = false;
    link->wifi_service_lease_ = {};
    link->bump_wifi_service_generation_locked();
    link->unlock();
    xEventGroupClearBits(link->wifi_events_, kWifiDisconnectedBit);
    xEventGroupSetBits(link->wifi_events_, kWifiConnectedBit);
    link->mark_control_state("wifi_ready");
  }
}

void KeyboardAudioLink::run_audio_stream(std::uint32_t generation,
                                         std::uint64_t session_id) {
  const auto config = config_snapshot();
  if (session_id == 0 || !should_run_stream(generation)) {
    finish_stream_task(generation, "invalid_task_owner", "missing_session");
    return;
  }
  if (!config.speaker_sync_key_valid) {
    finish_stream_task(generation, "auth_missing", "auth_missing");
    return;
  }

  lock();
  reset_diagnostics_locked();
  note_stream_target_locked(config);
  diagnostics_.session_id = session_id;
  diagnostics_.session_generation = generation;
  diagnostics_.stream_phase = "starting";
  unlock();

  esp_err_t err = ensure_wifi_ready(config, generation);
  if (err != ESP_OK) {
    if (should_run_stream(generation)) {
      ESP_LOGW(kTag, "keyboard mic UDP Wi-Fi failed: %s", esp_err_to_name(err));
      note_audio_error("wifi_failed", esp_err_to_name(err), false, false);
      finish_stream_task(generation, "wifi_start_failed", "wifi_failed");
    } else {
      finish_stream_task(generation, "stream_stop", "mic_stopped");
    }
    return;
  }

#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (!wait_for_microphone_hardware(generation)) {
    if (should_run_stream(generation)) {
      ESP_LOGW(kTag,
               "keyboard mic waited too long for speaker drain generation=%lu",
               static_cast<unsigned long>(generation));
      note_audio_error("speaker_busy",
                       "speaker_drain_timeout",
                       false,
                       false);
      finish_stream_task(generation,
                         "speaker_drain_timeout",
                         "speaker_busy");
    } else {
      finish_stream_task(generation, "stream_stop", "mic_stopped");
    }
    return;
  }
#endif

  int sock = -1;
  addrinfo* target = nullptr;
  const auto close_udp_target = [&sock, &target]() {
    if (sock >= 0) {
      shutdown(sock, 0);
      close(sock);
      sock = -1;
    }
    if (target != nullptr) {
      freeaddrinfo(target);
      target = nullptr;
    }
  };
  const auto open_udp_target = [&config, &sock, &target, &close_udp_target]() -> esp_err_t {
    close_udp_target();
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    const auto port_text = std::to_string(config.port);
    const int gai = getaddrinfo(config.host.c_str(), port_text.c_str(), &hints, &target);
    if (gai != 0 || target == nullptr) {
      return ESP_ERR_NOT_FOUND;
    }
    sock = socket(target->ai_family, target->ai_socktype, target->ai_protocol);
    if (sock < 0) {
      close_udp_target();
      return ESP_FAIL;
    }
    return ESP_OK;
  };

  err = open_udp_target();
  if (err != ESP_OK) {
    note_audio_error("udp_open_failed", esp_err_to_name(err), false, true);
    finish_stream_task(generation, "udp_open_failed", "udp_open_failed");
    return;
  }

  err = ensure_microphone_ready();
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "keyboard mic I2S init failed: %s", esp_err_to_name(err));
    close_udp_target();
    note_audio_error("mic_i2s_failed", esp_err_to_name(err), true, false);
    finish_stream_task(generation, "i2s_start_failed", "mic_i2s_failed");
    return;
  }

  QueueHandle_t frame_queue = capture_queue_;
  if (frame_queue == nullptr || capture_done_ == nullptr ||
      capture_worker_task_ == nullptr ||
      xQueueReset(frame_queue) != pdPASS) {
    shutdown_microphone();
    close_udp_target();
    note_audio_resource_error("aud_pool_off", "pool_queue_unavailable");
    finish_stream_task(generation, "capture_queue_unavailable", "aud_pool_off");
    return;
  }

  while (xSemaphoreTake(capture_done_, 0) == pdTRUE) {
  }
  lock();
  capture_job_generation_ = generation;
  capture_completed_generation_ = 0;
  unlock();
  xTaskNotifyGive(capture_worker_task_);

  mark_stream_phase(generation, ai_keyboard::AudioSessionPhase::Streaming, "mic_streaming");
  std::array<std::uint8_t, kAudioPacketMaxBytes> packet{};
  CapturedAudioFrame pending_frame{};
  bool has_pending_frame = false;
  bool capture_finished = false;
  std::uint32_t sent_packets = 0;
  std::uint32_t sent_bytes = 0;
  const char* stop_reason = "stream_stop";
  const char* final_status = "mic_stopped";
  bool clean_capture_end = false;
  bool capture_drained = false;
  const auto started_tick = xTaskGetTickCount();
  const auto take_capture_completion =
      [this, generation](TickType_t wait_ticks) {
    if (xSemaphoreTake(capture_done_, wait_ticks) != pdTRUE) {
      return false;
    }
    lock();
    const bool matches = capture_completed_generation_ == generation;
    unlock();
    return matches;
  };

  while (true) {
    const bool stream_running = should_run_stream(generation);
    if (!stream_running) {
      lock();
      const auto lifecycle_stop_reason = session_lifecycle_.stop_reason();
      unlock();
      if (lifecycle_stop_reason == "client_stop") {
        stop_reason = "client_stop";
        final_status = "mic_sent";
        clean_capture_end = true;
      } else if (lifecycle_stop_reason == "max_duration") {
        stop_reason = "max_duration";
        final_status = "mic_sent";
        clean_capture_end = true;
      }
    }
    const auto elapsed_ms =
        static_cast<std::uint32_t>((xTaskGetTickCount() - started_tick) * portTICK_PERIOD_MS);
    if (stream_running && elapsed_ms >= kAudioRuntimeMaxStreamMs) {
      stop_reason = "max_duration";
      final_status = "mic_sent";
      clean_capture_end = true;
      lock();
      session_lifecycle_.request_stop(0, stop_reason);
      lease_armed_ = false;
      unlock();
      continue;
    }
    if (stream_running && session_lease_expired()) {
      stop_reason = "lease_expired";
      final_status = "mic_lease_expired";
      lock();
      session_lifecycle_.request_stop(0, stop_reason);
      lease_armed_ = false;
      unlock();
      break;
    }

    if (!has_pending_frame) {
      if (xQueueReceive(frame_queue, &pending_frame, delay_ticks(kAudioFrameMs)) == pdTRUE) {
        has_pending_frame = true;
      } else {
        capture_finished = take_capture_completion(0);
        if (capture_finished) {
          if (clean_capture_end) {
            capture_drained = true;
          } else {
            stop_reason = "capture_stopped";
            final_status = "mic_read_failed";
          }
          break;
        }
        continue;
      }
    }

    if (pending_frame.capture_sequence != sent_packets) {
      stop_reason = "capture_sequence_gap";
      final_status = "udp_send_failed";
      note_audio_error(final_status, stop_reason, false, true);
      lock();
      session_lifecycle_.request_stop(0, stop_reason);
      lease_armed_ = false;
      unlock();
      break;
    }

    const ai_keyboard::AudioPacketMetadata metadata{
        .session_id = session_id,
        .capture_sequence = pending_frame.capture_sequence,
        .sample_rate = kAudioSampleRate,
        .capture_timestamp_ms = pending_frame.capture_timestamp_ms,
        .frame_samples = static_cast<std::uint16_t>(kAudioSamplesPerFrame),
        .payload_bytes = pending_frame.payload_bytes,
    };
    if (!ai_keyboard::encode_audio_packet_header(packet.data(), packet.size(), metadata)) {
      stop_reason = "packet_encode_failed";
      final_status = "mic_task_failed";
      note_audio_error(final_status, "invalid_audio_metadata", false, true);
      lock();
      session_lifecycle_.request_stop(0, stop_reason);
      lease_armed_ = false;
      unlock();
      break;
    }
    std::memcpy(packet.data() + ai_keyboard::kAudioPacketHeaderBytes,
                pending_frame.pcm.data(),
                pending_frame.payload_bytes);
    const auto authenticated_bytes =
        ai_keyboard::kAudioPacketHeaderBytes + pending_frame.payload_bytes;
    std::array<std::uint8_t, 32> authentication{};
    const auto* digest = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (digest == nullptr ||
        mbedtls_md_hmac(digest,
                        config.speaker_sync_key.data(),
                        config.speaker_sync_key.size(),
                        packet.data(),
                        authenticated_bytes,
                        authentication.data()) != 0) {
      stop_reason = "packet_auth_failed";
      final_status = "mic_task_failed";
      note_audio_error(final_status, "packet_auth_failed", false, true);
      lock();
      session_lifecycle_.request_stop(0, stop_reason);
      lease_armed_ = false;
      unlock();
      break;
    }
    std::memcpy(packet.data() + authenticated_bytes,
                authentication.data(),
                ai_keyboard::kAudioPacketAuthTagBytes);
    authentication.fill(0U);
    const auto packet_bytes =
        authenticated_bytes + ai_keyboard::kAudioPacketAuthTagBytes;
    const int sent =
        sendto(sock, packet.data(), packet_bytes, 0, target->ai_addr, target->ai_addrlen);
    if (sent < 0) {
      const int send_errno = errno;
      ESP_LOGW(kTag,
               "keyboard mic UDP send failed errno=%d session=%llu generation=%lu packets=%lu; recovering",
               send_errno,
               static_cast<unsigned long long>(session_id),
               static_cast<unsigned long>(generation),
               static_cast<unsigned long>(pending_frame.capture_sequence));
      mark_stream_phase(generation, ai_keyboard::AudioSessionPhase::Recovering,
                        "udp_recovering");
      lock();
      diagnostics_.send_errors += 1;
      diagnostics_.recovery_count += 1;
      diagnostics_.last_error = std::string("udp_send:") + std::to_string(send_errno);
      unlock();
      close_udp_target();
      err = ensure_wifi_ready(config, generation);
      if (err == ESP_OK) {
        err = open_udp_target();
      }
      if (err != ESP_OK || (!should_run_stream(generation) && !clean_capture_end)) {
        stop_reason = "udp_recovery_failed";
        final_status = "udp_send_failed";
        note_audio_error(final_status, esp_err_to_name(err), false, true);
        lock();
        session_lifecycle_.request_stop(0, stop_reason);
        lease_armed_ = false;
        unlock();
        break;
      }
      mark_stream_phase(generation, ai_keyboard::AudioSessionPhase::Streaming,
                        "mic_streaming");
      // pending_frame remains owned by the sender and is retried after the
      // socket recovers. Capture continues independently into frame_queue.
      continue;
    }

    sent_packets += 1;
    sent_bytes += pending_frame.payload_bytes;
    note_audio_packet(pending_frame.payload_bytes, pending_frame.rms_milli);
    has_pending_frame = false;
  }

  // Make every terminal sender path stop the capture owner before touching
  // the shared I2S channel or deleting its queue.
  lock();
  session_lifecycle_.request_stop(0, stop_reason);
  lease_armed_ = false;
  unlock();
  if (!capture_finished) {
    while (!take_capture_completion(delay_ticks(1500))) {
      ESP_LOGW(kTag,
               "keyboard mic waiting for capture task stop generation=%lu",
               static_cast<unsigned long>(generation));
    }
  }
  if (clean_capture_end && capture_drained && sent_packets > 0U) {
    packet.fill(0U);
    const ai_keyboard::AudioEndMetadata end_metadata{
        .session_id = session_id,
        .final_sequence = sent_packets,
        .sample_rate = kAudioSampleRate,
        .capture_timestamp_ms = static_cast<std::uint32_t>(
            (xTaskGetTickCount() - started_tick) * portTICK_PERIOD_MS),
    };
    std::array<std::uint8_t, 32> authentication{};
    const auto* digest = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool terminal_ready =
        ai_keyboard::encode_audio_end_header(
            packet.data(), packet.size(), end_metadata) &&
        digest != nullptr &&
        mbedtls_md_hmac(digest,
                        config.speaker_sync_key.data(),
                        config.speaker_sync_key.size(),
                        packet.data(),
                        ai_keyboard::kAudioPacketHeaderBytes,
                        authentication.data()) == 0;
    bool terminal_sent = false;
    if (terminal_ready) {
      std::memcpy(packet.data() + ai_keyboard::kAudioPacketHeaderBytes,
                  authentication.data(),
                  ai_keyboard::kAudioPacketAuthTagBytes);
      const auto terminal_bytes =
          ai_keyboard::kAudioPacketHeaderBytes +
          ai_keyboard::kAudioPacketAuthTagBytes;
      for (std::uint8_t attempt = 0U; attempt < 5U; ++attempt) {
        terminal_sent =
            sendto(sock,
                   packet.data(),
                   terminal_bytes,
                   0,
                   target->ai_addr,
                   target->ai_addrlen) ==
                static_cast<int>(terminal_bytes) ||
            terminal_sent;
        if (attempt + 1U < 5U) {
          vTaskDelay(delay_ticks(20));
        }
      }
    }
    authentication.fill(0U);
    if (!terminal_ready || !terminal_sent) {
      stop_reason = "terminal_send_failed";
      final_status = "udp_send_failed";
      note_audio_error(final_status, stop_reason, false, true);
    }
  }
  shutdown_microphone();
  xQueueReset(frame_queue);
  close_udp_target();
  ESP_LOGI(kTag,
           "keyboard mic UDP stream stopped reason=%s session=%llu generation=%lu packets=%lu bytes=%lu recoveries=%lu",
           stop_reason,
           static_cast<unsigned long long>(session_id),
           static_cast<unsigned long>(generation),
           static_cast<unsigned long>(sent_packets),
           static_cast<unsigned long>(sent_bytes),
           static_cast<unsigned long>(diagnostics().recovery_count));
  finish_stream_task(generation, stop_reason, final_status);
}

void KeyboardAudioLink::run_audio_capture(std::uint32_t generation,
                                          QueueHandle_t frame_queue) {
  std::uint32_t capture_sequence = 0;
  std::uint32_t consecutive_read_misses = 0;
  std::uint32_t total_read_misses = 0;
  std::uint32_t consecutive_recovery_failures = 0;
  std::uint32_t queue_overflows = 0;

  while (should_run_stream(generation)) {
    CapturedAudioFrame frame{};
    std::size_t frame_bytes = 0;
    const esp_err_t err = read_microphone_pcm16(frame.pcm.data(), frame.pcm.size(), &frame_bytes);
    if (err != ESP_OK || frame_bytes == 0) {
      total_read_misses += 1;
      consecutive_read_misses += 1;
      if (consecutive_read_misses == 1 ||
          consecutive_read_misses == kAudioMicReadRecoveryThreshold) {
        ESP_LOGW(kTag,
                 "keyboard mic I2S read miss err=%s consecutive=%lu total=%lu captured=%lu generation=%lu",
                 esp_err_to_name(err),
                 static_cast<unsigned long>(consecutive_read_misses),
                 static_cast<unsigned long>(total_read_misses),
                 static_cast<unsigned long>(capture_sequence),
                 static_cast<unsigned long>(generation));
      }
      if (consecutive_read_misses < kAudioMicReadRecoveryThreshold) {
        vTaskDelay(delay_ticks(kAudioMicReadRetryDelayMs));
        continue;
      }

      mark_stream_phase(generation, ai_keyboard::AudioSessionPhase::Recovering,
                        "mic_recovering");
      lock();
      diagnostics_.read_errors += 1;
      diagnostics_.recovery_count += 1;
      diagnostics_.last_error = std::string("i2s_read:") + esp_err_to_name(err);
      unlock();
      reset_microphone_channel();
      vTaskDelay(delay_ticks(kAudioRecoveryRetryDelayMs));
      const esp_err_t recovery_err =
          should_run_stream(generation) ? ensure_microphone_ready() : ESP_ERR_INVALID_STATE;
      if (recovery_err != ESP_OK) {
        consecutive_recovery_failures += 1;
        consecutive_read_misses = 0;
        if (consecutive_recovery_failures >= kAudioMaxConsecutiveRecoveryFailures) {
          note_audio_error("mic_read_failed", esp_err_to_name(recovery_err), true, false);
          lock();
          session_lifecycle_.request_stop(0, "i2s_recovery_exhausted");
          lease_armed_ = false;
          unlock();
          break;
        }
        continue;
      }
      consecutive_read_misses = 0;
      consecutive_recovery_failures = 0;
      mark_stream_phase(generation, ai_keyboard::AudioSessionPhase::Streaming,
                        "mic_streaming");
      continue;
    }

    consecutive_read_misses = 0;
    consecutive_recovery_failures = 0;
    if (!should_run_stream(generation)) {
      break;
    }

    frame.capture_sequence = capture_sequence++;
    frame.capture_timestamp_ms =
        static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
    frame.payload_bytes = static_cast<std::uint16_t>(frame_bytes);
    frame.rms_milli =
        static_cast<std::uint16_t>(pcm16_rms_milli(frame.pcm.data(), frame_bytes));

    if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
      CapturedAudioFrame dropped{};
      xQueueReceive(frame_queue, &dropped, 0);
      if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
        ESP_LOGE(kTag, "keyboard mic capture queue remained full after dropping oldest frame");
      }
      queue_overflows += 1;
      if (queue_overflows == 1 || queue_overflows % 50 == 0) {
        ESP_LOGW(kTag,
                 "keyboard mic capture queue overflow dropped_seq=%lu current_seq=%lu total=%lu",
                 static_cast<unsigned long>(dropped.capture_sequence),
                 static_cast<unsigned long>(frame.capture_sequence),
                 static_cast<unsigned long>(queue_overflows));
      }
      lock();
      diagnostics_.send_errors += 1;
      diagnostics_.last_error = "capture_queue_overflow";
      unlock();
    }
  }

  ESP_LOGI(kTag,
           "keyboard mic capture task stopped generation=%lu captured=%lu read_misses=%lu queue_overflows=%lu",
           static_cast<unsigned long>(generation),
           static_cast<unsigned long>(capture_sequence),
           static_cast<unsigned long>(total_read_misses),
           static_cast<unsigned long>(queue_overflows));
  lock();
  capture_completed_generation_ = generation;
  unlock();
  if (capture_done_ != nullptr) {
    xSemaphoreGive(capture_done_);
  }
}

esp_err_t KeyboardAudioLink::prepare_microphone_channel() {
  MicOpGuard mic_guard(mic_op_mutex_);
  return prepare_microphone_channel_locked();
}

esp_err_t KeyboardAudioLink::prepare_microphone_channel_locked() {
  if constexpr (ai_keyboard::kMicI2sBclkPin < 0 || ai_keyboard::kMicI2sWsPin < 0 ||
                ai_keyboard::kMicI2sDataInPin < 0) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  if (mic_rx_ != nullptr) {
    return ESP_OK;
  }

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(kMicI2sController, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = kAudioSamplesPerFrame;
  esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &mic_rx_);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "i2s_new_channel RX failed: %s", esp_err_to_name(err));
    return err;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kAudioSampleRate),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = static_cast<gpio_num_t>(ai_keyboard::kMicI2sBclkPin),
          .ws = static_cast<gpio_num_t>(ai_keyboard::kMicI2sWsPin),
          .dout = I2S_GPIO_UNUSED,
          .din = static_cast<gpio_num_t>(ai_keyboard::kMicI2sDataInPin),
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;

  err = i2s_channel_init_std_mode(mic_rx_, &std_cfg);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "i2s_channel_init_std_mode RX failed: %s", esp_err_to_name(err));
    i2s_del_channel(mic_rx_);
    mic_rx_ = nullptr;
    return err;
  }

  ESP_LOGI(kTag,
           "prepared V2 I2S mic rate=%luHz bclk=GPIO%d ws=GPIO%d din=GPIO%d channel=right capture=disabled",
           static_cast<unsigned long>(kAudioSampleRate),
           static_cast<int>(ai_keyboard::kMicI2sBclkPin),
           static_cast<int>(ai_keyboard::kMicI2sWsPin),
           static_cast<int>(ai_keyboard::kMicI2sDataInPin));
  return ESP_OK;
}

esp_err_t KeyboardAudioLink::ensure_microphone_ready() {
  MicOpGuard mic_guard(mic_op_mutex_);
  const esp_err_t prepare_err = prepare_microphone_channel_locked();
  if (prepare_err != ESP_OK) {
    return prepare_err;
  }
  if (mic_enabled_) {
    return ESP_OK;
  }

  const esp_err_t err = i2s_channel_enable(mic_rx_);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "i2s_channel_enable RX failed: %s", esp_err_to_name(err));
    return err;
  }
  mic_enabled_ = true;
  return ESP_OK;
}

esp_err_t KeyboardAudioLink::read_microphone_pcm16(std::uint8_t* frame,
                                                   std::size_t frame_size,
                                                   std::size_t* bytes_read) {
  if (frame == nullptr || bytes_read == nullptr || frame_size < kAudioFrameBytes) {
    return ESP_ERR_INVALID_ARG;
  }
  if (mic_rx_ == nullptr || !mic_enabled_) {
    return ESP_ERR_INVALID_STATE;
  }

  std::array<std::int32_t, kAudioSamplesPerFrame> samples{};
  std::size_t i2s_bytes_read = 0;
  const esp_err_t err = i2s_channel_read(mic_rx_,
                                         samples.data(),
                                         samples.size() * sizeof(samples[0]),
                                         &i2s_bytes_read,
                                         kAudioMicReadTimeoutMs);
  if (err != ESP_OK) {
    *bytes_read = 0;
    return err;
  }

  const auto sample_count = std::min(samples.size(), i2s_bytes_read / sizeof(samples[0]));
  if (sample_count == 0) {
    *bytes_read = 0;
    return ESP_ERR_TIMEOUT;
  }

  for (std::size_t index = 0; index < kAudioSamplesPerFrame; ++index) {
    const auto sample = index < sample_count ? sample32_to_pcm16(samples[index]) : 0;
    write_pcm16_le(frame, index, sample);
  }
  *bytes_read = kAudioFrameBytes;
  return ESP_OK;
}

void KeyboardAudioLink::shutdown_microphone() {
  MicOpGuard mic_guard(mic_op_mutex_);
  shutdown_microphone_locked();
}

void KeyboardAudioLink::shutdown_microphone_locked() {
  if (mic_rx_ == nullptr || !mic_enabled_) {
    return;
  }
  const esp_err_t err = i2s_channel_disable(mic_rx_);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "i2s_channel_disable RX failed: %s", esp_err_to_name(err));
  }
  mic_enabled_ = false;
}

void KeyboardAudioLink::reset_microphone_channel() {
  MicOpGuard mic_guard(mic_op_mutex_);
  reset_microphone_channel_locked();
}

void KeyboardAudioLink::reset_microphone_channel_locked() {
  shutdown_microphone_locked();
  if (mic_rx_ == nullptr) {
    return;
  }
  const esp_err_t err = i2s_del_channel(mic_rx_);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "i2s_del_channel RX failed during recovery: %s", esp_err_to_name(err));
  }
  mic_rx_ = nullptr;
}

void KeyboardAudioLink::preconnect_wifi(const char* reason) {
  const auto config = config_snapshot();
  if (!config.enabled) {
    return;
  }
  if (config.wifi_ssid.empty() || config.host.empty() || config.port == 0) {
    mark_status("config_incomplete");
    return;
  }

  const esp_err_t err = prepare_wifi_connection(config, reason, false, false);
  if (err != ESP_OK) {
    ESP_LOGW(kTag,
             "keyboard mic Wi-Fi preconnect failed reason=%s err=%s",
             reason == nullptr ? "" : reason,
             esp_err_to_name(err));
    note_audio_error("wifi_failed", esp_err_to_name(err), false, false);
    return;
  }

  ESP_LOGI(kTag,
           "keyboard mic Wi-Fi preconnect requested reason=%s ssid=%s host=%s port=%u",
           reason == nullptr ? "" : reason,
           config.wifi_ssid.c_str(),
           config.host.c_str(),
           static_cast<unsigned>(config.port));
}

esp_err_t KeyboardAudioLink::ensure_wifi_ready(const KeyboardAudioConfig& config,
                                               std::uint32_t generation) {
  return prepare_wifi_connection(config, "stream", true, true, generation);
}

esp_err_t KeyboardAudioLink::prepare_wifi_connection(const KeyboardAudioConfig& config,
                                                     const char* reason,
                                                     bool wait_for_connection,
                                                     bool abort_on_stream_stop,
                                                     std::uint32_t stream_generation) {
  esp_err_t err = begin();
  if (err != ESP_OK) {
    return err;
  }

  if (config.wifi_ssid.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  {
    // 串行化连接操作段:控制任务与音频任务可能并发冷启动重连,
    // 防止 set_config/disconnect 交错取消对方进行中的 connect。
    WifiOpGuard wifi_guard(wifi_op_mutex_);

    const bool same_wifi_config = wifi_started_ &&
                                  wifi_configured_ssid_ == config.wifi_ssid &&
                                  wifi_configured_password_ == config.wifi_password;
    if (same_wifi_config &&
        (xEventGroupGetBits(wifi_events_) & kWifiConnectedBit) != 0) {
      mark_control_state("wifi_ready");
      ESP_LOGI(kTag,
               "keyboard mic reusing connected Wi-Fi reason=%s ssid=%s",
               reason == nullptr ? "" : reason,
               config.wifi_ssid.c_str());
      return ESP_OK;
    }

    wifi_config_t wifi_config = {};
    copy_wifi_field(wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), config.wifi_ssid);
    copy_wifi_field(wifi_config.sta.password,
                    sizeof(wifi_config.sta.password),
                    config.wifi_password);
    wifi_config.sta.threshold.authmode =
        config.wifi_password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    if (!same_wifi_config) {
      xEventGroupClearBits(wifi_events_, kWifiConnectedBit);
      err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
      if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
      }
      wifi_configured_ssid_ = config.wifi_ssid;
      wifi_configured_password_ = config.wifi_password;
    }

    if (!wifi_started_) {
      err = esp_wifi_start();
      if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(kTag, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
      }
      wifi_started_ = true;
    }

    mark_control_state(wait_for_connection ? "wifi_connecting" : "wifi_preparing");
    if (!same_wifi_config) {
      esp_wifi_disconnect();
    }
    ESP_LOGI(kTag,
             "keyboard mic connecting Wi-Fi reason=%s ssid=%s reuse_config=%d wait=%d",
             reason == nullptr ? "" : reason,
             config.wifi_ssid.c_str(),
             same_wifi_config ? 1 : 0,
             wait_for_connection ? 1 : 0);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
      ESP_LOGW(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
      return err;
    }
  }
  if (!wait_for_connection) {
    return ESP_OK;
  }

  const auto started_tick = xTaskGetTickCount();
  while (true) {
    if (abort_on_stream_stop) {
      lock();
      const bool should_run = session_lifecycle_.should_run(stream_generation);
      unlock();
      if (!should_run) {
        return ESP_ERR_INVALID_STATE;
      }
    }
    const auto bits = xEventGroupWaitBits(wifi_events_,
                                          kWifiConnectedBit,
                                          pdFALSE,
                                          pdFALSE,
                                          delay_ticks(200));
    if ((bits & kWifiConnectedBit) != 0) {
      return ESP_OK;
    }
    const auto elapsed_ms =
        static_cast<std::uint32_t>((xTaskGetTickCount() - started_tick) * portTICK_PERIOD_MS);
    if (elapsed_ms >= kWifiConnectTimeoutMs) {
      return ESP_ERR_TIMEOUT;
    }
  }
}

bool KeyboardAudioLink::should_run_stream(std::uint32_t generation) const {
  lock();
  const bool should_run = session_lifecycle_.should_run(generation);
  unlock();
  return should_run;
}

#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
bool KeyboardAudioLink::wait_for_microphone_hardware(
    std::uint32_t generation) const {
  if (audio_io_arbiter_ == nullptr) {
    return true;
  }
  const auto started_tick = xTaskGetTickCount();
  while (should_run_stream(generation)) {
    if (audio_io_arbiter_->microphone_hardware_ready(generation)) {
      return true;
    }
    const auto elapsed_ms = static_cast<std::uint32_t>(
        (xTaskGetTickCount() - started_tick) * portTICK_PERIOD_MS);
    if (elapsed_ms >= kSpeakerDrainWaitTimeoutMs) {
      return false;
    }
    vTaskDelay(delay_ticks(kSpeakerDrainPollMs));
  }
  return false;
}
#endif

std::uint64_t KeyboardAudioLink::active_session_id() const {
  lock();
  const auto session_id = session_lifecycle_.session_id();
  unlock();
  return session_id;
}

bool KeyboardAudioLink::wait_for_task_stop(std::uint32_t timeout_ms) const {
  const auto started_tick = xTaskGetTickCount();
  while (true) {
    lock();
    const bool stopped =
        task_ == nullptr && session_lifecycle_.phase() == ai_keyboard::AudioSessionPhase::Idle;
    unlock();
    if (stopped) {
      return true;
    }
    const auto elapsed_ms =
        static_cast<std::uint32_t>((xTaskGetTickCount() - started_tick) * portTICK_PERIOD_MS);
    if (elapsed_ms >= timeout_ms) {
      return false;
    }
    vTaskDelay(delay_ticks(10));
  }
}

bool KeyboardAudioLink::mark_stream_phase(std::uint32_t generation,
                                          ai_keyboard::AudioSessionPhase phase,
                                          const char* status) {
  lock();
  bool updated = false;
  if (phase == ai_keyboard::AudioSessionPhase::Streaming) {
    updated = session_lifecycle_.mark_streaming(generation);
  } else if (phase == ai_keyboard::AudioSessionPhase::Recovering) {
    updated = session_lifecycle_.mark_recovering(generation);
  }
  if (updated) {
    capture_status_ = status == nullptr ? "unknown" : status;
    diagnostics_.stream_phase = ai_keyboard::audio_session_phase_name(phase);
  }
  unlock();
  return updated;
}

void KeyboardAudioLink::finish_stream_task(std::uint32_t generation,
                                           const char* stop_reason,
                                           const char* final_status) {
  lock();
  if (task_owner_generation_ == generation) {
    task_ = nullptr;
    task_owner_generation_ = 0;
  }
  if (session_lifecycle_.finish(generation, stop_reason)) {
    lease_armed_ = false;
    diagnostics_.stream_phase =
        ai_keyboard::audio_session_phase_name(session_lifecycle_.phase());
    diagnostics_.stop_reason = session_lifecycle_.stop_reason();
    if (capture_status_ != "disabled") {
      capture_status_ = final_status == nullptr ? "mic_stopped" : final_status;
    }
  }
  unlock();
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (audio_io_arbiter_ != nullptr) {
    audio_io_arbiter_->finish_microphone(generation);
  }
#endif
}

void KeyboardAudioLink::mark_status(const char* status) {
  lock();
  capture_status_ = status == nullptr ? "unknown" : status;
  unlock();
}

void KeyboardAudioLink::mark_control_state(const char* state) {
  lock();
  diagnostics_.control_state = state == nullptr ? "unknown" : state;
  unlock();
}

void KeyboardAudioLink::reset_diagnostics_locked() {
  diagnostics_ = {};
}

void KeyboardAudioLink::note_stream_target_locked(const KeyboardAudioConfig& config) {
  diagnostics_.stream_host = config.host;
  diagnostics_.stream_port = config.port;
}

void KeyboardAudioLink::note_audio_packet(std::uint32_t bytes, std::uint32_t rms_milli) {
  lock();
  diagnostics_.sent_packets += 1;
  diagnostics_.sent_bytes += bytes;
  diagnostics_.last_rms_milli = rms_milli;
  diagnostics_.peak_rms_milli = std::max(diagnostics_.peak_rms_milli, rms_milli);
  unlock();
}

void KeyboardAudioLink::note_audio_error(const char* status,
                                         const std::string& message,
                                         bool read_error,
                                         bool send_error) {
  lock();
  capture_status_ = status == nullptr ? "unknown" : status;
  diagnostics_.last_error = message;
  if (read_error) {
    diagnostics_.read_errors += 1;
  }
  if (send_error) {
    diagnostics_.send_errors += 1;
  }
  unlock();
}

void KeyboardAudioLink::note_audio_resource_error(const char* status, const char* stage) {
  constexpr std::uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const auto free_bytes = heap_caps_get_free_size(kInternalHeapCaps);
  const auto largest_bytes = heap_caps_get_largest_free_block(kInternalHeapCaps);
  const auto minimum_bytes = heap_caps_get_minimum_free_size(kInternalHeapCaps);
  const std::string stage_text = stage == nullptr ? "resource_unknown" : stage;

  lock();
  capture_status_ = status == nullptr ? "aud_resource" : status;
  diagnostics_.last_error =
      stage_text + " free=" + std::to_string(free_bytes) +
      " largest=" + std::to_string(largest_bytes) +
      " minimum=" + std::to_string(minimum_bytes);
  unlock();

  ESP_LOGW(kTag,
           "audio resource unavailable stage=%s free=%u largest=%u minimum=%u",
           stage_text.c_str(),
           static_cast<unsigned>(free_bytes),
           static_cast<unsigned>(largest_bytes),
           static_cast<unsigned>(minimum_bytes));
}

KeyboardAudioConfig KeyboardAudioLink::config_snapshot() const {
  lock();
  auto config = config_;
  unlock();
  return config;
}

void KeyboardAudioLink::lock() const {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
}

void KeyboardAudioLink::unlock() const {
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
}

}  // namespace easy_input
