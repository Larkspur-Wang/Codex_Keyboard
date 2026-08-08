#include "platform/speaker_assets_wifi.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "keyboard/audio_control_wire.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "speaker_assets/sound_asset_crypto.h"

namespace easy_input {
namespace {

constexpr const char* kTag = "speaker_wifi";
constexpr std::uint32_t kTaskStackBytes = 6U * 1024U;
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1U;
constexpr std::uint32_t kAcceptPollMs = 250U;
constexpr std::uint32_t kAuthTimeoutMs = 3000U;
constexpr std::uint32_t kFrameHeaderTimeoutMs = 30000U;
constexpr std::uint32_t kFramePayloadTimeoutMs = 3000U;
constexpr std::uint32_t kResponseTimeoutMs = 240000U;
constexpr std::uint32_t kSendTimeoutMs = 3000U;
constexpr std::uint32_t kResponsePollMs = 50U;
constexpr std::size_t kRecordPrefixBytes =
    speaker_assets::kSpeakerAssetsWifiRecordHeaderBytes;
constexpr std::array<std::uint8_t, 23> kDeviceIdContext{{
    'E', 'a', 's', 'y', 'I', 'n', 'p', 'u', 't', '/',
    's', 'p', 'e', 'a', 'k', 'e', 'r', '/', 'd', 'e',
    'v', 'i', 'c'}};

std::uint32_t monotonic_millis() {
  return static_cast<std::uint32_t>(
      esp_timer_get_time() / 1000);
}

bool same_route(
    const speaker_assets::SpeakerAssetsRouteToken& first,
    const speaker_assets::SpeakerAssetsRouteToken& second) {
  return speaker_assets::speaker_assets_route_equal(
      first, second);
}

bool route_valid(
    const speaker_assets::SpeakerAssetsRouteToken& route) {
  return route.transport ==
             speaker_assets::SpeakerAssetsTransport::Wifi &&
         route.route_id != 0U && route.generation != 0U;
}

bool wait_socket(
    int socket,
    bool readable,
    std::uint32_t timeout_ms) {
  fd_set read_set;
  fd_set write_set;
  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  if (readable) {
    FD_SET(socket, &read_set);
  } else {
    FD_SET(socket, &write_set);
  }
  timeval timeout{
      static_cast<long>(timeout_ms / 1000U),
      static_cast<long>((timeout_ms % 1000U) * 1000U),
  };
  const int result = select(
      socket + 1,
      readable ? &read_set : nullptr,
      readable ? nullptr : &write_set,
      nullptr,
      &timeout);
  return result > 0;
}

void close_socket(int* socket) {
  if (socket == nullptr || *socket < 0) {
    return;
  }
  shutdown(*socket, SHUT_RDWR);
  close(*socket);
  *socket = -1;
}

bool peer_matches_host(
    const sockaddr_in& peer,
    const KeyboardWifiServiceSnapshot& snapshot) {
  return snapshot.host_ipv4_valid &&
         peer.sin_family == AF_INET &&
         peer.sin_addr.s_addr == snapshot.host_ipv4;
}

bool peer_socket_open_non_consuming(int socket) {
  std::uint8_t next_byte = 0U;
  errno = 0;
  const int received = recv(
      socket,
      &next_byte,
      sizeof(next_byte),
      MSG_PEEK | MSG_DONTWAIT);
  const bool retryable =
      received < 0 &&
      (errno == EAGAIN || errno == EWOULDBLOCK ||
       errno == EINTR);
  return speaker_assets::
             classify_speaker_assets_wifi_peer_probe(
                 received, retryable) ==
         speaker_assets::SpeakerAssetsWifiPeerProbe::Open;
}

}  // namespace

esp_err_t SpeakerAssetsWifiCarrier::begin(
    KeyboardAudioLink* audio,
    TaskHandle_t supervisor_task,
    ReceiveFrameCallback receive_frame,
    ObserveRouteCallback observe_route,
    void* callback_context) {
  if (initialized_ || audio == nullptr ||
      supervisor_task == nullptr || receive_frame == nullptr ||
      observe_route == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  std::array<std::uint8_t, 6> mac{};
  const auto mac_result =
      esp_read_mac(mac.data(), ESP_MAC_WIFI_STA);
  if (mac_result != ESP_OK) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
    return mac_result;
  }
  speaker_assets::SoundSha256 identity;
  if (!identity.update(
          kDeviceIdContext.data(), kDeviceIdContext.size()) ||
      !identity.update(mac.data(), mac.size())) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
    return ESP_FAIL;
  }
  const auto digest = identity.finish();
  std::copy_n(
      digest.begin(), device_id_.size(), device_id_.begin());
  esp_fill_random(
      endpoint_nonce_.data(), endpoint_nonce_.size());

  audio_ = audio;
  supervisor_task_ = supervisor_task;
  receive_frame_ = receive_frame;
  observe_route_ = observe_route;
  callback_context_ = callback_context;
  activated_.store(false, std::memory_order_release);

  const auto task_result = xTaskCreatePinnedToCore(
      &SpeakerAssetsWifiCarrier::task_entry,
      "speaker_wifi",
      kTaskStackBytes,
      this,
      kTaskPriority,
      &task_,
      1);
  if (task_result != pdPASS) {
    audio_ = nullptr;
    supervisor_task_ = nullptr;
    receive_frame_ = nullptr;
    observe_route_ = nullptr;
    callback_context_ = nullptr;
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
    return ESP_ERR_NO_MEM;
  }

  // Do not publish a callback until every fallible allocation has succeeded.
  // KeyboardAudioLink invokes callbacks outside its mutex, so rolling one back
  // after publication could otherwise race an in-flight callback with mutex
  // deletion. The worker is still held behind activate() at this point.
  initialized_ = true;
  audio_->set_heartbeat_extension_callback(
      &SpeakerAssetsWifiCarrier::extend_heartbeat, this);
  return ESP_OK;
}

void SpeakerAssetsWifiCarrier::activate() {
  if (!initialized_ || task_ == nullptr) {
    return;
  }
  activated_.store(true, std::memory_order_release);
  xTaskNotifyGive(task_);
}

void SpeakerAssetsWifiCarrier::set_assets_ready(bool ready) {
  if (!initialized_) {
    return;
  }
  lock();
  state_.assets_ready = ready;
  unlock();
  if (task_ != nullptr) {
    xTaskNotifyGive(task_);
  }
}

void SpeakerAssetsWifiCarrier::set_usb_preferred(
    bool preferred) {
  if (!initialized_) {
    return;
  }
  bool changed = false;
  lock();
  changed = state_.usb_preferred != preferred;
  state_.usb_preferred = preferred;
  unlock();
  if (changed && task_ != nullptr) {
    xTaskNotifyGive(task_);
  }
}

bool SpeakerAssetsWifiCarrier::queue_response(
    const speaker_assets::SpeakerAssetsRouteToken& route,
    std::uint32_t runtime_reply_sequence,
    const std::uint8_t* payload,
    std::size_t payload_length) {
  if (!initialized_ || !route_valid(route) ||
      runtime_reply_sequence == 0U || payload == nullptr ||
      payload_length <
          speaker_assets::kSpeakerAssetsFrameHeaderBytes ||
      payload_length >
          speaker_assets::kSpeakerAssetsWifiFrameMaxBytes) {
    return false;
  }
  lock();
  const bool accepted =
      state_.route_active &&
      same_route(state_.route, route) &&
      !pending_response_.ready &&
      !response_in_flight_ &&
      !accepted_response_ready_;
  if (accepted) {
    pending_response_ = {};
    pending_response_.route = route;
    pending_response_.runtime_reply_sequence =
        runtime_reply_sequence;
    pending_response_.payload_length =
        static_cast<std::uint16_t>(payload_length);
    std::copy_n(
        payload, payload_length,
        pending_response_.payload.begin());
    pending_response_.ready = true;
  }
  unlock();
  if (accepted && task_ != nullptr) {
    xTaskNotifyGive(task_);
  }
  return accepted;
}

bool SpeakerAssetsWifiCarrier::take_response_accepted(
    AcceptedResponse* accepted) {
  if (!initialized_ || accepted == nullptr) {
    return false;
  }
  lock();
  const bool present = accepted_response_ready_;
  if (present) {
    *accepted = accepted_response_;
    accepted_response_ = {};
    accepted_response_ready_ = false;
  }
  unlock();
  if (present && task_ != nullptr) {
    xTaskNotifyGive(task_);
  }
  return present;
}

bool SpeakerAssetsWifiCarrier::route_active() const {
  if (!initialized_) {
    return false;
  }
  lock();
  const bool active = state_.route_active;
  unlock();
  return active;
}

bool SpeakerAssetsWifiCarrier::response_pending() const {
  if (!initialized_) {
    return false;
  }
  lock();
  const bool pending =
      pending_response_.ready || response_in_flight_ ||
      accepted_response_ready_;
  unlock();
  return pending;
}

void SpeakerAssetsWifiCarrier::publish_runtime_progress(
    std::uint32_t generation) {
  runtime_progress_generation_.store(
      generation, std::memory_order_release);
}

void SpeakerAssetsWifiCarrier::task_entry(void* context) {
  static_cast<SpeakerAssetsWifiCarrier*>(context)->run();
}

std::size_t SpeakerAssetsWifiCarrier::extend_heartbeat(
    void* context,
    std::uint8_t* heartbeat,
    std::size_t base_length,
    std::size_t capacity) {
  auto* carrier =
      static_cast<SpeakerAssetsWifiCarrier*>(context);
  if (carrier == nullptr || !carrier->initialized_ ||
      heartbeat == nullptr ||
      base_length != ai_keyboard::kHeartbeatPacketBytes ||
      capacity <
          speaker_assets::kSpeakerAssetsWifiDiscoveryBytes) {
    return base_length;
  }

  const auto snapshot =
      carrier->audio_->wifi_service_snapshot();
  ServiceState state{};
  std::array<
      std::uint8_t,
      speaker_assets::kSpeakerAssetsWifiIdentityBytes>
      endpoint_nonce{};
  carrier->lock();
  state = carrier->state_;
  endpoint_nonce = carrier->endpoint_nonce_;
  carrier->unlock();

  speaker_assets::SpeakerAssetsWifiDiscovery discovery{};
  if (snapshot.speaker_sync_key_valid) {
    discovery.flags |=
        speaker_assets::
            kSpeakerAssetsWifiDiscoveryKeyProvisioned;
  }
  if (state.assets_ready) {
    discovery.flags |=
        speaker_assets::kSpeakerAssetsWifiDiscoveryAssetsReady;
  }
  speaker_assets::SpeakerAssetsWifiPolicyInputs inputs{};
  inputs.configured = snapshot.configured;
  inputs.connected = snapshot.connected;
  inputs.disconnect_pending = snapshot.disconnect_pending;
  inputs.assets_ready = state.assets_ready;
  inputs.usb_preferred = state.usb_preferred;
  inputs.route_active = state.route_active;
  inputs.listener_ready = state.listener_ready;
  inputs.host_ipv4_valid = snapshot.host_ipv4_valid;
  inputs.key_valid = snapshot.speaker_sync_key_valid;
  inputs.key_epoch = snapshot.speaker_sync_key_epoch;
  inputs.generation = snapshot.generation;
  const bool ready =
      speaker_assets::speaker_assets_wifi_allow_new_route(
          inputs);
  if (ready) {
    discovery.flags |=
        speaker_assets::kSpeakerAssetsWifiDiscoveryReady;
  }
  discovery.key_epoch = snapshot.speaker_sync_key_epoch;
  discovery.device_id = carrier->device_id_;
  discovery.endpoint_nonce = endpoint_nonce;
  if (!speaker_assets::encode_speaker_assets_wifi_discovery(
          heartbeat,
          capacity,
          discovery,
          snapshot.speaker_sync_key,
          snapshot.speaker_sync_key_valid)) {
    return base_length;
  }
  return speaker_assets::kSpeakerAssetsWifiDiscoveryBytes;
}

void SpeakerAssetsWifiCarrier::run() {
  while (!activated_.load(std::memory_order_acquire)) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  }

  int listener = -1;
  std::uint32_t listener_generation = 0U;
  const auto retire_listener = [&]() {
    set_listener_ready(false);
    close_socket(&listener);
    listener_generation = 0U;
  };
  for (;;) {
    const auto snapshot = audio_->wifi_service_snapshot();
    if (listener >= 0 &&
        !speaker_assets::
            speaker_assets_wifi_listener_matches_generation(
                listener_generation, snapshot.generation)) {
      ESP_LOGI(
          kTag,
          "rebuilding speaker asset TCP listener generation=%lu->%lu",
          static_cast<unsigned long>(listener_generation),
          static_cast<unsigned long>(snapshot.generation));
      retire_listener();
    }
    if (!allow_new_route(snapshot, false)) {
      retire_listener();
      ulTaskNotifyTake(
          pdTRUE, pdMS_TO_TICKS(kAcceptPollMs));
      continue;
    }

    if (listener < 0) {
      listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
      if (listener < 0) {
        ESP_LOGW(
            kTag, "TCP socket unavailable errno=%d", errno);
        ulTaskNotifyTake(
            pdTRUE, pdMS_TO_TICKS(kAcceptPollMs));
        continue;
      }
      int reuse = 1;
      setsockopt(
          listener, SOL_SOCKET, SO_REUSEADDR,
          &reuse, sizeof(reuse));
      sockaddr_in local{};
      local.sin_family = AF_INET;
      local.sin_addr.s_addr = htonl(INADDR_ANY);
      local.sin_port = htons(
          speaker_assets::kSpeakerAssetsWifiTcpPort);
      if (bind(
              listener,
              reinterpret_cast<sockaddr*>(&local),
              sizeof(local)) != 0 ||
          listen(listener, 1) != 0) {
        ESP_LOGW(
            kTag,
            "TCP listener unavailable errno=%d",
            errno);
        close_socket(&listener);
        ulTaskNotifyTake(
            pdTRUE, pdMS_TO_TICKS(kAcceptPollMs));
        continue;
      }
      listener_generation = snapshot.generation;
      const auto ready_snapshot =
          audio_->wifi_service_snapshot();
      if (!speaker_assets::
              speaker_assets_wifi_listener_matches_generation(
                  listener_generation,
                  ready_snapshot.generation) ||
          !allow_new_route(ready_snapshot, false)) {
        retire_listener();
        continue;
      }
      rotate_endpoint_nonce();
      set_listener_ready(true);
      ESP_LOGI(
          kTag,
          "speaker asset TCP ready port=%u generation=%lu",
          static_cast<unsigned>(
              speaker_assets::kSpeakerAssetsWifiTcpPort),
          static_cast<unsigned long>(listener_generation));
    }

    if (!wait_socket(listener, true, kAcceptPollMs)) {
      continue;
    }
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    int client = accept(
        listener,
        reinterpret_cast<sockaddr*>(&peer),
        &peer_length);
    const int accept_error = client < 0 ? errno : 0;
    const bool retryable_accept =
        client < 0 &&
        (accept_error == EAGAIN ||
         accept_error == EWOULDBLOCK ||
         accept_error == EINTR);
    const auto accept_outcome =
        speaker_assets::classify_speaker_assets_wifi_accept_io(
            client, retryable_accept);
    if (accept_outcome ==
        speaker_assets::SpeakerAssetsWifiAcceptIo::Retry) {
      continue;
    }
    if (accept_outcome ==
        speaker_assets::
            SpeakerAssetsWifiAcceptIo::RebuildListener) {
      ESP_LOGW(
          kTag,
          "speaker asset TCP accept failed errno=%d generation=%lu; rebuilding listener",
          accept_error,
          static_cast<unsigned long>(listener_generation));
      retire_listener();
      continue;
    }
    int no_delay = 1;
    if (setsockopt(
            client,
            IPPROTO_TCP,
            TCP_NODELAY,
            &no_delay,
            sizeof(no_delay)) != 0) {
      ESP_LOGW(
          kTag,
          "TCP_NODELAY unavailable errno=%d",
          errno);
    }

    const auto accepted_snapshot =
        audio_->wifi_service_snapshot();
    if (!allow_new_route(accepted_snapshot, true) ||
        !peer_matches_host(peer, accepted_snapshot)) {
      ESP_LOGW(
          kTag,
          "rejected speaker asset TCP peer host_match=%u",
          peer_matches_host(peer, accepted_snapshot) ? 1U : 0U);
      close_socket(&client);
      retire_listener();
      continue;
    }
    // This listener epoch is now consumed by one accepted connection. Remove
    // Ready before authentication/session work so no second client can learn
    // or queue against an epoch that is already owned.
    set_listener_ready(false);
    service_connection(client, accepted_snapshot);
    close_socket(&client);
    // A Ready advertisement is valid for exactly one accepted TCP lifetime.
    // Rebuild even after a clean peer close so stale PCB/listener state can
    // never be re-advertised as a usable data plane.
    retire_listener();
  }
}

void SpeakerAssetsWifiCarrier::service_connection(
    int socket,
    const KeyboardWifiServiceSnapshot& snapshot) {
  std::array<
      std::uint8_t,
      speaker_assets::kSpeakerAssetsWifiIdentityBytes>
      endpoint_nonce{};
  lock();
  endpoint_nonce = endpoint_nonce_;
  unlock();
  std::array<std::uint8_t,
             speaker_assets::kSpeakerAssetsWifiClientAuthBytes>
      auth_bytes{};
  if (!read_exact(
          socket,
          auth_bytes.data(),
          auth_bytes.size(),
          kAuthTimeoutMs,
          snapshot.generation)) {
    return;
  }

  speaker_assets::SpeakerAssetsWifiClientAuth auth{};
  if (!speaker_assets::parse_speaker_assets_wifi_client_auth(
          auth_bytes.data(),
          auth_bytes.size(),
          snapshot.speaker_sync_key,
          device_id_,
          endpoint_nonce,
          &auth)) {
    ESP_LOGW(kTag, "speaker asset TCP authentication failed");
    std::copy_n(
        auth_bytes.begin() + 8U,
        auth.client_nonce.size(),
        auth.client_nonce.begin());
    std::array<
        std::uint8_t,
        speaker_assets::kSpeakerAssetsWifiServerReadyBytes>
        rejected{};
    const auto rejected_route_id = next_route_id();
    const auto rejected_generation =
        next_route_generation();
    if (speaker_assets::encode_speaker_assets_wifi_server_ready(
            &rejected,
            speaker_assets::
                SpeakerAssetsWifiReadyStatus::BadAuth,
            rejected_route_id,
            rejected_generation,
            snapshot.speaker_sync_key,
            device_id_,
            endpoint_nonce,
            auth.client_nonce)) {
      static_cast<void>(write_exact(
          socket,
          rejected.data(),
          rejected.size(),
          kSendTimeoutMs,
          snapshot.generation));
    }
    return;
  }

  const auto route = speaker_assets::SpeakerAssetsRouteToken{
      speaker_assets::SpeakerAssetsTransport::Wifi,
      next_route_id(),
      next_route_generation(),
  };
  auto status = speaker_assets::SpeakerAssetsWifiReadyStatus::Ok;
  KeyboardWifiServiceLease service_lease{};
  bool route_opened = false;
  if (auth.key_epoch != snapshot.speaker_sync_key_epoch) {
    status =
        speaker_assets::SpeakerAssetsWifiReadyStatus::WrongEpoch;
  } else if (!lifecycle_still_exact(snapshot.generation)) {
    status =
        speaker_assets::SpeakerAssetsWifiReadyStatus::NotReady;
  } else if (!audio_->acquire_wifi_service_lease(
                 snapshot.generation, &service_lease)) {
    status = speaker_assets::SpeakerAssetsWifiReadyStatus::Busy;
  } else if (!observe_route_(
                 callback_context_, route, true)) {
    status = speaker_assets::SpeakerAssetsWifiReadyStatus::Busy;
    static_cast<void>(
        audio_->release_wifi_service_lease(service_lease));
    service_lease = {};
  } else {
    route_opened = true;
  }

  std::array<
      std::uint8_t,
      speaker_assets::kSpeakerAssetsWifiServerReadyBytes>
      ready{};
  const bool ready_sent =
      speaker_assets::encode_speaker_assets_wifi_server_ready(
          &ready,
          status,
          route.route_id,
          route.generation,
          snapshot.speaker_sync_key,
          device_id_,
          endpoint_nonce,
          auth.client_nonce) &&
      write_exact(
          socket,
          ready.data(),
          ready.size(),
          kSendTimeoutMs,
          snapshot.generation);
  if (!ready_sent ||
      status != speaker_assets::SpeakerAssetsWifiReadyStatus::Ok) {
    if (route_opened) {
      static_cast<void>(
          observe_route_(callback_context_, route, false));
    }
    if (service_lease.valid()) {
      static_cast<void>(
          audio_->release_wifi_service_lease(service_lease));
    }
    return;
  }

  set_active_route(route);
  const auto session_key =
      speaker_assets::derive_speaker_assets_wifi_session_key(
          snapshot.speaker_sync_key,
          device_id_,
          endpoint_nonce,
          auth.client_nonce);
  std::uint32_t previous_sequence = 0U;
  bool keep_running = true;
  while (keep_running &&
         lifecycle_still_exact(snapshot.generation)) {
    std::array<
        std::uint8_t,
        speaker_assets::kSpeakerAssetsWifiRecordMaxBytes>
        encoded{};
    if (!read_exact(
            socket,
            encoded.data(),
            kRecordPrefixBytes,
            kFrameHeaderTimeoutMs,
            snapshot.generation)) {
      break;
    }
    const auto payload_length = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(encoded[6]) |
        (static_cast<std::uint16_t>(encoded[7]) << 8U));
    if (payload_length <
            speaker_assets::kSpeakerAssetsFrameHeaderBytes ||
        payload_length >
            speaker_assets::kSpeakerAssetsWifiFrameMaxBytes ||
        !read_exact(
            socket,
            encoded.data() + kRecordPrefixBytes,
            payload_length,
            kFramePayloadTimeoutMs,
            snapshot.generation)) {
      break;
    }
    speaker_assets::SpeakerAssetsWifiRecord request{};
    if (!speaker_assets::decode_speaker_assets_wifi_record(
            encoded.data(),
            kRecordPrefixBytes + payload_length,
            session_key,
            speaker_assets::SpeakerAssetsWifiRecordKind::Request,
            &request) ||
        (previous_sequence != 0U &&
         request.sequence !=
             (previous_sequence ==
                      std::numeric_limits<std::uint32_t>::max()
                  ? 1U
                  : previous_sequence + 1U))) {
      ESP_LOGW(kTag, "invalid speaker asset TCP record");
      break;
    }
    previous_sequence = request.sequence;
    audio_->note_wifi_service_activity();
    if (!receive_frame_(
            callback_context_,
            route,
            request.payload.data(),
            request.payload_length,
            monotonic_millis())) {
      ESP_LOGW(kTag, "speaker asset core admission busy");
      break;
    }

    auto response_watchdog =
        speaker_assets::
            start_speaker_assets_wifi_response_watchdog(
                monotonic_millis(),
                kResponseTimeoutMs,
                runtime_progress_generation_.load(
                    std::memory_order_acquire));
    PendingResponse response{};
    for (;;) {
      if (!lifecycle_still_exact(snapshot.generation)) {
        keep_running = false;
        break;
      }
      // The App's status read may deliberately close the TCP exchange before
      // the long-running Flash result is ready. MSG_PEEK observes EOF without
      // consuming an early next-record byte, so cleanup can release the exact
      // route/service lease immediately instead of waiting 240 seconds.
      if (!peer_socket_open_non_consuming(socket)) {
        keep_running = false;
        break;
      }
      if (take_pending_response(route, &response)) {
        break;
      }
      const auto now_ms = monotonic_millis();
      speaker_assets::
          observe_speaker_assets_wifi_response_progress(
              &response_watchdog,
              now_ms,
              kResponseTimeoutMs,
              runtime_progress_generation_.load(
                  std::memory_order_acquire));
      if (speaker_assets::
              speaker_assets_wifi_response_watchdog_expired(
                  response_watchdog, now_ms)) {
        ESP_LOGW(
            kTag,
            "speaker asset response made no progress for %lu ms route=%lu/%lu sequence=%lu",
            static_cast<unsigned long>(kResponseTimeoutMs),
            static_cast<unsigned long>(route.route_id),
            static_cast<unsigned long>(route.generation),
            static_cast<unsigned long>(request.sequence));
        keep_running = false;
        break;
      }
      ulTaskNotifyTake(
          pdTRUE, pdMS_TO_TICKS(kResponsePollMs));
    }
    if (!keep_running || !response.ready) {
      break;
    }

    speaker_assets::SpeakerAssetsWifiRecord response_record{};
    response_record.kind =
        speaker_assets::SpeakerAssetsWifiRecordKind::Response;
    response_record.sequence = request.sequence;
    response_record.payload_length = response.payload_length;
    std::copy_n(
        response.payload.begin(), response.payload_length,
        response_record.payload.begin());
    std::size_t response_length = 0U;
    if (!speaker_assets::encode_speaker_assets_wifi_record(
            response_record,
            session_key,
            &encoded,
            &response_length) ||
        !write_exact(
            socket,
            encoded.data(),
            response_length,
            kSendTimeoutMs,
            snapshot.generation)) {
      break;
    }
    audio_->note_wifi_service_activity();
    if (!publish_accepted_response(
            route, response.runtime_reply_sequence)) {
      keep_running = false;
      break;
    }
    // Do not admit the next network request until the platform task has
    // retired this exact Core reply and mailbox admission.
    for (;;) {
      lock();
      const bool accepted_pending =
          accepted_response_ready_ &&
          same_route(accepted_response_.route, route);
      unlock();
      if (!accepted_pending) {
        break;
      }
      if (!lifecycle_still_exact(snapshot.generation)) {
        keep_running = false;
        break;
      }
      ulTaskNotifyTake(
          pdTRUE, pdMS_TO_TICKS(kResponsePollMs));
    }
  }

  clear_pending_response(route);
  clear_active_route(route);
  static_cast<void>(
      observe_route_(callback_context_, route, false));
  static_cast<void>(
      audio_->release_wifi_service_lease(service_lease));
  // The consumed listener remains non-Ready until run() closes it, creates a
  // fresh listener/nonce epoch, and publishes the next Ready edge.
  audio_->request_heartbeat_refresh();
}

speaker_assets::SpeakerAssetsWifiPolicyInputs
SpeakerAssetsWifiCarrier::policy_inputs(
    const KeyboardWifiServiceSnapshot& snapshot) const {
  speaker_assets::SpeakerAssetsWifiPolicyInputs inputs{};
  lock();
  inputs.assets_ready = state_.assets_ready;
  inputs.listener_ready = state_.listener_ready;
  inputs.route_active = state_.route_active;
  inputs.usb_preferred = state_.usb_preferred;
  unlock();
  inputs.configured = snapshot.configured;
  inputs.connected = snapshot.connected;
  inputs.disconnect_pending = snapshot.disconnect_pending;
  inputs.host_ipv4_valid = snapshot.host_ipv4_valid;
  inputs.key_valid = snapshot.speaker_sync_key_valid;
  inputs.key_epoch = snapshot.speaker_sync_key_epoch;
  inputs.generation = snapshot.generation;
  return inputs;
}

bool SpeakerAssetsWifiCarrier::allow_new_route(
    const KeyboardWifiServiceSnapshot& snapshot,
    bool require_listener) const {
  auto inputs = policy_inputs(snapshot);
  if (!require_listener) {
    // Listener creation evaluates all admission inputs except the listener
    // that it is about to create.
    inputs.listener_ready = true;
  }
  return initialized_ &&
         speaker_assets::speaker_assets_wifi_allow_new_route(
             inputs);
}

bool SpeakerAssetsWifiCarrier::lifecycle_still_exact(
    std::uint32_t generation) const {
  const auto snapshot = audio_->wifi_service_snapshot();
  if (snapshot.generation != generation) {
    return false;
  }
  const auto inputs = policy_inputs(snapshot);
  if (inputs.route_active) {
    return speaker_assets::
        speaker_assets_wifi_keep_active_route(inputs);
  }
  // An accepted connection consumes the advertised listener epoch before it
  // becomes an active route. Its authentication/admission checks still need
  // the same network/config lifetime, but must not depend on Ready remaining
  // published for new clients.
  return allow_new_route(snapshot, false);
}

bool SpeakerAssetsWifiCarrier::read_exact(
    int socket,
    std::uint8_t* destination,
    std::size_t length,
    std::uint32_t timeout_ms,
    std::uint32_t expected_generation) {
  if (destination == nullptr || length == 0U) {
    return false;
  }
  const auto deadline = monotonic_millis() + timeout_ms;
  std::size_t offset = 0U;
  while (offset < length) {
    if (!lifecycle_still_exact(expected_generation)) {
      const auto current = audio_->wifi_service_snapshot();
      ESP_LOGI(
          kTag,
          "speaker asset TCP read stopped by lifecycle expected=%lu current=%lu connected=%u pending=%u offset=%u length=%u",
          static_cast<unsigned long>(expected_generation),
          static_cast<unsigned long>(current.generation),
          current.connected ? 1U : 0U,
          current.disconnect_pending ? 1U : 0U,
          static_cast<unsigned>(offset),
          static_cast<unsigned>(length));
      return false;
    }
    const auto now = monotonic_millis();
    if (static_cast<std::int32_t>(now - deadline) >= 0) {
      ESP_LOGW(
          kTag,
          "speaker asset TCP read timeout generation=%lu offset=%u length=%u timeout_ms=%lu",
          static_cast<unsigned long>(expected_generation),
          static_cast<unsigned>(offset),
          static_cast<unsigned>(length),
          static_cast<unsigned long>(timeout_ms));
      return false;
    }
    const auto remaining =
        static_cast<std::uint32_t>(deadline - now);
    if (!wait_socket(
            socket,
            true,
            std::min(remaining, kAcceptPollMs))) {
      continue;
    }
    const int received = recv(
        socket,
        destination + offset,
        length - offset,
        0);
    const int socket_error = received < 0 ? errno : 0;
    const bool retryable =
        received < 0 &&
        (socket_error == EAGAIN ||
         socket_error == EWOULDBLOCK ||
         socket_error == EINTR);
    switch (speaker_assets::classify_speaker_assets_wifi_socket_io(
        received, retryable)) {
      case speaker_assets::SpeakerAssetsWifiSocketIo::Progress:
        offset += static_cast<std::size_t>(received);
        break;
      case speaker_assets::SpeakerAssetsWifiSocketIo::Retry:
        continue;
      case speaker_assets::SpeakerAssetsWifiSocketIo::PeerClosed:
        ESP_LOGI(
            kTag,
            "speaker asset TCP peer closed while reading offset=%u length=%u",
            static_cast<unsigned>(offset),
            static_cast<unsigned>(length));
        return false;
      case speaker_assets::SpeakerAssetsWifiSocketIo::Fatal:
        ESP_LOGW(
            kTag,
            "speaker asset TCP recv failed errno=%d offset=%u length=%u",
            socket_error,
            static_cast<unsigned>(offset),
            static_cast<unsigned>(length));
        return false;
    }
  }
  return true;
}

bool SpeakerAssetsWifiCarrier::write_exact(
    int socket,
    const std::uint8_t* source,
    std::size_t length,
    std::uint32_t timeout_ms,
    std::uint32_t expected_generation) {
  if (source == nullptr || length == 0U) {
    return false;
  }
  const auto deadline = monotonic_millis() + timeout_ms;
  std::size_t offset = 0U;
  while (offset < length) {
    if (!lifecycle_still_exact(expected_generation)) {
      const auto current = audio_->wifi_service_snapshot();
      ESP_LOGI(
          kTag,
          "speaker asset TCP write stopped by lifecycle expected=%lu current=%lu connected=%u pending=%u offset=%u length=%u",
          static_cast<unsigned long>(expected_generation),
          static_cast<unsigned long>(current.generation),
          current.connected ? 1U : 0U,
          current.disconnect_pending ? 1U : 0U,
          static_cast<unsigned>(offset),
          static_cast<unsigned>(length));
      return false;
    }
    const auto now = monotonic_millis();
    if (static_cast<std::int32_t>(now - deadline) >= 0) {
      ESP_LOGW(
          kTag,
          "speaker asset TCP write timeout generation=%lu offset=%u length=%u timeout_ms=%lu",
          static_cast<unsigned long>(expected_generation),
          static_cast<unsigned>(offset),
          static_cast<unsigned>(length),
          static_cast<unsigned long>(timeout_ms));
      return false;
    }
    const auto remaining =
        static_cast<std::uint32_t>(deadline - now);
    if (!wait_socket(
            socket,
            false,
            std::min(remaining, kAcceptPollMs))) {
      continue;
    }
    const int sent = send(
        socket,
        source + offset,
        length - offset,
        0);
    const int socket_error = sent < 0 ? errno : 0;
    const bool retryable =
        sent < 0 &&
        (socket_error == EAGAIN ||
         socket_error == EWOULDBLOCK ||
         socket_error == EINTR);
    switch (speaker_assets::classify_speaker_assets_wifi_socket_io(
        sent, retryable)) {
      case speaker_assets::SpeakerAssetsWifiSocketIo::Progress:
        offset += static_cast<std::size_t>(sent);
        break;
      case speaker_assets::SpeakerAssetsWifiSocketIo::Retry:
        continue;
      case speaker_assets::SpeakerAssetsWifiSocketIo::PeerClosed:
        ESP_LOGI(
            kTag,
            "speaker asset TCP peer closed while writing offset=%u length=%u",
            static_cast<unsigned>(offset),
            static_cast<unsigned>(length));
        return false;
      case speaker_assets::SpeakerAssetsWifiSocketIo::Fatal:
        ESP_LOGW(
            kTag,
            "speaker asset TCP send failed errno=%d offset=%u length=%u",
            socket_error,
            static_cast<unsigned>(offset),
            static_cast<unsigned>(length));
        return false;
    }
  }
  return true;
}

bool SpeakerAssetsWifiCarrier::take_pending_response(
    const speaker_assets::SpeakerAssetsRouteToken& route,
    PendingResponse* response) {
  if (response == nullptr) {
    return false;
  }
  lock();
  const bool present =
      pending_response_.ready &&
      same_route(pending_response_.route, route);
  if (present) {
    *response = pending_response_;
    pending_response_ = {};
    response_in_flight_ = true;
  }
  unlock();
  return present;
}

bool SpeakerAssetsWifiCarrier::publish_accepted_response(
    const speaker_assets::SpeakerAssetsRouteToken& route,
    std::uint32_t runtime_reply_sequence) {
  lock();
  const bool accepted =
      !accepted_response_ready_ &&
      route_valid(route) &&
      runtime_reply_sequence != 0U;
  if (accepted) {
    accepted_response_ = {
        route,
        runtime_reply_sequence,
    };
    accepted_response_ready_ = true;
    response_in_flight_ = false;
  }
  unlock();
  if (accepted && supervisor_task_ != nullptr) {
    xTaskNotifyGive(supervisor_task_);
  }
  return accepted;
}

void SpeakerAssetsWifiCarrier::clear_pending_response(
    const speaker_assets::SpeakerAssetsRouteToken& route) {
  lock();
  if (pending_response_.ready &&
      same_route(pending_response_.route, route)) {
    pending_response_ = {};
  }
  if (state_.route_active &&
      same_route(state_.route, route)) {
    response_in_flight_ = false;
  }
  unlock();
}

void SpeakerAssetsWifiCarrier::set_listener_ready(bool ready) {
  bool changed = false;
  lock();
  changed = state_.listener_ready != ready;
  state_.listener_ready = ready;
  unlock();
  if (changed && audio_ != nullptr) {
    // Registration-time refresh can precede asynchronous bind/listen. Publish
    // the exact Ready transition instead of making the App wait for the next
    // 2-4 second periodic heartbeat. Clearing Ready is equally important.
    audio_->request_heartbeat_refresh();
  }
}

void SpeakerAssetsWifiCarrier::set_active_route(
    const speaker_assets::SpeakerAssetsRouteToken& route) {
  lock();
  state_.route_active = true;
  state_.route = route;
  unlock();
  if (audio_ != nullptr) {
    // Route admission changes the advertised endpoint from Ready to Busy.
    // Wake the control task only after releasing the carrier mutex because
    // heartbeat extension encoding reads this same state.
    audio_->request_heartbeat_refresh();
  }
}

void SpeakerAssetsWifiCarrier::clear_active_route(
    const speaker_assets::SpeakerAssetsRouteToken& route) {
  lock();
  if (state_.route_active &&
      same_route(state_.route, route)) {
    state_.route_active = false;
    state_.route = {};
  }
  unlock();
}

void SpeakerAssetsWifiCarrier::rotate_endpoint_nonce() {
  std::array<
      std::uint8_t,
      speaker_assets::kSpeakerAssetsWifiIdentityBytes>
      next{};
  esp_fill_random(next.data(), next.size());
  if (std::all_of(
          next.begin(), next.end(),
          [](std::uint8_t byte) { return byte == 0U; })) {
    next[0] = 1U;
  }
  lock();
  endpoint_nonce_ = next;
  unlock();
}

std::uint32_t SpeakerAssetsWifiCarrier::next_route_id() {
  lock();
  const auto value = next_route_id_;
  next_route_id_ =
      next_route_id_ ==
              std::numeric_limits<std::uint32_t>::max()
          ? 1U
          : next_route_id_ + 1U;
  unlock();
  return value;
}

std::uint32_t
SpeakerAssetsWifiCarrier::next_route_generation() {
  lock();
  const auto value = next_route_generation_;
  next_route_generation_ =
      next_route_generation_ ==
              std::numeric_limits<std::uint32_t>::max()
          ? 1U
          : next_route_generation_ + 1U;
  unlock();
  return value;
}

void SpeakerAssetsWifiCarrier::lock() const {
  configASSERT(mutex_ != nullptr);
  configASSERT(
      xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE);
}

void SpeakerAssetsWifiCarrier::unlock() const {
  configASSERT(mutex_ != nullptr);
  configASSERT(xSemaphoreGive(mutex_) == pdTRUE);
}

}  // namespace easy_input
