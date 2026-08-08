#include "speaker_assets/diagnostic_link_anchor.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/esp_sound_bank_storage.h"
#include "speaker_assets/speaker_assets_flash_runner.h"
#include "speaker_assets/speaker_assets_protocol.h"
#include "speaker_assets/speaker_assets_runtime.h"
#include "speaker_assets/speaker_assets_session.h"
#include "speaker_assets/speaker_assets_store_executor.h"
#include "speaker_assets/sound_asset_crypto.h"
#include "speaker_assets/sound_asset_format.h"
#include "speaker_assets/sound_asset_store.h"

namespace easy_input::speaker_assets {
namespace {

class DiagnosticRunnerSynchronization final
    : public SpeakerAssetsFlashRunnerSynchronization {
 public:
  void lock() override {}
  void unlock() override {}
  void notify_worker() override {}
  void notify_supervisor() override {}
  void wait_worker() override {}
};

// This function is deliberately retained but never called. Its relocations
// force the diagnostic ELF to resolve every public storage layer against the
// real ESP-IDF adapter, closing the gap where --gc-sections could otherwise
// discard an unreferenced component after it compiled successfully.
void never_call_storage_link_probe() {
  EspSoundBankStorage storage;
  DiagnosticRunnerSynchronization runner_synchronization;
  SpeakerAssetsCooperativeStoreRunner runner(
      storage, runner_synchronization);
  SoundAssetStore store(storage);
  std::array<std::uint8_t, 1> byte{};
  SoundBankSnapshot snapshot{};
  SoundManifestSummary summary{};
  SoundSha256Digest manifest_digest{};
  SoundSha256Digest bundle_digest{};
  SoundBundlePlan plan{};
  SoundUpdateIdentity identity{};
  SoundUpdateProgress progress{};
  SoundTransactionOutcome outcome{};
  SoundReadLease lease{};
  SpeakerAssetsFrame frame{};
  SpeakerAssetsRouteToken route{
      SpeakerAssetsTransport::Usb, 0U, 1U};
  SpeakerAssetsPlanAssembler plan_assembler;
  SpeakerAssetsBlockAssembler block_assembler;
  SpeakerAssetsSession session;
  SpeakerAssetsRuntimeMailbox runtime_mailbox;
  SpeakerAssetsRuntimeMailboxRecord runtime_mailbox_record{};
  SpeakerAssetsRuntimeCore runtime;
  SpeakerAssetsRuntimeReply runtime_reply{};
  SpeakerAssetsLogicalRequestLease logical_lease{};
  SpeakerAssetsEmission emission{};
  SpeakerAssetsActionCompletion completion{};
  std::array<std::uint8_t, kSpeakerAssetsUsbFrameBytes> usb_frame{};
  std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes> wifi_frame{};
  std::array<std::uint8_t, kSpeakerAssetsPlanWireBytes> plan_wire{};
  std::size_t encoded_length = 0U;

  runner.publish_priority_allowed(true);
  static_cast<void>(runner.priority_allowed());
  static_cast<void>(runner.priority_epoch());
  static_cast<void>(runner.step(emission.action, &completion));
  static_cast<void>(runner.worker_run_once());
  SpeakerAssetsFlashPermit requested_permit{};
  static_cast<void>(runner.requested_permit(&requested_permit));
  static_cast<void>(runner.job_active());

  static_cast<void>(storage.open());
  static_cast<void>(storage.is_open());
  static_cast<void>(
      storage.read(SoundBankId::A, 0U, byte.data(), byte.size()));
  static_cast<void>(
      storage.write(SoundBankId::A, 0U, byte.data(), byte.size()));
  static_cast<void>(
      storage.erase(SoundBankId::A, 0U, kSoundSectorSize));

  static_cast<void>(store.scan());
  static_cast<void>(store.selection());
  static_cast<void>(store.begin_or_resume_update(plan, &identity));
  static_cast<void>(store.begin_or_resume_update(plan, &identity));
  static_cast<void>(
      store.resume_update(identity.transaction_id, &identity));
  static_cast<void>(store.query_transaction_outcome(
      identity.transaction_id, &outcome));
  static_cast<void>(store.discard_invalid_staging(plan));
  static_cast<void>(store.write_manifest(byte.data(), byte.size()));
  static_cast<void>(
      store.write_payload_block(0U, byte.data(), byte.size()));
  static_cast<void>(store.commit_update());
  static_cast<void>(store.abort_update());
  static_cast<void>(store.acquire_active_read(&lease));
  static_cast<void>(store.release_read(lease));
  static_cast<void>(store.update_active());
  static_cast<void>(store.update_bank());
  static_cast<void>(store.update_generation());
  static_cast<void>(store.update_progress(&progress));

  static_cast<void>(
      encode_speaker_assets_usb_frame(frame, &usb_frame));
  static_cast<void>(decode_speaker_assets_usb_frame(
      usb_frame.data(), usb_frame.size(), &frame));
  static_cast<void>(encode_speaker_assets_wifi_frame(
      frame, &wifi_frame, &encoded_length));
  static_cast<void>(decode_speaker_assets_wifi_frame(
      wifi_frame.data(), encoded_length, &frame));
  static_cast<void>(
      encode_sound_bundle_plan_wire(plan, &plan_wire));
  static_cast<void>(decode_sound_bundle_plan_wire(
      plan_wire.data(), plan_wire.size(), &plan));
  static_cast<void>(speaker_assets_route_equal(route, route));
  static_cast<void>(runtime_mailbox.enqueue_route_opened(route));
  static_cast<void>(
      runtime_mailbox.take_next(&runtime_mailbox_record));
  static_cast<void>(
      runtime.import_mailbox_record(runtime_mailbox_record));
  const auto open_step = runtime.step(0U, false, nullptr);
  SpeakerAssetsRuntimeStepResult runtime_step_result{};
  SpeakerAssetsLogicalRequestLease runtime_step_release{};
  static_cast<void>(open_step.inspect(
      &runtime_step_result, &runtime_step_release));

  static_cast<void>(runtime_mailbox.enqueue_usb_frame(
      route, usb_frame.data(), usb_frame.size(), 0U));
  static_cast<void>(
      runtime_mailbox.logical_request_lease(&logical_lease));
  static_cast<void>(
      runtime_mailbox.take_next(&runtime_mailbox_record));
  static_cast<void>(
      runtime.import_mailbox_record(runtime_mailbox_record));
  const auto data_step = runtime.step(0U, false, nullptr);
  static_cast<void>(data_step.inspect(
      &runtime_step_result, &runtime_step_release));
  static_cast<void>(runtime_mailbox.release_logical_request(
      runtime_step_release));

  static_cast<void>(runtime.front_reply(&runtime_reply));
  static_cast<void>(
      runtime.front_reply_for_route(route, &runtime_reply));
  static_cast<void>(
      runtime.pop_reply_if_sequence(runtime_reply.sequence));
  static_cast<void>(
      runtime.remove_reply_if_sequence(runtime_reply.sequence));
  static_cast<void>(
      runtime_mailbox.release_logical_request(logical_lease));
  static_cast<void>(runtime_mailbox.enqueue_route_closed(route));
  static_cast<void>(
      runtime_mailbox.take_next(&runtime_mailbox_record));
  static_cast<void>(
      runtime.import_mailbox_record(runtime_mailbox_record));
  const auto close_step = runtime.step(0U, false, nullptr);
  static_cast<void>(close_step.inspect(
      &runtime_step_result, &runtime_step_release));
  static_cast<void>(runtime_mailbox.data_size());
  static_cast<void>(runtime_mailbox.close_size());
  static_cast<void>(runtime_mailbox.raw_data_credit());
  static_cast<void>(
      runtime_mailbox.logical_request_reserved_for(route));
  runtime_mailbox.clear();
  static_cast<void>(runtime.ingress_size());
  static_cast<void>(runtime.lifecycle_size());
  static_cast<void>(runtime.reply_size());
  static_cast<void>(runtime.active_route_count());
  static_cast<void>(runtime.raw_ingress_credit());
  static_cast<void>(
      runtime.advertised_ingress_credit(route, false));
  static_cast<void>(runtime.action_pending());
  static_cast<void>(runtime.session_phase());
  static_cast<void>(plan_assembler.begin(route, 1U));
  static_cast<void>(plan_assembler.accept_fragment(
      route, 1U, 0U, byte.data(), byte.size()));
  static_cast<void>(plan_assembler.decode(&plan));
  static_cast<void>(plan_assembler.first_missing_offset());
  static_cast<void>(plan_assembler.copy_received_bitmap(
      0U, byte.data(), byte.size()));
  static_cast<void>(block_assembler.begin(
      SpeakerAssetsRegion::Manifest, 0U, 1U));
  static_cast<void>(block_assembler.accept_fragment(
      SpeakerAssetsRegion::Manifest,
      0U,
      0U,
      byte.data(),
      byte.size()));
  static_cast<void>(block_assembler.first_missing_offset());
  static_cast<void>(block_assembler.copy_received_bitmap(
      0U, byte.data(), byte.size()));
  route.transport = SpeakerAssetsTransport::Wifi;
  frame.request_id = 1U;
  static_cast<void>(session.consume(route, frame, &emission));
  static_cast<void>(session.complete(completion, &emission));
  static_cast<void>(session.expire_partial(route));
  session.route_closed(route);
  session.all_routes_closed();
  static_cast<void>(session.phase());
  static_cast<void>(session.route_bound());
  static_cast<void>(session.route());
  static_cast<void>(session.session_cookie());
  static_cast<void>(session.action_pending());
  static_cast<void>(session.pending_action_token());
  static_cast<void>(session.progress());
  static_cast<void>(session.replay_entry_count());
  static_cast<void>(
      speaker_assets_status_from_store_result(SoundStoreResult::Ok));
  static_cast<void>(
      execute_speaker_assets_store_action(
          store, emission.action, &completion));

  static_cast<void>(
      validate_sound_bank(storage, SoundBankId::A, &snapshot));
  static_cast<void>(select_sound_banks(snapshot, snapshot));
  static_cast<void>(validate_sound_manifest(
      storage, SoundBankId::A, 32U, 0U, &summary));
  static_cast<void>(calculate_sound_bank_digests(
      storage,
      SoundBankId::A,
      32U,
      0U,
      &manifest_digest,
      &bundle_digest));

  SoundSha256 hash;
  static_cast<void>(hash.update(byte.data(), byte.size()));
  static_cast<void>(hash.finish());
  static_cast<void>(
      sound_crc32_iso_hdlc(byte.data(), byte.size()));
  static_cast<void>(
      sound_digest_equal(manifest_digest, bundle_digest));
}

using DiagnosticLinkProbe = void (*)();
const std::array<DiagnosticLinkProbe, 1> kDiagnosticLinkProbes{{
    &never_call_storage_link_probe,
}};

}  // namespace

const void* diagnostic_link_probe_table() {
  return static_cast<const void*>(kDiagnosticLinkProbes.data());
}

}  // namespace easy_input::speaker_assets

extern "C" const void*
easy_input_speaker_assets_diagnostic_link_anchor() {
  return easy_input::speaker_assets::diagnostic_link_probe_table();
}
