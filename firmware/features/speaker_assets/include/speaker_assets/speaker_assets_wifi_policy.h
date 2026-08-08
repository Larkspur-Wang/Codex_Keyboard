#pragma once

#include <cstdint>

namespace easy_input::speaker_assets {

// Pure lifecycle policy shared by the production carrier and host tests.
// USB preference is intentionally an admission rule only: changing transport
// preference must never tear down an already authenticated transaction.
struct SpeakerAssetsWifiPolicyInputs {
  bool assets_ready = false;
  bool listener_ready = false;
  bool route_active = false;
  bool usb_preferred = false;
  bool configured = false;
  bool connected = false;
  bool disconnect_pending = false;
  bool host_ipv4_valid = false;
  bool key_valid = false;
  std::uint16_t key_epoch = 0U;
  std::uint32_t generation = 0U;
};

constexpr bool speaker_assets_wifi_base_ready(
    const SpeakerAssetsWifiPolicyInputs& inputs) {
  return inputs.assets_ready && inputs.configured &&
         inputs.connected && !inputs.disconnect_pending &&
         inputs.host_ipv4_valid && inputs.key_valid &&
         inputs.key_epoch != 0U && inputs.generation != 0U;
}

constexpr bool speaker_assets_wifi_allow_new_route(
    const SpeakerAssetsWifiPolicyInputs& inputs) {
  return speaker_assets_wifi_base_ready(inputs) &&
         inputs.listener_ready && !inputs.route_active &&
         !inputs.usb_preferred;
}

constexpr bool speaker_assets_wifi_keep_active_route(
    const SpeakerAssetsWifiPolicyInputs& inputs) {
  return speaker_assets_wifi_base_ready(inputs) &&
         inputs.route_active;
}

// A TCP listener belongs to the exact Wi-Fi/IP lifetime in which bind/listen
// succeeded. A fast disconnect -> reconnect may advance the generation before
// the carrier's outer loop ever observes base_ready=false, so socket presence
// alone is not proof that the currently advertised port is reachable.
constexpr bool speaker_assets_wifi_listener_matches_generation(
    std::uint32_t listener_generation,
    std::uint32_t current_generation) {
  return listener_generation != 0U &&
         current_generation != 0U &&
         listener_generation == current_generation;
}

enum class SpeakerAssetsWifiAcceptIo : std::uint8_t {
  Accepted,
  Retry,
  RebuildListener,
};

// accept() returns a non-negative client descriptor on success. Transient
// would-block/interrupted results stay within the accept loop; every permanent
// error revokes discovery Ready and rebuilds the listener.
constexpr SpeakerAssetsWifiAcceptIo
classify_speaker_assets_wifi_accept_io(
    int accept_result,
    bool retryable_error) {
  if (accept_result >= 0) {
    return SpeakerAssetsWifiAcceptIo::Accepted;
  }
  return retryable_error
             ? SpeakerAssetsWifiAcceptIo::Retry
             : SpeakerAssetsWifiAcceptIo::RebuildListener;
}

// Classification for a non-consuming recv(MSG_PEEK | MSG_DONTWAIT) probe.
// Positive means bytes are waiting and must remain untouched for the normal
// record reader. A retryable error (would-block or interrupted syscall) means
// the peer must still be treated as connected. EOF and non-retryable socket
// errors close the route.
enum class SpeakerAssetsWifiPeerProbe : std::uint8_t {
  Open,
  Closed,
};

constexpr SpeakerAssetsWifiPeerProbe
classify_speaker_assets_wifi_peer_probe(
    int receive_result,
    bool retryable_error) {
  if (receive_result > 0 ||
      (receive_result < 0 && retryable_error)) {
    return SpeakerAssetsWifiPeerProbe::Open;
  }
  return SpeakerAssetsWifiPeerProbe::Closed;
}

// Classification shared by the blocking exact-read/write loops and host
// tests. select() readiness is only a hint: lwIP may still report a transient
// EAGAIN/EWOULDBLOCK or EINTR before any byte is transferred. Those outcomes
// must stay inside the bounded deadline instead of tearing down a healthy
// authenticated route.
enum class SpeakerAssetsWifiSocketIo : std::uint8_t {
  Progress,
  Retry,
  PeerClosed,
  Fatal,
};

constexpr SpeakerAssetsWifiSocketIo
classify_speaker_assets_wifi_socket_io(
    int transfer_result,
    bool retryable_error) {
  if (transfer_result > 0) {
    return SpeakerAssetsWifiSocketIo::Progress;
  }
  if (transfer_result == 0) {
    return SpeakerAssetsWifiSocketIo::PeerClosed;
  }
  return retryable_error
             ? SpeakerAssetsWifiSocketIo::Retry
             : SpeakerAssetsWifiSocketIo::Fatal;
}

// Long erase/verify work is split into bounded permitted units. The response
// watchdog therefore measures lack of Store progress instead of absolute wall
// time: every completed unit renews the bounded deadline.
struct SpeakerAssetsWifiResponseWatchdog {
  std::uint32_t deadline_ms = 0U;
  std::uint32_t progress_generation = 0U;
};

constexpr SpeakerAssetsWifiResponseWatchdog
start_speaker_assets_wifi_response_watchdog(
    std::uint32_t now_ms,
    std::uint32_t timeout_ms,
    std::uint32_t progress_generation) {
  return {
      now_ms + timeout_ms,
      progress_generation,
  };
}

constexpr void observe_speaker_assets_wifi_response_progress(
    SpeakerAssetsWifiResponseWatchdog* watchdog,
    std::uint32_t now_ms,
    std::uint32_t timeout_ms,
    std::uint32_t progress_generation) {
  if (watchdog == nullptr ||
      watchdog->progress_generation == progress_generation) {
    return;
  }
  watchdog->progress_generation = progress_generation;
  watchdog->deadline_ms = now_ms + timeout_ms;
}

constexpr bool speaker_assets_wifi_response_watchdog_expired(
    const SpeakerAssetsWifiResponseWatchdog& watchdog,
    std::uint32_t now_ms) {
  return static_cast<std::int32_t>(
             now_ms - watchdog.deadline_ms) >= 0;
}

}  // namespace easy_input::speaker_assets
