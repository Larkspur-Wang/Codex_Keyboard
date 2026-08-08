#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

#include "speaker_assets/speaker_assets_runtime.h"

namespace {

using easy_input::speaker_assets::SoundStoreResult;
using easy_input::speaker_assets::SoundBankId;
using easy_input::speaker_assets::SoundBundlePlan;
using easy_input::speaker_assets::SoundUpdateProgress;
using easy_input::speaker_assets::SpeakerAssetsActionCompletion;
using easy_input::speaker_assets::SpeakerAssetsActionExecutionResult;
using easy_input::speaker_assets::SpeakerAssetsActionKind;
using easy_input::speaker_assets::SpeakerAssetsActionView;
using easy_input::speaker_assets::SpeakerAssetsFrame;
using easy_input::speaker_assets::SpeakerAssetsLogicalRequestLease;
using easy_input::speaker_assets::SpeakerAssetsOpcode;
using easy_input::speaker_assets::SpeakerAssetsProtocolResult;
using easy_input::speaker_assets::SpeakerAssetsRouteToken;
using easy_input::speaker_assets::SpeakerAssetsRuntimeActionExecutor;
using easy_input::speaker_assets::SpeakerAssetsRuntimeCore;
using easy_input::speaker_assets::SpeakerAssetsRuntimeEnqueueResult;
using easy_input::speaker_assets::SpeakerAssetsRuntimeMailbox;
using easy_input::speaker_assets::SpeakerAssetsRuntimeMailboxKind;
using easy_input::speaker_assets::SpeakerAssetsRuntimeMailboxRecord;
using easy_input::speaker_assets::SpeakerAssetsRuntimeReply;
using easy_input::speaker_assets::SpeakerAssetsRuntimeStepResult;
using easy_input::speaker_assets::SpeakerAssetsSessionPhase;
using easy_input::speaker_assets::SpeakerAssetsStatus;
using easy_input::speaker_assets::SpeakerAssetsTransport;

bool g_track_allocations = false;
std::size_t g_allocation_count = 0U;

constexpr std::uint8_t kSingleRequestFlags =
    easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
    easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment |
    easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment;

SpeakerAssetsRouteToken usb_route(std::uint32_t generation) {
  SpeakerAssetsRouteToken route{};
  route.transport = SpeakerAssetsTransport::Usb;
  route.generation = generation;
  return route;
}

SpeakerAssetsRouteToken wifi_route(std::uint32_t connection_id,
                                   std::uint32_t generation) {
  SpeakerAssetsRouteToken route{};
  route.transport = SpeakerAssetsTransport::Wifi;
  route.route_id = connection_id;
  route.generation = generation;
  return route;
}

SpeakerAssetsFrame make_request(SpeakerAssetsOpcode opcode,
                                std::uint32_t request_id) {
  SpeakerAssetsFrame frame{};
  frame.opcode = static_cast<std::uint8_t>(opcode);
  frame.flags = kSingleRequestFlags;
  frame.request_id = request_id;
  return frame;
}

std::array<
    std::uint8_t,
    easy_input::speaker_assets::kSpeakerAssetsUsbFrameBytes>
encode_usb(const SpeakerAssetsFrame& frame) {
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsUsbFrameBytes>
      encoded{};
  assert(
      easy_input::speaker_assets::encode_speaker_assets_usb_frame(
          frame, &encoded) ==
      SpeakerAssetsProtocolResult::Ok);
  return encoded;
}

struct EncodedWifiFrame {
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsWifiFrameMaxBytes>
      bytes{};
  std::size_t length = 0U;
};

EncodedWifiFrame encode_wifi(const SpeakerAssetsFrame& frame) {
  EncodedWifiFrame encoded{};
  assert(
      easy_input::speaker_assets::encode_speaker_assets_wifi_frame(
          frame, &encoded.bytes, &encoded.length) ==
      SpeakerAssetsProtocolResult::Ok);
  assert(
      encoded.length ==
      easy_input::speaker_assets::kSpeakerAssetsFrameHeaderBytes +
          frame.body_length);
  return encoded;
}

void open_route(SpeakerAssetsRuntimeCore& runtime,
                const SpeakerAssetsRouteToken& route,
                std::uint32_t now_ms = 0U) {
  assert(runtime.enqueue_route_opened(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(now_ms, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
}

SpeakerAssetsRuntimeReply pop_front_reply(
    SpeakerAssetsRuntimeCore& runtime) {
  SpeakerAssetsRuntimeReply reply{};
  assert(runtime.front_reply(&reply));
  assert(reply.sequence != 0U);
  assert(runtime.pop_reply_if_sequence(reply.sequence));
  return reply;
}

SpeakerAssetsLogicalRequestLease current_lease(
    const SpeakerAssetsRuntimeMailbox& mailbox) {
  SpeakerAssetsLogicalRequestLease lease{};
  assert(mailbox.logical_request_lease(&lease));
  assert(lease.admission_id != 0U);
  return lease;
}

bool leases_equal(
    const SpeakerAssetsLogicalRequestLease& first,
    const SpeakerAssetsLogicalRequestLease& second) {
  return easy_input::speaker_assets::
      speaker_assets_logical_request_lease_equal(first, second);
}

void assert_status(const SpeakerAssetsRuntimeReply& reply,
                   SpeakerAssetsStatus status) {
  assert(reply.frame.body_length >= 1U);
  assert(reply.frame.body[0] ==
         static_cast<std::uint8_t>(status));
  const bool is_error =
      (reply.frame.flags &
       easy_input::speaker_assets::kSpeakerAssetsFlagError) != 0U;
  assert(is_error == (status != SpeakerAssetsStatus::Ok));
}

class FakeExecutor final
    : public SpeakerAssetsRuntimeActionExecutor {
 public:
  SpeakerAssetsActionExecutionResult step(
      const SpeakerAssetsActionView& action,
      SpeakerAssetsActionCompletion* completion) override {
    assert(completion != nullptr);
    ++calls;
    last_token = action.token;
    last_kind = action.kind;
    if (reject) {
      return SpeakerAssetsActionExecutionResult::Rejected;
    }
    if (pending_steps != 0U) {
      --pending_steps;
      return SpeakerAssetsActionExecutionResult::Pending;
    }
    *completion = {};
    completion->token =
        action.token + completion_token_delta;
    completion->kind = action.kind;
    completion->result = SoundStoreResult::Ok;
    if (action.kind ==
        SpeakerAssetsActionKind::QueryCurrentActive) {
      completion->current_active_valid = true;
      completion->current_active.valid = false;
    }
    return SpeakerAssetsActionExecutionResult::Completed;
  }

  std::size_t calls = 0U;
  std::size_t pending_steps = 0U;
  bool reject = false;
  std::uint32_t completion_token_delta = 0U;
  std::uint32_t last_token = 0U;
  SpeakerAssetsActionKind last_kind =
      SpeakerAssetsActionKind::Begin;
};

class BeginReadyExecutor final
    : public SpeakerAssetsRuntimeActionExecutor {
 public:
  explicit BeginReadyExecutor(
      const SoundUpdateProgress& ready_progress)
      : progress(ready_progress) {}

  SpeakerAssetsActionExecutionResult step(
      const SpeakerAssetsActionView& action,
      SpeakerAssetsActionCompletion* completion) override {
    assert(completion != nullptr);
    assert(action.kind == SpeakerAssetsActionKind::Begin);
    assert(action.plan != nullptr);
    ++calls;
    *completion = {};
    completion->token = action.token;
    completion->kind = action.kind;
    completion->result = SoundStoreResult::Ok;
    completion->identity_valid = true;
    completion->identity = progress.identity;
    completion->progress_valid = true;
    completion->progress = progress;
    return SpeakerAssetsActionExecutionResult::Completed;
  }

  SoundUpdateProgress progress{};
  std::size_t calls = 0U;
};

class ReadyProgressExecutor final
    : public SpeakerAssetsRuntimeActionExecutor {
 public:
  explicit ReadyProgressExecutor(
      const SoundUpdateProgress& ready_progress)
      : progress(ready_progress) {}

  SpeakerAssetsActionExecutionResult step(
      const SpeakerAssetsActionView& action,
      SpeakerAssetsActionCompletion* completion) override {
    assert(completion != nullptr);
    assert(action.kind == SpeakerAssetsActionKind::ResumeQuery);
    ++calls;
    if (pending_steps != 0U) {
      --pending_steps;
      return SpeakerAssetsActionExecutionResult::Pending;
    }
    *completion = {};
    completion->token = action.token;
    completion->kind = action.kind;
    completion->result = SoundStoreResult::Ok;
    completion->identity_valid = true;
    completion->identity = progress.identity;
    completion->progress_valid = true;
    completion->progress = progress;
    completion->outcome_valid = true;
    completion->outcome =
        easy_input::speaker_assets::
            SpeakerAssetsTransactionOutcome::Active;
    return SpeakerAssetsActionExecutionResult::Completed;
  }

  SoundUpdateProgress progress{};
  std::size_t calls = 0U;
  std::size_t pending_steps = 0U;
};

class BorrowedManifestExecutor final
    : public SpeakerAssetsRuntimeActionExecutor {
 public:
  explicit BorrowedManifestExecutor(
      const std::array<std::uint8_t, 32U>& expected_bytes)
      : expected(expected_bytes) {}

  SpeakerAssetsActionExecutionResult step(
      const SpeakerAssetsActionView& action,
      SpeakerAssetsActionCompletion* completion) override {
    assert(completion != nullptr);
    assert(action.kind ==
           SpeakerAssetsActionKind::WriteManifest);
    assert(action.bytes != nullptr);
    assert(action.length == expected.size());
    assert(std::equal(
        action.bytes,
        action.bytes + action.length,
        expected.begin()));
    ++calls;
    if (calls == 1U) {
      borrowed_pointer = action.bytes;
      return SpeakerAssetsActionExecutionResult::Pending;
    }
    assert(action.bytes == borrowed_pointer);
    *completion = {};
    completion->token = action.token;
    completion->kind = action.kind;
    completion->result = SoundStoreResult::Ok;
    return SpeakerAssetsActionExecutionResult::Completed;
  }

  std::array<std::uint8_t, 32U> expected{};
  const std::uint8_t* borrowed_pointer = nullptr;
  std::size_t calls = 0U;
};

SoundBundlePlan make_minimal_runtime_plan() {
  SoundBundlePlan plan{};
  plan.manifest_bytes = 32U;
  plan.manifest_sha256.fill(0x11U);
  plan.bundle_sha256.fill(0x22U);
  return plan;
}

SoundUpdateProgress make_minimal_runtime_progress(
    const SoundBundlePlan& plan) {
  SoundUpdateProgress progress{};
  progress.identity.generation = 1U;
  progress.identity.target_bank = SoundBankId::B;
  progress.identity.transaction_id.fill(0x33U);
  progress.manifest_bytes = plan.manifest_bytes;
  progress.payload_bytes = plan.payload_bytes;
  progress.payload_block_count = 0U;
  progress.bundle_sha256 = plan.bundle_sha256;
  return progress;
}

std::uint32_t establish_ready_runtime_session(
    SpeakerAssetsRuntimeCore* runtime,
    const SpeakerAssetsRouteToken& route,
    const SoundBundlePlan& plan,
    BeginReadyExecutor* executor,
    std::uint32_t now_ms) {
  assert(runtime != nullptr);
  assert(executor != nullptr);
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes>
      encoded_plan{};
  assert(easy_input::speaker_assets::
             encode_sound_bundle_plan_wire(
                 plan, &encoded_plan) ==
         SpeakerAssetsProtocolResult::Ok);
  constexpr std::size_t kChunkBytes = 32U;
  static_assert(
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes %
              kChunkBytes ==
          0U);
  for (std::size_t offset = 0U;
       offset < encoded_plan.size();
       offset += kChunkBytes) {
    SpeakerAssetsFrame begin =
        make_request(SpeakerAssetsOpcode::Begin, 1200U);
    begin.flags =
        easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
    if (offset == 0U) {
      begin.flags = static_cast<std::uint8_t>(
          begin.flags |
          easy_input::speaker_assets::
              kSpeakerAssetsFlagFirstFragment);
    }
    if (offset + kChunkBytes == encoded_plan.size()) {
      begin.flags = static_cast<std::uint8_t>(
          begin.flags |
          easy_input::speaker_assets::
              kSpeakerAssetsFlagLastFragment);
    }
    begin.object_offset = static_cast<std::uint32_t>(offset);
    begin.body_length = kChunkBytes;
    std::copy_n(
        encoded_plan.data() + offset,
        kChunkBytes,
        begin.body.begin());
    const auto encoded = encode_usb(begin);
    assert(runtime->enqueue_usb_frame(
               route,
               encoded.data(),
               encoded.size(),
               now_ms) ==
           SpeakerAssetsRuntimeEnqueueResult::Accepted);
    const auto result =
        runtime->step(now_ms, true, nullptr);
    if (offset + kChunkBytes == encoded_plan.size()) {
      assert(result ==
             SpeakerAssetsRuntimeStepResult::ActionQueued);
    } else {
      assert(result ==
             SpeakerAssetsRuntimeStepResult::ReplyQueued);
      assert_status(
          pop_front_reply(*runtime), SpeakerAssetsStatus::Ok);
    }
  }
  assert(runtime->step(now_ms, true, executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  const auto reply = pop_front_reply(*runtime);
  assert_status(reply, SpeakerAssetsStatus::Ok);
  assert(reply.frame.session_cookie != 0U);
  assert(runtime->session_phase() ==
         SpeakerAssetsSessionPhase::Ready);
  return reply.frame.session_cookie;
}

void test_callback_mailbox_decouples_owner_processing() {
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeCore runtime;
  SpeakerAssetsRuntimeMailboxRecord record{};
  const auto route = usb_route(5U);
  const auto request =
      make_request(SpeakerAssetsOpcode::Capabilities, 6U);
  const auto encoded = encode_usb(request);

  assert(mailbox.enqueue_route_opened(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 7U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.data_size() == 2U);
  assert(runtime.lifecycle_size() == 0U);
  assert(runtime.ingress_size() == 0U);

  // The callback-facing operation ends after this fixed copy/pop. Decode and
  // Session work happen only after the platform releases the mailbox lock.
  assert(mailbox.take_next(&record));
  assert(record.kind ==
         SpeakerAssetsRuntimeMailboxKind::RouteOpened);
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(7U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);

  assert(mailbox.take_next(&record));
  assert(record.kind ==
         SpeakerAssetsRuntimeMailboxKind::UsbFrame);
  assert(record.received_ms == 7U);
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(7U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  const auto reply = pop_front_reply(runtime);
  assert(reply.frame.request_id == request.request_id);
  assert_status(reply, SpeakerAssetsStatus::Ok);
  assert(!mailbox.take_next(&record));
}

void test_callback_mailbox_close_is_independent_and_dominant() {
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeCore runtime;
  SpeakerAssetsRuntimeMailboxRecord record{};
  const auto route = usb_route(8U);
  const auto request =
      make_request(SpeakerAssetsOpcode::Capabilities, 9U);
  const auto encoded = encode_usb(request);

  assert(mailbox.enqueue_route_opened(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 10U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.enqueue_route_closed(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.close_size() == 1U);
  // A delayed duplicate open cannot overtake a pending close.
  assert(mailbox.enqueue_route_opened(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Coalesced);

  // Close traffic has its own queue and is taken before all data, even though
  // Open and USB payload arrived first.
  assert(mailbox.take_next(&record));
  assert(record.kind ==
         SpeakerAssetsRuntimeMailboxKind::RouteClosed);
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(10U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);

  // Exact Close reclaims the pending Open and payload immediately, so stale
  // data cannot consume capacity while the owner applies the tombstone.
  assert(!mailbox.take_next(&record));
  assert(mailbox.data_size() == 0U);
  assert(mailbox.close_size() == 0U);
}

void test_endpoint_accept_retirement_immediately_admits_next_usb_request() {
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeCore runtime;
  SpeakerAssetsRuntimeMailboxRecord record{};
  const auto route = usb_route(24U);
  open_route(runtime, route);

  const auto first_bytes = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 25U));
  assert(mailbox.enqueue_usb_frame(
             route,
             first_bytes.data(),
             first_bytes.size(),
             0U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto first_lease = current_lease(mailbox);
  assert(mailbox.take_next(&record));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(0U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);

  SpeakerAssetsRuntimeReply accepted_reply{};
  assert(runtime.front_reply_for_route(route, &accepted_reply));
  assert(leases_equal(accepted_reply.lease, first_lease));

  const auto second_bytes = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 26U));
  assert(mailbox.enqueue_usb_frame(
             route,
             second_bytes.data(),
             second_bytes.size(),
             1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Full);

  // Model tud_hid_report() accepting the first reply. Retirement removes the
  // exact Core reply and compare-and-releases its admission before the endpoint
  // lifetime lock can admit the host's following SET_REPORT.
  assert(runtime.remove_reply_if_sequence(accepted_reply.sequence));
  assert(mailbox.release_logical_request(accepted_reply.lease));
  assert(mailbox.enqueue_usb_frame(
             route,
             second_bytes.data(),
             second_bytes.size(),
             1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);

  const auto second_lease = current_lease(mailbox);
  assert(second_lease.admission_id != first_lease.admission_id);
  assert(!mailbox.release_logical_request(first_lease));
  assert(leases_equal(current_lease(mailbox), second_lease));
  assert(mailbox.release_logical_request(second_lease));
}

void test_pending_action_and_usb_replies_preserve_exact_lease() {
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeCore runtime(0xD00U, 0xE00U);
  SpeakerAssetsRuntimeMailboxRecord record{};
  FakeExecutor executor;
  executor.pending_steps = 1U;
  const auto route = usb_route(44U);
  open_route(runtime, route);
  const auto current = encode_usb(
      make_request(SpeakerAssetsOpcode::CurrentActive, 45U));
  assert(mailbox.enqueue_usb_frame(
             route,
             current.data(),
             current.size(),
             0U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto first_lease = current_lease(mailbox);
  assert(mailbox.take_next(&record));
  assert(record.admission_id == first_lease.admission_id);
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(0U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(runtime.step(1U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionExecutionPending);
  assert(runtime.step(2U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  const auto first_reply = pop_front_reply(runtime);
  assert(leases_equal(first_reply.lease, first_lease));
  assert(mailbox.release_logical_request(first_reply.lease));

  const auto capabilities = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 46U));
  assert(mailbox.enqueue_usb_frame(
             route,
             capabilities.data(),
             capabilities.size(),
             3U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto second_lease = current_lease(mailbox);
  assert(second_lease.admission_id != first_lease.admission_id);
  assert(!mailbox.release_logical_request(first_reply.lease));
  assert(leases_equal(current_lease(mailbox), second_lease));
  assert(mailbox.take_next(&record));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(3U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  const auto second_reply = pop_front_reply(runtime);
  assert(leases_equal(second_reply.lease, second_lease));
  assert(mailbox.release_logical_request(second_reply.lease));
}

void test_all_routes_close_orphans_unbound_action_and_partial() {
  SpeakerAssetsRuntimeCore action_runtime(0xB00U, 0xC00U);
  FakeExecutor executor;
  const auto action_route = usb_route(151U);
  open_route(action_runtime, action_route);
  const auto current = encode_usb(
      make_request(SpeakerAssetsOpcode::CurrentActive, 152U));
  assert(action_runtime.enqueue_usb_frame(
             action_route,
             current.data(),
             current.size(),
             0U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(action_runtime.step(0U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(action_runtime.action_pending());

  SpeakerAssetsRuntimeMailboxRecord revoke{};
  revoke.kind = SpeakerAssetsRuntimeMailboxKind::AllRoutesClosed;
  revoke.route = action_route;
  assert(action_runtime.import_mailbox_record(revoke) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(action_runtime.active_route_count() == 0U);
  assert(action_runtime.action_pending());
  assert(action_runtime.step(1U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert(!action_runtime.action_pending());
  assert(action_runtime.reply_size() == 0U);
  assert(action_runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);

  SpeakerAssetsRuntimeCore partial_runtime;
  const auto partial_route = usb_route(153U);
  open_route(partial_runtime, partial_route);
  SpeakerAssetsFrame begin =
      make_request(SpeakerAssetsOpcode::Begin, 154U);
  begin.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
      easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment;
  begin.body_length = 8U;
  for (std::size_t index = 0U; index < begin.body_length; ++index) {
    begin.body[index] = static_cast<std::uint8_t>(index + 1U);
  }
  const auto begin_encoded = encode_usb(begin);
  assert(partial_runtime.enqueue_usb_frame(
             partial_route,
             begin_encoded.data(),
             begin_encoded.size(),
             0U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(partial_runtime.step(0U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(partial_runtime), SpeakerAssetsStatus::Ok);
  assert(partial_runtime.session_phase() ==
         SpeakerAssetsSessionPhase::BeginAssembly);

  revoke.route = partial_route;
  assert(partial_runtime.import_mailbox_record(revoke) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(partial_runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);

  const auto replacement_route = usb_route(155U);
  open_route(partial_runtime, replacement_route);
  assert(partial_runtime.enqueue_usb_frame(
             replacement_route,
             begin_encoded.data(),
             begin_encoded.size(),
             2U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(partial_runtime.step(2U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(partial_runtime), SpeakerAssetsStatus::Ok);
  assert(partial_runtime.session_phase() ==
         SpeakerAssetsSessionPhase::BeginAssembly);
}

void test_route_lifetime_and_usb_decode() {
  SpeakerAssetsRuntimeCore runtime;
  const auto route = usb_route(7U);
  const auto request =
      make_request(SpeakerAssetsOpcode::Capabilities, 10U);
  const auto encoded = encode_usb(request);

  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::StaleRouteDropped);
  assert(runtime.reply_size() == 0U);

  open_route(runtime, route, 2U);
  assert(runtime.advertised_ingress_credit(route, true) == 1U);
  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 3U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(3U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  const auto reply = pop_front_reply(runtime);
  assert(easy_input::speaker_assets::speaker_assets_route_equal(
      reply.route, route));
  assert(reply.frame.request_id == request.request_id);
  assert_status(reply, SpeakerAssetsStatus::Ok);

  auto corrupt = encoded;
  corrupt[20] ^= 0x80U;
  assert(runtime.enqueue_usb_frame(
             route, corrupt.data(), corrupt.size(), 4U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(4U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::InvalidFrameDropped);
}

void test_input_pause_and_cooperative_action() {
  SpeakerAssetsRuntimeCore runtime(0x100U, 0x200U);
  FakeExecutor executor;
  const auto route = usb_route(11U);
  open_route(runtime, route);

  const auto current =
      make_request(SpeakerAssetsOpcode::CurrentActive, 20U);
  const auto current_bytes = encode_usb(current);
  assert(runtime.enqueue_usb_frame(
             route,
             current_bytes.data(),
             current_bytes.size(),
             10U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(10U, false, &executor) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);
  assert(executor.calls == 0U);
  const auto paused = pop_front_reply(runtime);
  assert_status(paused, SpeakerAssetsStatus::PausedForInput);

  const auto capabilities =
      make_request(SpeakerAssetsOpcode::Capabilities, 21U);
  const auto capabilities_bytes = encode_usb(capabilities);
  assert(runtime.enqueue_usb_frame(
             route,
             capabilities_bytes.data(),
             capabilities_bytes.size(),
             11U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(11U, false, &executor) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::Ok);

  assert(runtime.enqueue_usb_frame(
             route,
             current_bytes.data(),
             current_bytes.size(),
             12U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(12U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(runtime.action_pending());
  assert(runtime.advertised_ingress_credit(route, true) == 0U);
  assert(runtime.step(13U, false, &executor) ==
         SpeakerAssetsRuntimeStepResult::PausedForInput);
  assert(executor.calls == 0U);

  executor.pending_steps = 1U;
  assert(runtime.step(14U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionExecutionPending);
  const auto token = executor.last_token;
  assert(token != 0U);
  assert(runtime.action_pending());
  assert(runtime.step(15U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert(executor.last_token == token);
  assert(executor.calls == 2U);
  assert(!runtime.action_pending());
  assert(runtime.reply_size() == 1U);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::Ok);
}

void test_input_pause_keeps_same_route_unit_partial_alive() {
  constexpr std::uint32_t timeout_ms = 100U;
  SpeakerAssetsRuntimeCore runtime(0x101U, 0x201U, timeout_ms);
  const auto route = usb_route(12U);
  open_route(runtime, route);

  auto plan = make_minimal_runtime_plan();
  plan.payload_bytes = 64U;
  plan.payload_block_crc32[0] = 0x44556677U;
  auto progress = make_minimal_runtime_progress(plan);
  progress.payload_block_count = 1U;
  BeginReadyExecutor executor(progress);
  const auto cookie = establish_ready_runtime_session(
      &runtime, route, plan, &executor, 0U);

  SpeakerAssetsFrame first =
      make_request(SpeakerAssetsOpcode::Data, 22U);
  first.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
      easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment |
      easy_input::speaker_assets::kSpeakerAssetsFlagPayloadRegion;
  first.session_cookie = cookie;
  first.body_length = 32U;
  first.body.fill(0x51U);
  const auto first_bytes = encode_usb(first);
  assert(runtime.enqueue_usb_frame(
             route,
             first_bytes.data(),
             first_bytes.size(),
             10U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(10U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::Ok);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::UnitAssembly);

  SpeakerAssetsFrame last = first;
  last.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
      easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment |
      easy_input::speaker_assets::kSpeakerAssetsFlagPayloadRegion;
  last.object_offset = 32U;
  last.body.fill(0x52U);
  const auto last_bytes = encode_usb(last);

  // The queued same-route retry crosses the ordinary partial timeout while
  // voice/edit input owns the resource path. It must pause without discarding
  // the first half of the payload unit.
  assert(runtime.enqueue_usb_frame(
             route,
             last_bytes.data(),
             last_bytes.size(),
             10U + timeout_ms) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(10U + timeout_ms, false, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(runtime),
      SpeakerAssetsStatus::PausedForInput);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::UnitAssembly);

  // Once input releases the resource path, the exact frame continues the same
  // unit instead of being replayed against Ready as a BadRequest.
  assert(runtime.enqueue_usb_frame(
             route,
             last_bytes.data(),
             last_bytes.size(),
             11U + timeout_ms) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(11U + timeout_ms, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(runtime.action_pending());
}

void test_ready_session_lease_releases_only_volatile_binding() {
  constexpr std::uint32_t lease_ms =
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimeSessionLeaseMs;
  static_assert(lease_ms == 10000U);
  SpeakerAssetsRuntimeCore runtime(
      0x210U,
      0x310U,
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimePartialTimeoutMs,
      lease_ms);
  const auto route = usb_route(13U);
  open_route(runtime, route);

  const auto plan = make_minimal_runtime_plan();
  const auto progress = make_minimal_runtime_progress(plan);
  BeginReadyExecutor begin_executor(progress);
  static_cast<void>(establish_ready_runtime_session(
      &runtime, route, plan, &begin_executor, 0U));
  assert(begin_executor.calls == 1U);

  // A complete request from a different exact route must not renew the owner
  // session. Physical routes remain open independently of this protocol lease.
  const auto other_usb_route = usb_route(16U);
  open_route(runtime, other_usb_route, 1U);
  const auto capabilities = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 23U));
  assert(runtime.enqueue_usb_frame(
             other_usb_route,
             capabilities.data(),
             capabilities.size(),
             lease_ms - 1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(lease_ms - 1U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  static_cast<void>(pop_front_reply(runtime));

  assert(runtime.step(lease_ms, true, &begin_executor) ==
         SpeakerAssetsRuntimeStepResult::SessionLeaseExpired);
  assert(begin_executor.calls == 1U);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);
  assert(!runtime.action_pending());
  assert(runtime.active_route_count() == 2U);

  // Expiry performs no Abort or invalid-staging erase. The same still-open
  // route can immediately query durable A/B truth instead of receiving Busy.
  const auto current = encode_usb(
      make_request(SpeakerAssetsOpcode::CurrentActive, 24U));
  assert(runtime.enqueue_usb_frame(
             route,
             current.data(),
             current.size(),
             lease_ms + 1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(lease_ms + 1U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  FakeExecutor current_executor;
  assert(runtime.step(lease_ms + 2U, true, &current_executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert(current_executor.calls == 1U);
  assert(current_executor.last_kind ==
         SpeakerAssetsActionKind::QueryCurrentActive);
  assert(current_executor.last_kind != SpeakerAssetsActionKind::Abort);
  assert(current_executor.last_kind !=
         SpeakerAssetsActionKind::DiscardInvalidStaging);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::Ok);
}

void test_session_lease_expiry_preserves_outstanding_reply_lease() {
  constexpr std::uint32_t lease_ms =
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimeSessionLeaseMs;
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeMailboxRecord record{};
  SpeakerAssetsRuntimeCore runtime;
  const auto route = usb_route(19U);
  open_route(runtime, route);

  const auto plan = make_minimal_runtime_plan();
  const auto progress = make_minimal_runtime_progress(plan);
  BeginReadyExecutor begin_executor(progress);
  static_cast<void>(establish_ready_runtime_session(
      &runtime, route, plan, &begin_executor, 0U));

  const auto capabilities = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 27U));
  assert(mailbox.enqueue_usb_frame(
             route,
             capabilities.data(),
             capabilities.size(),
             1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto outstanding_lease = current_lease(mailbox);
  assert(mailbox.take_next(&record));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  SpeakerAssetsRuntimeReply outstanding_reply{};
  assert(runtime.front_reply(&outstanding_reply));
  assert(leases_equal(
      outstanding_reply.lease, outstanding_lease));

  const auto expired =
      runtime.step(1U + lease_ms, true, nullptr);
  assert(expired.result ==
         SpeakerAssetsRuntimeStepResult::SessionLeaseExpired);
  assert(expired.release_now.admission_id == 0U);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);
  assert(runtime.reply_size() == 1U);
  assert(leases_equal(
      current_lease(mailbox), outstanding_lease));

  SpeakerAssetsRuntimeReply retained_reply{};
  assert(runtime.front_reply(&retained_reply));
  assert(retained_reply.sequence == outstanding_reply.sequence);
  assert(runtime.remove_reply_if_sequence(retained_reply.sequence));
  assert(mailbox.release_logical_request(retained_reply.lease));

  // Retirement of the old response still gates and releases the exact
  // admission. The next request receives a fresh, non-ABA lease.
  assert(mailbox.enqueue_usb_frame(
             route,
             capabilities.data(),
             capabilities.size(),
             2U + lease_ms) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto replacement_lease = current_lease(mailbox);
  assert(replacement_lease.admission_id !=
         outstanding_lease.admission_id);
  assert(!mailbox.release_logical_request(outstanding_lease));
  assert(mailbox.release_logical_request(replacement_lease));
}

void test_unauthenticated_same_route_requests_do_not_renew_session_lease() {
  constexpr std::uint32_t lease_ms =
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimeSessionLeaseMs;
  SpeakerAssetsRuntimeCore runtime;
  const auto route = usb_route(20U);
  open_route(runtime, route);

  const auto plan = make_minimal_runtime_plan();
  const auto progress = make_minimal_runtime_progress(plan);
  BeginReadyExecutor begin_executor(progress);
  const auto cookie = establish_ready_runtime_session(
      &runtime, route, plan, &begin_executor, 0U);

  // Status discovery shares the same physical route but is not part of the
  // bound transaction. It must not keep an abandoned cookie alive forever.
  const auto capabilities = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 28U));
  assert(runtime.enqueue_usb_frame(
             route,
             capabilities.data(),
             capabilities.size(),
             lease_ms / 3U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(lease_ms / 3U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::Ok);

  auto wrong_cookie_query =
      make_request(SpeakerAssetsOpcode::Query, 29U);
  wrong_cookie_query.session_cookie = cookie + 1U;
  const auto wrong_cookie_bytes = encode_usb(wrong_cookie_query);
  assert(runtime.enqueue_usb_frame(
             route,
             wrong_cookie_bytes.data(),
             wrong_cookie_bytes.size(),
             (lease_ms * 2U) / 3U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(
             (lease_ms * 2U) / 3U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(runtime),
      SpeakerAssetsStatus::TransactionMismatch);

  assert(runtime.step(lease_ms, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::SessionLeaseExpired);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);
}

void test_expired_session_rejects_exact_old_query_replay() {
  constexpr std::uint32_t lease_ms =
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimeSessionLeaseMs;
  SpeakerAssetsRuntimeCore runtime;
  const auto route = usb_route(22U);
  open_route(runtime, route);

  const auto plan = make_minimal_runtime_plan();
  const auto progress = make_minimal_runtime_progress(plan);
  BeginReadyExecutor begin_executor(progress);
  const auto cookie = establish_ready_runtime_session(
      &runtime, route, plan, &begin_executor, 0U);

  auto query = make_request(SpeakerAssetsOpcode::Query, 32U);
  query.session_cookie = cookie;
  const auto query_bytes = encode_usb(query);
  assert(runtime.enqueue_usb_frame(
             route,
             query_bytes.data(),
             query_bytes.size(),
             1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  ReadyProgressExecutor query_executor(progress);
  assert(runtime.step(2U, true, &query_executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::Ok);
  assert(query_executor.calls == 1U);

  assert(runtime.step(2U + lease_ms, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::SessionLeaseExpired);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);

  // The physical route remains cached, so this is byte-for-byte the same
  // route/cookie/request_id/opcode as the old successful Query. Logical route
  // revocation must clear its volatile replay record; otherwise lookup_replay
  // would bypass Idle's cookie check and return the stale Ok.
  assert(runtime.enqueue_usb_frame(
             route,
             query_bytes.data(),
             query_bytes.size(),
             3U + lease_ms) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(3U + lease_ms, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(runtime),
      SpeakerAssetsStatus::TransactionMismatch);
  assert(query_executor.calls == 1U);
  assert(!runtime.action_pending());
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);
}

void test_paused_complete_requests_renew_ready_session_lease() {
  constexpr std::uint32_t lease_ms =
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimeSessionLeaseMs;
  SpeakerAssetsRuntimeCore runtime(
      0x220U,
      0x320U,
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimePartialTimeoutMs,
      lease_ms);
  const auto route = usb_route(17U);
  open_route(runtime, route);

  const auto plan = make_minimal_runtime_plan();
  const auto progress = make_minimal_runtime_progress(plan);
  BeginReadyExecutor begin_executor(progress);
  const auto cookie = establish_ready_runtime_session(
      &runtime, route, plan, &begin_executor, 0U);

  auto query = make_request(SpeakerAssetsOpcode::Query, 25U);
  query.session_cookie = cookie;
  const auto query_bytes = encode_usb(query);
  constexpr std::array<std::uint32_t, 3U> retry_times{
      9000U, 18000U, 27000U};
  for (const auto retry_ms : retry_times) {
    assert(runtime.enqueue_usb_frame(
               route,
               query_bytes.data(),
               query_bytes.size(),
               retry_ms) ==
           SpeakerAssetsRuntimeEnqueueResult::Accepted);
    assert(runtime.step(retry_ms, false, nullptr) ==
           SpeakerAssetsRuntimeStepResult::ReplyQueued);
    assert_status(
        pop_front_reply(runtime),
        SpeakerAssetsStatus::PausedForInput);
    assert(runtime.session_phase() ==
           SpeakerAssetsSessionPhase::Ready);
  }

  // Voice/edit input has lasted far longer than one lease, but the App's
  // complete same-route retries keep the session valid. Only a full lease of
  // silence after the final retry releases it.
  assert(runtime.step(
             retry_times.back() + lease_ms - 1U,
             false,
             nullptr) ==
         SpeakerAssetsRuntimeStepResult::Idle);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Ready);
  assert(runtime.step(
             retry_times.back() + lease_ms,
             false,
             nullptr) ==
         SpeakerAssetsRuntimeStepResult::SessionLeaseExpired);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Idle);
}

void test_pending_action_suspends_ready_session_lease() {
  constexpr std::uint32_t lease_ms =
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimeSessionLeaseMs;
  SpeakerAssetsRuntimeCore runtime(
      0x230U,
      0x330U,
      easy_input::speaker_assets::
          kSpeakerAssetsRuntimePartialTimeoutMs,
      lease_ms);
  const auto route = usb_route(18U);
  open_route(runtime, route);

  const auto plan = make_minimal_runtime_plan();
  const auto progress = make_minimal_runtime_progress(plan);
  BeginReadyExecutor begin_executor(progress);
  const auto cookie = establish_ready_runtime_session(
      &runtime, route, plan, &begin_executor, 0U);

  auto query = make_request(SpeakerAssetsOpcode::Query, 26U);
  query.session_cookie = cookie;
  const auto query_bytes = encode_usb(query);
  assert(runtime.enqueue_usb_frame(
             route,
             query_bytes.data(),
             query_bytes.size(),
             1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(runtime.action_pending());

  ReadyProgressExecutor query_executor(progress);
  query_executor.pending_steps = 2U;
  assert(runtime.step(lease_ms * 2U, true, &query_executor) ==
         SpeakerAssetsRuntimeStepResult::ActionExecutionPending);
  assert(runtime.step(lease_ms * 4U, false, &query_executor) ==
         SpeakerAssetsRuntimeStepResult::PausedForInput);
  assert(runtime.step(lease_ms * 6U, true, &query_executor) ==
         SpeakerAssetsRuntimeStepResult::ActionExecutionPending);
  assert(runtime.action_pending());
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::ActionPending);

  assert(runtime.step(lease_ms * 8U, true, &query_executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert(query_executor.calls == 3U);
  assert(!runtime.action_pending());
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Ready);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::Ok);

  // The pending interval does not consume the renewed post-completion lease.
  assert(runtime.step(
             lease_ms * 9U - 1U,
             true,
             nullptr) ==
         SpeakerAssetsRuntimeStepResult::Idle);
  assert(runtime.step(
             lease_ms * 9U,
             true,
             nullptr) ==
         SpeakerAssetsRuntimeStepResult::SessionLeaseExpired);
}

void test_route_close_preempts_and_orphans_action() {
  SpeakerAssetsRuntimeCore runtime(0x300U, 0x400U);
  FakeExecutor executor;
  const auto route = usb_route(15U);
  open_route(runtime, route);
  const auto request =
      make_request(SpeakerAssetsOpcode::CurrentActive, 30U);
  const auto encoded = encode_usb(request);

  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 2U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.enqueue_route_closed(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);

  assert(runtime.step(2U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
  assert(runtime.ingress_size() == 0U);
  assert(runtime.active_route_count() == 0U);
  assert(runtime.action_pending());
  assert(runtime.step(3U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert(executor.calls == 1U);
  assert(runtime.reply_size() == 0U);

  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 4U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(4U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::StaleRouteDropped);
}

void test_stale_completion_keeps_action_and_reply_reservation() {
  SpeakerAssetsRuntimeCore runtime(0x500U, 0x600U);
  FakeExecutor executor;
  const auto route = usb_route(18U);
  open_route(runtime, route);

  // Three immediate replies leave exactly one slot. The Store action must
  // retain that slot until a matching completion can enqueue its ACK.
  for (std::uint32_t index = 0U; index < 3U; ++index) {
    const auto capabilities = make_request(
        SpeakerAssetsOpcode::Capabilities, 40U + index);
    const auto encoded = encode_usb(capabilities);
    assert(runtime.enqueue_usb_frame(
               route, encoded.data(), encoded.size(), index) ==
           SpeakerAssetsRuntimeEnqueueResult::Accepted);
    assert(runtime.step(index, true, &executor) ==
           SpeakerAssetsRuntimeStepResult::ReplyQueued);
  }
  assert(runtime.reply_size() == 3U);

  const auto current =
      make_request(SpeakerAssetsOpcode::CurrentActive, 50U);
  const auto encoded = encode_usb(current);
  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 10U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(10U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);

  executor.completion_token_delta = 1U;
  assert(runtime.step(11U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::SessionRejected);
  assert(runtime.action_pending());
  assert(runtime.reply_size() == 3U);

  executor.completion_token_delta = 0U;
  assert(runtime.step(12U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionCompleted);
  assert(!runtime.action_pending());
  assert(runtime.reply_size() ==
         easy_input::speaker_assets::
             kSpeakerAssetsRuntimeReplyCapacity);
}

void test_executor_rejection_unwinds_normal_and_orphaned_actions() {
  SpeakerAssetsRuntimeCore runtime(0x700U, 0x800U);
  FakeExecutor executor;
  executor.reject = true;
  const auto route = usb_route(19U);
  open_route(runtime, route);
  const auto current =
      make_request(SpeakerAssetsOpcode::CurrentActive, 60U);
  const auto encoded = encode_usb(current);

  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(runtime.step(2U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ExecutorRejected);
  assert(!runtime.action_pending());
  assert(runtime.reply_size() == 1U);
  assert_status(
      pop_front_reply(runtime),
      SpeakerAssetsStatus::StorageUnavailable);
  assert(runtime.advertised_ingress_credit(route, true) == 1U);

  SpeakerAssetsRuntimeCore orphaned(0x900U, 0xA00U);
  open_route(orphaned, route);
  assert(orphaned.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 3U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(orphaned.step(3U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ActionQueued);
  assert(orphaned.enqueue_route_closed(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(orphaned.step(4U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
  assert(orphaned.step(5U, true, &executor) ==
         SpeakerAssetsRuntimeStepResult::ExecutorRejected);
  assert(!orphaned.action_pending());
  assert(orphaned.reply_size() == 0U);
}

void test_reply_backpressure_preserves_ingress() {
  SpeakerAssetsRuntimeCore runtime;
  const auto route = usb_route(20U);
  open_route(runtime, route);

  for (std::size_t index = 0U;
       index <
           easy_input::speaker_assets::
               kSpeakerAssetsRuntimeReplyCapacity;
       ++index) {
    const auto request = make_request(
        SpeakerAssetsOpcode::Capabilities,
        static_cast<std::uint32_t>(100U + index));
    const auto encoded = encode_usb(request);
    assert(runtime.enqueue_usb_frame(
               route, encoded.data(), encoded.size(), 1U) ==
           SpeakerAssetsRuntimeEnqueueResult::Accepted);
    assert(runtime.step(1U, true, nullptr) ==
           SpeakerAssetsRuntimeStepResult::ReplyQueued);
  }
  assert(runtime.reply_size() ==
         easy_input::speaker_assets::
             kSpeakerAssetsRuntimeReplyCapacity);

  const auto extra =
      make_request(SpeakerAssetsOpcode::Capabilities, 200U);
  const auto extra_encoded = encode_usb(extra);
  assert(runtime.enqueue_usb_frame(
             route,
             extra_encoded.data(),
             extra_encoded.size(),
             2U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(2U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyBackpressure);
  assert(runtime.ingress_size() == 1U);

  SpeakerAssetsRuntimeReply first{};
  assert(runtime.front_reply(&first));
  assert(!runtime.pop_reply_if_sequence(first.sequence + 1U));
  assert(runtime.pop_reply_if_sequence(first.sequence));
  assert(runtime.step(3U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert(runtime.ingress_size() == 0U);
}

void test_unit_retransmits_expire_and_replay_against_ready_state() {
  constexpr std::uint32_t timeout_ms = 200U;
  SpeakerAssetsRuntimeCore runtime(1U, 1U, timeout_ms);
  const auto route = usb_route(704U);
  open_route(runtime, route);
  const auto plan = make_minimal_runtime_plan();
  const auto progress = make_minimal_runtime_progress(plan);
  BeginReadyExecutor executor(progress);
  const auto cookie = establish_ready_runtime_session(
      &runtime, route, plan, &executor, 0U);

  SpeakerAssetsFrame first =
      make_request(SpeakerAssetsOpcode::Data, 1201U);
  first.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
      easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment;
  first.session_cookie = cookie;
  first.body_length = 8U;
  first.body.fill(0x41U);
  const auto first_encoded = encode_usb(first);
  assert(runtime.enqueue_usb_frame(
             route,
             first_encoded.data(),
             first_encoded.size(),
             100U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(100U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(pop_front_reply(runtime), SpeakerAssetsStatus::Ok);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::UnitAssembly);

  SpeakerAssetsFrame middle = first;
  middle.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
  middle.object_offset = 16U;
  middle.body.fill(0x42U);
  const auto middle_encoded = encode_usb(middle);
  assert(runtime.enqueue_usb_frame(
             route,
             middle_encoded.data(),
             middle_encoded.size(),
             110U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(110U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(pop_front_reply(runtime), SpeakerAssetsStatus::Ok);

  // The duplicate is accepted by the old Unit assembler but adds no bytes.
  // At the deadline the old partial is cleared and the same middle fragment
  // is replayed in Ready, where a fragment without FIRST is BadRequest.
  assert(runtime.enqueue_usb_frame(
             route,
             middle_encoded.data(),
             middle_encoded.size(),
             110U + timeout_ms) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto duplicate_middle =
      runtime.step(110U + timeout_ms, true, nullptr);
  assert(duplicate_middle.result ==
         SpeakerAssetsRuntimeStepResult::PartialExpired);
  assert(duplicate_middle.release_now.admission_id == 0U);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::BadRequest);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::Ready);

  SpeakerAssetsFrame old_first = first;
  old_first.request_id = 1202U;
  const auto old_first_encoded = encode_usb(old_first);
  assert(runtime.enqueue_usb_frame(
             route,
             old_first_encoded.data(),
             old_first_encoded.size(),
             400U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(400U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(pop_front_reply(runtime), SpeakerAssetsStatus::Ok);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::UnitAssembly);

  // Against the old Unit this is Busy because request_id differs. Once that
  // inactive partial expires, replaying the FIRST fragment in Ready starts a
  // new, internally consistent Unit and therefore returns Ok instead.
  SpeakerAssetsFrame replacement_first = old_first;
  replacement_first.request_id = 1203U;
  const auto replacement_first_encoded =
      encode_usb(replacement_first);
  assert(runtime.enqueue_usb_frame(
             route,
             replacement_first_encoded.data(),
             replacement_first_encoded.size(),
             400U + timeout_ms) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto busy_at_deadline =
      runtime.step(400U + timeout_ms, true, nullptr);
  assert(busy_at_deadline.result ==
         SpeakerAssetsRuntimeStepResult::PartialExpired);
  const auto replacement_reply = pop_front_reply(runtime);
  assert(replacement_reply.frame.request_id == 1203U);
  assert_status(replacement_reply, SpeakerAssetsStatus::Ok);
  assert(runtime.session_phase() ==
         SpeakerAssetsSessionPhase::UnitAssembly);
}

void test_queue_full_rejects_newest_and_close_purges_old() {
  SpeakerAssetsRuntimeCore runtime;
  const auto route = usb_route(500U);
  open_route(runtime, route);
  const auto request =
      make_request(SpeakerAssetsOpcode::Capabilities, 600U);
  const auto encoded = encode_usb(request);
  for (std::size_t index = 0U;
       index <
           easy_input::speaker_assets::
               kSpeakerAssetsRuntimeIngressCapacity;
       ++index) {
    assert(runtime.enqueue_usb_frame(
               route,
               encoded.data(),
               encoded.size(),
               static_cast<std::uint32_t>(index)) ==
           SpeakerAssetsRuntimeEnqueueResult::Accepted);
  }
  assert(runtime.enqueue_usb_frame(
             route, encoded.data(), encoded.size(), 99U) ==
         SpeakerAssetsRuntimeEnqueueResult::Full);
  assert(runtime.ingress_size() ==
         easy_input::speaker_assets::
             kSpeakerAssetsRuntimeIngressCapacity);
  assert(runtime.enqueue_route_closed(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(100U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
  assert(runtime.ingress_size() == 0U);
}

void test_wifi_exact_frame_flows_through_mailbox_and_core() {
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeCore runtime;
  SpeakerAssetsRuntimeMailboxRecord record{};
  const auto route = wifi_route(41U, 410U);
  const auto request =
      make_request(SpeakerAssetsOpcode::Capabilities, 411U);
  const auto encoded = encode_wifi(request);
  assert(
      encoded.length ==
      easy_input::speaker_assets::kSpeakerAssetsFrameHeaderBytes);

  assert(mailbox.enqueue_route_opened(route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.enqueue_wifi_frame(
             route,
             encoded.bytes.data(),
             encoded.length,
             12U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto lease = current_lease(mailbox);

  assert(mailbox.take_next(&record));
  assert(record.kind ==
         SpeakerAssetsRuntimeMailboxKind::RouteOpened);
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(12U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);

  assert(mailbox.take_next(&record));
  assert(record.kind ==
         SpeakerAssetsRuntimeMailboxKind::WifiFrame);
  assert(record.length == encoded.length);
  assert(record.admission_id == lease.admission_id);
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(12U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);

  SpeakerAssetsRuntimeReply reply{};
  assert(runtime.front_reply_for_route(route, &reply));
  assert(reply.frame.request_id == request.request_id);
  assert_status(reply, SpeakerAssetsStatus::Ok);
  assert(leases_equal(reply.lease, lease));
  assert(runtime.remove_reply_if_sequence(reply.sequence));
  assert(mailbox.release_logical_request(reply.lease));
  assert(!mailbox.logical_request_lease(&reply.lease));
}

void test_wifi_uses_exact_length_without_fixed_padding() {
  SpeakerAssetsRuntimeCore runtime;
  const auto route = wifi_route(42U, 420U);
  open_route(runtime, route);

  auto request =
      make_request(SpeakerAssetsOpcode::Capabilities, 421U);
  const auto empty = encode_wifi(request);
  assert(
      empty.length ==
      easy_input::speaker_assets::kSpeakerAssetsFrameHeaderBytes);
  assert(runtime.enqueue_wifi_frame(
             route,
             empty.bytes.data(),
             empty.length,
             1U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(1U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(pop_front_reply(runtime), SpeakerAssetsStatus::Ok);

  request.request_id = 422U;
  request.body_length =
      easy_input::speaker_assets::kSpeakerAssetsWifiFrameBodyBytes;
  for (std::size_t index = 0U;
       index < request.body_length;
       ++index) {
    request.body[index] =
        static_cast<std::uint8_t>(index ^ 0xA5U);
  }
  const auto full = encode_wifi(request);
  assert(
      full.length ==
      easy_input::speaker_assets::kSpeakerAssetsWifiFrameMaxBytes);
  assert(runtime.enqueue_wifi_frame(
             route,
             full.bytes.data(),
             full.length,
             2U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(2U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert_status(
      pop_front_reply(runtime), SpeakerAssetsStatus::BadRequest);

  assert(runtime.enqueue_wifi_frame(
             route,
             empty.bytes.data(),
             easy_input::speaker_assets::kSpeakerAssetsFrameHeaderBytes -
                 1U,
             3U) ==
         SpeakerAssetsRuntimeEnqueueResult::InvalidArgument);
  assert(runtime.enqueue_wifi_frame(
             route,
             full.bytes.data(),
             full.bytes.size() + 1U,
             3U) ==
         SpeakerAssetsRuntimeEnqueueResult::InvalidArgument);
}

void test_mailbox_admission_is_global_across_usb_and_wifi() {
  SpeakerAssetsRuntimeMailbox mailbox;
  const auto usb = usb_route(430U);
  const auto wifi = wifi_route(43U, 431U);
  const auto usb_bytes = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 432U));
  const auto wifi_bytes = encode_wifi(
      make_request(SpeakerAssetsOpcode::Capabilities, 433U));

  assert(mailbox.enqueue_wifi_frame(
             wifi,
             wifi_bytes.bytes.data(),
             wifi_bytes.length,
             4U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto first_lease = current_lease(mailbox);
  assert(easy_input::speaker_assets::speaker_assets_route_equal(
      first_lease.route, wifi));
  assert(mailbox.enqueue_usb_frame(
             usb,
             usb_bytes.data(),
             usb_bytes.size(),
             4U) ==
         SpeakerAssetsRuntimeEnqueueResult::Full);

  SpeakerAssetsLogicalRequestLease wrong = first_lease;
  ++wrong.admission_id;
  assert(!mailbox.release_logical_request(wrong));
  assert(mailbox.release_logical_request(first_lease));
  assert(mailbox.data_size() == 0U);

  assert(mailbox.enqueue_usb_frame(
             usb,
             usb_bytes.data(),
             usb_bytes.size(),
             5U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto second_lease = current_lease(mailbox);
  assert(second_lease.admission_id != first_lease.admission_id);
  assert(easy_input::speaker_assets::speaker_assets_route_equal(
      second_lease.route, usb));
  assert(mailbox.release_logical_request(second_lease));
}

void test_wifi_close_is_close_first_and_generation_exact() {
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeCore runtime;
  SpeakerAssetsRuntimeMailboxRecord record{};
  const auto old_route = wifi_route(44U, 440U);
  const auto new_route = wifi_route(44U, 441U);
  const auto request = encode_wifi(
      make_request(SpeakerAssetsOpcode::Capabilities, 442U));

  assert(mailbox.enqueue_route_opened(old_route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.enqueue_wifi_frame(
             old_route,
             request.bytes.data(),
             request.length,
             6U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.enqueue_route_closed(old_route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);

  assert(mailbox.take_next(&record));
  assert(record.kind ==
         SpeakerAssetsRuntimeMailboxKind::RouteClosed);
  assert(easy_input::speaker_assets::speaker_assets_route_equal(
      record.route, old_route));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(6U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
  assert(!mailbox.take_next(&record));

  // The exact closed lifetime cannot be revived, while the next generation on
  // the same connection-local route ID is independent.
  assert(runtime.enqueue_route_opened(old_route) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(7U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::RouteCapacityExceeded);
  open_route(runtime, new_route, 8U);
  assert(runtime.enqueue_wifi_frame(
             new_route,
             request.bytes.data(),
             request.length,
             9U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(9U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  const auto reply = pop_front_reply(runtime);
  assert(easy_input::speaker_assets::speaker_assets_route_equal(
      reply.route, new_route));
  assert_status(reply, SpeakerAssetsStatus::Ok);
}

void test_reply_selection_is_exact_across_usb_and_wifi() {
  SpeakerAssetsRuntimeCore runtime;
  const auto usb = usb_route(450U);
  const auto wifi = wifi_route(45U, 451U);
  open_route(runtime, usb);
  open_route(runtime, wifi);
  const auto usb_bytes = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 452U));
  const auto wifi_bytes = encode_wifi(
      make_request(SpeakerAssetsOpcode::Capabilities, 453U));

  assert(runtime.enqueue_usb_frame(
             usb,
             usb_bytes.data(),
             usb_bytes.size(),
             10U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.enqueue_wifi_frame(
             wifi,
             wifi_bytes.bytes.data(),
             wifi_bytes.length,
             10U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(10U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  assert(runtime.step(10U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);

  SpeakerAssetsRuntimeReply wifi_reply{};
  assert(runtime.front_reply_for_route(wifi, &wifi_reply));
  assert(wifi_reply.frame.request_id == 453U);
  assert(runtime.remove_reply_if_sequence(wifi_reply.sequence));

  SpeakerAssetsRuntimeReply usb_reply{};
  assert(runtime.front_reply(&usb_reply));
  assert(easy_input::speaker_assets::speaker_assets_route_equal(
      usb_reply.route, usb));
  assert(usb_reply.frame.request_id == 452U);
  assert(runtime.pop_reply_if_sequence(usb_reply.sequence));
  assert(runtime.reply_size() == 0U);
}

void test_usb_and_wifi_hot_paths_do_not_allocate() {
  SpeakerAssetsRuntimeMailbox mailbox;
  SpeakerAssetsRuntimeCore runtime;
  SpeakerAssetsRuntimeMailboxRecord record{};
  const auto usb = usb_route(460U);
  const auto wifi = wifi_route(46U, 461U);
  const auto usb_bytes = encode_usb(
      make_request(SpeakerAssetsOpcode::Capabilities, 462U));
  const auto wifi_bytes = encode_wifi(
      make_request(SpeakerAssetsOpcode::Capabilities, 463U));

  g_allocation_count = 0U;
  g_track_allocations = true;
  assert(mailbox.enqueue_route_opened(usb) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.take_next(&record));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(11U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
  assert(mailbox.enqueue_usb_frame(
             usb,
             usb_bytes.data(),
             usb_bytes.size(),
             11U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto usb_lease = current_lease(mailbox);
  assert(mailbox.take_next(&record));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(11U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  const auto usb_reply = pop_front_reply(runtime);
  assert(mailbox.release_logical_request(usb_reply.lease));
  assert(leases_equal(usb_reply.lease, usb_lease));

  assert(mailbox.enqueue_route_opened(wifi) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(mailbox.take_next(&record));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(12U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::LifecycleApplied);
  assert(mailbox.enqueue_wifi_frame(
             wifi,
             wifi_bytes.bytes.data(),
             wifi_bytes.length,
             12U) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  const auto wifi_lease = current_lease(mailbox);
  assert(mailbox.take_next(&record));
  assert(runtime.import_mailbox_record(record) ==
         SpeakerAssetsRuntimeEnqueueResult::Accepted);
  assert(runtime.step(12U, true, nullptr) ==
         SpeakerAssetsRuntimeStepResult::ReplyQueued);
  const auto wifi_reply = pop_front_reply(runtime);
  assert(mailbox.release_logical_request(wifi_reply.lease));
  assert(leases_equal(wifi_reply.lease, wifi_lease));
  g_track_allocations = false;
  assert(g_allocation_count == 0U);
}


static_assert(
    sizeof(SpeakerAssetsRuntimeMailbox) <= 4U * 1024U,
    "speaker assets mailbox must fit its callback-safe RAM budget");
static_assert(
    sizeof(SpeakerAssetsRuntimeCore) <= 16U * 1024U,
    "speaker assets runtime must fit its fixed owner-task RAM budget");

}  // namespace

void* operator new(std::size_t size) {
  if (g_track_allocations) {
    ++g_allocation_count;
  }
  if (void* pointer = std::malloc(size == 0U ? 1U : size)) {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* pointer) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

int main() {
  test_callback_mailbox_decouples_owner_processing();
  test_callback_mailbox_close_is_independent_and_dominant();
  test_endpoint_accept_retirement_immediately_admits_next_usb_request();
  test_pending_action_and_usb_replies_preserve_exact_lease();
  test_all_routes_close_orphans_unbound_action_and_partial();
  test_route_lifetime_and_usb_decode();
  test_input_pause_and_cooperative_action();
  test_input_pause_keeps_same_route_unit_partial_alive();
  test_ready_session_lease_releases_only_volatile_binding();
  test_session_lease_expiry_preserves_outstanding_reply_lease();
  test_unauthenticated_same_route_requests_do_not_renew_session_lease();
  test_expired_session_rejects_exact_old_query_replay();
  test_paused_complete_requests_renew_ready_session_lease();
  test_pending_action_suspends_ready_session_lease();
  test_route_close_preempts_and_orphans_action();
  test_stale_completion_keeps_action_and_reply_reservation();
  test_executor_rejection_unwinds_normal_and_orphaned_actions();
  test_reply_backpressure_preserves_ingress();
  test_unit_retransmits_expire_and_replay_against_ready_state();
  test_queue_full_rejects_newest_and_close_purges_old();
  test_wifi_exact_frame_flows_through_mailbox_and_core();
  test_wifi_uses_exact_length_without_fixed_padding();
  test_mailbox_admission_is_global_across_usb_and_wifi();
  test_wifi_close_is_close_first_and_generation_exact();
  test_reply_selection_is_exact_across_usb_and_wifi();
  test_usb_and_wifi_hot_paths_do_not_allocate();
  return 0;
}
