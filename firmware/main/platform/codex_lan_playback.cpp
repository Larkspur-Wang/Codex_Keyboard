#include "platform/codex_lan_playback.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace easy_input {
namespace {

constexpr const char* kTag = "codex_play";
constexpr std::uint32_t kRequestRetryMs = 500U;
constexpr std::uint32_t kReceiveTimeoutMs = 30000U;
constexpr std::uint32_t kFinishedRetryMs = 500U;
constexpr std::uint32_t kCancelRetryMs = 100U;
constexpr std::uint8_t kMaximumRequestRetries = 12U;
constexpr std::uint8_t kMaximumFinishedRetries = 120U;
constexpr std::uint8_t kMaximumCancelRetries = 20U;
constexpr std::size_t kStreamingPrebufferBytes = 24U * 1024U;

std::uint32_t millis() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

std::uint64_t random_nonce() {
  std::uint64_t nonce =
      (static_cast<std::uint64_t>(esp_random()) << 32U) | esp_random();
  return nonce == 0U ? 1U : nonce;
}

}  // namespace

esp_err_t CodexLanPlayback::begin(
    KeyboardAudioLink* audio,
    SpeakerOutput* speaker,
    ai_keyboard::AudioIoArbiter* audio_io_arbiter,
    easy_codex::CodexSlotState* slots,
    TaskHandle_t supervisor_task) {
  if (audio_ != nullptr || audio == nullptr || speaker == nullptr ||
      audio_io_arbiter == nullptr || slots == nullptr ||
      supervisor_task == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  audio_ = audio;
  speaker_ = speaker;
  audio_io_arbiter_ = audio_io_arbiter;
  slots_ = slots;
  supervisor_task_ = supervisor_task;
  return ESP_OK;
}

bool CodexLanPlayback::request(
    std::uint8_t slot,
    std::uint32_t request_generation,
    std::uint32_t connection_generation) {
  if (slot < 1U || slot > 4U ||
      request_generation == 0U || connection_generation == 0U) {
    return false;
  }
  if (phase_ == Phase::Cancelling) {
    deferred_slot_ = slot;
    deferred_request_generation_ = request_generation;
    deferred_connection_generation_ = connection_generation;
    return true;
  }
  if (phase_ == Phase::AwaitBegin) {
    cleanup(false);
  } else if (phase_ != Phase::Idle) {
    return false;
  }
  const auto snapshot = audio_->wifi_service_snapshot();
  if (!snapshot.configured || !snapshot.connected ||
      snapshot.disconnect_pending || !snapshot.host_ipv4_valid ||
      snapshot.host_port == 0U || snapshot.generation != connection_generation ||
      !snapshot.speaker_sync_key_valid ||
      !audio_->acquire_wifi_service_lease(connection_generation, &wifi_lease_)) {
    return false;
  }
  key_ = snapshot.speaker_sync_key;
  host_ipv4_ = snapshot.host_ipv4;
  host_port_ = snapshot.host_port;
  request_ = {
      slot,
      request_generation,
      connection_generation,
      random_nonce(),
  };
  if (!open_socket(snapshot) || !send_request()) {
    cleanup(false);
    return false;
  }
  phase_ = Phase::AwaitBegin;
  request_retries_ = 0U;
  last_send_ms_ = millis();
  audio_->note_wifi_service_activity();
  ESP_LOGI(kTag,
           "request slot=%u request_generation=%lu connection_generation=%lu",
           static_cast<unsigned>(slot),
           static_cast<unsigned long>(request_generation),
           static_cast<unsigned long>(connection_generation));
  return true;
}

void CodexLanPlayback::preempt(
    const easy_codex::PlaybackIdentity& identity) {
  if (phase_ == Phase::Idle) {
    return;
  }
  deferred_slot_ = 0U;
  deferred_request_generation_ = 0U;
  deferred_connection_generation_ = 0U;
  if (easy_codex::valid_playback_identity(identity) &&
      slot_identity() != identity) {
    return;
  }
  if (easy_codex::valid_playback_identity(slot_identity())) {
    send_ack(3U);
  }
  stream_cancelled_.store(true, std::memory_order_release);
  phase_ = Phase::Cancelling;
  cancel_retries_ = 0U;
  cancel_acknowledged_ = false;
  last_send_ms_ = millis();
  speaker_->poll(false);
}

void CodexLanPlayback::poll() {
  if (phase_ == Phase::Idle) {
    return;
  }
  if (phase_ == Phase::Cancelling) {
    receive_packets();
    speaker_->poll(false);
    const auto now = millis();
    const bool cancel_has_identity =
        easy_codex::valid_playback_identity(slot_identity());
    if (cancel_has_identity && !cancel_acknowledged_ &&
        cancel_retries_ < kMaximumCancelRetries &&
        now - last_send_ms_ >= kCancelRetryMs) {
      send_ack(3U);
      ++cancel_retries_;
      last_send_ms_ = now;
    }
    if (!speaker_->busy() &&
        (!cancel_has_identity || cancel_acknowledged_ ||
         cancel_retries_ >= kMaximumCancelRetries)) {
      const auto deferred_slot = deferred_slot_;
      const auto deferred_request_generation = deferred_request_generation_;
      const auto deferred_connection_generation = deferred_connection_generation_;
      deferred_slot_ = 0U;
      deferred_request_generation_ = 0U;
      deferred_connection_generation_ = 0U;
      cleanup(false);
      if (deferred_slot != 0U) {
        request(deferred_slot,
                deferred_request_generation,
                deferred_connection_generation);
      }
    }
    return;
  }

  receive_packets();
  const auto now = millis();
  if (phase_ == Phase::AwaitBegin &&
      now - last_send_ms_ >= kRequestRetryMs) {
    if (request_retries_ >= kMaximumRequestRetries || !send_request()) {
      fail("begin_timeout");
      return;
    }
    ++request_retries_;
    last_send_ms_ = now;
  } else if ((phase_ == Phase::Receiving || phase_ == Phase::Playing) &&
             received_bytes_ < begin_.total_bytes &&
             now - last_send_ms_ >= kReceiveTimeoutMs) {
    fail("data_timeout");
    return;
  } else if (phase_ == Phase::Playing && !speaker_->busy()) {
    if (speaker_->last_result() ==
            ai_keyboard::SpeakerPlaybackResult::Succeeded &&
        slots_->mark_playback_drained(
            slot_identity(), begin_.total_samples)) {
      phase_ = Phase::FinishedPendingAck;
      finished_retries_ = 0U;
      if (!send_finished()) {
        fail("finished_send");
        return;
      }
      last_send_ms_ = now;
    } else {
      fail("speaker_failed");
      return;
    }
  } else if (phase_ == Phase::FinishedPendingAck &&
             now - last_send_ms_ >= kFinishedRetryMs) {
    if (finished_retries_ >= kMaximumFinishedRetries ||
        !send_finished()) {
      fail("finished_timeout");
      return;
    }
    ++finished_retries_;
    last_send_ms_ = now;
  }
}

bool CodexLanPlayback::active() const {
  return phase_ != Phase::Idle;
}

bool CodexLanPlayback::sleep_blocked() const {
  return active() || speaker_->busy();
}

bool CodexLanPlayback::open_socket(
    const KeyboardWifiServiceSnapshot& snapshot) {
  (void)snapshot;
  socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (socket_ < 0) {
    return false;
  }
  const int flags = fcntl(socket_, F_GETFL, 0);
  if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
    close(socket_);
    socket_ = -1;
    return false;
  }
  return true;
}

bool CodexLanPlayback::send_request() {
  std::array<std::uint8_t, easy_codex::kPlaybackRequestBytes> packet{};
  if (!easy_codex::encode_playback_request(
          request_, key_, packet.data(), packet.size())) {
    return false;
  }
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(host_port_);
  destination.sin_addr.s_addr = host_ipv4_;
  return sendto(socket_,
                packet.data(),
                packet.size(),
                0,
                reinterpret_cast<sockaddr*>(&destination),
                sizeof(destination)) == static_cast<int>(packet.size());
}

bool CodexLanPlayback::send_ack(std::uint8_t status) {
  std::array<std::uint8_t, easy_codex::kPlaybackAckBytes> packet{};
  const easy_codex::PlaybackWireAck ack{
      begin_.identity,
      status,
      status == 3U && failure_diagnostic_ != 0U
          ? UINT32_C(0xEC000000) | failure_diagnostic_
          : static_cast<std::uint32_t>(received_bytes_),
  };
  if (!easy_codex::encode_playback_ack(
          ack, key_, packet.data(), packet.size())) {
    return false;
  }
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(host_port_);
  destination.sin_addr.s_addr = host_ipv4_;
  return sendto(socket_, packet.data(), packet.size(), 0,
                reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) ==
         static_cast<int>(packet.size());
}

bool CodexLanPlayback::send_finished() {
  std::array<std::uint8_t, easy_codex::kPlaybackFinishedBytes> packet{};
  if (!easy_codex::encode_playback_finished(
          {begin_.identity, begin_.total_samples},
          key_, packet.data(), packet.size())) {
    return false;
  }
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(host_port_);
  destination.sin_addr.s_addr = host_ipv4_;
  return sendto(socket_, packet.data(), packet.size(), 0,
                reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) ==
         static_cast<int>(packet.size());
}

void CodexLanPlayback::receive_packets() {
  std::array<std::uint8_t,
             easy_codex::kPlaybackDataHeaderBytes +
                 easy_codex::kPlaybackChunkBytes +
                 easy_codex::kPlaybackAuthTagBytes>
      packet{};
  // Drain one complete Host send window per main-loop pass. The production
  // lwIP UDP mailbox holds six datagrams, so the Host window and this bound
  // remain aligned without overflowing the socket queue before this task runs.
  for (std::size_t attempt = 0U; attempt < 6U; ++attempt) {
    sockaddr_in source{};
    socklen_t source_length = sizeof(source);
    const int received = recvfrom(
        socket_, packet.data(), packet.size(), 0,
        reinterpret_cast<sockaddr*>(&source), &source_length);
    if (received < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        fail("socket_receive");
      }
      return;
    }
    if (!source_is_host(source.sin_addr.s_addr)) {
      continue;
    }
    if (static_cast<std::size_t>(received) >= 4U &&
        std::memcmp(packet.data(), "EIPB", 4U) == 0) {
      handle_begin(packet.data(), static_cast<std::size_t>(received));
    } else if (static_cast<std::size_t>(received) >= 4U &&
               std::memcmp(packet.data(), "EIPD", 4U) == 0) {
      handle_data(packet.data(), static_cast<std::size_t>(received));
    } else if (static_cast<std::size_t>(received) >= 4U &&
               std::memcmp(packet.data(), "EIPK", 4U) == 0) {
      handle_finished_ack(packet.data(), static_cast<std::size_t>(received));
    }
    if (phase_ == Phase::Idle || phase_ == Phase::Cancelling) {
      return;
    }
  }
}

void CodexLanPlayback::handle_begin(
    const std::uint8_t* packet,
    std::size_t length) {
  easy_codex::PlaybackWireBegin decoded{};
  if (!easy_codex::decode_playback_begin(
          packet, length, key_, &decoded) ||
      decoded.identity.slot != request_.slot ||
      decoded.identity.request_generation != request_.request_generation ||
      decoded.identity.connection_generation != request_.connection_generation ||
      decoded.request_nonce != request_.nonce) {
    return;
  }
  if ((phase_ == Phase::Receiving || phase_ == Phase::Playing) &&
      easy_codex::playback_wire_identity_equal(decoded.identity, begin_.identity) &&
      decoded.total_bytes == begin_.total_bytes &&
      decoded.total_samples == begin_.total_samples &&
      decoded.request_nonce == begin_.request_nonce) {
    send_ack(0U);
    return;
  }
  if (phase_ != Phase::AwaitBegin) {
    return;
  }
  const auto total_frames =
      (decoded.total_bytes + decoded.chunk_bytes - 1U) / decoded.chunk_bytes;
  begin_ = decoded;
  const easy_codex::PlaybackBegin state_begin{
      {decoded.identity.slot,
       decoded.identity.summary_generation,
       decoded.identity.lease,
       decoded.identity.connection_generation},
      decoded.identity.request_generation,
      total_frames,
      decoded.total_samples,
  };
  if (slots_->begin_playback(state_begin) !=
      easy_codex::PlaybackBeginResult::Accepted) {
    begin_ = {};
    return;
  }
  encoded_ = static_cast<std::uint8_t*>(heap_caps_malloc(
      decoded.total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (encoded_ == nullptr) {
    slots_->abort_playback(slot_identity());
    fail("psram_allocate");
    return;
  }
  received_bytes_ = 0U;
  available_bytes_.store(0U, std::memory_order_release);
  stream_cancelled_.store(false, std::memory_order_release);
  phase_ = Phase::Receiving;
  last_send_ms_ = millis();
  if (!send_ack(0U)) {
    fail("begin_ack");
  }
}

void CodexLanPlayback::handle_data(
    const std::uint8_t* packet,
    std::size_t length) {
  easy_codex::PlaybackWireData data{};
  if (!easy_codex::decode_playback_data(
          packet,
          length,
          begin_.request_nonce,
          key_,
          decrypted_chunk_.data(),
          decrypted_chunk_.size(),
          &data) ||
      !easy_codex::playback_wire_identity_equal(data.identity, begin_.identity)) {
    return;
  }
  if (data.offset < received_bytes_) {
    const bool replay_phase =
        phase_ == Phase::Receiving || phase_ == Phase::Playing ||
        phase_ == Phase::FinishedPendingAck;
    if (replay_phase && easy_codex::playback_data_matches_received_prefix(
                            data, encoded_, received_bytes_)) {
      send_ack(0U);
    }
    return;
  }
  if (phase_ != Phase::Receiving && phase_ != Phase::Playing) {
    return;
  }
  if (data.offset != received_bytes_ ||
      data.payload_length >
          static_cast<std::size_t>(begin_.total_bytes) - received_bytes_) {
    send_ack(1U);
    return;
  }
  const auto sequence =
      static_cast<std::uint32_t>(received_bytes_ / begin_.chunk_bytes);
  if (slots_->accept_playback_frame(slot_identity(), sequence) !=
      easy_codex::PlaybackFrameResult::Accepted) {
    send_ack(2U);
    return;
  }
  std::memcpy(encoded_ + received_bytes_, data.payload, data.payload_length);
  received_bytes_ += data.payload_length;
  available_bytes_.store(received_bytes_, std::memory_order_release);
  last_send_ms_ = millis();
  if (!send_ack(0U)) {
    fail("data_ack");
    return;
  }
  if (phase_ == Phase::Receiving &&
      received_bytes_ >=
          std::min<std::size_t>(begin_.total_bytes, kStreamingPrebufferBytes) &&
      !begin_speaker_playback()) {
    fail("speaker_start");
    return;
  }
  if (received_bytes_ == begin_.total_bytes && !mark_transfer_complete()) {
    fail("transfer_complete");
  }
}

void CodexLanPlayback::handle_finished_ack(
    const std::uint8_t* packet,
    std::size_t length) {
  easy_codex::PlaybackWireIdentity identity{};
  std::uint8_t status = 0U;
  if (!easy_codex::decode_playback_finished_ack(
          packet, length, key_, &identity, &status) ||
      !easy_codex::playback_wire_identity_equal(identity, begin_.identity)) {
    return;
  }
  if (phase_ == Phase::Cancelling && status == 1U) {
    cancel_acknowledged_ = true;
    return;
  }
  if (phase_ != Phase::FinishedPendingAck || status != 0U) {
    return;
  }
  if (slots_->acknowledge_finished(slot_identity())) {
    ESP_LOGI(kTag,
             "finished_ack slot=%u generation=%llu bytes=%u samples=%llu",
             static_cast<unsigned>(begin_.identity.slot),
             static_cast<unsigned long long>(begin_.identity.summary_generation),
             static_cast<unsigned>(begin_.total_bytes),
             static_cast<unsigned long long>(begin_.total_samples));
    cleanup(false);
  }
}

bool CodexLanPlayback::begin_speaker_playback() {
  failure_diagnostic_ = 0U;
  if (!speaker_->ready()) {
    const auto result = speaker_->begin(supervisor_task_, audio_io_arbiter_);
    if (result != ESP_OK) {
      failure_diagnostic_ = 1U;
      return false;
    }
  }
  if (!speaker_->request_streaming_asset(
          encoded_,
          begin_.total_bytes,
          begin_.total_samples,
          &CodexLanPlayback::read_streaming_asset,
          this)) {
    failure_diagnostic_ = static_cast<std::uint8_t>(
        10U + static_cast<std::uint8_t>(
                  speaker_->last_request_failure()));
    return false;
  }
  if (!slots_->mark_playback_started(slot_identity())) {
    failure_diagnostic_ = 2U;
    return false;
  }
  phase_ = Phase::Playing;
  ESP_LOGI(kTag,
           "streaming slot=%u generation=%llu buffered=%u bytes=%u samples=%llu",
           static_cast<unsigned>(begin_.identity.slot),
           static_cast<unsigned long long>(begin_.identity.summary_generation),
           static_cast<unsigned>(received_bytes_),
           static_cast<unsigned>(begin_.total_bytes),
           static_cast<unsigned long long>(begin_.total_samples));
  return true;
}

bool CodexLanPlayback::mark_transfer_complete() {
  const auto frame_count =
      (begin_.total_bytes + begin_.chunk_bytes - 1U) / begin_.chunk_bytes;
  if (frame_count == 0U ||
      !slots_->mark_playback_transfer_complete(
          slot_identity(), frame_count - 1U)) {
    failure_diagnostic_ = 3U;
    speaker_->poll(false);
    return false;
  }
  ESP_LOGI(kTag,
           "transfer complete slot=%u generation=%llu bytes=%u",
           static_cast<unsigned>(begin_.identity.slot),
           static_cast<unsigned long long>(begin_.identity.summary_generation),
           static_cast<unsigned>(begin_.total_bytes));
  return true;
}

speaker_assets::SoundAssetReadResult CodexLanPlayback::read_streaming_asset(
    void* context,
    std::uint32_t offset,
    std::uint8_t* destination,
    std::size_t length) {
  auto* playback = static_cast<CodexLanPlayback*>(context);
  if (playback == nullptr || destination == nullptr || length == 0U ||
      offset > playback->begin_.total_bytes ||
      length > static_cast<std::size_t>(playback->begin_.total_bytes - offset)) {
    return speaker_assets::SoundAssetReadResult::InvalidArgument;
  }
  const auto end = static_cast<std::size_t>(offset) + length;
  while (playback->available_bytes_.load(std::memory_order_acquire) < end) {
    if (playback->stream_cancelled_.load(std::memory_order_acquire)) {
      return speaker_assets::SoundAssetReadResult::IoError;
    }
    vTaskDelay(1U);
  }
  std::memcpy(destination, playback->encoded_ + offset, length);
  return speaker_assets::SoundAssetReadResult::Ok;
}

void CodexLanPlayback::fail(const char* reason) {
  last_failure_ = reason == nullptr ? "failed" : reason;
  if (failure_diagnostic_ == 0U) {
    if (std::strcmp(last_failure_, "socket_receive") == 0) {
      failure_diagnostic_ = 20U;
    } else if (std::strcmp(last_failure_, "begin_timeout") == 0) {
      failure_diagnostic_ = 21U;
    } else if (std::strcmp(last_failure_, "data_timeout") == 0) {
      failure_diagnostic_ = 22U;
    } else if (std::strcmp(last_failure_, "begin_ack") == 0) {
      failure_diagnostic_ = 23U;
    } else if (std::strcmp(last_failure_, "data_ack") == 0) {
      failure_diagnostic_ = 24U;
    } else if (std::strcmp(last_failure_, "psram_allocate") == 0) {
      failure_diagnostic_ = 25U;
    } else if (std::strcmp(last_failure_, "speaker_failed") == 0) {
      failure_diagnostic_ = 26U;
    } else if (std::strcmp(last_failure_, "finished_send") == 0) {
      failure_diagnostic_ = 27U;
    } else if (std::strcmp(last_failure_, "finished_timeout") == 0) {
      failure_diagnostic_ = 28U;
    } else {
      failure_diagnostic_ = 29U;
    }
  }
  ESP_LOGW(kTag, "failed phase=%u reason=%s",
           static_cast<unsigned>(phase_), last_failure_);
  if (easy_codex::valid_playback_identity(slot_identity())) {
    slots_->abort_playback(slot_identity());
  }
  stream_cancelled_.store(true, std::memory_order_release);
  phase_ = Phase::Cancelling;
  cancel_retries_ = 0U;
  cancel_acknowledged_ = false;
  last_send_ms_ = millis();
  if (easy_codex::valid_playback_identity(slot_identity()) && send_ack(3U)) {
    ++cancel_retries_;
  }
  speaker_->poll(false);
}

void CodexLanPlayback::cleanup(bool abort_slot) {
  stream_cancelled_.store(true, std::memory_order_release);
  if (socket_ >= 0) {
    close(socket_);
    socket_ = -1;
  }
  if (abort_slot && easy_codex::valid_playback_identity(slot_identity())) {
    slots_->abort_playback(slot_identity());
  }
  if (encoded_ != nullptr) {
    std::memset(encoded_, 0, begin_.total_bytes);
    heap_caps_free(encoded_);
    encoded_ = nullptr;
  }
  if (wifi_lease_.valid()) {
    audio_->release_wifi_service_lease(wifi_lease_);
    wifi_lease_ = {};
  }
  key_.fill(0U);
  host_ipv4_ = 0U;
  host_port_ = 0U;
  request_ = {};
  begin_ = {};
  received_bytes_ = 0U;
  available_bytes_.store(0U, std::memory_order_release);
  request_retries_ = 0U;
  finished_retries_ = 0U;
  cancel_retries_ = 0U;
  cancel_acknowledged_ = false;
  failure_diagnostic_ = 0U;
  phase_ = Phase::Idle;
}

bool CodexLanPlayback::source_is_host(std::uint32_t address) const {
  return address == host_ipv4_;
}

easy_codex::PlaybackIdentity CodexLanPlayback::slot_identity() const {
  return {
      begin_.identity.slot,
      begin_.identity.summary_generation,
      begin_.identity.lease,
      begin_.identity.connection_generation,
  };
}

}  // namespace easy_input
