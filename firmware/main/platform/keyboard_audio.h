#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "driver/i2s_types.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "keyboard/audio_session.h"
#include "keyboard/codex_playback_wire.h"
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
#include "keyboard/audio_io_arbiter.h"
#endif

namespace easy_input {

struct KeyboardAudioConfig {
  bool enabled = false;
  std::string wifi_ssid;
  std::string wifi_password;
  std::string host;
  std::uint16_t port = 17333;
  std::array<std::uint8_t, 32> speaker_sync_key{};
  std::uint16_t speaker_sync_key_epoch = 0U;
  bool speaker_sync_key_valid = false;
};

struct KeyboardWifiServiceSnapshot {
  bool configured = false;
  bool connected = false;
  bool disconnect_pending = false;
  bool host_ipv4_valid = false;
  std::uint32_t host_ipv4 = 0U;
  std::uint16_t host_port = 0U;
  std::uint32_t generation = 0U;
  std::array<std::uint8_t, 32> speaker_sync_key{};
  std::uint16_t speaker_sync_key_epoch = 0U;
  bool speaker_sync_key_valid = false;
};

struct KeyboardWifiServiceLease {
  std::uint32_t lease_id = 0U;
  std::uint32_t generation = 0U;

  bool valid() const {
    return lease_id != 0U && generation != 0U;
  }
};

struct KeyboardAudioDiagnostics {
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
  std::uint16_t stream_port = 0;
};

class KeyboardAudioLink {
 public:
  using ActivityCallback = void (*)(void* context);
  using HeartbeatExtensionCallback = std::size_t (*)(
      void* context,
      std::uint8_t* heartbeat,
      std::size_t base_length,
      std::size_t capacity);

  esp_err_t begin();
  void set_activity_callback(ActivityCallback callback, void* context);
  void set_heartbeat_extension_callback(
      HeartbeatExtensionCallback callback,
      void* context);
  // Request a fresh control heartbeat after externally visible discovery
  // state changes. Requests coalesce by generation and remain pending until
  // the control task successfully sends a heartbeat.
  void request_heartbeat_refresh();
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  void set_audio_io_arbiter(ai_keyboard::AudioIoArbiter* arbiter);
#endif
  void configure(const KeyboardAudioConfig& config);
  void stop_all_streams();
  void start_stream(const char* reason, std::uint64_t session_id);
  void stop_stream(std::uint64_t session_id);
  std::string capture_status() const;
  KeyboardAudioDiagnostics diagnostics() const;
  bool streaming() const;
  // 只有整机已经满足深睡条件时,才由电源策略请求释放音频控制 Wi-Fi。
  void request_wifi_release_for_deep_sleep();
  // 任意设备活动会取消尚未完成的深睡释放;若已断网则恢复控制通道。
  // 设备活动不刷新音频网络时钟,避免普通按键把 Wi-Fi 拉回活跃功耗档。
  void cancel_wifi_release_for_device_activity();
  // 物理 PTT 按下表示 App 即将经 Wi-Fi 下发 start。它属于音频网络活动,
  // 在重连冷却期内也要触发快速重连,但不直接启动录音或改变 HID 语义。
  void prepare_for_audio_trigger();
  // Wi-Fi UDP 通道收到的 JSON 配置(与 BLE/USB 同一消费路径,app_main 轮询)。
  bool take_pending_config(std::string* json);
  bool take_pending_mailbox_status(
      easy_codex::MailboxWireStatus* status);
  // light sleep 门控:Wi-Fi 已连接或正在流式才禁睡;深闲释放后恢复可睡。
  bool wifi_active_or_streaming() const;
  KeyboardWifiServiceSnapshot wifi_service_snapshot() const;
  bool acquire_wifi_service_lease(
      std::uint32_t expected_generation,
      KeyboardWifiServiceLease* lease);
  bool release_wifi_service_lease(
      const KeyboardWifiServiceLease& lease);
  void note_wifi_service_activity();

 private:
  static void task_entry(void* arg);
  static void capture_task_entry(void* arg);
  static void control_task_entry(void* arg);
  static void event_handler(void* arg,
                            esp_event_base_t event_base,
                            std::int32_t event_id,
                            void* event_data);

  void run_audio_stream(std::uint32_t generation, std::uint64_t session_id);
  void run_audio_capture(std::uint32_t generation,
                         QueueHandle_t frame_queue);
  void run_control_channel();
  bool arm_session_lease(std::uint64_t session_id);
  bool session_lease_expired() const;
  void preconnect_wifi(const char* reason);
  esp_err_t ensure_wifi_ready(const KeyboardAudioConfig& config, std::uint32_t generation);
  esp_err_t prepare_wifi_connection(const KeyboardAudioConfig& config,
                                    const char* reason,
                                    bool wait_for_connection,
                                    bool abort_on_stream_stop,
                                    std::uint32_t stream_generation = 0);
  esp_err_t prepare_microphone_channel();
  esp_err_t prepare_microphone_channel_locked();
  esp_err_t ensure_microphone_ready();
  esp_err_t read_microphone_pcm16(std::uint8_t* frame,
                                  std::size_t frame_size,
                                  std::size_t* bytes_read);
  void shutdown_microphone();
  void shutdown_microphone_locked();
  void reset_microphone_channel();
  void reset_microphone_channel_locked();
  std::uint64_t active_session_id() const;
  bool should_run_stream(std::uint32_t generation) const;
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  bool wait_for_microphone_hardware(std::uint32_t generation) const;
#endif
  bool wait_for_task_stop(std::uint32_t timeout_ms) const;
  bool mark_stream_phase(std::uint32_t generation,
                         ai_keyboard::AudioSessionPhase phase,
                         const char* status);
  void finish_stream_task(std::uint32_t generation,
                          const char* stop_reason,
                          const char* final_status);
  void mark_status(const char* status);
  void mark_control_state(const char* state);
  void reset_diagnostics_locked();
  void note_stream_target_locked(const KeyboardAudioConfig& config);
  void note_audio_packet(std::uint32_t bytes, std::uint32_t rms_milli);
  void note_audio_error(const char* status, const std::string& message, bool read_error, bool send_error);
  void note_audio_resource_error(const char* status, const char* stage);
  void bump_wifi_service_generation_locked();
  void note_network_activity();
  void note_remote_activity();
  bool stop_wifi_for_inactive_audio();
  bool begin_wifi_release_for_deep_sleep();
  KeyboardAudioConfig config_snapshot() const;
  void lock() const;
  void unlock() const;

  mutable SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t wifi_op_mutex_ = nullptr;
  SemaphoreHandle_t mic_op_mutex_ = nullptr;
  SemaphoreHandle_t capture_done_ = nullptr;
  KeyboardAudioConfig config_;
  std::string capture_status_ = "disabled";
  enum class InitState : std::uint8_t {
    Uninitialized,
    Initializing,
    Ready,
    Failed,
  };
  InitState init_state_ = InitState::Uninitialized;
  esp_err_t init_error_ = ESP_OK;
  bool wifi_started_ = false;
  bool mic_enabled_ = false;
  std::string wifi_configured_ssid_;
  std::string wifi_configured_password_;
  // The audio sender/capture workers and their queue are created once during
  // begin(). The queue payload storage is an explicit PSRAM bulk allocation;
  // its FreeRTOS control block and all task stacks remain in internal SRAM.
  // task_ remains the active-session alias used by lifecycle/power code.
  QueueHandle_t capture_queue_ = nullptr;
  std::uint8_t* capture_queue_storage_ = nullptr;
  StaticQueue_t* capture_queue_control_ = nullptr;
  TaskHandle_t stream_worker_task_ = nullptr;
  TaskHandle_t capture_worker_task_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::uint32_t task_owner_generation_ = 0;
  std::uint32_t stream_job_generation_ = 0;
  std::uint64_t stream_job_session_id_ = 0;
  std::uint32_t capture_job_generation_ = 0;
  std::uint32_t capture_completed_generation_ = 0;
  TaskHandle_t control_task_ = nullptr;
  ai_keyboard::AudioSessionLifecycle session_lifecycle_;
  bool lease_armed_ = false;
  TickType_t lease_deadline_tick_ = 0;
  TickType_t last_network_activity_tick_ = 0;
  bool deep_sleep_release_requested_ = false;
  TickType_t deep_sleep_release_requested_tick_ = 0;
  bool wifi_released_for_deep_sleep_ = false;
  bool wifi_disconnect_pending_ = false;
  bool wifi_reconnect_requested_ = false;
  TickType_t wifi_reconnect_requested_tick_ = 0;
  std::uint32_t control_heartbeat_request_generation_ = 0;
  ActivityCallback activity_callback_ = nullptr;
  void* activity_callback_context_ = nullptr;
  HeartbeatExtensionCallback heartbeat_extension_callback_ = nullptr;
  void* heartbeat_extension_context_ = nullptr;
  std::uint32_t wifi_service_generation_ = 1U;
  std::uint32_t wifi_service_host_ipv4_ = 0U;
  bool wifi_service_host_ipv4_valid_ = false;
  KeyboardWifiServiceLease wifi_service_lease_{};
  std::uint32_t next_wifi_service_lease_id_ = 1U;
  // acquire_wifi_service_lease() may synchronously force WIFI_PS_NONE from
  // the speaker carrier task. The control task observes this generation and
  // invalidates its local PS cache before restoring the current idle policy.
  std::atomic<std::uint32_t> wifi_ingress_power_generation_{0U};
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  ai_keyboard::AudioIoArbiter* audio_io_arbiter_ = nullptr;
#endif
  std::string pending_config_json_;
  easy_codex::MailboxWireStatus pending_mailbox_status_{};
  bool pending_mailbox_status_ready_ = false;
  EventGroupHandle_t wifi_events_ = nullptr;
  esp_netif_t* wifi_netif_ = nullptr;
  i2s_chan_handle_t mic_rx_ = nullptr;
  KeyboardAudioDiagnostics diagnostics_;
};

}  // namespace easy_input
