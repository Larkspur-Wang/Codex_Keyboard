#include <cassert>

#include "speaker_assets/speaker_assets_wifi_policy.h"

namespace {

easy_input::speaker_assets::SpeakerAssetsWifiPolicyInputs
ready_inputs() {
  return {
      true,
      true,
      false,
      false,
      true,
      true,
      false,
      true,
      true,
      7U,
      11U,
  };
}

void usb_preference_blocks_only_new_routes() {
  auto inputs = ready_inputs();
  assert(
      easy_input::speaker_assets::
          speaker_assets_wifi_allow_new_route(inputs));

  inputs.usb_preferred = true;
  assert(
      !easy_input::speaker_assets::
           speaker_assets_wifi_allow_new_route(inputs));

  inputs.route_active = true;
  assert(
      easy_input::speaker_assets::
          speaker_assets_wifi_keep_active_route(inputs));
}

void active_route_requires_exact_network_lifetime() {
  auto inputs = ready_inputs();
  inputs.route_active = true;
  assert(
      easy_input::speaker_assets::
          speaker_assets_wifi_keep_active_route(inputs));

  inputs.connected = false;
  assert(
      !easy_input::speaker_assets::
           speaker_assets_wifi_keep_active_route(inputs));
  inputs.connected = true;
  inputs.disconnect_pending = true;
  assert(
      !easy_input::speaker_assets::
           speaker_assets_wifi_keep_active_route(inputs));
  inputs.disconnect_pending = false;
  inputs.key_epoch = 0U;
  assert(
      !easy_input::speaker_assets::
           speaker_assets_wifi_keep_active_route(inputs));
}

void active_route_survives_listener_epoch_consumption() {
  auto inputs = ready_inputs();
  // accept() consumes discovery Ready before authentication/session work.
  // The connected route owns its client socket and exact network generation;
  // it must not depend on the now-retired listener's Ready bit.
  inputs.listener_ready = false;
  inputs.route_active = true;
  assert(
      easy_input::speaker_assets::
          speaker_assets_wifi_keep_active_route(inputs));
}

void listener_is_bound_to_one_exact_network_generation() {
  using easy_input::speaker_assets::
      speaker_assets_wifi_listener_matches_generation;

  assert(speaker_assets_wifi_listener_matches_generation(
      11U, 11U));
  // A direct g1 -> g2 transition must rebuild even if no poll observed the
  // disconnected interval between the two ready snapshots.
  assert(!speaker_assets_wifi_listener_matches_generation(
      11U, 12U));
  assert(!speaker_assets_wifi_listener_matches_generation(
      0U, 12U));
  assert(!speaker_assets_wifi_listener_matches_generation(
      12U, 0U));
}

void accept_retries_only_transient_errors() {
  using easy_input::speaker_assets::SpeakerAssetsWifiAcceptIo;
  using easy_input::speaker_assets::
      classify_speaker_assets_wifi_accept_io;

  assert(classify_speaker_assets_wifi_accept_io(0, false) ==
         SpeakerAssetsWifiAcceptIo::Accepted);
  assert(classify_speaker_assets_wifi_accept_io(7, false) ==
         SpeakerAssetsWifiAcceptIo::Accepted);
  assert(classify_speaker_assets_wifi_accept_io(-1, true) ==
         SpeakerAssetsWifiAcceptIo::Retry);
  assert(classify_speaker_assets_wifi_accept_io(-1, false) ==
         SpeakerAssetsWifiAcceptIo::RebuildListener);
}

void non_consuming_peer_probe_distinguishes_idle_from_closed() {
  using easy_input::speaker_assets::SpeakerAssetsWifiPeerProbe;
  using easy_input::speaker_assets::
      classify_speaker_assets_wifi_peer_probe;

  assert(classify_speaker_assets_wifi_peer_probe(1, false) ==
         SpeakerAssetsWifiPeerProbe::Open);
  assert(classify_speaker_assets_wifi_peer_probe(-1, true) ==
         SpeakerAssetsWifiPeerProbe::Open);
  assert(classify_speaker_assets_wifi_peer_probe(0, false) ==
         SpeakerAssetsWifiPeerProbe::Closed);
  assert(classify_speaker_assets_wifi_peer_probe(-1, false) ==
         SpeakerAssetsWifiPeerProbe::Closed);
}

void exact_socket_io_retries_transient_errors() {
  using easy_input::speaker_assets::SpeakerAssetsWifiSocketIo;
  using easy_input::speaker_assets::
      classify_speaker_assets_wifi_socket_io;

  assert(classify_speaker_assets_wifi_socket_io(17, false) ==
         SpeakerAssetsWifiSocketIo::Progress);
  assert(classify_speaker_assets_wifi_socket_io(-1, true) ==
         SpeakerAssetsWifiSocketIo::Retry);
  assert(classify_speaker_assets_wifi_socket_io(0, false) ==
         SpeakerAssetsWifiSocketIo::PeerClosed);
  assert(classify_speaker_assets_wifi_socket_io(-1, false) ==
         SpeakerAssetsWifiSocketIo::Fatal);
}

void response_watchdog_renews_only_on_store_progress() {
  using easy_input::speaker_assets::
      observe_speaker_assets_wifi_response_progress;
  using easy_input::speaker_assets::
      speaker_assets_wifi_response_watchdog_expired;
  using easy_input::speaker_assets::
      start_speaker_assets_wifi_response_watchdog;

  auto watchdog =
      start_speaker_assets_wifi_response_watchdog(
          1000U, 240000U, 7U);
  assert(!speaker_assets_wifi_response_watchdog_expired(
      watchdog, 240999U));
  observe_speaker_assets_wifi_response_progress(
      &watchdog, 200000U, 240000U, 8U);
  assert(!speaker_assets_wifi_response_watchdog_expired(
      watchdog, 241000U));
  // Seeing the same generation again is not progress and cannot keep a
  // genuinely wedged Store action alive forever.
  observe_speaker_assets_wifi_response_progress(
      &watchdog, 300000U, 240000U, 8U);
  assert(speaker_assets_wifi_response_watchdog_expired(
      watchdog, 440000U));
}

}  // namespace

int main() {
  usb_preference_blocks_only_new_routes();
  active_route_requires_exact_network_lifetime();
  active_route_survives_listener_epoch_consumption();
  listener_is_bound_to_one_exact_network_generation();
  accept_retries_only_transient_errors();
  non_consuming_peer_probe_distinguishes_idle_from_closed();
  exact_socket_io_retries_transient_errors();
  response_watchdog_renews_only_on_store_progress();
  return 0;
}
