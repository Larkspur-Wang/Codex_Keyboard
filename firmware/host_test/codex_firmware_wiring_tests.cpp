#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_file(const std::string& path) {
  std::ifstream stream(path);
  assert(stream.good());
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

void route_lifetime_is_fail_closed_before_physical_input() {
  const auto source = read_file(
      std::string(EASY_INPUT_REPO_ROOT) + "/main/app_main.cpp");
  assert(source.find(
             "snapshot.configured && snapshot.connected &&") !=
         std::string::npos);
  assert(source.find("!snapshot.disconnect_pending") != std::string::npos);
  assert(source.find("snapshot.host_ipv4_valid") != std::string::npos);
  assert(source.find(
             "app->codex_slots.disconnect(app->codex_route_generation)") !=
         std::string::npos);

  const auto reconcile = source.find(
      "reconcile_codex_route_lifetime(&app);");
  const auto input_poll = source.find(
      "app.inputs.poll(millis(), handle_input_event, &app);");
  assert(reconcile != std::string::npos);
  assert(input_poll != std::string::npos);
  assert(reconcile < input_poll);
}

void normal_slot_input_uses_only_the_reconciled_generation() {
  const auto source = read_file(
      std::string(EASY_INPUT_REPO_ROOT) + "/main/app_main.cpp");
  assert(source.find(
             "event.input, event.phase, app->codex_route_generation") !=
         std::string::npos);
  assert(source.find(
             "app->audio.wifi_service_snapshot().generation") ==
         std::string::npos);

  const auto platform = source.find(
      "app->platform_selection.handle_event(event.input, event.phase, now)");
  const auto codex = source.find(
      "app->codex_slots.handle_input(");
  assert(platform != std::string::npos);
  assert(codex != std::string::npos);
  assert(platform < codex);
}

void ptt_release_drains_tail_then_emits_an_authenticated_terminal() {
  const auto app = read_file(
      std::string(EASY_INPUT_REPO_ROOT) + "/main/app_main.cpp");
  const auto audio = read_file(
      std::string(EASY_INPUT_REPO_ROOT) +
      "/main/platform/keyboard_audio.cpp");
  assert(app.find("app->audio.start_stream(\"codex_slot\", session_id)") !=
         std::string::npos);
  assert(app.find("app->audio.stop_stream(session_id)") !=
         std::string::npos);
  assert(audio.find("lifecycle_stop_reason == \"client_stop\"") !=
         std::string::npos);
  assert(audio.find("pending_frame.capture_sequence != sent_packets") !=
         std::string::npos);
  assert(audio.find("capture_drained = true") != std::string::npos);
  assert(audio.find("clean_capture_end && capture_drained && sent_packets > 0U") !=
         std::string::npos);
  assert(audio.find("encode_audio_end_header") != std::string::npos);
  assert(audio.find("mbedtls_md_hmac") != std::string::npos);
}

void lower_row_playback_uses_authenticated_lan_psram_and_speaker_path() {
  const auto app = read_file(
      std::string(EASY_INPUT_REPO_ROOT) + "/main/app_main.cpp");
  const auto playback = read_file(
      std::string(EASY_INPUT_REPO_ROOT) +
      "/main/platform/codex_lan_playback.cpp");
  const auto wire = read_file(
      std::string(EASY_INPUT_REPO_ROOT) +
      "/components/keyboard/src/codex_playback_wire.cpp");
  assert(app.find("app->codex_playback.request(") != std::string::npos);
  assert(app.find("app->codex_playback.preempt(") != std::string::npos);
  assert(app.find("app.codex_playback.poll()") != std::string::npos);
  assert(app.find("app->codex_playback.sleep_blocked()") !=
         std::string::npos);
  assert(app.find("app->codex_playback.active()") != std::string::npos);
  assert(playback.find("decode_playback_begin") != std::string::npos);
  assert(playback.find("decoded.request_nonce != request_.nonce") !=
         std::string::npos);
  assert(playback.find("decode_playback_data") != std::string::npos);
  assert(playback.find("phase_ == Phase::Playing") != std::string::npos);
  assert(playback.find("playback_data_matches_received_prefix") !=
         std::string::npos);
  assert(wire.find("mbedtls_gcm_auth_decrypt") != std::string::npos);
  assert(playback.find("send_ack(3U)") != std::string::npos);
  assert(playback.find("deferred_slot_") != std::string::npos);
  assert(playback.find("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT") !=
         std::string::npos);
  assert(playback.find("request_streaming_asset(") !=
         std::string::npos);
  assert(playback.find("kStreamingPrebufferBytes = 24U * 1024U") !=
         std::string::npos);
  assert(playback.find("available_bytes_.store(received_bytes_") !=
         std::string::npos);
  assert(playback.find("read_streaming_asset") !=
         std::string::npos);
  assert(playback.find("mark_transfer_complete()") !=
         std::string::npos);
  assert(playback.find("send_finished()") != std::string::npos);
  assert(playback.find("acknowledge_finished") != std::string::npos);
  const auto fail_begin = playback.find("void CodexLanPlayback::fail(");
  const auto cleanup_begin = playback.find("void CodexLanPlayback::cleanup(");
  assert(fail_begin != std::string::npos);
  assert(cleanup_begin != std::string::npos);
  assert(fail_begin < cleanup_begin);
  const auto fail_section = playback.substr(
      fail_begin, cleanup_begin - fail_begin);
  assert(fail_section.find("send_ack(3U)") != std::string::npos);
  assert(fail_section.find("cleanup(false)") == std::string::npos);
  assert(playback.find("kReceiveTimeoutMs = 30000U") !=
         std::string::npos);
  assert(fail_section.find("data_timeout") != std::string::npos);
  assert(fail_section.find("failure_diagnostic_ = 22U") !=
         std::string::npos);
}

void mailbox_status_is_authenticated_and_restored_as_led_background() {
  const auto app = read_file(
      std::string(EASY_INPUT_REPO_ROOT) + "/main/app_main.cpp");
  const auto audio = read_file(
      std::string(EASY_INPUT_REPO_ROOT) +
      "/main/platform/keyboard_audio.cpp");
  const auto leds = read_file(
      std::string(EASY_INPUT_REPO_ROOT) +
      "/main/platform/led_strip_status.cpp");
  assert(audio.find("decode_mailbox_status") != std::string::npos);
  assert(audio.find("from.sin_addr.s_addr == dest.sin_addr.s_addr") !=
         std::string::npos);
  assert(audio.find("mailbox.heartbeat_sequence == heartbeat_seq - 1U") !=
         std::string::npos);
  assert(app.find("apply_pending_mailbox_status(&app, millis())") !=
         std::string::npos);
  assert(app.find("status.unread_slots, status.coverage_by_slot, "
                  "status.running_tasks") != std::string::npos);
  assert(leds.find("mailbox_frame_for_slots") != std::string::npos);
  const auto mailbox_background = leds.find("if (mailbox_status_active_)");
  const auto agent_background = leds.find("if (agent_status_valid(now_ms))");
  assert(mailbox_background != std::string::npos);
  assert(agent_background != std::string::npos);
  assert(mailbox_background < agent_background);
}

void encoder_controls_board_speaker_and_press_uses_embedded_prompt() {
  const auto app = read_file(
      std::string(EASY_INPUT_REPO_ROOT) + "/main/app_main.cpp");
  const auto speaker = read_file(
      std::string(EASY_INPUT_REPO_ROOT) + "/main/platform/speaker_output.cpp");

  assert(app.find("adjust_speaker_volume_for_wired_encoder_step") !=
         std::string::npos);
  assert(app.find("clockwise_up") != std::string::npos);
  assert(app.find("counter_clockwise_down") != std::string::npos);
  assert(app.find("speaker.set_volume_level(adjusted)") != std::string::npos);
  assert(app.find("save_speaker_volume") != std::string::npos);
  assert(app.find("speaker_assets::volume_prompt") != std::string::npos);
  assert(app.find("speaker.begin(app->platform_task, &app->audio_io_arbiter)") !=
         std::string::npos);
  assert(app.find("request_embedded_asset") != std::string::npos);
  assert(app.find("queue_consumer_tap_for_epoch") == std::string::npos);
  assert(app.find("send_consumer_tap_for_owner") == std::string::npos);
  assert(speaker.find("scale_speaker_sample") != std::string::npos);
}

}  // namespace

int main() {
  route_lifetime_is_fail_closed_before_physical_input();
  normal_slot_input_uses_only_the_reconciled_generation();
  ptt_release_drains_tail_then_emits_an_authenticated_terminal();
  lower_row_playback_uses_authenticated_lan_psram_and_speaker_path();
  mailbox_status_is_authenticated_and_restored_as_led_background();
  encoder_controls_board_speaker_and_press_uses_embedded_prompt();
  return 0;
}
