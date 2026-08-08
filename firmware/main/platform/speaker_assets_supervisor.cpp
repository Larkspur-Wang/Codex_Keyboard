#include "platform/speaker_assets_supervisor.h"

#include <array>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "platform/keyboard_audio.h"
#include "platform/usb_hid.h"
#include "speaker_assets/speaker_assets_protocol.h"

namespace easy_input {
namespace {

constexpr const char* kTag = "speaker_assets";
constexpr std::uint32_t kStoreTaskStackBytes = 12U * 1024U;
constexpr UBaseType_t kStoreTaskPriority = tskIDLE_PRIORITY + 1U;
constexpr std::size_t kMaximumCoreStepsPerPoll = 8U;
constexpr std::uint32_t kInternalHeapCaps =
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

bool lease_valid(
    const speaker_assets::SpeakerAssetsLogicalRequestLease& lease) {
  return lease.admission_id != 0U;
}

bool same_sound_read_lease(
    const speaker_assets::SoundReadLease& first,
    const speaker_assets::SoundReadLease& second) {
  return first.valid && second.valid &&
         first.lease_id == second.lease_id &&
         first.bank == second.bank &&
         first.generation == second.generation &&
         speaker_assets::sound_digest_equal(
             first.bundle_sha256, second.bundle_sha256);
}

std::uint32_t monotonic_millis() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

void log_internal_heap(const char* phase) {
  ESP_LOGI(
      kTag,
      "heap phase=%s internal_free=%lu internal_largest=%lu",
      phase,
      static_cast<unsigned long>(
          heap_caps_get_free_size(kInternalHeapCaps)),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(kInternalHeapCaps)));
}

}  // namespace

esp_err_t SpeakerAssetsPlatformSynchronization::begin(
    TaskHandle_t supervisor_task) {
  if (exchange_mutex_ != nullptr || worker_signal_ != nullptr ||
      supervisor_task == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  exchange_mutex_ = xSemaphoreCreateMutex();
  worker_signal_ = xSemaphoreCreateBinary();
  if (exchange_mutex_ == nullptr || worker_signal_ == nullptr) {
    if (exchange_mutex_ != nullptr) {
      vSemaphoreDelete(exchange_mutex_);
      exchange_mutex_ = nullptr;
    }
    if (worker_signal_ != nullptr) {
      vSemaphoreDelete(worker_signal_);
      worker_signal_ = nullptr;
    }
    return ESP_ERR_NO_MEM;
  }
  supervisor_task_ = supervisor_task;
  return ESP_OK;
}

void SpeakerAssetsPlatformSynchronization::lock() {
  configASSERT(exchange_mutex_ != nullptr);
  configASSERT(
      xSemaphoreTake(exchange_mutex_, portMAX_DELAY) == pdTRUE);
}

void SpeakerAssetsPlatformSynchronization::unlock() {
  configASSERT(exchange_mutex_ != nullptr);
  configASSERT(xSemaphoreGive(exchange_mutex_) == pdTRUE);
}

void SpeakerAssetsPlatformSynchronization::notify_worker() {
  if (worker_signal_ != nullptr) {
    xSemaphoreGive(worker_signal_);
  }
}

void SpeakerAssetsPlatformSynchronization::notify_supervisor() {
  if (supervisor_task_ != nullptr) {
    xTaskNotifyGive(supervisor_task_);
  }
}

void SpeakerAssetsPlatformSynchronization::wait_worker() {
  configASSERT(worker_signal_ != nullptr);
  configASSERT(
      xSemaphoreTake(worker_signal_, portMAX_DELAY) == pdTRUE);
}

SpeakerAssetsSupervisor::SpeakerAssetsSupervisor()
    : store_runner_(storage_, synchronization_) {}

esp_err_t SpeakerAssetsSupervisor::begin_local(
    UsbHidTransport* usb,
    TaskHandle_t platform_task) {
  if (initialized_ || usb == nullptr || platform_task == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!local_setup_ready_) {
    usb_ = usb;
    platform_task_ = platform_task;

    log_internal_heap("store_before");
    const auto synchronization_result =
        synchronization_.begin(platform_task_);
    if (synchronization_result != ESP_OK) {
      log_internal_heap("store_failed");
      usb_ = nullptr;
      platform_task_ = nullptr;
      return synchronization_result;
    }

    const auto storage_result = storage_.open();
    if (storage_result !=
        speaker_assets::SoundStorageIoResult::Ok) {
      // Keep the protocol endpoint alive so the App receives a stable
      // StorageUnavailable response instead of an opaque transport timeout.
      ESP_LOGW(kTag,
               "sound bank partitions unavailable result=%u",
               static_cast<unsigned>(storage_result));
    }
    local_setup_ready_ = true;
  } else if (usb_ != usb || platform_task_ != platform_task) {
    // A retained synchronization/storage setup belongs to the exact platform
    // owner supplied on the first attempt.
    return ESP_ERR_INVALID_STATE;
  }

  const auto task_result = xTaskCreatePinnedToCore(
      &SpeakerAssetsSupervisor::store_task_entry,
      "sound_store",
      kStoreTaskStackBytes,
      this,
      kStoreTaskPriority,
      &store_task_,
      1);
  if (task_result != pdPASS) {
    log_internal_heap("store_failed");
    store_task_ = nullptr;
    // Keep the already-created synchronization and opened storage. The caller
    // can retry only this allocation after the idle task has reclaimed memory.
    return ESP_ERR_NO_MEM;
  }
  log_internal_heap("store_after");

  initialized_ = true;
  usb_->set_speaker_assets_frame_callback(
      &SpeakerAssetsSupervisor::receive_usb_frame, this);
  usb_->set_speaker_assets_response_accepted_callback(
      &SpeakerAssetsSupervisor::retire_usb_reply_on_endpoint_accept,
      this);
  ESP_LOGI(kTag,
           "local USB sound sync ready storage=%s",
           storage_.is_open() ? "ready" : "unavailable");
  return ESP_OK;
}

esp_err_t SpeakerAssetsSupervisor::start_wifi(
    KeyboardAudioLink* audio) {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (audio == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (wifi_started_.load(std::memory_order_acquire)) {
    return ESP_OK;
  }

  log_internal_heap("wifi_before");
  const auto wifi_result = wifi_.begin(
      audio,
      platform_task_,
      &SpeakerAssetsSupervisor::receive_wifi_frame,
      &SpeakerAssetsSupervisor::observe_wifi_route,
      this);
  if (wifi_result == ESP_OK) {
    wifi_started_.store(true, std::memory_order_release);
    wifi_.set_assets_ready(storage_.is_open());
    wifi_.activate();
    log_internal_heap("wifi_after");
    ESP_LOGI(kTag, "Wi-Fi sound sync ready");
    return ESP_OK;
  }

  // SpeakerAssetsWifiCarrier::begin() rolls back every partial allocation on
  // error. Do not publish wifi_started_, so a later call can retry without
  // affecting the already-live Store and USB path.
  log_internal_heap("wifi_failed");
  ESP_LOGW(
      kTag,
      "speaker asset Wi-Fi fallback unavailable: %s",
      esp_err_to_name(wifi_result));
  return wifi_result;
}

bool SpeakerAssetsSupervisor::ready() const {
  return initialized_;
}

bool SpeakerAssetsSupervisor::wifi_ready() const {
  return initialized_ &&
         wifi_started_.load(std::memory_order_acquire);
}

bool SpeakerAssetsSupervisor::boot_idle() const {
  return initialized_ &&
         boot_read_state_ == BootReadState::Idle &&
         boot_job_.job_id == 0U &&
         !prepared_boot_.prepared &&
         !prepared_boot_.lease.valid &&
         !prepared_boot_.asset.valid &&
         !outstanding_boot_lease_.valid &&
         !boot_release_requested_;
}

bool SpeakerAssetsSupervisor::transfer_active() const {
  if (!initialized_) {
    return false;
  }
  bool mailbox_active = false;
  portENTER_CRITICAL(&mailbox_mux_);
  speaker_assets::SpeakerAssetsLogicalRequestLease lease{};
  mailbox_active = mailbox_.logical_request_lease(&lease);
  portEXIT_CRITICAL(&mailbox_mux_);
  const bool wifi_active =
      wifi_started_.load(std::memory_order_acquire) &&
      (wifi_.route_active() || wifi_.response_pending());
  return mailbox_active || core_.action_pending() ||
         core_.reply_size() != 0U || store_runner_.job_active() ||
         queued_reply_sequence_ != 0U ||
         wifi_active ||
         boot_read_state_ != BootReadState::Idle;
}

SpeakerAssetsBootPlaybackResult
SpeakerAssetsSupervisor::take_boot_playback(
    speaker_assets::SoundReadLease* lease,
    speaker_assets::SoundResolvedAsset* asset) {
  if (lease == nullptr || asset == nullptr) {
    return SpeakerAssetsBootPlaybackResult::Unavailable;
  }
  *lease = {};
  *asset = {};
  if (!initialized_ || !storage_.is_open()) {
    return SpeakerAssetsBootPlaybackResult::Unavailable;
  }

  if (boot_read_state_ == BootReadState::Idle) {
    const auto started =
        store_runner_.start_prepare_boot_read(&boot_job_);
    if (started ==
        speaker_assets::SpeakerAssetsInternalJobStartResult::Accepted) {
      boot_read_state_ = BootReadState::Preparing;
    } else if (
        started ==
        speaker_assets::SpeakerAssetsInternalJobStartResult::InvalidArgument) {
      ESP_LOGE(kTag, "failed to start Boot sound read job");
      return SpeakerAssetsBootPlaybackResult::Unavailable;
    }
    return SpeakerAssetsBootPlaybackResult::Pending;
  }

  if (boot_read_state_ == BootReadState::Ready) {
    *lease = prepared_boot_.lease;
    *asset = prepared_boot_.asset;
    outstanding_boot_lease_ = prepared_boot_.lease;
    prepared_boot_ = {};
    boot_read_state_ = BootReadState::LeaseOutstanding;
    return SpeakerAssetsBootPlaybackResult::Ready;
  }

  if (boot_read_state_ == BootReadState::FactoryDefault) {
    prepared_boot_ = {};
    boot_job_ = {};
    boot_read_state_ = BootReadState::Idle;
    return SpeakerAssetsBootPlaybackResult::FactoryDefault;
  }

  if (boot_read_state_ == BootReadState::Unavailable) {
    prepared_boot_ = {};
    boot_job_ = {};
    boot_read_state_ = BootReadState::Idle;
    return SpeakerAssetsBootPlaybackResult::Unavailable;
  }

  return SpeakerAssetsBootPlaybackResult::Pending;
}

bool SpeakerAssetsSupervisor::queue_playback_lease_release(
    const speaker_assets::SoundReadLease& lease) {
  if (!initialized_ ||
      boot_read_state_ != BootReadState::LeaseOutstanding ||
      !same_sound_read_lease(lease, outstanding_boot_lease_)) {
    return false;
  }
  // The supervisor keeps its own immutable copy, so SpeakerOutput's caller may
  // clear its copy as soon as this fixed one-slot request is accepted.
  boot_release_requested_ = true;
  return true;
}

speaker_assets::SoundBankStorage&
SpeakerAssetsSupervisor::playback_storage() {
  return storage_;
}

bool SpeakerAssetsSupervisor::receive_usb_frame(
    void* context,
    std::uint32_t endpoint_epoch,
    const std::uint8_t* frame,
    std::size_t length) {
  auto* supervisor =
      static_cast<SpeakerAssetsSupervisor*>(context);
  if (supervisor == nullptr) {
    return false;
  }
  return supervisor->enqueue_usb_frame(
      endpoint_epoch, frame, length, monotonic_millis());
}

bool SpeakerAssetsSupervisor::retire_usb_reply_on_endpoint_accept(
    void* context,
    std::uint32_t endpoint_epoch,
    std::uint32_t runtime_reply_sequence) {
  auto* supervisor =
      static_cast<SpeakerAssetsSupervisor*>(context);
  return supervisor != nullptr &&
         supervisor->retire_accepted_usb_reply(
             endpoint_epoch, runtime_reply_sequence);
}

bool SpeakerAssetsSupervisor::receive_wifi_frame(
    void* context,
    const speaker_assets::SpeakerAssetsRouteToken& route,
    const std::uint8_t* frame,
    std::size_t length,
    std::uint32_t received_ms) {
  auto* supervisor =
      static_cast<SpeakerAssetsSupervisor*>(context);
  return supervisor != nullptr &&
         supervisor->wifi_started_.load(std::memory_order_acquire) &&
         supervisor->enqueue_wifi_frame(
             route,
             frame,
             length,
             received_ms);
}

bool SpeakerAssetsSupervisor::observe_wifi_route(
    void* context,
    const speaker_assets::SpeakerAssetsRouteToken& route,
    bool opened) {
  auto* supervisor =
      static_cast<SpeakerAssetsSupervisor*>(context);
  return supervisor != nullptr &&
         supervisor->wifi_started_.load(std::memory_order_acquire) &&
         supervisor->enqueue_wifi_route(route, opened);
}

bool SpeakerAssetsSupervisor::enqueue_usb_frame(
    std::uint32_t endpoint_epoch,
    const std::uint8_t* frame,
    std::size_t length,
    std::uint32_t received_ms) {
  if (!initialized_ || endpoint_epoch == 0U ||
      frame == nullptr) {
    return false;
  }
  const speaker_assets::SpeakerAssetsRouteToken route{
      speaker_assets::SpeakerAssetsTransport::Usb,
      0U,
      endpoint_epoch,
  };
  portENTER_CRITICAL(&mailbox_mux_);
  const bool route_ready =
      ensure_mailbox_usb_route_locked(endpoint_epoch);
  const auto result = route_ready
      ? mailbox_.enqueue_usb_frame(
            route, frame, length, received_ms)
      : speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Full;
  portEXIT_CRITICAL(&mailbox_mux_);
  const bool accepted =
      result ==
      speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Accepted;
  if (accepted && platform_task_ != nullptr) {
    xTaskNotifyGive(platform_task_);
  }
  return accepted;
}

bool SpeakerAssetsSupervisor::enqueue_wifi_frame(
    const speaker_assets::SpeakerAssetsRouteToken& route,
    const std::uint8_t* frame,
    std::size_t length,
    std::uint32_t received_ms) {
  if (!initialized_ ||
      !wifi_started_.load(std::memory_order_acquire) ||
      route.transport !=
          speaker_assets::SpeakerAssetsTransport::Wifi ||
      route.route_id == 0U || route.generation == 0U ||
      frame == nullptr) {
    return false;
  }
  portENTER_CRITICAL(&mailbox_mux_);
  const bool route_ready =
      ensure_mailbox_wifi_route_locked(route);
  const auto result =
      route_ready
          ? mailbox_.enqueue_wifi_frame(
                route, frame, length, received_ms)
          : speaker_assets::
                SpeakerAssetsRuntimeEnqueueResult::Full;
  portEXIT_CRITICAL(&mailbox_mux_);
  const bool accepted =
      result ==
          speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Accepted;
  if (accepted && platform_task_ != nullptr) {
    xTaskNotifyGive(platform_task_);
  }
  return accepted;
}

bool SpeakerAssetsSupervisor::enqueue_wifi_route(
    const speaker_assets::SpeakerAssetsRouteToken& route,
    bool opened) {
  if (!initialized_ ||
      !wifi_started_.load(std::memory_order_acquire) ||
      route.transport !=
          speaker_assets::SpeakerAssetsTransport::Wifi ||
      route.route_id == 0U || route.generation == 0U) {
    return false;
  }
  portENTER_CRITICAL(&mailbox_mux_);
  speaker_assets::SpeakerAssetsRuntimeEnqueueResult result =
      speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Full;
  if (opened) {
    result = ensure_mailbox_wifi_route_locked(route)
                 ? speaker_assets::
                       SpeakerAssetsRuntimeEnqueueResult::Accepted
                 : speaker_assets::
                       SpeakerAssetsRuntimeEnqueueResult::Full;
  } else {
    result = mailbox_.enqueue_route_closed(route);
    forget_mailbox_wifi_route_locked(route);
  }
  portEXIT_CRITICAL(&mailbox_mux_);
  const bool accepted =
      result ==
          speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Accepted ||
      result ==
          speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Coalesced;
  if (accepted && platform_task_ != nullptr) {
    xTaskNotifyGive(platform_task_);
  }
  return accepted;
}

void SpeakerAssetsSupervisor::store_task_entry(void* context) {
  static_cast<SpeakerAssetsSupervisor*>(context)->run_store_worker();
}

void SpeakerAssetsSupervisor::run_store_worker() {
  for (;;) {
    synchronization_.wait_worker();
    while (store_runner_.worker_run_once() ==
           speaker_assets::SpeakerAssetsFlashWorkerResult::Completed) {
    }
  }
}

bool SpeakerAssetsSupervisor::ensure_mailbox_usb_route_locked(
    std::uint32_t endpoint_epoch) {
  if (mailbox_usb_route_.generation == endpoint_epoch) {
    return endpoint_epoch != 0U;
  }
  if (mailbox_usb_route_.generation != 0U) {
    static_cast<void>(
        mailbox_.enqueue_route_closed(mailbox_usb_route_));
    mailbox_usb_route_ = {};
  }
  if (endpoint_epoch == 0U) {
    return false;
  }
  const speaker_assets::SpeakerAssetsRouteToken next_route{
      speaker_assets::SpeakerAssetsTransport::Usb,
      0U,
      endpoint_epoch,
  };
  const auto opened = mailbox_.enqueue_route_opened(next_route);
  if (opened !=
          speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Accepted &&
      opened !=
          speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Coalesced) {
    return false;
  }
  mailbox_usb_route_ = next_route;
  return true;
}

bool SpeakerAssetsSupervisor::ensure_mailbox_wifi_route_locked(
    const speaker_assets::SpeakerAssetsRouteToken& exact_route) {
  if (exact_route.transport !=
          speaker_assets::SpeakerAssetsTransport::Wifi ||
      exact_route.route_id == 0U ||
      exact_route.generation == 0U) {
    return false;
  }
  if (speaker_assets::speaker_assets_route_equal(
          mailbox_wifi_route_, exact_route)) {
    return true;
  }
  if (mailbox_wifi_route_.generation != 0U) {
    static_cast<void>(
        mailbox_.enqueue_route_closed(mailbox_wifi_route_));
    mailbox_wifi_route_ = {};
  }
  if (mailbox_.enqueue_route_opened(exact_route) !=
      speaker_assets::SpeakerAssetsRuntimeEnqueueResult::Accepted) {
    return false;
  }
  mailbox_wifi_route_ = exact_route;
  return true;
}

void SpeakerAssetsSupervisor::forget_mailbox_wifi_route_locked(
    const speaker_assets::SpeakerAssetsRouteToken& exact_route) {
  if (speaker_assets::speaker_assets_route_equal(
          mailbox_wifi_route_, exact_route)) {
    mailbox_wifi_route_ = {};
  }
}

void SpeakerAssetsSupervisor::observe_usb_route() {
  const auto epoch = usb_->connection_epoch();
  const bool route_changed = epoch != usb_route_.generation;
  portENTER_CRITICAL(&mailbox_mux_);
  static_cast<void>(ensure_mailbox_usb_route_locked(epoch));
  portEXIT_CRITICAL(&mailbox_mux_);

  if (!route_changed) {
    return;
  }
  usb_route_ = {};
  if (epoch != 0U) {
    usb_route_ = {
        speaker_assets::SpeakerAssetsTransport::Usb,
        0U,
        epoch,
    };
  }
  // A response queued for an old physical endpoint can no longer discharge
  // the logical reply selected from Core. Exact Close will purge both Core and
  // mailbox state; forgetting this local selection prevents a later endpoint
  // from acknowledging the old sequence.
  queued_reply_sequence_ = 0U;
  queued_reply_lease_ = {};
}

void SpeakerAssetsSupervisor::import_one_mailbox_record() {
  speaker_assets::SpeakerAssetsRuntimeMailboxRecord record{};
  portENTER_CRITICAL(&mailbox_mux_);
  const bool present = mailbox_.take_next(&record);
  portEXIT_CRITICAL(&mailbox_mux_);
  if (present) {
    static_cast<void>(core_.import_mailbox_record(record));
  }
}

void SpeakerAssetsSupervisor::advance_boot_read_job() {
  if (boot_read_state_ == BootReadState::Preparing) {
    speaker_assets::SpeakerAssetsPrepareBootReadCompletion completion{};
    const auto result = store_runner_.poll_prepare_boot_read(
        boot_job_, &completion);
    if (result ==
        speaker_assets::SpeakerAssetsInternalJobPollResult::Pending) {
      return;
    }
    boot_job_ = {};
    if (result ==
            speaker_assets::SpeakerAssetsInternalJobPollResult::Completed &&
        completion.acquire_result ==
            speaker_assets::SoundStoreResult::FactoryBlank) {
      prepared_boot_ = {};
      boot_read_state_ = BootReadState::FactoryDefault;
      ESP_LOGI(
          kTag,
          "no committed sound preference; factory Boot sound selected");
      return;
    }
    if (result !=
            speaker_assets::SpeakerAssetsInternalJobPollResult::Completed ||
        !completion.prepared) {
      if (result ==
          speaker_assets::SpeakerAssetsInternalJobPollResult::Completed) {
        if (completion.resolve_result !=
            speaker_assets::SoundAssetReadResult::NotMapped) {
          ESP_LOGW(
              kTag,
              "Boot sound unavailable acquire=%u resolve=%u cleanup=%u",
              static_cast<unsigned>(completion.acquire_result),
              static_cast<unsigned>(completion.resolve_result),
              static_cast<unsigned>(completion.cleanup_result));
        }
      } else {
        ESP_LOGE(kTag,
                 "Boot sound read job lost result=%u",
                 static_cast<unsigned>(result));
      }
      prepared_boot_ = {};
      boot_read_state_ = BootReadState::Unavailable;
      return;
    }
    prepared_boot_ = completion;
    boot_read_state_ = BootReadState::Ready;
    return;
  }

  if (boot_read_state_ == BootReadState::LeaseOutstanding &&
      boot_release_requested_) {
    const auto started = store_runner_.start_release_read(
        outstanding_boot_lease_, &boot_job_);
    if (started ==
        speaker_assets::SpeakerAssetsInternalJobStartResult::Accepted) {
      boot_read_state_ = BootReadState::ReleasePending;
    } else if (
        started ==
        speaker_assets::SpeakerAssetsInternalJobStartResult::InvalidArgument) {
      ESP_LOGE(kTag, "failed to start Boot sound lease release");
    }
    return;
  }

  if (boot_read_state_ == BootReadState::ReleasePending) {
    speaker_assets::SoundStoreResult completion =
        speaker_assets::SoundStoreResult::InvalidArgument;
    const auto result = store_runner_.poll_release_read(
        boot_job_, &completion);
    if (result ==
        speaker_assets::SpeakerAssetsInternalJobPollResult::Pending) {
      return;
    }
    boot_job_ = {};
    if (result ==
            speaker_assets::SpeakerAssetsInternalJobPollResult::Completed &&
        completion == speaker_assets::SoundStoreResult::Ok) {
      outstanding_boot_lease_ = {};
      boot_release_requested_ = false;
      boot_read_state_ = BootReadState::Idle;
      return;
    }
    ESP_LOGE(kTag,
             "Boot sound lease release failed poll=%u store=%u",
             static_cast<unsigned>(result),
             static_cast<unsigned>(completion));
    // Keep the exact lease and retry through the same Store owner. A protocol
    // update must never overtake a playback bank that may still be pinned.
    boot_read_state_ = BootReadState::LeaseOutstanding;
  }
}

bool SpeakerAssetsSupervisor::release_mailbox_lease(
    const speaker_assets::SpeakerAssetsLogicalRequestLease& lease) {
  if (!lease_valid(lease)) {
    return true;
  }
  portENTER_CRITICAL(&mailbox_mux_);
  const bool released =
      mailbox_.release_logical_request(lease);
  speaker_assets::SpeakerAssetsLogicalRequestLease current{};
  const bool exact_lease_still_reserved =
      !released &&
      mailbox_.logical_request_lease(&current) &&
      speaker_assets::speaker_assets_logical_request_lease_equal(
          current, lease);
  portEXIT_CRITICAL(&mailbox_mux_);
  // A route close may already have discharged this exact lease. Treat that as
  // idempotent success, but never compare-and-clear a newer ABA admission.
  return released || !exact_lease_still_reserved;
}

void SpeakerAssetsSupervisor::run_core_steps(
    std::uint32_t now_ms,
    bool resource_steps_allowed) {
  for (std::size_t index = 0U;
       index < kMaximumCoreStepsPerPoll;
       ++index) {
    const auto outcome = core_.step(
        now_ms, resource_steps_allowed, &store_runner_);
    speaker_assets::SpeakerAssetsRuntimeStepResult result{};
    speaker_assets::SpeakerAssetsLogicalRequestLease release{};
    if (!outcome.inspect(&result, &release)) {
      break;
    }
    static_cast<void>(release_mailbox_lease(release));
    switch (result) {
      case speaker_assets::SpeakerAssetsRuntimeStepResult::Idle:
      case speaker_assets::SpeakerAssetsRuntimeStepResult::PausedForInput:
      case speaker_assets::SpeakerAssetsRuntimeStepResult::ReplyBackpressure:
      case speaker_assets::SpeakerAssetsRuntimeStepResult::ActionExecutionPending:
        return;
      default:
        break;
    }
  }
}

bool SpeakerAssetsSupervisor::retire_accepted_usb_reply(
    std::uint32_t endpoint_epoch,
    std::uint32_t runtime_reply_sequence) {
  if (!initialized_ || endpoint_epoch == 0U ||
      runtime_reply_sequence == 0U ||
      queued_reply_sequence_ != runtime_reply_sequence ||
      !lease_valid(queued_reply_lease_) ||
      queued_reply_lease_.route.transport !=
          speaker_assets::SpeakerAssetsTransport::Usb ||
      queued_reply_lease_.route.generation != endpoint_epoch) {
    ESP_LOGW(
        kTag,
        "ignored stale USB sound reply ack sent=%lu epoch=%lu expected=%lu epoch=%lu",
        static_cast<unsigned long>(runtime_reply_sequence),
        static_cast<unsigned long>(endpoint_epoch),
        static_cast<unsigned long>(queued_reply_sequence_),
        static_cast<unsigned long>(
            queued_reply_lease_.route.generation));
    return false;
  }

  const auto lease = queued_reply_lease_;
  const bool removed =
      core_.remove_reply_if_sequence(runtime_reply_sequence);
  const bool lease_discharged =
      release_mailbox_lease(lease);
  if (!removed) {
    // Removal is idempotent: an exact route close may already have purged the
    // logical reply while leaving this endpoint-accepted notification in flight.
    ESP_LOGD(
        kTag,
        "USB sound reply already retired sequence=%lu epoch=%lu",
        static_cast<unsigned long>(runtime_reply_sequence),
        static_cast<unsigned long>(endpoint_epoch));
  }
  if (!lease_discharged) {
    ESP_LOGE(
        kTag,
        "USB sound reply admission remained reserved sequence=%lu epoch=%lu",
        static_cast<unsigned long>(runtime_reply_sequence),
        static_cast<unsigned long>(endpoint_epoch));
    return false;
  }

  queued_reply_sequence_ = 0U;
  queued_reply_lease_ = {};
  return true;
}

void SpeakerAssetsSupervisor::retire_physically_sent_reply() {
  std::uint32_t sent_sequence = 0U;
  std::uint32_t sent_epoch = 0U;
  if (!usb_->take_speaker_assets_response_sent(
          &sent_sequence, &sent_epoch)) {
    return;
  }
  static_cast<void>(
      retire_accepted_usb_reply(sent_epoch, sent_sequence));
}

void SpeakerAssetsSupervisor::retire_accepted_wifi_reply() {
  if (!wifi_started_.load(std::memory_order_acquire)) {
    return;
  }
  SpeakerAssetsWifiCarrier::AcceptedResponse accepted{};
  if (!wifi_.take_response_accepted(&accepted)) {
    return;
  }
  speaker_assets::SpeakerAssetsRuntimeReply reply{};
  if (!core_.front_reply_for_route(accepted.route, &reply)) {
    // Exact Close may already have purged this reply and its admission.
    ESP_LOGD(
        kTag,
        "Wi-Fi sound reply already closed sequence=%lu route=%lu/%lu",
        static_cast<unsigned long>(
            accepted.runtime_reply_sequence),
        static_cast<unsigned long>(accepted.route.route_id),
        static_cast<unsigned long>(accepted.route.generation));
    return;
  }
  if (reply.sequence != accepted.runtime_reply_sequence ||
      !speaker_assets::speaker_assets_route_equal(
          reply.lease.route, accepted.route)) {
    ESP_LOGE(
        kTag,
        "ignored mismatched Wi-Fi sound reply accept got=%lu "
        "expected=%lu route=%lu/%lu",
        static_cast<unsigned long>(
            accepted.runtime_reply_sequence),
        static_cast<unsigned long>(reply.sequence),
        static_cast<unsigned long>(accepted.route.route_id),
        static_cast<unsigned long>(accepted.route.generation));
    return;
  }
  const bool removed = core_.remove_reply_if_sequence(
      accepted.runtime_reply_sequence);
  const bool lease_discharged =
      release_mailbox_lease(reply.lease);
  if (!removed) {
    ESP_LOGD(
        kTag,
        "Wi-Fi sound reply already retired sequence=%lu route=%lu/%lu",
        static_cast<unsigned long>(
            accepted.runtime_reply_sequence),
        static_cast<unsigned long>(accepted.route.route_id),
        static_cast<unsigned long>(accepted.route.generation));
  }
  if (!lease_discharged) {
    ESP_LOGE(
        kTag,
        "Wi-Fi sound admission remained reserved sequence=%lu "
        "route=%lu/%lu",
        static_cast<unsigned long>(
            accepted.runtime_reply_sequence),
        static_cast<unsigned long>(accepted.route.route_id),
        static_cast<unsigned long>(accepted.route.generation));
  }
}

void SpeakerAssetsSupervisor::offer_usb_reply() {
  if (queued_reply_sequence_ != 0U ||
      usb_route_.generation == 0U) {
    return;
  }
  speaker_assets::SpeakerAssetsRuntimeReply reply{};
  if (!core_.front_reply_for_route(usb_route_, &reply)) {
    return;
  }
  std::array<std::uint8_t,
             speaker_assets::kSpeakerAssetsUsbFrameBytes>
      encoded{};
  if (speaker_assets::encode_speaker_assets_usb_frame(
          reply.frame, &encoded) !=
      speaker_assets::SpeakerAssetsProtocolResult::Ok) {
    ESP_LOGE(kTag,
             "failed to encode USB sound reply sequence=%lu",
             static_cast<unsigned long>(reply.sequence));
    return;
  }
  if (!usb_->queue_speaker_assets_response_for_epoch(
          reply.sequence,
          encoded.data(),
          encoded.size(),
          usb_route_.generation)) {
    return;
  }
  queued_reply_sequence_ = reply.sequence;
  queued_reply_lease_ = reply.lease;
}

void SpeakerAssetsSupervisor::offer_wifi_reply() {
  if (!wifi_started_.load(std::memory_order_acquire)) {
    return;
  }
  if (wifi_.response_pending()) {
    return;
  }
  speaker_assets::SpeakerAssetsRuntimeReply reply{};
  if (!core_.front_reply(&reply) ||
      reply.route.transport !=
          speaker_assets::SpeakerAssetsTransport::Wifi) {
    return;
  }
  std::array<
      std::uint8_t,
      speaker_assets::kSpeakerAssetsWifiFrameMaxBytes>
      encoded{};
  std::size_t encoded_length = 0U;
  if (speaker_assets::encode_speaker_assets_wifi_frame(
          reply.frame, &encoded, &encoded_length) !=
      speaker_assets::SpeakerAssetsProtocolResult::Ok) {
    ESP_LOGE(
        kTag,
        "failed to encode Wi-Fi sound reply sequence=%lu route=%lu/%lu",
        static_cast<unsigned long>(reply.sequence),
        static_cast<unsigned long>(reply.route.route_id),
        static_cast<unsigned long>(reply.route.generation));
    return;
  }
  static_cast<void>(wifi_.queue_response(
      reply.route,
      reply.sequence,
      encoded.data(),
      encoded_length));
}

void SpeakerAssetsSupervisor::poll(
    std::uint32_t now_ms,
    bool resource_steps_allowed) {
  if (!initialized_) {
    return;
  }
  observe_usb_route();
  if (wifi_started_.load(std::memory_order_acquire)) {
    wifi_.set_usb_preferred(usb_route_.generation != 0U);
  }
  retire_physically_sent_reply();
  if (wifi_started_.load(std::memory_order_acquire)) {
    retire_accepted_wifi_reply();
  }
  import_one_mailbox_record();
  store_runner_.publish_priority_allowed(resource_steps_allowed);
  if (wifi_started_.load(std::memory_order_acquire)) {
    wifi_.publish_runtime_progress(
        store_runner_.completed_unit_generation());
  }
  advance_boot_read_job();
  if (boot_read_state_ == BootReadState::Idle) {
    run_core_steps(now_ms, resource_steps_allowed);
  }
  offer_usb_reply();
  if (wifi_started_.load(std::memory_order_acquire)) {
    offer_wifi_reply();
  }
}

}  // namespace easy_input
