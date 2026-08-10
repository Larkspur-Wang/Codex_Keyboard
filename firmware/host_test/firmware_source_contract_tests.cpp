#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "speaker_assets/speaker_assets_runtime.h"

#ifndef EASY_INPUT_REPO_ROOT
#error "EASY_INPUT_REPO_ROOT must identify the firmware repository"
#endif

namespace {

using easy_input::speaker_assets::SpeakerAssetsRouteToken;
using easy_input::speaker_assets::SpeakerAssetsRuntimeActionExecutor;
using easy_input::speaker_assets::SpeakerAssetsRuntimeCore;
using easy_input::speaker_assets::SpeakerAssetsRuntimeEnqueueResult;
using easy_input::speaker_assets::SpeakerAssetsRuntimeMailboxRecord;
using easy_input::speaker_assets::SpeakerAssetsRuntimeStepResult;

template <typename T, typename = void>
struct has_direct_usb_core_enqueue : std::false_type {};

template <typename T>
struct has_direct_usb_core_enqueue<
    T,
    std::void_t<decltype(std::declval<T&>().enqueue_usb_frame(
        std::declval<const SpeakerAssetsRouteToken&>(),
        std::declval<const std::uint8_t*>(),
        std::declval<std::size_t>(),
        std::declval<std::uint32_t>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_direct_wifi_core_enqueue : std::false_type {};

template <typename T>
struct has_direct_wifi_core_enqueue<
    T,
    std::void_t<decltype(std::declval<T&>().enqueue_wifi_frame(
        std::declval<const SpeakerAssetsRouteToken&>(),
        std::declval<const std::uint8_t*>(),
        std::declval<std::size_t>(),
        std::declval<std::uint32_t>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_result_only_step_comparison : std::false_type {};

template <typename T>
struct has_result_only_step_comparison<
    T,
    std::void_t<decltype(
        std::declval<const T&>() ==
        SpeakerAssetsRuntimeStepResult::Idle)>> : std::true_type {};

template <typename T, typename = void>
struct has_public_step_result_field : std::false_type {};

template <typename T>
struct has_public_step_result_field<
    T,
    std::void_t<decltype(std::declval<const T&>().result)>>
    : std::true_type {};

template <typename T, typename = void>
struct has_public_step_release_field : std::false_type {};

template <typename T>
struct has_public_step_release_field<
    T,
    std::void_t<decltype(std::declval<const T&>().release_now)>>
    : std::true_type {};

static_assert(
    !has_direct_usb_core_enqueue<SpeakerAssetsRuntimeCore>::value,
    "product RuntimeCore must reject direct zero-lease USB ingress");
static_assert(
    !has_direct_wifi_core_enqueue<SpeakerAssetsRuntimeCore>::value,
    "product RuntimeCore must reject direct zero-lease Wi-Fi ingress");
static_assert(
    !has_result_only_step_comparison<
        SpeakerAssetsRuntimeCore::StepOutcome>::value,
    "product StepOutcome must not silently decay to result-only usage");
static_assert(
    !has_public_step_result_field<
        SpeakerAssetsRuntimeCore::StepOutcome>::value,
    "product StepOutcome result must be inspected with its release lease");
static_assert(
    !has_public_step_release_field<
        SpeakerAssetsRuntimeCore::StepOutcome>::value,
    "product StepOutcome release lease must be inspected with its result");
static_assert(std::is_same_v<
              decltype(std::declval<
                           const SpeakerAssetsRuntimeCore::StepOutcome&>()
                           .inspect(
                               std::declval<
                                   SpeakerAssetsRuntimeStepResult*>(),
                               std::declval<
                                   easy_input::speaker_assets::
                                       SpeakerAssetsLogicalRequestLease*>())),
              bool>);
static_assert(std::is_same_v<
              decltype(std::declval<SpeakerAssetsRuntimeCore&>()
                           .import_mailbox_record(
                               std::declval<
                                   const SpeakerAssetsRuntimeMailboxRecord&>())),
              SpeakerAssetsRuntimeEnqueueResult>);
static_assert(std::is_same_v<
              decltype(std::declval<SpeakerAssetsRuntimeCore&>().step(
                  std::declval<std::uint32_t>(),
                  std::declval<bool>(),
                  std::declval<
                      SpeakerAssetsRuntimeActionExecutor*>())),
              SpeakerAssetsRuntimeCore::StepOutcome>);

std::string read_source(const std::string& relative_path) {
  std::ifstream input(std::string(EASY_INPUT_REPO_ROOT) + "/" + relative_path);
  assert(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string section(const std::string& source,
                    const std::string& begin_marker,
                    const std::string& end_marker) {
  const auto begin = source.find(begin_marker);
  assert(begin != std::string::npos);
  const auto end = source.find(end_marker, begin + begin_marker.size());
  assert(end != std::string::npos);
  return source.substr(begin, end - begin);
}

std::size_t count_occurrences(const std::string& source,
                              const std::string& needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = source.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::string without_ascii_whitespace(const std::string& source) {
  std::string compact;
  compact.reserve(source.size());
  for (const char character : source) {
    if (!std::isspace(static_cast<unsigned char>(character))) {
      compact.push_back(character);
    }
  }
  return compact;
}

void battery_update_releases_hidd_lock_before_entering_nimble() {
  const auto source = read_source("components/esp_hid/src/nimble_hidd.c");
  const auto body = section(source,
                            "static int nimble_hidd_dev_battery_set",
                            "/* if mode is NULL");
  const auto nimble_call =
      body.find("const int rc = ble_svc_bas_battery_level_set(level);");
  assert(nimble_call != std::string::npos);

  const auto last_lock = body.rfind("\n    lock_hidd();", nimble_call);
  const auto last_unlock = body.rfind("\n    unlock_hidd();", nimble_call);
  const auto owner_lock = body.rfind("\n    lock_owner_gate();", nimble_call);
  const auto normal_hidd_unlock =
      body.find("\n    unlock_hidd();\n\n    /*", last_lock);
  const auto owner_unlock_after_call =
      body.find("\n        unlock_owner_gate();", nimble_call);
  assert(last_lock != std::string::npos);
  assert(last_unlock != std::string::npos);
  assert(last_unlock > last_lock);
  assert(owner_lock != std::string::npos);
  assert(normal_hidd_unlock != std::string::npos);
  const auto normal_owner_unlock =
      body.find("unlock_owner_gate();", normal_hidd_unlock);
  assert(normal_owner_unlock != std::string::npos);
  assert(normal_owner_unlock > nimble_call);
  assert(owner_unlock_after_call != std::string::npos);
}

void gatt_status_read_callback_is_cache_only_and_refreshes_once() {
  const auto source = read_source("main/platform/ble_hid.cpp");
  const auto access = section(source,
                              "int BleHidTransport::handle_config_access",
                              "void BleHidTransport::note_control_connection");
  const auto read_end = access.find(
      "if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)");
  assert(read_end != std::string::npos);
  const auto read_path = access.substr(0, read_end);

  assert(read_path.find("copy_status_json_for_read") != std::string::npos);
  assert(read_path.find("ctxt->offset == 0") != std::string::npos);
  assert(read_path.find("conn_handle != BLE_HS_CONN_HANDLE_NONE") !=
         std::string::npos);
  assert(read_path.find("current_status_json") == std::string::npos);
  assert(read_path.find("connected()") == std::string::npos);
  assert(read_path.find("connection_identity()") == std::string::npos);
  assert(read_path.find("note_control_connection") == std::string::npos);
  assert(read_path.find("update_battery_level") == std::string::npos);
  assert(read_path.find("ESP_LOGI") == std::string::npos);
}

void connection_power_reset_has_one_balanced_critical_section() {
  const auto source = read_source("main/platform/ble_hid.cpp");
  const auto reset = section(
      source,
      "void BleHidTransport::reset_connection_power_state",
      "const char* BleHidTransport::connection_profile_name");
  const auto enter = reset.find("portENTER_CRITICAL(&connection_power_mux_)");
  const auto exit = reset.find("portEXIT_CRITICAL(&connection_power_mux_)");
  assert(enter != std::string::npos);
  assert(exit != std::string::npos);
  assert(reset.find("portENTER_CRITICAL(&connection_power_mux_)", enter + 1) ==
         std::string::npos);
  assert(reset.find("portEXIT_CRITICAL(&connection_power_mux_)", exit + 1) ==
         std::string::npos);
  assert(exit > enter);
}

void connection_update_failures_share_bounded_backoff() {
  const auto source = read_source("main/platform/ble_hid.cpp");
  const auto request = section(
      source,
      "void BleHidTransport::request_connection_parameters",
      "void BleHidTransport::reconcile_connection_power_profile");
  assert(request.find("schedule_connection_update_retry_locked(now_us)") !=
         std::string::npos);
  assert(request.find("desired_changed_while_requesting") !=
         std::string::npos);

  const auto update_event = section(source,
                                    "case BLE_GAP_EVENT_CONN_UPDATE: {",
                                    "case BLE_GAP_EVENT_CONN_UPDATE_REQ:");
  assert(update_event.find("schedule_connection_update_retry_locked(now_us)") !=
         std::string::npos);
  assert(update_event.find("desired_changed_while_in_flight") !=
         std::string::npos);
}

void ble_status_refresh_publishes_current_config_fingerprint() {
  const auto source = read_source("main/app_main.cpp");
  const auto refresh = section(source,
                               "void process_pending_status_refresh",
                               "void log_power_mode_transition");
  const auto ble_branch = refresh.substr(refresh.find("if (ble_requested)"));

  assert(refresh.find("config_state.last_applied_json()") != std::string::npos);
  assert(ble_branch.find("publish_config_status(") != std::string::npos);
  assert(ble_branch.find("applied_json.size()") != std::string::npos);
  assert(ble_branch.find("applied_crc") != std::string::npos);
  assert(ble_branch.find("publish_config_status_without_payload") ==
         std::string::npos);
}

void speaker_probe_status_uses_one_generation_for_usb_and_ble() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto refresh = section(app_main,
                               "void process_pending_status_refresh",
                               "void log_power_mode_transition");
  const auto diagnostic_branch = section(
      refresh,
      "#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)",
      "#else");
  const auto publisher = section(app_main,
                                 "std::string publish_config_status",
                                 "void publish_config_status_for_json");
  const auto finalize = section(speaker_output,
                                "void SpeakerOutput::finalize_probe",
                                "void SpeakerOutput::refresh_probe_metrics_locked");
  const auto begin = section(speaker_output,
                             "esp_err_t SpeakerOutput::begin(",
                             "bool SpeakerOutput::ready() const");
  const auto request = section(
      speaker_output,
      "bool SpeakerOutput::request_diagnostic_tone()",
      "void SpeakerOutput::poll(bool playback_allowed)");
  const auto busy_rejection_begin = request.find("if (!ticket.accepted)");
  const auto accepted_reset = request.find("reset_probe_run(ticket.generation)");
  assert(busy_rejection_begin != std::string::npos);
  assert(accepted_reset != std::string::npos);
  assert(busy_rejection_begin < accepted_reset);
  const auto busy_rejection =
      request.substr(busy_rejection_begin,
                     accepted_reset - busy_rejection_begin);

  assert(diagnostic_branch.find(
             "const auto speaker_probe = app->speaker.probe_snapshot();") !=
         std::string::npos);
  assert(diagnostic_branch.find(
             "const auto speaker_status_json = publish_config_status(") !=
         std::string::npos);
  assert(diagnostic_branch.find("\"spk_probe\"") != std::string::npos);
  assert(diagnostic_branch.find("&speaker_probe") != std::string::npos);
  assert(diagnostic_branch.find("speaker_status_json.empty()") !=
         std::string::npos);
  assert(diagnostic_branch.find(
             "speaker_status_json,\n           usb_request_epoch") !=
         std::string::npos);
  assert(diagnostic_branch.find("\"battery\"") == std::string::npos);
  assert(publisher.find("snapshot.speaker = speaker_probe;") !=
         std::string::npos);

  assert(speaker_output.find("portENTER_CRITICAL(&probe_mux_)") !=
         std::string::npos);
  assert(speaker_output.find("portEXIT_CRITICAL(&probe_mux_)") !=
         std::string::npos);
  assert(finalize.find("resolve_speaker_probe_terminal(") !=
         std::string::npos);
  assert(finalize.find("record_probe_terminal(terminal.stage") !=
         std::string::npos);
  assert(begin.find("begin_snapshot.generation = 0;") != std::string::npos);
  assert(begin.find("probe_snapshot_ = begin_snapshot;") != std::string::npos);
  assert(busy_rejection.find("record_probe_state") == std::string::npos);
  assert(busy_rejection.find("record_probe_terminal") == std::string::npos);
  assert(speaker_output.find("finish_opus_probe_metrics(generation)") !=
         std::string::npos);
  assert(speaker_output.find("record_cleanup_failure(generation") !=
         std::string::npos);
  assert(speaker_output.find("observe_speaker_probe_cleanup(") !=
         std::string::npos);
}

void release_build_rejects_internal_ram_profile_drift() {
  const auto cmake = read_source("CMakeLists.txt");
  const auto defaults = read_source("sdkconfig.defaults");

  assert(cmake.find("\"${CMAKE_BINARY_DIR}/sdkconfig\"") !=
         std::string::npos);
  assert(cmake.find("\"${CMAKE_SOURCE_DIR}/sdkconfig.defaults\"") !=
         std::string::npos);
  assert(cmake.find("EasyInput V2 only supports IDF_TARGET=esp32s3") !=
         std::string::npos);
  assert(cmake.find("\"EasyInput production SoC target\"") !=
         std::string::npos);
  assert(cmake.find("CONFIG_IDF_TARGET_ESP32S3") != std::string::npos);
  assert(cmake.find("easy_input_ignored_source_sdkconfig") !=
         std::string::npos);
  assert(cmake.find("The ignored source-tree sdkconfig is not a valid") !=
         std::string::npos);
  assert(cmake.find("CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE EQUAL 4096") !=
         std::string::npos);
  assert(cmake.find("CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT EQUAL 12") !=
         std::string::npos);
  assert(cmake.find("CONFIG_BT_NIMBLE_MAX_CONNECTIONS EQUAL 3") !=
         std::string::npos);
  assert(cmake.find("CONFIG_ESP_COREDUMP_ENABLE_TO_NONE") !=
         std::string::npos);
  assert(cmake.find("CONFIG_FREERTOS_ISR_STACKSIZE EQUAL 1536") !=
         std::string::npos);
  assert(cmake.find(
             "CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL EQUAL 12") !=
         std::string::npos);
  assert(cmake.find(
             "CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SUPERVISION_TMO EQUAL 400") !=
         std::string::npos);
  assert(cmake.find("CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0") !=
         std::string::npos);
  assert(cmake.find("CONFIG_SPIRAM_MODE_OCT") !=
         std::string::npos);
  assert(cmake.find("CONFIG_SPIRAM_SPEED_80M") !=
         std::string::npos);
  assert(cmake.find("CONFIG_SPIRAM_USE_CAPS_ALLOC") !=
         std::string::npos);
  assert(cmake.find("EasyInput V2 PSRAM memory-domain profile drifted") !=
         std::string::npos);
  assert(cmake.find("Do not reuse an ignored/stale sdkconfig") !=
         std::string::npos);

  assert(defaults.find("CONFIG_IDF_TARGET=\"esp32s3\"") !=
         std::string::npos);
  assert(defaults.find("CONFIG_IDF_TARGET_ESP32S3=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=12") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3") !=
         std::string::npos);
  assert(defaults.find("CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_FREERTOS_ISR_STACKSIZE=1536") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL=12") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MAX_CONN_INTERVAL=36") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SLAVE_LATENCY=0") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SUPERVISION_TMO=400") !=
         std::string::npos);
  assert(defaults.find("CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM=y") != std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_MODE_OCT=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_SPEED_80M=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_USE_CAPS_ALLOC=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_IGNORE_NOTFOUND=n") !=
         std::string::npos);
}

void audio_start_reuses_boot_resource_pool() {
  const auto source = read_source("main/platform/keyboard_audio.cpp");
  const auto header = read_source("main/platform/keyboard_audio.h");
  const auto begin = section(source,
                             "esp_err_t KeyboardAudioLink::begin()",
                             "void KeyboardAudioLink::configure");
  const auto start = section(source,
                             "void KeyboardAudioLink::start_stream",
                             "void KeyboardAudioLink::stop_stream");
  const auto stream = section(source,
                              "void KeyboardAudioLink::run_audio_stream",
                              "void KeyboardAudioLink::run_audio_capture");
  const auto capture = section(source,
                               "void KeyboardAudioLink::run_audio_capture",
                               "esp_err_t KeyboardAudioLink::prepare_microphone_channel");
  const auto workers = section(source,
                               "void KeyboardAudioLink::task_entry",
                               "void KeyboardAudioLink::control_task_entry");

  assert(source.find("constexpr std::size_t kAudioCaptureQueueFrames = 64;") !=
         std::string::npos);
  assert(begin.find(
             "heap_caps_malloc(\n"
             "              kAudioCaptureQueueBytes,\n"
             "              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)") !=
         std::string::npos);
  assert(begin.find(
             "heap_caps_calloc(\n"
             "              1U,\n"
             "              sizeof(StaticQueue_t),\n"
             "              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)") !=
         std::string::npos);
  assert(begin.find(
             "xQueueCreateStatic(\n"
             "      kAudioCaptureQueueFrames,\n"
             "      sizeof(CapturedAudioFrame),\n"
             "      capture_queue_storage_,\n"
             "      capture_queue_control_)") !=
         std::string::npos);
  assert(begin.find("esp_psram_is_initialized()") !=
         std::string::npos);
  assert(begin.find("esp_psram_get_size()") !=
         std::string::npos);
  assert(begin.find("aud_psram") != std::string::npos);
  assert(begin.find("vQueueDelete(capture_queue_)") !=
         std::string::npos);
  assert(begin.find("heap_caps_free(capture_queue_storage_)") !=
         std::string::npos);
  assert(begin.find("heap_caps_free(capture_queue_control_)") !=
         std::string::npos);
  assert(begin.find("xQueueCreateWithCaps") == std::string::npos);
  assert(begin.find("vQueueDeleteWithCaps") == std::string::npos);
  assert(begin.find("xQueueCreate(kAudioCaptureQueueFrames") ==
         std::string::npos);
  assert(begin.find("xSemaphoreCreateBinary()") != std::string::npos);
  assert(begin.find("\"mic_udp\"") != std::string::npos);
  assert(begin.find("\"mic_capture\"") != std::string::npos);
  assert(begin.find("aud_psram") != std::string::npos);
  assert(begin.find("aud_pool_sig") != std::string::npos);
  assert(begin.find("aud_pool_tx") != std::string::npos);
  assert(begin.find("aud_pool_cap") != std::string::npos);
  assert(begin.find("aud_ctrl") != std::string::npos);
  assert(begin.find("init_state_ == InitState::Failed") != std::string::npos);
  assert(begin.find("init_state_ = InitState::Failed") != std::string::npos);

  assert(start.find("xTaskCreate") == std::string::npos);
  assert(start.find("new (std::nothrow)") == std::string::npos);
  assert(start.find("stream_job_generation_ = generation") != std::string::npos);
  assert(start.find("xTaskNotifyGive(stream_worker_task_)") != std::string::npos);

  assert(stream.find("xQueueCreate") == std::string::npos);
  assert(stream.find("xTaskCreate") == std::string::npos);
  assert(stream.find("vQueueDelete") == std::string::npos);
  assert(stream.find("xQueueReset(frame_queue)") != std::string::npos);
  assert(stream.find("xTaskNotifyGive(capture_worker_task_)") !=
         std::string::npos);
  assert(stream.find("capture_completed_generation_ == generation") !=
         std::string::npos);
  assert(stream.find("xSemaphoreTake(capture_done_") != std::string::npos);
  assert(capture.find("xSemaphoreGive(capture_done_)") != std::string::npos);
  assert(capture.find("xTaskNotifyGive(stream_task)") == std::string::npos);

  assert(workers.find("for (;;)") != std::string::npos);
  assert(workers.find("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)") !=
         std::string::npos);
  assert(header.find("QueueHandle_t capture_queue_ = nullptr;") !=
         std::string::npos);
  assert(header.find(
             "std::uint8_t* capture_queue_storage_ = nullptr;") !=
         std::string::npos);
  assert(header.find(
             "StaticQueue_t* capture_queue_control_ = nullptr;") !=
         std::string::npos);
  assert(header.find("SemaphoreHandle_t capture_done_ = nullptr;") !=
         std::string::npos);
  assert(header.find("TaskHandle_t stream_worker_task_ = nullptr;") !=
         std::string::npos);
  assert(header.find("TaskHandle_t capture_worker_task_ = nullptr;") !=
         std::string::npos);
  assert(header.find("InitState init_state_ = InitState::Uninitialized;") !=
         std::string::npos);
  assert(header.find("std::uint32_t capture_completed_generation_ = 0;") !=
         std::string::npos);
  assert(source.find("heap_caps_get_largest_free_block") != std::string::npos);
  assert(source.find("static_assert(sizeof(CapturedAudioFrame) == 652)") !=
         std::string::npos);
}

void speaker_probe_is_default_off_and_outside_input_hot_path() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto app_main = read_source("main/app_main.cpp");
  const auto keyboard_audio =
      read_source("main/platform/keyboard_audio.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto input_handler = section(app_main,
                                     "void handle_input_event(const easy_input::InputEvent& event, void* context) {",
                                     "void load_stored_config");
  const auto speaker_boot = section(
      app_main,
      "#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)\n"
      "  app.speaker.mark_boot_pending(",
      "#endif\n  sync_led_status");

  const auto option = section(root_cmake,
                              "set(\n  EASY_INPUT_SPEAKER_DIAGNOSTIC",
                              "# The production PCB");
  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find(
             "set(\n"
             "  EASY_INPUT_SPEAKER_ASSETS_PRODUCT\n"
             "  ON\n") != std::string::npos);
  assert(main_cmake.find("if(EASY_INPUT_SPEAKER_DIAGNOSTIC)") !=
         std::string::npos);
  assert(main_cmake.find(
             "list(APPEND easy_input_main_sources \"platform/speaker_output.cpp\")") !=
         std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_DIAGNOSTIC=1") !=
         std::string::npos);
  assert(app_main.find("#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)") !=
         std::string::npos);
  assert(keyboard_audio.find(
             "constexpr i2s_port_t kMicI2sController = I2S_NUM_0;") !=
         std::string::npos);
  assert(speaker_output.find(
             "constexpr i2s_port_t kSpeakerI2sController = I2S_NUM_1;") !=
         std::string::npos);
  assert(speaker_output.find(
             "ai_keyboard::kSpeakerPlaybackSampleRate") !=
         std::string::npos);
  assert(speaker_output.find(
             "std::array<std::int16_t, 48> kTone1kHz") !=
         std::string::npos);
  assert(speaker_output.find(
             "I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG") !=
         std::string::npos);
  assert(speaker_output.find(
             "standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;") !=
         std::string::npos);
  assert(speaker_output.find(
             "static_assert(kSamplesPerFrame == 480U);") !=
         std::string::npos);
  assert(speaker_output.find(
             "static_assert(kFadeSamples == 720U);") !=
         std::string::npos);
  assert(speaker_output.find(
             "static_assert(kToneFrames == 60U);") !=
         std::string::npos);
  assert(speaker_output.find("kSampleRate = 24000") == std::string::npos);
  assert(app_main.find(
             "app.audio.set_audio_io_arbiter(&app.audio_io_arbiter);") !=
         std::string::npos);
  assert(keyboard_audio.find("wait_for_microphone_hardware(generation)") !=
         std::string::npos);
  assert(speaker_output.find(
             "audio_io_arbiter_->microphone_requested()") !=
         std::string::npos);
  const auto pending_assignment =
      app_main.find("app.speaker_probe_pending = true;");
  assert(pending_assignment != std::string::npos);
  assert(app_main.find("app.speaker_probe_pending = true;",
                       pending_assignment + 1) == std::string::npos);
  const auto tone_request =
      app_main.find("app->speaker.request_diagnostic_tone()");
  assert(tone_request != std::string::npos);
  assert(app_main.find("app->speaker.request_diagnostic_tone()",
                       tone_request + 1) == std::string::npos);
  assert(speaker_boot.find("wake_cause != ESP_SLEEP_WAKEUP_EXT1") !=
         std::string::npos);
  assert(speaker_boot.find(
             "speaker boot sound skipped after deep-sleep key wake") !=
         std::string::npos);
  assert(speaker_boot.find("external_power_status_active") ==
         std::string::npos);
  assert(speaker_boot.find("usb_vbus_status_present") ==
         std::string::npos);
  assert(speaker_boot.find(".ble.connected()") == std::string::npos);
  assert(speaker_boot.find(".usb.mounted()") == std::string::npos);
  assert(speaker_boot.find("skipped on battery power") ==
         std::string::npos);
  assert(input_handler.find("speaker") == std::string::npos);
  assert(input_handler.find("play") == std::string::npos);
}

void speaker_opus_probe_is_conditional_fixed_and_reusable() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto main_manifest = read_source("main/idf_component.yml");
  const auto adapter_cmake =
      read_source("diagnostics/speaker_opus_probe/CMakeLists.txt");
  const auto adapter_manifest =
      read_source("diagnostics/speaker_opus_probe/idf_component.yml");
  const auto adapter =
      read_source("diagnostics/speaker_opus_probe/speaker_opus_probe.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto app_main = read_source("main/app_main.cpp");
  const auto fixture = read_source(
      "diagnostics/speaker_opus_probe/assets/easyinput_boot_probe.ogg");
  const auto input_handler = section(
      app_main,
      "void handle_input_event(const easy_input::InputEvent& event, void* context) {",
      "void load_stored_config");

  const auto option = section(
      root_cmake,
      "set(\n  EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC",
      "# The production PCB");
  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find(
             "NOT EASY_INPUT_SPEAKER_DIAGNOSTIC") != std::string::npos);
  assert(option.find(
             "diagnostics/speaker_opus_probe") != std::string::npos);
  assert(root_cmake.find(
             "\"${CMAKE_BINARY_DIR}/dependencies.opus.lock\"") !=
         std::string::npos);
  assert(root_cmake.find(
             "EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC_ENABLED") !=
         std::string::npos);

  assert(main_manifest.find("esp_audio_codec") == std::string::npos);
  assert(main_cmake.find(
             "idf_build_get_property(\n"
             "  easy_input_speaker_opus_diagnostic_enabled\n"
             "  EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC_ENABLED") !=
         std::string::npos);
  assert(main_cmake.find(
             "if(easy_input_speaker_opus_diagnostic_enabled)") !=
         std::string::npos);
  assert(main_cmake.find(
             "list(APPEND easy_input_main_requires speaker_opus_probe)") !=
         std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC=1") !=
         std::string::npos);

  assert(adapter_manifest.find(
             "espressif/esp_audio_codec: \"==2.5.0\"") !=
         std::string::npos);
  assert(adapter_manifest.find("idf: \"==5.5.5\"") !=
         std::string::npos);
  assert(adapter_cmake.find(
             "\"assets/easyinput_boot_probe.ogg\"") !=
         std::string::npos);
  assert(fixture.size() == 1734U);
  assert(fixture.compare(0, 4, "OggS") == 0);
  assert(fixture.find("OpusHead") != std::string::npos);

  assert(adapter.find("esp_opus_dec_register()") != std::string::npos);
  assert(adapter.find("esp_ogg_dec_register()") != std::string::npos);
  assert(adapter.find("esp_audio_dec_register_default") ==
         std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_register_default") ==
         std::string::npos);
  assert(adapter.find("ESP_AUDIO_SIMPLE_DEC_TYPE_OGG") !=
         std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_open") != std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_reset") != std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_process") != std::string::npos);
  assert(adapter.find("_binary_easyinput_boot_probe_ogg_start") !=
         std::string::npos);
  assert(adapter.find("_binary_easyinput_boot_probe_ogg_end") !=
         std::string::npos);
  assert(adapter.find("heap_caps_malloc") != std::string::npos);
  assert(adapter.find("realloc(") == std::string::npos);
  assert(adapter.find(
             "static_assert(kMaximumDecodedFrameBytes == 1920U);") !=
         std::string::npos);

  assert(speaker_output.find("opus_probe_.begin()") != std::string::npos);
  assert(speaker_output.find("opus_probe_.reset()") != std::string::npos);
  assert(speaker_output.find("opus_probe_.decode_next") !=
         std::string::npos);
  assert(speaker_output.find(
             "constexpr std::uint32_t kWorkerStackBytes = 20U * 1024U;") !=
         std::string::npos);
  assert(speaker_output.find(
             "std::min(kSamplesPerFrame, decoded.sample_count - offset)") !=
         std::string::npos);
  assert(speaker_output.find("cancelled(generation)") !=
         std::string::npos);
  assert(speaker_output.find("heap_caps_get_minimum_free_size") !=
         std::string::npos);
  assert(speaker_output.find("uxTaskGetStackHighWaterMark") !=
         std::string::npos);
  assert(speaker_output.find("maximum_decode_call_us_") !=
         std::string::npos);
  const auto play_tone = section(
      speaker_output,
      "SpeakerOutput::WorkerResult SpeakerOutput::play_sound(",
      "#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)\n"
      "esp_err_t SpeakerOutput::prepare_opus_first_frame");
  const auto first_decode =
      play_tone.find("prepare_opus_first_frame(generation, &first_opus_frame)");
  const auto clocks_on = play_tone.find("i2s_channel_enable(tx_channel_)");
  assert(first_decode != std::string::npos);
  assert(clocks_on != std::string::npos);
  assert(first_decode < clocks_on);
  assert(input_handler.find("opus") == std::string::npos);
  assert(input_handler.find("speaker") == std::string::npos);
}

void speaker_ima_probe_is_conditional_allocation_free_and_isolated() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto adapter_cmake =
      read_source("diagnostics/speaker_ima_adpcm_probe/CMakeLists.txt");
  const auto decoder_header = read_source(
      "diagnostics/speaker_ima_adpcm_probe/include/ima_adpcm_decoder.h");
  const auto decoder = read_source(
      "diagnostics/speaker_ima_adpcm_probe/ima_adpcm_decoder.cpp");
  const auto adapter = read_source(
      "diagnostics/speaker_ima_adpcm_probe/speaker_ima_adpcm_probe.cpp");
  const auto asset = read_source(
      "diagnostics/speaker_ima_adpcm_probe/assets/"
      "easyinput_boot_probe_eiad.h");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto audio_contract = read_source(
      "components/keyboard/include/keyboard/speaker_audio_contract.h");
  const auto app_main = read_source("main/app_main.cpp");
  const auto input_handler = section(
      app_main,
      "void handle_input_event(const easy_input::InputEvent& event, void* context) {",
      "void load_stored_config");

  const auto option = section(
      root_cmake,
      "set(\n  EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC",
      "# The production PCB");
  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find("NOT EASY_INPUT_SPEAKER_DIAGNOSTIC") !=
         std::string::npos);
  assert(option.find("mutually exclusive") != std::string::npos);
  assert(option.find("diagnostics/speaker_ima_adpcm_probe") !=
         std::string::npos);
  assert(root_cmake.find(
             "EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC_ENABLED") !=
         std::string::npos);
  assert(main_cmake.find(
             "idf_build_get_property(\n"
             "  easy_input_speaker_ima_adpcm_diagnostic_enabled\n"
             "  EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC_ENABLED") !=
         std::string::npos);
  assert(main_cmake.find(
             "list(APPEND easy_input_main_requires "
             "speaker_ima_adpcm_probe)") != std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC=1") !=
         std::string::npos);

  assert(adapter_cmake.find("idf_component_register") != std::string::npos);
  assert(adapter_cmake.find("esp_audio_codec") == std::string::npos);
  assert(adapter.find("esp_audio") == std::string::npos);
  assert(adapter.find("Opus") == std::string::npos);
  assert(decoder.find("malloc") == std::string::npos);
  assert(decoder.find("calloc") == std::string::npos);
  assert(decoder.find("realloc") == std::string::npos);
  assert(decoder.find("new ") == std::string::npos);
  assert(decoder.find("std::vector") == std::string::npos);
  assert(decoder_header.find(
             "kImaAdpcmAssetSampleRate = 48000") != std::string::npos);
  assert(decoder_header.find(
             "kImaAdpcmFrameSamples = 480") != std::string::npos);
  assert(decoder_header.find(
             "static_assert(sizeof(ImaAdpcmDecoder) <= 48U)") !=
         std::string::npos);
  assert(asset.find("decode_base64<6872U>") != std::string::npos);
  assert(asset.find(
             "static_assert(kEasyInputBootProbeEiad.size() == 6872U)") !=
         std::string::npos);

  assert(speaker_output.find("ima_probe_.begin()") != std::string::npos);
  assert(speaker_output.find("ima_probe_.reset()") != std::string::npos);
  assert(speaker_output.find("ima_probe_.decode_next") !=
         std::string::npos);
  assert(speaker_output.find(
             "constexpr std::uint32_t kWorkerStackBytes = 4096;") !=
         std::string::npos);
  assert(audio_contract.find(
             "kSpeakerPlaybackPreloadZeroFrames = 1U") !=
         std::string::npos);
  assert(audio_contract.find(
             "speaker_normal_drain_zero_frames(") !=
         std::string::npos);
  assert(audio_contract.find(
             "speaker_first_pcm_queue_upper_bound_us(") !=
         std::string::npos);
  const auto preload = section(
      speaker_output,
      "esp_err_t SpeakerOutput::preload_zero_dma(",
      "esp_err_t SpeakerOutput::write_samples(");
  const auto preload_call =
      preload.find("i2s_channel_preload_data(tx_channel_,");
  assert(preload_call != std::string::npos);
  assert(preload.find("i2s_channel_preload_data(tx_channel_,",
                      preload_call + 1U) == std::string::npos);
  assert(preload.find("bytes_loaded == expected_bytes") !=
         std::string::npos);
  const auto play_tone = section(
      speaker_output,
      "SpeakerOutput::WorkerResult SpeakerOutput::play_sound(",
      "#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)\n"
      "esp_err_t SpeakerOutput::prepare_opus_first_frame");
  const auto first_decode =
      play_tone.find("prepare_ima_first_frame(generation,");
  const auto clocks_on = play_tone.find("i2s_channel_enable(tx_channel_)");
  assert(first_decode != std::string::npos);
  assert(clocks_on != std::string::npos);
  assert(first_decode < clocks_on);
  assert(play_tone.find(
             "play_ima_frames(generation, frame.data(), frame.size())") !=
         std::string::npos);
  assert(play_tone.find(
             "write_samples(frame.data(), frame.size())") !=
         std::string::npos);
  const auto cancellation_snapshot =
      play_tone.find("bool cancellation_seen = cancelled(generation);");
  const auto normal_drain =
      play_tone.find("index < kNormalDrainZeroFrames");
  const auto stop_clock =
      play_tone.find("i2s_channel_disable(tx_channel_)", normal_drain);
  assert(cancellation_snapshot != std::string::npos);
  assert(normal_drain != std::string::npos);
  assert(stop_clock != std::string::npos);
  assert(cancellation_snapshot < normal_drain);
  assert(normal_drain < stop_clock);
  assert(play_tone.find(
             "if (err == ESP_OK && !cancellation_seen)") !=
         std::string::npos);
  assert(speaker_output.find(
             "conservative_first_pcm_latency_us(requested)") !=
         std::string::npos);
  const auto prepare_ima = section(
      speaker_output,
      "esp_err_t SpeakerOutput::prepare_ima_first_frame(",
      "esp_err_t SpeakerOutput::play_ima_frames(");
  assert(prepare_ima.find(
             "std::fill(output + *output_samples, "
             "output + output_capacity, 0)") !=
         std::string::npos);
  const auto stream_ima = section(
      speaker_output,
      "esp_err_t SpeakerOutput::play_ima_frames(",
      "esp_err_t SpeakerOutput::finish_ima_probe_metrics(");
  assert(stream_ima.find(
             "std::fill(frame + decoded_samples, "
             "frame + frame_capacity, 0)") !=
         std::string::npos);
  assert(stream_ima.find(
             "write_samples(frame, frame_capacity)") !=
         std::string::npos);
  const auto ima_begin_failure = section(
      speaker_output,
      "err = ima_probe_.begin();",
      "#endif\n\n  supervisor_task_");
  assert(ima_begin_failure.find(
             "ai_keyboard::SpeakerProbeStage::DecodeReset") !=
         std::string::npos);
  assert(ima_begin_failure.find(
             "ai_keyboard::SpeakerProbeError::DecodeReset") !=
         std::string::npos);
  assert(ima_begin_failure.find(
             "ai_keyboard::SpeakerProbeError::InvalidArgument") ==
         std::string::npos);
  assert(app_main.find(
             "\"0.4.40-idf-v2-spk-ima-probe\"") != std::string::npos);
  assert(input_handler.find("ima") == std::string::npos);
  assert(input_handler.find("speaker") == std::string::npos);
}

void speaker_asset_storage_reserves_exact_banks_and_has_production_gate() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto partitions = read_source("partitions.csv");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto app_main = read_source("main/app_main.cpp");
  const auto assets_link_anchor =
      read_source("features/speaker_assets/diagnostic_link_anchor.cpp");
  const auto nvs_store = read_source("main/platform/nvs_store.cpp");
  const auto config_state =
      read_source("components/keyboard/src/config_state.cpp");
  const auto input_handler = section(
      app_main,
      "void handle_input_event(const easy_input::InputEvent& event, void* context) {",
      "void load_stored_config");
  const auto option = section(
      root_cmake,
      "set(\n  EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC",
      "# The production PCB");
  const auto app_main_entry = section(
      app_main,
      "extern \"C\" void app_main(void) {",
      "  static AppContext app;");

  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find(
             "if(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC OR\n"
             "   EASY_INPUT_SPEAKER_ASSETS_PRODUCT)\n"
             "  list(APPEND EXTRA_COMPONENT_DIRS\n"
             "    \"${CMAKE_SOURCE_DIR}/features/speaker_assets\")\n"
             "endif()") != std::string::npos);
  const auto assets_component =
      read_source("features/speaker_assets/CMakeLists.txt");
  assert(assets_component.find("WHOLE_ARCHIVE") !=
         std::string::npos);
  assert(assets_component.find("diagnostic_link_anchor.cpp") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "#include \"speaker_assets/speaker_assets_flash_runner.h\"") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "SpeakerAssetsCooperativeStoreRunner runner(") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "runner.publish_priority_allowed(true);") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "runner.step(emission.action, &completion)") !=
         std::string::npos);
  assert(assets_link_anchor.find("runner.worker_run_once()") !=
         std::string::npos);
  assert(root_cmake.find(
             "idf_build_set_property(\n"
             "  EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC_ENABLED\n"
             "  \"${EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC}\"\n"
             ")") !=
         std::string::npos);
  assert(main_cmake.find(
             "if(easy_input_speaker_assets_diagnostic_enabled)\n"
             "  list(APPEND easy_input_main_requires speaker_assets)\n"
             "endif()") != std::string::npos);
  assert(main_cmake.find(
             "\"platform/speaker_assets_supervisor.cpp\"") !=
         std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_ASSETS_PRODUCT=1") !=
         std::string::npos);
  assert(main_cmake.find(
             "if(easy_input_speaker_assets_diagnostic_enabled)\n"
             "  target_compile_definitions(\n"
             "    ${COMPONENT_LIB}\n"
             "    PRIVATE EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC=1\n"
             "  )\n"
             "endif()") != std::string::npos);
  assert(app_main_entry.find(
             "#if defined(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC)\n"
             "  // Link-only diagnostic:") != std::string::npos);
  assert(app_main_entry.find(
             "const void* volatile speaker_assets_link_anchor =\n"
             "      easy_input_speaker_assets_diagnostic_link_anchor();") !=
         std::string::npos);
  assert(app_main_entry.find(
             "static_cast<void>(speaker_assets_link_anchor);\n"
             "#endif") !=
         std::string::npos);
  assert(root_cmake.find("easy_input_expected_partition_entries") !=
         std::string::npos);
  assert(root_cmake.find(
             "\"sound_a,0x40,0x00,0x310000,0x90000,\"") !=
         std::string::npos);
  assert(root_cmake.find(
             "\"sound_b,0x40,0x01,0x3a0000,0x90000,\"") !=
         std::string::npos);

  const auto nvs = partitions.find(
      "nvs,      data, nvs,     0x9000,   0x6000,");
  const auto phy = partitions.find(
      "phy_init, data, phy,     0xf000,   0x1000,");
  const auto factory = partitions.find(
      "factory,  app,  factory, 0x10000,  0x300000,");
  const auto sound_a = partitions.find(
      "sound_a,  0x40, 0x00,    0x310000, 0x90000,");
  const auto sound_b = partitions.find(
      "sound_b,  0x40, 0x01,    0x3A0000, 0x90000,");
  assert(nvs != std::string::npos);
  assert(phy != std::string::npos);
  assert(factory != std::string::npos);
  assert(sound_a != std::string::npos);
  assert(sound_b != std::string::npos);
  assert(nvs < phy);
  assert(phy < factory);
  assert(factory < sound_a);
  assert(sound_a < sound_b);

  assert(main_cmake.find("sound_asset_store") == std::string::npos);
  assert(app_main.find(
             "app->speaker_assets.begin_local(\n"
             "              &app->usb, app->platform_task)") !=
         std::string::npos);
  assert(input_handler.find("sound_asset") == std::string::npos);
  assert(input_handler.find("sound_bank") == std::string::npos);
  assert(nvs_store.find("sound_asset") == std::string::npos);
  assert(nvs_store.find("sound_bank") == std::string::npos);
  assert(config_state.find("sound_asset") == std::string::npos);
  assert(config_state.find("sound_bank") == std::string::npos);
}

void speaker_asset_usb_lifetime_precedes_first_frame() {
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");
  const auto callback_path = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::enqueue_usb_frame(",
      "void SpeakerAssetsSupervisor::store_task_entry");
  const auto ensure_position = callback_path.find(
      "ensure_mailbox_usb_route_locked(endpoint_epoch)");
  const auto frame_position = callback_path.find(
      "mailbox_.enqueue_usb_frame(");
  assert(ensure_position != std::string::npos);
  assert(frame_position != std::string::npos);
  assert(ensure_position < frame_position);

  const auto route_publication = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::ensure_mailbox_usb_route_locked(",
      "void SpeakerAssetsSupervisor::observe_usb_route");
  const auto close_position = route_publication.find(
      "mailbox_.enqueue_route_closed(");
  const auto open_position = route_publication.find(
      "mailbox_.enqueue_route_opened(");
  assert(close_position != std::string::npos);
  assert(open_position != std::string::npos);
  assert(close_position < open_position);
}

void speaker_asset_wifi_carrier_is_the_only_wireless_bulk_path() {
  const auto ble_header = read_source("main/platform/ble_hid.h");
  const auto ble = read_source("main/platform/ble_hid.cpp");
  const auto supervisor_header =
      read_source("main/platform/speaker_assets_supervisor.h");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");
  const auto protocol_header = read_source(
      "features/speaker_assets/include/speaker_assets/"
      "speaker_assets_protocol.h");
  const auto protocol = read_source(
      "features/speaker_assets/speaker_assets_protocol.cpp");
  const auto runtime_header = read_source(
      "features/speaker_assets/include/speaker_assets/"
      "speaker_assets_runtime.h");
  const auto runtime = read_source(
      "features/speaker_assets/speaker_assets_runtime.cpp");
  const auto carrier =
      read_source("main/platform/speaker_assets_wifi.cpp");

  // Bluetooth remains the keyboard's HID/config transport, but it must not
  // expose or route the retired speaker bulk service.
  assert(ble_header.find("SpeakerAssets") == std::string::npos);
  assert(ble.find("SpeakerAssets") == std::string::npos);

  assert(supervisor_header.find("SpeakerAssetsWifiCarrier wifi_;") !=
         std::string::npos);
  assert(supervisor.find("enqueue_wifi_frame(") !=
         std::string::npos);
  assert(supervisor.find("retire_accepted_wifi_reply()") !=
         std::string::npos);
  assert(supervisor.find("offer_wifi_reply()") !=
         std::string::npos);
  assert(protocol.find("encode_speaker_assets_wifi_frame(") !=
         std::string::npos);
  assert(runtime_header.find("enqueue_wifi_frame(") !=
         std::string::npos);
  assert(runtime.find("SpeakerAssetsTransport::Wifi") !=
         std::string::npos);
  assert(protocol_header.find("Wifi = 3U") != std::string::npos);
  assert(carrier.find("TCP_NODELAY") != std::string::npos);
  assert(
      carrier.find(
          "setsockopt(\n"
          "            client,\n"
          "            IPPROTO_TCP,\n"
          "            TCP_NODELAY") != std::string::npos);

  const auto peer_probe = section(
      carrier,
      "bool peer_socket_open_non_consuming(int socket)",
      "}  // namespace");
  assert(peer_probe.find("MSG_PEEK | MSG_DONTWAIT") !=
         std::string::npos);
  assert(peer_probe.find(
             "classify_speaker_assets_wifi_peer_probe(") !=
         std::string::npos);

  const auto response_wait = section(
      carrier,
      "PendingResponse response{};",
      "if (!keep_running || !response.ready)");
  const auto probe_position = response_wait.find(
      "peer_socket_open_non_consuming(socket)");
  const auto response_position = response_wait.find(
      "take_pending_response(route, &response)");
  assert(probe_position != std::string::npos);
  assert(response_position != std::string::npos);
  assert(probe_position < response_position);

  const auto activate_route = section(
      carrier,
      "void SpeakerAssetsWifiCarrier::set_active_route(",
      "void SpeakerAssetsWifiCarrier::clear_active_route(");
  const auto publish_busy =
      activate_route.find("state_.route_active = true;");
  const auto unlock_route = activate_route.find("unlock();");
  const auto refresh_busy = activate_route.find(
      "audio_->request_heartbeat_refresh();");
  assert(publish_busy != std::string::npos);
  assert(unlock_route != std::string::npos);
  assert(refresh_busy != std::string::npos);
  assert(publish_busy < unlock_route);
  assert(unlock_route < refresh_busy);

  const auto cleanup = section(
      carrier,
      "clear_pending_response(route);",
      "speaker_assets::SpeakerAssetsWifiPolicyInputs");
  const auto clear_pending =
      cleanup.find("clear_pending_response(route);");
  const auto clear_route =
      cleanup.find("clear_active_route(route);");
  const auto close_route =
      cleanup.find("observe_route_(callback_context_, route, false)");
  const auto release_service =
      cleanup.find("release_wifi_service_lease(service_lease)");
  assert(clear_pending != std::string::npos);
  assert(clear_route != std::string::npos);
  assert(close_route != std::string::npos);
  assert(release_service != std::string::npos);
  assert(clear_pending < clear_route);
  assert(clear_route < close_route);
  assert(close_route < release_service);
}

void speaker_asset_endpoint_accept_retires_before_lifetime_unlock() {
  const auto usb_header =
      read_source("main/platform/usb_hid.h");
  const auto usb = read_source("main/platform/usb_hid.cpp");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");

  assert(usb_header.find(
             "using SpeakerAssetsResponseAcceptedCallback = bool (*)(") !=
         std::string::npos);
  assert(usb_header.find(
             "std::uint32_t endpoint_epoch,\n"
             "      std::uint32_t runtime_reply_sequence);") !=
         std::string::npos);

  const auto send_path = section(
      usb,
      "UsbHidTransport::try_send_speaker_assets_response()",
      "bool UsbHidTransport::status_response_pending()");
  const auto endpoint_accept =
      send_path.find("const bool accepted = tud_hid_report(");
  const auto synchronous_retirement =
      send_path.find("speaker_assets_response_accepted_callback_(");
  const auto response_reset =
      send_path.find("reset_speaker_assets_response();",
                     synchronous_retirement);
  const auto lifetime_unlock =
      send_path.find("unlock_lifetime();", response_reset);
  assert(endpoint_accept != std::string::npos);
  assert(synchronous_retirement != std::string::npos);
  assert(response_reset != std::string::npos);
  assert(lifetime_unlock != std::string::npos);
  assert(endpoint_accept < synchronous_retirement);
  assert(synchronous_retirement < response_reset);
  assert(response_reset < lifetime_unlock);
  assert(send_path.find(
             "if (!retired_synchronously) {\n"
             "    // Preserve the owner-task polling path") !=
         std::string::npos);
  assert(send_path.find(
             "speaker_assets_sent_sequence_ = sent_sequence;") !=
         std::string::npos);
  assert(send_path.find(
             "speaker_assets_sent_epoch_ = sent_epoch;") !=
         std::string::npos);

  const auto receive_path = section(
      usb,
      "void UsbHidTransport::receive_speaker_assets_report(",
      "void UsbHidTransport::queue_completed_config(");
  const auto request_lifetime_lock =
      receive_path.find("lock_current_epoch(endpoint_epoch)");
  const auto request_callback =
      receive_path.find("speaker_assets_frame_callback_(");
  const auto request_lifetime_unlock =
      receive_path.find("unlock_lifetime();", request_callback);
  assert(request_lifetime_lock != std::string::npos);
  assert(request_callback != std::string::npos);
  assert(request_lifetime_unlock != std::string::npos);
  assert(request_lifetime_lock < request_callback);
  assert(request_callback < request_lifetime_unlock);

  assert(supervisor.find(
             "set_speaker_assets_response_accepted_callback(\n"
             "      &SpeakerAssetsSupervisor::"
             "retire_usb_reply_on_endpoint_accept,") !=
         std::string::npos);
  const auto retirement = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::retire_accepted_usb_reply(",
      "void SpeakerAssetsSupervisor::retire_physically_sent_reply()");
  assert(retirement.find(
             "queued_reply_sequence_ != runtime_reply_sequence") !=
         std::string::npos);
  assert(retirement.find(
             "queued_reply_lease_.route.generation != endpoint_epoch") !=
         std::string::npos);
  assert(retirement.find(
             "core_.remove_reply_if_sequence(runtime_reply_sequence)") !=
         std::string::npos);
  assert(retirement.find("release_mailbox_lease(lease)") !=
         std::string::npos);
  assert(retirement.find("queued_reply_sequence_ = 0U;") !=
         std::string::npos);
  assert(retirement.find("queued_reply_lease_ = {};") !=
         std::string::npos);
  assert(retirement.find("usb_->") == std::string::npos);

  const auto fallback = section(
      supervisor,
      "void SpeakerAssetsSupervisor::retire_physically_sent_reply()",
      "void SpeakerAssetsSupervisor::offer_usb_reply()");
  assert(fallback.find(
             "take_speaker_assets_response_sent(\n"
             "          &sent_sequence, &sent_epoch)") !=
         std::string::npos);
  assert(fallback.find(
             "retire_accepted_usb_reply(sent_epoch, sent_sequence)") !=
         std::string::npos);
}

void speaker_asset_boot_read_uses_the_protocol_store_runner() {
  const auto supervisor_header =
      read_source("main/platform/speaker_assets_supervisor.h");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");
  const auto wifi_carrier_header =
      read_source("main/platform/speaker_assets_wifi.h");
  const auto wifi_carrier =
      read_source("main/platform/speaker_assets_wifi.cpp");
  const auto app_main = read_source("main/app_main.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto speaker_assets_cmake =
      read_source("features/speaker_assets/CMakeLists.txt");
  const auto store =
      read_source("features/speaker_assets/sound_asset_store.cpp");

  assert(supervisor_header.find("SoundAssetStore playback_store_") ==
         std::string::npos);
  assert(supervisor.find("playback_store_") == std::string::npos);
  assert(supervisor.find(
             "store_runner_.start_prepare_boot_read(&boot_job_)") !=
         std::string::npos);
  assert(supervisor.find(
             "store_runner_.poll_prepare_boot_read(") !=
         std::string::npos);
  assert(supervisor.find(
             "store_runner_.start_release_read(") !=
         std::string::npos);
  assert(supervisor.find(
             "store_runner_.poll_release_read(") !=
         std::string::npos);
  assert(supervisor.find(
             "if (boot_read_state_ == BootReadState::Idle) {\n"
             "    run_core_steps(now_ms, resource_steps_allowed);") !=
         std::string::npos);
  assert(supervisor.find(
             "completion.acquire_result ==\n"
             "            speaker_assets::SoundStoreResult::FactoryBlank") !=
         std::string::npos);
  assert(supervisor.find(
             "SpeakerAssetsBootPlaybackResult::FactoryDefault") !=
         std::string::npos);
  assert(store.find(
             "bank_a_result == SoundStoreResult::FactoryBlank &&\n"
             "      bank_b_result == SoundStoreResult::FactoryBlank") !=
         std::string::npos);
  assert(speaker_assets_cmake.find("EMBED_FILES") !=
         std::string::npos);
  assert(speaker_assets_cmake.find(
             "\"assets/waytoagi.eiad\"") !=
         std::string::npos);
  assert(speaker_output.find(
             "asset_decoder_.open_embedded(encoded, encoded_bytes)") !=
         std::string::npos);

  const auto local_begin = section(
      supervisor,
      "esp_err_t SpeakerAssetsSupervisor::begin_local(",
      "esp_err_t SpeakerAssetsSupervisor::start_wifi(");
  assert(local_begin.find("set_speaker_assets_frame_callback(") !=
         std::string::npos);
  assert(local_begin.find(
             "set_speaker_assets_response_accepted_callback(") !=
         std::string::npos);
  assert(local_begin.find("wifi_.begin(") == std::string::npos);
  assert(local_begin.find("if (!local_setup_ready_)") !=
         std::string::npos);
  const auto store_task_failure = section(
      local_begin,
      "if (task_result != pdPASS)",
      "log_internal_heap(\"store_after\")");
  assert(store_task_failure.find("usb_ = nullptr") ==
         std::string::npos);
  assert(store_task_failure.find("platform_task_ = nullptr") ==
         std::string::npos);
  assert(supervisor_header.find(
             "bool local_setup_ready_ = false;") !=
         std::string::npos);

  const auto wifi_begin = section(
      supervisor,
      "esp_err_t SpeakerAssetsSupervisor::start_wifi(",
      "bool SpeakerAssetsSupervisor::ready() const");
  assert(wifi_begin.find("wifi_.begin(") != std::string::npos);
  const auto publish_wifi = wifi_begin.find(
      "wifi_started_.store(true, std::memory_order_release);");
  const auto set_assets_ready =
      wifi_begin.find("wifi_.set_assets_ready(storage_.is_open());");
  const auto activate_wifi = wifi_begin.find("wifi_.activate();");
  assert(publish_wifi != std::string::npos);
  assert(set_assets_ready != std::string::npos);
  assert(activate_wifi != std::string::npos);
  assert(publish_wifi < set_assets_ready);
  assert(set_assets_ready < activate_wifi);
  assert(supervisor_header.find(
             "std::atomic<bool> wifi_started_{false};") !=
         std::string::npos);

  const auto carrier_begin = section(
      wifi_carrier,
      "esp_err_t SpeakerAssetsWifiCarrier::begin(",
      "void SpeakerAssetsWifiCarrier::activate()");
  const auto create_task =
      carrier_begin.find("xTaskCreatePinnedToCore(");
  const auto publish_initialized =
      carrier_begin.find("initialized_ = true;");
  const auto publish_callback = carrier_begin.find(
      "audio_->set_heartbeat_extension_callback(");
  assert(create_task != std::string::npos);
  assert(publish_initialized != std::string::npos);
  assert(publish_callback != std::string::npos);
  assert(create_task < publish_initialized);
  assert(publish_initialized < publish_callback);
  assert(carrier_begin.find(
             "set_heartbeat_extension_callback(nullptr, nullptr)") ==
         std::string::npos);
  assert(wifi_carrier_header.find(
             "std::atomic<bool> activated_{false};") !=
         std::string::npos);
  const auto carrier_run = section(
      wifi_carrier,
      "void SpeakerAssetsWifiCarrier::run()",
      "void SpeakerAssetsWifiCarrier::service_connection(");
  const auto activation_gate = carrier_run.find(
      "while (!activated_.load(std::memory_order_acquire))");
  const auto listener_socket =
      carrier_run.find("listener = socket(");
  assert(activation_gate != std::string::npos);
  assert(listener_socket != std::string::npos);
  assert(activation_gate < listener_socket);
  assert(carrier_run.find(
             "std::uint32_t listener_generation = 0U;") !=
         std::string::npos);
  assert(carrier_run.find(
             "speaker_assets_wifi_listener_matches_generation(") !=
         std::string::npos);
  assert(carrier_run.find(
             "listener_generation = snapshot.generation;") !=
         std::string::npos);
  assert(carrier_run.find(
             "SpeakerAssetsWifiAcceptIo::RebuildListener") !=
         std::string::npos);
  assert(carrier_run.find(
             "retire_listener();") !=
         std::string::npos);
  const auto listener_ready = section(
      wifi_carrier,
      "void SpeakerAssetsWifiCarrier::set_listener_ready(bool ready)",
      "void SpeakerAssetsWifiCarrier::set_active_route(");
  assert(listener_ready.find(
             "changed = state_.listener_ready != ready;") !=
         std::string::npos);
  assert(listener_ready.find(
             "audio_->request_heartbeat_refresh();") !=
         std::string::npos);

  const auto product_boot = section(
      app_main,
      "#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)\n"
      "  // Keep the microphone's frozen resource pool first.",
      "#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)");
  assert(product_boot.find(
             "app.speaker_skip_boot_after_deep_sleep =") !=
         std::string::npos);
  assert(product_boot.find("speaker_assets.begin_local(") ==
         std::string::npos);
  assert(product_boot.find("speaker.begin(") == std::string::npos);
  assert(product_boot.find(
             "wake_cause == ESP_SLEEP_WAKEUP_EXT1") !=
         std::string::npos);

  const auto lifecycle = section(
      app_main,
      "void service_speaker(AppContext* app)",
      "bool speaker_asset_resource_steps_allowed(");
  const auto begin_local =
      lifecycle.find("app->speaker_assets.begin_local(");
  const auto resolve_boot =
      lifecycle.find("take_boot_playback(");
  const auto begin_output =
      lifecycle.find("app->speaker.begin(");
  const auto request_asset =
      lifecycle.find("app->speaker.request_asset(");
  const auto request_factory =
      lifecycle.find("app->speaker.request_embedded_asset(");
  const auto request_shutdown =
      lifecycle.find("app->speaker.request_shutdown()");
  const auto release_lease =
      lifecycle.find("queue_playback_lease_release(");
  const auto wait_idle =
      lifecycle.find("app->speaker_assets.boot_idle()");
  const auto start_wifi =
      lifecycle.find("app->speaker_assets.start_wifi(&app->audio)");
  const auto refresh_heartbeat =
      lifecycle.find("app->audio.request_heartbeat_refresh()");
  assert(begin_local != std::string::npos);
  assert(resolve_boot != std::string::npos);
  assert(begin_output != std::string::npos);
  assert(request_asset != std::string::npos);
  assert(request_factory != std::string::npos);
  assert(request_shutdown != std::string::npos);
  assert(release_lease != std::string::npos);
  assert(wait_idle != std::string::npos);
  assert(start_wifi != std::string::npos);
  assert(refresh_heartbeat != std::string::npos);
  assert(begin_local < start_wifi);
  assert(start_wifi < resolve_boot);
  assert(resolve_boot < begin_output);
  assert(begin_output < request_asset);
  assert(begin_output < request_factory);
  assert(request_asset < request_shutdown);
  assert(request_factory < request_shutdown);
  assert(request_shutdown < release_lease);
  assert(release_lease < wait_idle);
  assert(start_wifi < refresh_heartbeat);
  assert(lifecycle.find(
             "speaker_boot_pipeline_allowed(startup_inputs)") !=
         std::string::npos);
  assert(lifecycle.find(
             "app->audio_ready,\n"
             "      app->speaker_assets.wifi_ready()") !=
         std::string::npos);
  assert(lifecycle.find(
             "static_cast<std::int32_t>(\n"
             "            now_ms - app->speaker_local_retry_after_ms) >= 0") !=
         std::string::npos);
  assert(lifecycle.find(
             "static_cast<std::int32_t>(\n"
             "            now_ms - app->speaker_wifi_retry_after_ms) >= 0") !=
         std::string::npos);
  assert(lifecycle.find(
             "SpeakerStartupPhase::WaitPlayback &&\n"
             "      handoff_complete && !app->speaker.busy()") !=
         std::string::npos);
  assert(lifecycle.find(
             "SpeakerStartupPhase::WaitLeaseIdle &&\n"
             "      app->speaker_assets.boot_idle()") !=
         std::string::npos);
  assert(lifecycle.find(
             "app->speaker.shutdown_complete() &&\n"
             "      app->speaker_assets.boot_idle()") ==
         std::string::npos);

  const auto worker_loop = section(
      speaker_output,
      "void SpeakerOutput::run()",
      "SpeakerOutput::WorkerResult SpeakerOutput::play_sound(");
  const auto decoder_close =
      worker_loop.find("asset_decoder_.close();");
  const auto completion =
      worker_loop.find("publish_completed(generation, result);");
  assert(decoder_close != std::string::npos);
  assert(completion != std::string::npos);
  assert(decoder_close < completion);
  assert(worker_loop.find("vTaskSuspend(nullptr);") !=
         std::string::npos);

  const auto shutdown_cleanup = section(
      speaker_output,
      "if (shutdown_requested_.load(std::memory_order_acquire) &&",
      "bool SpeakerOutput::request_shutdown()");
  const auto suspended =
      shutdown_cleanup.find("eTaskGetState(worker_task_) != eSuspended");
  const auto disable =
      shutdown_cleanup.find("i2s_channel_disable(tx_channel_)");
  const auto delete_channel =
      shutdown_cleanup.find("i2s_del_channel(tx_channel_)");
  const auto delete_task =
      shutdown_cleanup.find("vTaskDelete(worker_task_)");
  assert(suspended != std::string::npos);
  assert(disable != std::string::npos);
  assert(delete_channel != std::string::npos);
  assert(delete_task != std::string::npos);
  assert(suspended < disable);
  assert(disable < delete_channel);
  assert(delete_channel < delete_task);
  const auto failed_begin_cleanup = section(
      speaker_output,
      "void SpeakerOutput::poll(bool playback_allowed)",
      "bool SpeakerOutput::request_shutdown()");
  assert(failed_begin_cleanup.find(
             "if (worker_task_ == nullptr && tx_channel_ != nullptr)") !=
         std::string::npos);
  assert(failed_begin_cleanup.find(
             "const auto delete_result = i2s_del_channel(tx_channel_)") !=
         std::string::npos);

  const auto rejected_playback = lifecycle.find(
      "ESP_LOGW(kTag, \"boot asset playback request rejected\");");
  assert(rejected_playback != std::string::npos);
  const auto rejected_shutdown = lifecycle.find(
      "SpeakerStartupPhase::ShutdownOutput", rejected_playback);
  assert(rejected_shutdown != std::string::npos);
  assert(rejected_shutdown - rejected_playback < 300);
}

void speaker_asset_transfer_drives_the_production_poll_interval() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto full_inputs = section(
      app_main,
      "ai_keyboard::PowerPolicyInputs power_policy_inputs(AppContext* app,\n"
      "                                                   std::uint32_t now_ms) {",
      "bool controlled_light_sleep_allowed(");
  assert(full_inputs.find(
             "inputs.audio_streaming = app->audio.streaming();") !=
         std::string::npos);
  assert(full_inputs.find(
             "app->speaker_assets.transfer_active()") !=
         std::string::npos);
  assert(full_inputs.find(
             "app->codex_playback.sleep_blocked()") !=
         std::string::npos);

  const auto poll_interval = section(
      app_main,
      "std::uint32_t poll_interval_ms(AppContext* app, std::uint32_t now_ms) {",
      "bool controlled_light_sleep_allowed(");
  assert(poll_interval.find(
             "power_policy_inputs(app, now_ms)") !=
         std::string::npos);
  assert(poll_interval.find(
             "base_power_policy_inputs(app, now_ms)") ==
         std::string::npos);

  assert(app_main.find(
             "poll_interval_ms(&app, now)") !=
         std::string::npos);
  assert(app_main.find(
             "poll_interval_ms(&app, millis())") !=
         std::string::npos);
}

void speaker_asset_wifi_admission_owns_ingress_power_and_listener_epoch() {
  const auto audio =
      read_source("main/platform/keyboard_audio.cpp");
  const auto acquire = section(
      audio,
      "bool KeyboardAudioLink::acquire_wifi_service_lease(",
      "bool KeyboardAudioLink::release_wifi_service_lease(");
  const auto lease_publish =
      acquire.find("wifi_service_lease_ = {");
  const auto ingress_active =
      acquire.find("esp_wifi_set_ps(WIFI_PS_NONE)");
  const auto successful_return =
      acquire.rfind("return true;");
  assert(acquire.find("WifiOpGuard wifi_guard(wifi_op_mutex_);") !=
         std::string::npos);
  assert(lease_publish != std::string::npos);
  assert(ingress_active != std::string::npos);
  assert(successful_return != std::string::npos);
  assert(lease_publish < ingress_active);
  assert(ingress_active < successful_return);
  assert(acquire.find("wifi service ingress activation failed") !=
         std::string::npos);

  const auto carrier =
      read_source("main/platform/speaker_assets_wifi.cpp");
  const auto run = section(
      carrier,
      "void SpeakerAssetsWifiCarrier::run()",
      "void SpeakerAssetsWifiCarrier::service_connection(");
  const auto nonce_rotation =
      run.find("rotate_endpoint_nonce();");
  const auto ready_publish =
      run.find("set_listener_ready(true);", nonce_rotation);
  assert(nonce_rotation != std::string::npos);
  assert(ready_publish != std::string::npos);
  assert(nonce_rotation < ready_publish);

  const auto service =
      run.find("service_connection(client, accepted_snapshot);");
  const auto client_close =
      run.find("close_socket(&client);", service);
  const auto listener_retire =
      run.find("retire_listener();", client_close);
  assert(service != std::string::npos);
  assert(client_close != std::string::npos);
  assert(listener_retire != std::string::npos);
  assert(service < client_close);
  assert(client_close < listener_retire);
}

void peripheral_power_lifecycle_is_system_owned_and_ordered() {
  const auto controller =
      read_source("main/platform/peripheral_power.cpp");
  const auto controller_header =
      read_source("main/platform/peripheral_power.h");
  const auto leds =
      read_source("main/platform/led_strip_status.cpp");
  const auto leds_header =
      read_source("main/platform/led_strip_status.h");
  const auto audio_arbiter =
      read_source("components/keyboard/src/audio_io_arbiter.cpp");
  const auto keyboard_audio =
      read_source("main/platform/keyboard_audio.cpp");
  const auto app_main = read_source("main/app_main.cpp");

  assert(controller_header.find("class PeripheralPowerController") !=
         std::string::npos);
  assert(controller.find("PeripheralPowerOwner::DeviceAwake") !=
         std::string::npos);
  assert(controller.find(
             "kPeripheralPowerSettleUs = 20U * 1000U") !=
         std::string::npos);

  const auto awake = section(
      controller,
      "esp_err_t PeripheralPowerController::begin_awake()",
      "bool PeripheralPowerController::ready() const");
  const auto configure = awake.find("gpio_config(&power_config)");
  const auto preserve_light_sleep = awake.find("gpio_sleep_sel_dis(");
  const auto safe_before_power = awake.find(
      "configure_command_pins_safe_for_rail_transition()");
  const auto drive_high = awake.find("apply_power_state()");
  const auto settle = awake.find("esp_rom_delay_us(kPeripheralPowerSettleUs)");
  assert(configure != std::string::npos);
  assert(preserve_light_sleep != std::string::npos);
  assert(safe_before_power != std::string::npos);
  assert(drive_high != std::string::npos);
  assert(settle != std::string::npos);
  assert(configure < preserve_light_sleep);
  assert(preserve_light_sleep < safe_before_power);
  assert(safe_before_power < drive_high);
  assert(drive_high < settle);

  assert(leds.find("kPeripheralPowerEnablePin") == std::string::npos);
  assert(leds.find("set_peripheral_power_enabled") == std::string::npos);
  assert(leds.find("PeripheralPowerLeaseSet") == std::string::npos);
  assert(leds_header.find("set_audio_power_hold") == std::string::npos);
  assert(leds_header.find("set_speaker_power_hold") == std::string::npos);

  std::size_t gpio8_writer_count = 0;
  const std::array<const char*, 2> source_roots{{"main", "components"}};
  for (const auto* relative_root : source_roots) {
    const auto root = std::filesystem::path(EASY_INPUT_REPO_ROOT) /
                      relative_root;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto extension = entry.path().extension().string();
      if (extension != ".cpp" && extension != ".h" && extension != ".c") {
        continue;
      }
      std::ifstream input(entry.path());
      std::ostringstream contents;
      contents << input.rdbuf();
      gpio8_writer_count += count_occurrences(
          without_ascii_whitespace(contents.str()),
          "gpio_set_level(static_cast<gpio_num_t>(ai_keyboard::kPeripheralPowerEnablePin),");
    }
  }
  assert(gpio8_writer_count == 1);

  const auto startup = section(
      app_main,
      "extern \"C\" void app_main(void)",
      "while (true) {");
  const auto power_begin =
      startup.find("app.peripheral_power.begin_awake()");
  const auto audio_begin = startup.find("app.audio.begin()");
  const auto led_begin = startup.find("app.leds.begin()");
  assert(power_begin != std::string::npos);
  assert(audio_begin != std::string::npos);
  assert(led_begin != std::string::npos);
  assert(power_begin < audio_begin);
  assert(audio_begin < led_begin);

  const auto light_sleep = section(
      app_main,
      "bool try_controlled_light_sleep(",
      "bool deep_sleep_allowed(");
  assert(light_sleep.find("prepare_for_deep_sleep") == std::string::npos);
  assert(light_sleep.find("peripheral_power") == std::string::npos);

  const auto deep_sleep = section(
      app_main,
      "void maybe_enter_deep_sleep(",
      "void show_encoder_volume_feedback(");
  const auto wake_setup =
      deep_sleep.find("esp_sleep_enable_ext1_wakeup_io(");
  const auto audio_gate =
      deep_sleep.find("try_begin_audio_deep_sleep_quiesce(app)");
  const auto first_final_gate =
      deep_sleep.find("const auto final_decision");
  const auto led_quiesce =
      deep_sleep.find("app->leds.prepare_for_deep_sleep()");
  const auto commit_gate =
      deep_sleep.find("const auto commit_decision");
  const auto power_off =
      deep_sleep.find("app->peripheral_power.prepare_for_deep_sleep()");
  const auto sleep_start = deep_sleep.find("esp_deep_sleep_start()");
  assert(wake_setup != std::string::npos);
  assert(audio_gate != std::string::npos);
  assert(first_final_gate != std::string::npos);
  assert(led_quiesce != std::string::npos);
  assert(commit_gate != std::string::npos);
  assert(power_off != std::string::npos);
  assert(sleep_start != std::string::npos);
  assert(wake_setup < audio_gate);
  assert(audio_gate < first_final_gate);
  assert(first_final_gate < led_quiesce);
  assert(led_quiesce < commit_gate);
  assert(commit_gate < power_off);
  assert(power_off < sleep_start);
  assert(count_occurrences(deep_sleep, "app->inputs.activity_pending()") >=
         2);
  assert(deep_sleep.find("cancel_audio_deep_sleep_quiesce(app)") !=
         std::string::npos);
  assert(deep_sleep.find("esp_restart()") != std::string::npos);

  assert(audio_arbiter.find("try_begin_deep_sleep_quiesce()") !=
         std::string::npos);
  assert(audio_arbiter.find("try_enter_runtime_transition()") !=
         std::string::npos);
  assert(keyboard_audio.find(
             "!audio_io_arbiter_->request_microphone(generation)") !=
         std::string::npos);
  assert(keyboard_audio.find("deep_sleep_quiescing") !=
         std::string::npos);
  assert(app_main.find(
             "app->audio_io_arbiter.microphone_generation() != 0") !=
         std::string::npos);

  const auto power_down = section(
      controller,
      "esp_err_t PeripheralPowerController::prepare_for_deep_sleep()",
      "esp_err_t PeripheralPowerController::set_owner_hold(");
  const auto safe_outputs =
      power_down.find("configure_command_pins_safe_for_rail_transition()");
  const auto clear_awake = power_down.find("power_leases_.clear()");
  const auto rail_low = power_down.find("apply_power_state()");
  assert(safe_outputs != std::string::npos);
  assert(clear_awake != std::string::npos);
  assert(rail_low != std::string::npos);
  assert(safe_outputs < clear_awake);
  assert(clear_awake < rail_low);
  assert(power_down.find("ready_ = false") < safe_outputs);
  assert(power_down.find("DeviceAwake", rail_low) == std::string::npos);

  assert(app_main.find("0.6.2-easy-codex-board-volume") !=
         std::string::npos);
}

}  // namespace

int main() {
  battery_update_releases_hidd_lock_before_entering_nimble();
  gatt_status_read_callback_is_cache_only_and_refreshes_once();
  connection_power_reset_has_one_balanced_critical_section();
  connection_update_failures_share_bounded_backoff();
  ble_status_refresh_publishes_current_config_fingerprint();
  speaker_probe_status_uses_one_generation_for_usb_and_ble();
  release_build_rejects_internal_ram_profile_drift();
  audio_start_reuses_boot_resource_pool();
  speaker_probe_is_default_off_and_outside_input_hot_path();
  speaker_opus_probe_is_conditional_fixed_and_reusable();
  speaker_ima_probe_is_conditional_allocation_free_and_isolated();
  speaker_asset_storage_reserves_exact_banks_and_has_production_gate();
  speaker_asset_usb_lifetime_precedes_first_frame();
  speaker_asset_wifi_carrier_is_the_only_wireless_bulk_path();
  speaker_asset_transfer_drives_the_production_poll_interval();
  speaker_asset_wifi_admission_owns_ingress_power_and_listener_epoch();
  speaker_asset_endpoint_accept_retires_before_lifetime_unlock();
  speaker_asset_boot_read_uses_the_protocol_store_runner();
  peripheral_power_lifecycle_is_system_owned_and_ordered();
  return 0;
}
