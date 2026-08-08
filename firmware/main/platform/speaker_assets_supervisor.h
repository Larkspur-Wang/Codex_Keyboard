#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/speaker_assets_wifi.h"
#include "speaker_assets/esp_sound_bank_storage.h"
#include "speaker_assets/speaker_assets_flash_runner.h"
#include "speaker_assets/speaker_assets_runtime.h"
#include "speaker_assets/sound_asset_reader.h"

namespace easy_input {

class UsbHidTransport;
class KeyboardAudioLink;

enum class SpeakerAssetsBootPlaybackResult : std::uint8_t {
  Pending,
  Ready,
  FactoryDefault,
  Unavailable,
};

// FreeRTOS-owned synchronization for the allocation-free speaker asset core.
// The binary semaphore is deliberately latched: a grant or accepted job that
// arrives immediately before the Store worker waits cannot be lost.
class SpeakerAssetsPlatformSynchronization final
    : public speaker_assets::SpeakerAssetsFlashRunnerSynchronization {
 public:
  esp_err_t begin(TaskHandle_t supervisor_task);

  void lock() override;
  void unlock() override;
  void notify_worker() override;
  void notify_supervisor() override;
  void wait_worker() override;

 private:
  SemaphoreHandle_t exchange_mutex_ = nullptr;
  SemaphoreHandle_t worker_signal_ = nullptr;
  TaskHandle_t supervisor_task_ = nullptr;
};

// Production USB/Wi-Fi resource path. Local storage, the Store worker, and USB
// callbacks start independently from the optional Wi-Fi carrier. TinyUSB and
// Wi-Fi callbacks only copy one fixed logical record into the mailbox;
// protocol/session work stays on the platform task and all Flash/hash work
// stays on the dedicated Store task.
class SpeakerAssetsSupervisor final {
 public:
  SpeakerAssetsSupervisor();
  SpeakerAssetsSupervisor(const SpeakerAssetsSupervisor&) = delete;
  SpeakerAssetsSupervisor& operator=(const SpeakerAssetsSupervisor&) = delete;

  esp_err_t begin_local(UsbHidTransport* usb,
                        TaskHandle_t platform_task);
  // Idempotent after success and retryable after a failed carrier allocation.
  // Local storage, USB callbacks, and the Store worker remain usable on error.
  esp_err_t start_wifi(KeyboardAudioLink* audio);
  void poll(std::uint32_t now_ms, bool resource_steps_allowed);

  bool ready() const;
  bool wifi_ready() const;
  bool boot_idle() const;
  bool transfer_active() const;
  SpeakerAssetsBootPlaybackResult take_boot_playback(
      speaker_assets::SoundReadLease* lease,
      speaker_assets::SoundResolvedAsset* asset);
  bool queue_playback_lease_release(
      const speaker_assets::SoundReadLease& lease);
  speaker_assets::SoundBankStorage& playback_storage();

 private:
  enum class BootReadState : std::uint8_t {
    Idle,
    Preparing,
    Ready,
    FactoryDefault,
    Unavailable,
    LeaseOutstanding,
    ReleasePending,
  };

  static bool receive_usb_frame(void* context,
                                std::uint32_t endpoint_epoch,
                                const std::uint8_t* frame,
                                std::size_t length);
  static bool retire_usb_reply_on_endpoint_accept(
      void* context,
      std::uint32_t endpoint_epoch,
      std::uint32_t runtime_reply_sequence);
  static bool receive_wifi_frame(
      void* context,
      const speaker_assets::SpeakerAssetsRouteToken& route,
      const std::uint8_t* frame,
      std::size_t length,
      std::uint32_t received_ms);
  static bool observe_wifi_route(
      void* context,
      const speaker_assets::SpeakerAssetsRouteToken& route,
      bool opened);
  static void store_task_entry(void* context);

  bool enqueue_usb_frame(std::uint32_t endpoint_epoch,
                         const std::uint8_t* frame,
                         std::size_t length,
                         std::uint32_t received_ms);
  bool enqueue_wifi_frame(
      const speaker_assets::SpeakerAssetsRouteToken& route,
      const std::uint8_t* frame,
      std::size_t length,
      std::uint32_t received_ms);
  bool enqueue_wifi_route(
      const speaker_assets::SpeakerAssetsRouteToken& route,
      bool opened);
  void run_store_worker();
  bool ensure_mailbox_usb_route_locked(std::uint32_t endpoint_epoch);
  bool ensure_mailbox_wifi_route_locked(
      const speaker_assets::SpeakerAssetsRouteToken& exact_route);
  void forget_mailbox_wifi_route_locked(
      const speaker_assets::SpeakerAssetsRouteToken& exact_route);
  void observe_usb_route();
  void import_one_mailbox_record();
  void advance_boot_read_job();
  void run_core_steps(std::uint32_t now_ms,
                      bool resource_steps_allowed);
  bool release_mailbox_lease(
      const speaker_assets::SpeakerAssetsLogicalRequestLease& lease);
  bool retire_accepted_usb_reply(
      std::uint32_t endpoint_epoch,
      std::uint32_t runtime_reply_sequence);
  void retire_physically_sent_reply();
  void retire_accepted_wifi_reply();
  void offer_usb_reply();
  void offer_wifi_reply();

  mutable portMUX_TYPE mailbox_mux_ = portMUX_INITIALIZER_UNLOCKED;
  speaker_assets::SpeakerAssetsRuntimeMailbox mailbox_;
  speaker_assets::SpeakerAssetsRuntimeCore core_;
  SpeakerAssetsPlatformSynchronization synchronization_;
  speaker_assets::EspSoundBankStorage storage_;
  speaker_assets::SpeakerAssetsCooperativeStoreRunner store_runner_;
  // Boot resolution and release use StoreRunner's one private SoundAssetStore.
  // This exact lease therefore pins the same bank that protocol updates use.
  speaker_assets::SpeakerAssetsInternalJobHandle boot_job_{};
  speaker_assets::SpeakerAssetsPrepareBootReadCompletion
      prepared_boot_{};
  speaker_assets::SoundReadLease outstanding_boot_lease_{};
  BootReadState boot_read_state_ = BootReadState::Idle;
  bool boot_release_requested_ = false;

  UsbHidTransport* usb_ = nullptr;
  TaskHandle_t platform_task_ = nullptr;
  TaskHandle_t store_task_ = nullptr;
  SpeakerAssetsWifiCarrier wifi_;
  // Callback/mailbox publication lifetime. This is touched only while
  // mailbox_mux_ is held and guarantees Open precedes the first frame.
  speaker_assets::SpeakerAssetsRouteToken mailbox_usb_route_{};
  speaker_assets::SpeakerAssetsRouteToken mailbox_wifi_route_{};
  // Platform-owner lifetime used to select and send replies.
  speaker_assets::SpeakerAssetsRouteToken usb_route_{};
  speaker_assets::SpeakerAssetsLogicalRequestLease queued_reply_lease_{};
  std::uint32_t queued_reply_sequence_ = 0;
  bool local_setup_ready_ = false;
  bool initialized_ = false;
  std::atomic<bool> wifi_started_{false};
};

}  // namespace easy_input
