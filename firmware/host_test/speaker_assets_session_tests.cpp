#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/speaker_assets_session.h"

namespace {

using easy_input::speaker_assets::SoundBankId;
using easy_input::speaker_assets::SoundBankSnapshot;
using easy_input::speaker_assets::SoundBundlePlan;
using easy_input::speaker_assets::SoundStoreResult;
using easy_input::speaker_assets::SoundUpdateIdentity;
using easy_input::speaker_assets::SoundUpdateProgress;
using easy_input::speaker_assets::SpeakerAssetsActionCompletion;
using easy_input::speaker_assets::SpeakerAssetsActionKind;
using easy_input::speaker_assets::SpeakerAssetsActionView;
using easy_input::speaker_assets::SpeakerAssetsEmission;
using easy_input::speaker_assets::SpeakerAssetsEmissionKind;
using easy_input::speaker_assets::SpeakerAssetsFrame;
using easy_input::speaker_assets::SpeakerAssetsOpcode;
using easy_input::speaker_assets::SpeakerAssetsProtocolResult;
using easy_input::speaker_assets::SpeakerAssetsRegion;
using easy_input::speaker_assets::SpeakerAssetsResumeQueryMode;
using easy_input::speaker_assets::SpeakerAssetsRouteToken;
using easy_input::speaker_assets::SpeakerAssetsSession;
using easy_input::speaker_assets::SpeakerAssetsSessionPhase;
using easy_input::speaker_assets::SpeakerAssetsSessionResult;
using easy_input::speaker_assets::SpeakerAssetsStatus;
using easy_input::speaker_assets::SpeakerAssetsTransport;
using easy_input::speaker_assets::SpeakerAssetsTransactionOutcome;

constexpr std::size_t kSessionRamBudgetBytes = 8U * 1024U;
constexpr std::size_t kUsbFragmentBytes = 32U;
constexpr std::uint8_t kControlFlags =
    easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
    easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment |
    easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment;

std::uint16_t read_le16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(bytes[0]) |
      (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
  std::uint32_t value = 0U;
  for (std::size_t index = 0U; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index])
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_le64(const std::uint8_t* bytes) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index])
             << (index * 8U);
  }
  return value;
}

SpeakerAssetsRouteToken usb_route(std::uint32_t generation) {
  SpeakerAssetsRouteToken route{};
  route.transport = SpeakerAssetsTransport::Usb;
  route.generation = generation;
  return route;
}

bool routes_equal(const SpeakerAssetsRouteToken& first,
                  const SpeakerAssetsRouteToken& second) {
  return first.transport == second.transport &&
         first.route_id == second.route_id &&
         first.generation == second.generation;
}

bool identities_equal(const SoundUpdateIdentity& first,
                      const SoundUpdateIdentity& second) {
  return first.generation == second.generation &&
         first.target_bank == second.target_bank &&
         first.transaction_id == second.transaction_id;
}

bool plans_equal(const SoundBundlePlan& first,
                 const SoundBundlePlan& second) {
  return first.base_generation == second.base_generation &&
         first.base_bundle_sha256 == second.base_bundle_sha256 &&
         first.manifest_bytes == second.manifest_bytes &&
         first.payload_bytes == second.payload_bytes &&
         first.manifest_crc32 == second.manifest_crc32 &&
         first.manifest_sha256 == second.manifest_sha256 &&
         first.bundle_sha256 == second.bundle_sha256 &&
         first.payload_block_crc32 == second.payload_block_crc32;
}

bool frames_equal(const SpeakerAssetsFrame& first,
                  const SpeakerAssetsFrame& second) {
  return first.opcode == second.opcode &&
         first.flags == second.flags &&
         first.request_id == second.request_id &&
         first.session_cookie == second.session_cookie &&
         first.object_offset == second.object_offset &&
         first.body_length == second.body_length &&
         first.body == second.body;
}

SoundBundlePlan make_plan(std::uint32_t manifest_bytes = 84U,
                          std::uint32_t payload_bytes =
                              easy_input::speaker_assets::
                                  kSoundPayloadBlockSize +
                              73U) {
  SoundBundlePlan plan{};
  plan.base_generation = 7U;
  for (std::size_t index = 0U;
       index < plan.base_bundle_sha256.size();
       ++index) {
    plan.base_bundle_sha256[index] =
        static_cast<std::uint8_t>(0x10U + index);
    plan.manifest_sha256[index] =
        static_cast<std::uint8_t>(0x40U + index);
    plan.bundle_sha256[index] =
        static_cast<std::uint8_t>(0x80U + index);
  }
  plan.manifest_bytes = manifest_bytes;
  plan.payload_bytes = payload_bytes;
  plan.manifest_crc32 = 0x78563412U;
  const auto block_count =
      (static_cast<std::size_t>(payload_bytes) +
       easy_input::speaker_assets::kSoundPayloadBlockSize - 1U) /
      easy_input::speaker_assets::kSoundPayloadBlockSize;
  for (std::size_t index = 0U; index < block_count; ++index) {
    plan.payload_block_crc32[index] =
        static_cast<std::uint32_t>(0xA0B00000U + index);
  }
  return plan;
}

SoundUpdateProgress make_progress(const SoundBundlePlan& plan,
                                  std::uint8_t transaction_seed = 0xC0U) {
  SoundUpdateProgress progress{};
  progress.identity.generation = plan.base_generation + 1U;
  progress.identity.target_bank = SoundBankId::B;
  for (std::size_t index = 0U;
       index < progress.identity.transaction_id.size();
       ++index) {
    progress.identity.transaction_id[index] =
        static_cast<std::uint8_t>(transaction_seed + index);
  }
  progress.manifest_bytes = plan.manifest_bytes;
  progress.payload_bytes = plan.payload_bytes;
  progress.payload_block_count = static_cast<std::uint16_t>(
      (static_cast<std::size_t>(plan.payload_bytes) +
       easy_input::speaker_assets::kSoundPayloadBlockSize - 1U) /
      easy_input::speaker_assets::kSoundPayloadBlockSize);
  progress.bundle_sha256 = plan.bundle_sha256;
  return progress;
}

SpeakerAssetsFrame make_control_request(SpeakerAssetsOpcode opcode,
                                        std::uint32_t request_id,
                                        std::uint32_t session_cookie) {
  SpeakerAssetsFrame frame{};
  frame.opcode = static_cast<std::uint8_t>(opcode);
  frame.flags = kControlFlags;
  frame.request_id = request_id;
  frame.session_cookie = session_cookie;
  return frame;
}

SpeakerAssetsFrame make_bytes_request(
    SpeakerAssetsOpcode opcode,
    std::uint32_t request_id,
    std::uint32_t session_cookie,
    std::uint32_t object_offset,
    std::uint8_t flags,
    const std::uint8_t* bytes,
    std::size_t length) {
  assert(bytes != nullptr);
  assert(length != 0U);
  assert(length <= easy_input::speaker_assets::
                       kSpeakerAssetsWifiFrameBodyBytes);
  SpeakerAssetsFrame frame{};
  frame.opcode = static_cast<std::uint8_t>(opcode);
  frame.flags = flags;
  frame.request_id = request_id;
  frame.session_cookie = session_cookie;
  frame.object_offset = object_offset;
  frame.body_length = static_cast<std::uint16_t>(length);
  std::copy_n(bytes, length, frame.body.begin());
  return frame;
}

void assert_reply(const SpeakerAssetsEmission& emission,
                  const SpeakerAssetsFrame& request,
                  SpeakerAssetsStatus expected_status) {
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(emission.reply.opcode == request.opcode);
  assert(emission.reply.request_id == request.request_id);
  assert(
      (emission.reply.flags &
       easy_input::speaker_assets::kSpeakerAssetsFlagResponse) != 0U);
  assert(emission.reply.body_length >= 1U);
  assert(
      emission.reply.body[0] == static_cast<std::uint8_t>(expected_status));
  const bool error =
      (emission.reply.flags &
       easy_input::speaker_assets::kSpeakerAssetsFlagError) != 0U;
  assert(error == (expected_status != SpeakerAssetsStatus::Ok));
}

void assert_error_reply(const SpeakerAssetsEmission& emission,
                        const SpeakerAssetsFrame& request) {
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(emission.reply.opcode == request.opcode);
  assert(emission.reply.request_id == request.request_id);
  assert(
      (emission.reply.flags &
       easy_input::speaker_assets::kSpeakerAssetsFlagResponse) != 0U);
  assert(
      (emission.reply.flags &
       easy_input::speaker_assets::kSpeakerAssetsFlagError) != 0U);
  assert(emission.reply.body_length >= 1U);
  assert(emission.reply.body[0] !=
         static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok));
}

void assert_fragment_reply(const SpeakerAssetsEmission& emission,
                           const SpeakerAssetsFrame& request) {
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(emission.reply.opcode == request.opcode);
  assert(emission.reply.request_id == request.request_id);
  assert(
      (emission.reply.flags &
       easy_input::speaker_assets::kSpeakerAssetsFlagResponse) != 0U);
  assert(emission.reply.body_length >= 4U);
  assert(emission.reply.body[0] ==
         static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok));
  assert(emission.reply.body[3] <=
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes -
             4U);
  assert(emission.reply.body_length ==
         static_cast<std::uint16_t>(4U + emission.reply.body[3]));
  // The bitmap byte window begins at or before the first missing byte.
  assert(static_cast<std::uint32_t>(
             read_le16(emission.reply.body.data() + 1U)) *
             8U <=
         emission.reply.object_offset);
}

bool reply_contains_transaction_id(
    const SpeakerAssetsFrame& reply,
    const std::array<
        std::uint8_t,
        easy_input::speaker_assets::kSoundTransactionIdBytes>&
        transaction_id) {
  if (reply.body_length < transaction_id.size()) {
    return false;
  }
  const auto end =
      reply.body.begin() + static_cast<std::ptrdiff_t>(reply.body_length);
  return std::search(reply.body.begin(),
                     end,
                     transaction_id.begin(),
                     transaction_id.end()) != end;
}

SpeakerAssetsActionView drive_begin_to_action(
    SpeakerAssetsSession* session,
    const SpeakerAssetsRouteToken& route,
    const SoundBundlePlan& plan,
    std::uint32_t request_id) {
  assert(session != nullptr);
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);
  static_assert(
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes %
              kUsbFragmentBytes ==
          0U);
  constexpr std::size_t kFragmentCount =
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes /
      kUsbFragmentBytes;

  const auto send_fragment =
      [&](std::size_t index,
          SpeakerAssetsEmissionKind expected_kind) -> SpeakerAssetsEmission {
    assert(index < kFragmentCount);
    std::uint8_t flags =
        easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
    if (index == 0U) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment);
    }
    if (index + 1U == kFragmentCount) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment);
    }
    const auto frame = make_bytes_request(
        SpeakerAssetsOpcode::Begin,
        request_id,
        0U,
        static_cast<std::uint32_t>(index * kUsbFragmentBytes),
        flags,
        encoded.data() + index * kUsbFragmentBytes,
        kUsbFragmentBytes);
    SpeakerAssetsEmission emission{};
    assert(session->consume(route, frame, &emission) ==
           SpeakerAssetsSessionResult::Ok);
    assert(emission.kind == expected_kind);
    if (expected_kind == SpeakerAssetsEmissionKind::Reply) {
      assert_fragment_reply(emission, frame);
    }
    return emission;
  };

  // FIRST establishes the exact owner. Everything after it is deliberately
  // shuffled, LAST arrives while a hole remains, and one middle fragment is
  // duplicated exactly.
  static_cast<void>(
      send_fragment(0U, SpeakerAssetsEmissionKind::Reply));
  static_cast<void>(
      send_fragment(kFragmentCount - 1U,
                    SpeakerAssetsEmissionKind::Reply));
  static_cast<void>(
      send_fragment(5U, SpeakerAssetsEmissionKind::Reply));
  static_cast<void>(
      send_fragment(5U, SpeakerAssetsEmissionKind::Reply));
  for (std::size_t index = kFragmentCount - 2U; index > 0U; --index) {
    if (index == 5U || index == 7U) {
      continue;
    }
    static_cast<void>(
        send_fragment(index, SpeakerAssetsEmissionKind::Reply));
  }

  assert(session->phase() == SpeakerAssetsSessionPhase::BeginAssembly);
  assert(!session->action_pending());
  const auto final =
      send_fragment(7U, SpeakerAssetsEmissionKind::Action);
  assert(session->phase() == SpeakerAssetsSessionPhase::ActionPending);
  assert(session->action_pending());
  assert(final.action.kind == SpeakerAssetsActionKind::Begin);
  assert(final.action.request_id == request_id);
  assert(final.action.session_cookie == 0U);
  assert(routes_equal(final.action.route, route));
  assert(final.action.plan != nullptr);
  assert(plans_equal(*final.action.plan, plan));
  assert(final.action.bytes == nullptr);
  assert(final.action.length == 0U);
  assert(final.action.token != 0U);
  assert(session->pending_action_token() == final.action.token);
  return final.action;
}

SpeakerAssetsEmission drive_plan_to_final_emission(
    SpeakerAssetsSession* session,
    const SpeakerAssetsRouteToken& route,
    const SoundBundlePlan& plan,
    SpeakerAssetsOpcode opcode,
    std::uint32_t request_id) {
  assert(session != nullptr);
  assert(opcode == SpeakerAssetsOpcode::Begin ||
         opcode == SpeakerAssetsOpcode::RecoverInvalidStaging);
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);
  static_assert(
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes %
              kUsbFragmentBytes ==
          0U);
  constexpr std::size_t kFragmentCount =
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes /
      kUsbFragmentBytes;

  SpeakerAssetsEmission final{};
  for (std::size_t index = 0U; index < kFragmentCount; ++index) {
    std::uint8_t flags =
        easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
    if (index == 0U) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment);
    }
    if (index + 1U == kFragmentCount) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment);
    }
    const auto frame = make_bytes_request(
        opcode,
        request_id,
        0U,
        static_cast<std::uint32_t>(index * kUsbFragmentBytes),
        flags,
        encoded.data() + index * kUsbFragmentBytes,
        kUsbFragmentBytes);
    SpeakerAssetsEmission emission{};
    assert(session->consume(route, frame, &emission) ==
           SpeakerAssetsSessionResult::Ok);
    if (index + 1U == kFragmentCount) {
      final = emission;
    } else {
      assert_fragment_reply(emission, frame);
    }
  }
  return final;
}

SoundUpdateProgress establish_ready_session(
    SpeakerAssetsSession* session,
    const SpeakerAssetsRouteToken& route,
    const SoundBundlePlan& plan,
    std::uint32_t begin_request_id,
    std::uint8_t transaction_seed = 0xC0U) {
  const auto action =
      drive_begin_to_action(session, route, plan, begin_request_id);
  const auto progress = make_progress(plan, transaction_seed);
  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.identity_valid = true;
  completion.identity = progress.identity;
  completion.progress_valid = true;
  completion.progress = progress;
  SpeakerAssetsEmission emission{};
  assert(session->complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(emission.reply.opcode ==
         static_cast<std::uint8_t>(SpeakerAssetsOpcode::Begin));
  assert(emission.reply.request_id == begin_request_id);
  assert(emission.reply.body_length >= 1U);
  assert(emission.reply.body[0] ==
         static_cast<std::uint8_t>(SpeakerAssetsStatus::Ok));
  assert(emission.reply.session_cookie != 0U);
  assert(emission.reply.session_cookie == session->session_cookie());
  assert(emission.reply.body_length == 36U);
  assert(emission.reply.body[1] ==
         static_cast<std::uint8_t>(
             progress.identity.target_bank));
  assert(read_le16(emission.reply.body.data() + 2U) ==
         progress.payload_block_count);
  assert(read_le64(emission.reply.body.data() + 4U) ==
         progress.identity.generation);
  assert(read_le32(emission.reply.body.data() + 12U) ==
         progress.manifest_bytes);
  assert(read_le32(emission.reply.body.data() + 16U) ==
         progress.payload_bytes);
  assert(reply_contains_transaction_id(
      emission.reply, progress.identity.transaction_id));
  assert(session->phase() == SpeakerAssetsSessionPhase::Ready);
  assert(session->route_bound());
  assert(routes_equal(session->route(), route));
  assert(!session->action_pending());
  assert(session->progress() != nullptr);
  assert(identities_equal(
      session->progress()->identity, progress.identity));
  return progress;
}

SpeakerAssetsActionView drive_unit_to_action(
    SpeakerAssetsSession* session,
    const SpeakerAssetsRouteToken& route,
    std::uint32_t session_cookie,
    std::uint32_t request_id,
    SpeakerAssetsRegion region,
    std::uint32_t unit_base,
    const std::uint8_t* bytes,
    std::size_t length,
    std::size_t fragment_bytes) {
  assert(session != nullptr);
  assert(bytes != nullptr);
  assert(length != 0U);
  assert(fragment_bytes != 0U);
  assert(fragment_bytes <=
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes);

  std::size_t offset = 0U;
  SpeakerAssetsEmission final{};
  while (offset < length) {
    const auto chunk = std::min(fragment_bytes, length - offset);
    std::uint8_t flags =
        easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
    if (region == SpeakerAssetsRegion::Payload) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagPayloadRegion);
    }
    if (offset == 0U) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment);
    }
    if (offset + chunk == length) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment);
    }
    const auto frame = make_bytes_request(
        SpeakerAssetsOpcode::Data,
        request_id,
        session_cookie,
        unit_base + static_cast<std::uint32_t>(offset),
        flags,
        bytes + offset,
        chunk);
    SpeakerAssetsEmission emission{};
    assert(session->consume(route, frame, &emission) ==
           SpeakerAssetsSessionResult::Ok);
    if (offset + chunk == length) {
      assert(emission.kind == SpeakerAssetsEmissionKind::Action);
      final = emission;
    } else {
      assert_fragment_reply(emission, frame);
    }
    offset += chunk;
  }

  assert(final.action.request_id == request_id);
  assert(final.action.session_cookie == session_cookie);
  assert(routes_equal(final.action.route, route));
  assert(final.action.bytes != nullptr);
  assert(final.action.length == length);
  assert(std::equal(
      final.action.bytes, final.action.bytes + length, bytes));
  assert(final.action.token != 0U);
  return final.action;
}

SpeakerAssetsEmission drive_unit_to_reply(
    SpeakerAssetsSession* session,
    const SpeakerAssetsRouteToken& route,
    std::uint32_t session_cookie,
    std::uint32_t request_id,
    SpeakerAssetsRegion region,
    std::uint32_t unit_base,
    const std::uint8_t* bytes,
    std::size_t length,
    std::size_t fragment_bytes) {
  assert(session != nullptr);
  assert(bytes != nullptr);
  assert(length != 0U);
  assert(fragment_bytes != 0U);
  assert(fragment_bytes <=
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes);

  std::size_t offset = 0U;
  SpeakerAssetsEmission final{};
  while (offset < length) {
    const auto chunk = std::min(fragment_bytes, length - offset);
    std::uint8_t flags =
        easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
    if (region == SpeakerAssetsRegion::Payload) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagPayloadRegion);
    }
    if (offset == 0U) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment);
    }
    if (offset + chunk == length) {
      flags = static_cast<std::uint8_t>(
          flags |
          easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment);
    }
    const auto frame = make_bytes_request(
        SpeakerAssetsOpcode::Data,
        request_id,
        session_cookie,
        unit_base + static_cast<std::uint32_t>(offset),
        flags,
        bytes + offset,
        chunk);
    SpeakerAssetsEmission emission{};
    assert(session->consume(route, frame, &emission) ==
           SpeakerAssetsSessionResult::Ok);
    assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
    if (offset + chunk == length) {
      final = emission;
    } else {
      assert_fragment_reply(emission, frame);
    }
    offset += chunk;
  }
  assert(!session->action_pending());
  return final;
}

SpeakerAssetsEmission complete_success(
    SpeakerAssetsSession* session,
    const SpeakerAssetsActionView& action,
    const SoundUpdateProgress& progress,
    bool progress_valid) {
  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.progress_valid = progress_valid;
  completion.progress = progress;
  if (action.kind == SpeakerAssetsActionKind::ResumeQuery) {
    completion.identity_valid = true;
    completion.identity = progress.identity;
    completion.outcome_valid = true;
    completion.outcome =
        easy_input::speaker_assets::
            SpeakerAssetsTransactionOutcome::Active;
  }
  SpeakerAssetsEmission emission{};
  assert(session->complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  return emission;
}

void capabilities_are_immediate_and_do_not_bind() {
  SpeakerAssetsSession session(0x100U, 0x200U);
  const auto route = usb_route(1U);
  auto request =
      make_control_request(SpeakerAssetsOpcode::Capabilities, 1U, 0U);

  SpeakerAssetsEmission emission{};
  assert(session.consume(route, request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, request, SpeakerAssetsStatus::Ok);
  assert(emission.reply.session_cookie == 0U);
  assert(emission.reply.body_length == 11U);
  assert(emission.reply.body[1] ==
         easy_input::speaker_assets::kSpeakerAssetsProtocolVersion);
  assert(emission.reply.body[2] ==
         easy_input::speaker_assets::kSpeakerAssetsCapabilities);
  assert(emission.reply.body[3] ==
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes);
  assert(emission.reply.body[4] ==
         easy_input::speaker_assets::kSpeakerAssetsWifiFrameBodyBytes);
  assert(read_le16(emission.reply.body.data() + 5U) ==
         easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes);
  assert(read_le16(emission.reply.body.data() + 7U) ==
         easy_input::speaker_assets::kSoundPayloadBlockSize);
  assert(emission.reply.body[9] ==
         easy_input::speaker_assets::
             kSpeakerAssetsProgressBitmapBytes);
  assert(emission.reply.body[10] ==
         easy_input::speaker_assets::kSpeakerAssetsReplayEntries);
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(!session.action_pending());
  assert(session.progress() == nullptr);
}

void current_active_query_is_durable_replayable_and_usb_sized() {
  SpeakerAssetsSession session(0x101U, 0x201U);
  const auto route = usb_route(2U);
  const auto request = make_control_request(
      SpeakerAssetsOpcode::CurrentActive, 0x78563412U, 0U);

  SpeakerAssetsEmission emission{};
  assert(session.consume(route, request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto no_active_action = emission.action;
  assert(no_active_action.kind ==
         SpeakerAssetsActionKind::QueryCurrentActive);
  assert(routes_equal(no_active_action.route, route));
  assert(no_active_action.session_cookie == 0U);

  SpeakerAssetsActionCompletion completion{};
  completion.token = no_active_action.token;
  completion.kind = no_active_action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.current_active_valid = true;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, request, SpeakerAssetsStatus::Ok);
  assert(emission.reply.session_cookie == 0U);
  assert(emission.reply.object_offset == 0U);
  assert(emission.reply.body_length == 38U);
  assert(emission.reply.body[1] == 0xFFU);
  assert(std::all_of(
      emission.reply.body.begin() + 2U,
      emission.reply.body.begin() + emission.reply.body_length,
      [](std::uint8_t value) { return value == 0U; }));
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(!session.route_bound());
  assert(session.replay_entry_count() == 1U);

  const auto absent_reply = emission.reply;
  emission = {};
  assert(session.consume(route, request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(frames_equal(emission.reply, absent_reply));
  assert(!session.action_pending());

  const auto active_request = make_control_request(
      SpeakerAssetsOpcode::CurrentActive, 0x78563413U, 0U);
  emission = {};
  assert(session.consume(route, active_request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto active_action = emission.action;

  SoundBankSnapshot snapshot{};
  snapshot.valid = true;
  snapshot.bank = SoundBankId::B;
  snapshot.generation = 0x1122334455667788ULL;
  snapshot.base_generation = snapshot.generation - 1U;
  snapshot.manifest_bytes = 84U;
  snapshot.payload_bytes = 6872U;
  for (std::size_t index = 0U;
       index < snapshot.transaction_id.size();
       ++index) {
    snapshot.transaction_id[index] =
        static_cast<std::uint8_t>(0x20U + index);
  }
  for (std::size_t index = 0U;
       index < snapshot.bundle_sha256.size();
       ++index) {
    snapshot.manifest_sha256[index] =
        static_cast<std::uint8_t>(0x40U + index);
    snapshot.bundle_sha256[index] =
        static_cast<std::uint8_t>(0x80U + index);
  }

  completion = {};
  completion.token = active_action.token;
  completion.kind = active_action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.current_active_valid = true;
  completion.current_active = snapshot;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, active_request, SpeakerAssetsStatus::Ok);
  assert(emission.reply.session_cookie == 0U);
  assert(emission.reply.object_offset == 0x55667788U);
  assert(emission.reply.body_length ==
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes - 1U);
  assert(emission.reply.body[1] ==
         static_cast<std::uint8_t>(SoundBankId::B));
  assert(read_le32(emission.reply.body.data() + 2U) ==
         0x11223344U);
  assert(std::equal(
      snapshot.bundle_sha256.begin(),
      snapshot.bundle_sha256.end(),
      emission.reply.body.begin() + 6U));

  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsUsbFrameBytes> usb{};
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             emission.reply, &usb) ==
         SpeakerAssetsProtocolResult::Ok);
  constexpr std::array<std::uint8_t, 63> kCurrentActiveUsbGolden{{
      0x45, 0x49, 0x41, 0x01, 0x09, 0x01, 0x26, 0x00,
      0x13, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00,
      0x88, 0x77, 0x66, 0x55, 0x1C, 0x09, 0x7D, 0x7C,
      0x00, 0x01, 0x44, 0x33, 0x22, 0x11, 0x80, 0x81,
      0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
      0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91,
      0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
      0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0x00,
  }};
  assert(usb == kCurrentActiveUsbGolden);
  SpeakerAssetsFrame decoded{};
  assert(easy_input::speaker_assets::decode_speaker_assets_usb_frame(
             usb.data(), usb.size(), &decoded) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(frames_equal(decoded, emission.reply));

  SpeakerAssetsSession invalid_completion_session(0x102U, 0x202U);
  const auto invalid_request = make_control_request(
      SpeakerAssetsOpcode::CurrentActive, 0x78563414U, 0U);
  emission = {};
  assert(invalid_completion_session.consume(
             route, invalid_request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  completion = {};
  completion.token = emission.action.token;
  completion.kind = emission.action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.current_active = snapshot;
  emission = {};
  assert(invalid_completion_session.complete(
             completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(
      emission, invalid_request, SpeakerAssetsStatus::InternalError);
  assert(invalid_completion_session.phase() ==
         SpeakerAssetsSessionPhase::Idle);
  assert(!invalid_completion_session.route_bound());

  const auto invalid_base_request = make_control_request(
      SpeakerAssetsOpcode::CurrentActive, 0x78563415U, 0U);
  emission = {};
  assert(invalid_completion_session.consume(
             route, invalid_base_request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  completion = {};
  completion.token = emission.action.token;
  completion.kind = emission.action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.current_active_valid = true;
  completion.current_active = snapshot;
  --completion.current_active.base_generation;
  emission = {};
  assert(invalid_completion_session.complete(
             completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(
      emission, invalid_base_request,
      SpeakerAssetsStatus::InternalError);
  assert(invalid_completion_session.phase() ==
         SpeakerAssetsSessionPhase::Idle);
  assert(!invalid_completion_session.route_bound());
}

void begin_usb_fragments_wait_for_every_hole_and_return_identity() {
  SpeakerAssetsSession session(0x12345678U, 0x10203040U);
  const auto route = usb_route(11U);
  const auto plan = make_plan();
  const auto progress =
      establish_ready_session(&session, route, plan, 100U);

  assert(session.session_cookie() != 0U);
  assert(session.replay_entry_count() == 1U);
  assert(session.progress() != nullptr);
  assert(session.progress()->manifest_bytes == plan.manifest_bytes);
  assert(session.progress()->payload_bytes == plan.payload_bytes);
  assert(session.progress()->payload_block_count == 2U);
  assert(identities_equal(
      session.progress()->identity, progress.identity));
}

void manifest_and_tail_payload_blocks_are_borrowed_until_completion() {
  SpeakerAssetsSession session(0x200U, 0x300U);
  const auto route = usb_route(12U);
  const auto plan = make_plan();
  auto progress =
      establish_ready_session(&session, route, plan, 200U, 0x90U);
  const auto cookie = session.session_cookie();

  std::array<std::uint8_t, 84U> manifest{};
  for (std::size_t index = 0U; index < manifest.size(); ++index) {
    manifest[index] =
        static_cast<std::uint8_t>(0x31U + index * 7U);
  }
  const auto manifest_action = drive_unit_to_action(
      &session,
      route,
      cookie,
      201U,
      SpeakerAssetsRegion::Manifest,
      0U,
      manifest.data(),
      manifest.size(),
      31U);
  assert(manifest_action.kind ==
         SpeakerAssetsActionKind::WriteManifest);
  assert(manifest_action.block_index == 0U);

  const auto* borrowed_manifest = manifest_action.bytes;
  progress.manifest_complete = true;
  auto emission =
      complete_success(&session, manifest_action, progress, true);
  auto manifest_request =
      make_control_request(SpeakerAssetsOpcode::Data, 201U, cookie);
  assert_reply(emission, manifest_request, SpeakerAssetsStatus::Ok);
  const auto manifest_reply = emission.reply;
  assert(session.phase() == SpeakerAssetsSessionPhase::Ready);
  assert(session.progress() != nullptr);
  assert(session.progress()->manifest_complete);
  assert(borrowed_manifest != nullptr);

  // Retrying the complete logical unit reproduces the cached durable reply
  // without a second Store action. Reusing the same request ID with different
  // bytes is a conflict, not a new write.
  emission = drive_unit_to_reply(
      &session,
      route,
      cookie,
      201U,
      SpeakerAssetsRegion::Manifest,
      0U,
      manifest.data(),
      manifest.size(),
      31U);
  assert(frames_equal(emission.reply, manifest_reply));
  auto conflicting_manifest = manifest;
  conflicting_manifest[47U] ^= 0x80U;
  emission = drive_unit_to_reply(
      &session,
      route,
      cookie,
      201U,
      SpeakerAssetsRegion::Manifest,
      0U,
      conflicting_manifest.data(),
      conflicting_manifest.size(),
      31U);
  assert_error_reply(emission, manifest_request);
  assert(session.phase() == SpeakerAssetsSessionPhase::Ready);
  assert(session.progress() != nullptr);
  assert(session.progress()->manifest_complete);

  std::array<std::uint8_t, 73U> tail{};
  for (std::size_t index = 0U; index < tail.size(); ++index) {
    tail[index] =
        static_cast<std::uint8_t>(0xE0U ^ (index * 11U));
  }
  const auto tail_action = drive_unit_to_action(
      &session,
      route,
      cookie,
      202U,
      SpeakerAssetsRegion::Payload,
      easy_input::speaker_assets::kSoundPayloadBlockSize,
      tail.data(),
      tail.size(),
      37U);
  assert(tail_action.kind ==
         SpeakerAssetsActionKind::WritePayloadBlock);
  assert(tail_action.block_index == 1U);
  assert(tail_action.length == tail.size());
  assert(std::equal(
      tail_action.bytes,
      tail_action.bytes + tail_action.length,
      tail.begin()));

  progress.payload_complete_bitmap[0] = 0x02U;
  emission = complete_success(&session, tail_action, progress, true);
  auto tail_request =
      make_control_request(SpeakerAssetsOpcode::Data, 202U, cookie);
  assert_reply(emission, tail_request, SpeakerAssetsStatus::Ok);
  assert(session.progress() != nullptr);
  assert(session.progress()->payload_complete_bitmap[0] == 0x02U);
  assert(session.session_cookie() == cookie);
}

void query_commit_and_abort_emit_actions_and_replay_terminal_replies() {
  const auto route = usb_route(13U);
  const auto plan = make_plan();

  SpeakerAssetsSession commit_session(0x300U, 0x400U);
  auto progress = establish_ready_session(
      &commit_session, route, plan, 300U, 0x40U);
  const auto commit_cookie = commit_session.session_cookie();

  const auto query =
      make_control_request(
          SpeakerAssetsOpcode::Query, 301U, commit_cookie);
  SpeakerAssetsEmission emission{};
  assert(commit_session.consume(route, query, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto query_action = emission.action;
  assert(query_action.kind ==
         SpeakerAssetsActionKind::ResumeQuery);
  assert(query_action.query_mode ==
         SpeakerAssetsResumeQueryMode::CurrentProgress);
  assert(identities_equal(
      query_action.expected_identity, progress.identity));

  emission =
      complete_success(&commit_session, query_action, progress, true);
  assert_reply(emission, query, SpeakerAssetsStatus::Ok);
  const auto query_reply = emission.reply;
  assert(query_reply.body_length <=
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes);

  emission = {};
  assert(commit_session.consume(route, query, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(frames_equal(emission.reply, query_reply));
  assert(!commit_session.action_pending());

  auto conflicting_request =
      make_control_request(
          SpeakerAssetsOpcode::Commit, 301U, commit_cookie);
  emission = {};
  assert(commit_session.consume(route, conflicting_request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_error_reply(emission, conflicting_request);
  assert(!commit_session.action_pending());

  const auto commit =
      make_control_request(
          SpeakerAssetsOpcode::Commit, 302U, commit_cookie);
  emission = {};
  assert(commit_session.consume(route, commit, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto commit_action = emission.action;
  assert(commit_action.kind == SpeakerAssetsActionKind::Commit);
  assert(identities_equal(
      commit_action.expected_identity, progress.identity));
  emission =
      complete_success(&commit_session, commit_action, progress, false);
  assert_reply(emission, commit, SpeakerAssetsStatus::Ok);
  const auto commit_reply = emission.reply;
  assert(!commit_session.route_bound());
  assert(commit_session.session_cookie() == 0U);

  // A successful terminal command keeps a bounded tombstone, so a lost reply
  // cannot execute COMMIT a second time after the cookie is revoked.
  emission = {};
  assert(commit_session.consume(route, commit, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(frames_equal(emission.reply, commit_reply));
  assert(!commit_session.action_pending());

  SpeakerAssetsSession abort_session(0x500U, 0x600U);
  progress = establish_ready_session(
      &abort_session, route, plan, 310U, 0x60U);
  const auto abort =
      make_control_request(
          SpeakerAssetsOpcode::Abort,
          311U,
          abort_session.session_cookie());
  emission = {};
  assert(abort_session.consume(route, abort, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto abort_action = emission.action;
  assert(abort_action.kind == SpeakerAssetsActionKind::Abort);
  assert(identities_equal(
      abort_action.expected_identity, progress.identity));
  emission =
      complete_success(&abort_session, abort_action, progress, false);
  assert_reply(emission, abort, SpeakerAssetsStatus::Ok);
  const auto abort_reply = emission.reply;
  assert(!abort_session.route_bound());

  emission = {};
  assert(abort_session.consume(route, abort, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(frames_equal(emission.reply, abort_reply));
  assert(!abort_session.action_pending());
}

void route_generation_change_revokes_cookie_and_requires_resume() {
  SpeakerAssetsSession session(0x700U, 0x800U);
  const auto first_route = usb_route(21U);
  const auto second_route = usb_route(22U);
  const auto plan = make_plan();
  const auto progress = establish_ready_session(
      &session, first_route, plan, 400U, 0x20U);
  const auto stale_cookie = session.session_cookie();

  session.route_closed(first_route);
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(session.progress() == nullptr);

  const auto stale_query =
      make_control_request(
          SpeakerAssetsOpcode::Query, 401U, stale_cookie);
  SpeakerAssetsEmission emission{};
  assert(session.consume(second_route, stale_query, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_error_reply(emission, stale_query);
  assert(!session.route_bound());
  assert(!session.action_pending());

  auto resume =
      make_control_request(SpeakerAssetsOpcode::Resume, 402U, 0U);
  resume.body_length =
      easy_input::speaker_assets::kSoundTransactionIdBytes;
  std::copy(
      progress.identity.transaction_id.begin(),
      progress.identity.transaction_id.end(),
      resume.body.begin());
  emission = {};
  assert(session.consume(second_route, resume, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto resume_action = emission.action;
  assert(resume_action.kind ==
         SpeakerAssetsActionKind::ResumeQuery);
  assert(resume_action.query_mode ==
         SpeakerAssetsResumeQueryMode::ResumeOrRebind);
  assert(resume_action.expected_identity.transaction_id ==
         progress.identity.transaction_id);

  emission =
      complete_success(&session, resume_action, progress, true);
  assert_reply(emission, resume, SpeakerAssetsStatus::Ok);
  assert(session.route_bound());
  assert(routes_equal(session.route(), second_route));
  assert(session.session_cookie() != 0U);
  assert(session.session_cookie() != stale_cookie);
  assert(reply_contains_transaction_id(
      emission.reply, progress.identity.transaction_id));
}

void route_close_orphans_pending_action_without_synthesizing_abort() {
  SpeakerAssetsSession session(0x900U, 0xA00U);
  const auto route = usb_route(31U);
  const auto plan = make_plan();
  const auto progress =
      establish_ready_session(&session, route, plan, 500U, 0x30U);
  const auto replay_entries = session.replay_entry_count();
  assert(replay_entries != 0U);

  const auto query =
      make_control_request(
          SpeakerAssetsOpcode::Query,
          501U,
          session.session_cookie());
  SpeakerAssetsEmission emission{};
  assert(session.consume(route, query, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  assert(emission.action.kind ==
         SpeakerAssetsActionKind::ResumeQuery);
  const auto query_action = emission.action;

  session.route_closed(route);
  assert(session.action_pending());
  assert(session.pending_action_token() == query_action.token);
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(session.replay_entry_count() == 0U);

  emission = complete_success(
      &session, query_action, progress, true);
  assert(emission.kind == SpeakerAssetsEmissionKind::None);
  assert(!session.action_pending());
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(session.replay_entry_count() == 0U);
}

void stale_and_duplicate_completions_never_finish_the_wrong_action() {
  SpeakerAssetsSession session(0xB00U, 0xC00U);
  const auto route = usb_route(41U);
  const auto plan = make_plan();
  const auto progress =
      establish_ready_session(&session, route, plan, 600U, 0x50U);

  SpeakerAssetsActionCompletion completion{};
  SpeakerAssetsEmission emission{};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::NoPendingAction);
  assert(emission.kind == SpeakerAssetsEmissionKind::None);

  const auto query =
      make_control_request(
          SpeakerAssetsOpcode::Query,
          601U,
          session.session_cookie());
  assert(session.consume(route, query, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto action = emission.action;

  completion = {};
  completion.token = action.token + 1U;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.identity_valid = true;
  completion.identity = progress.identity;
  completion.outcome_valid = true;
  completion.outcome =
      easy_input::speaker_assets::
          SpeakerAssetsTransactionOutcome::Active;
  completion.progress_valid = true;
  completion.progress = progress;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::StaleCompletion);
  assert(emission.kind == SpeakerAssetsEmissionKind::None);
  assert(session.action_pending());
  assert(session.pending_action_token() == action.token);

  completion.token = action.token;
  completion.kind = SpeakerAssetsActionKind::Commit;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::StaleCompletion);
  assert(emission.kind == SpeakerAssetsEmissionKind::None);
  assert(session.action_pending());

  completion.kind = action.kind;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, query, SpeakerAssetsStatus::Ok);
  assert(!session.action_pending());

  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::NoPendingAction);
  assert(emission.kind == SpeakerAssetsEmissionKind::None);

  const auto error_query =
      make_control_request(
          SpeakerAssetsOpcode::Query,
          602U,
          session.session_cookie());
  assert(session.consume(route, error_query, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto error_action = emission.action;
  completion = {};
  completion.token = error_action.token;
  completion.kind = error_action.kind;
  completion.result = SoundStoreResult::Unavailable;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(
      emission,
      error_query,
      easy_input::speaker_assets::
          speaker_assets_status_from_store_result(
              SoundStoreResult::Unavailable));
  assert(session.route_bound());
  assert(!session.action_pending());
}

void mismatched_success_progress_fails_closed_without_issuing_cookie() {
  SpeakerAssetsSession session(0xD00U, 0xE00U);
  const auto route = usb_route(51U);
  const auto plan = make_plan();
  const auto action =
      drive_begin_to_action(&session, route, plan, 700U);
  auto mismatched_progress = make_progress(plan, 0x70U);
  ++mismatched_progress.payload_bytes;

  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.identity_valid = true;
  completion.identity = mismatched_progress.identity;
  completion.progress_valid = true;
  completion.progress = mismatched_progress;
  SpeakerAssetsEmission emission{};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  auto begin_request =
      make_control_request(SpeakerAssetsOpcode::Begin, 700U, 0U);
  assert_error_reply(emission, begin_request);
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(session.progress() == nullptr);
  assert(!session.action_pending());
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
}

void begin_identity_disagreement_fails_closed() {
  SpeakerAssetsSession session(0xF00U, 0x1000U);
  const auto route = usb_route(52U);
  const auto plan = make_plan();
  const auto action =
      drive_begin_to_action(&session, route, plan, 710U);
  const auto progress = make_progress(plan, 0x72U);

  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.identity_valid = true;
  completion.identity = progress.identity;
  ++completion.identity.generation;
  completion.progress_valid = true;
  completion.progress = progress;
  SpeakerAssetsEmission emission{};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  const auto request =
      make_control_request(SpeakerAssetsOpcode::Begin, 710U, 0U);
  assert_error_reply(emission, request);
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
}

void partial_assembly_timeout_only_clears_ram() {
  const auto route = usb_route(61U);
  const auto unrelated = usb_route(62U);
  const auto plan = make_plan();
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> wire{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &wire) == SpeakerAssetsProtocolResult::Ok);

  SpeakerAssetsSession begin_session(0x1100U, 0x1200U);
  const auto first_begin = make_bytes_request(
      SpeakerAssetsOpcode::Begin,
      720U,
      0U,
      0U,
      static_cast<std::uint8_t>(
          easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
          easy_input::speaker_assets::
              kSpeakerAssetsFlagFirstFragment),
      wire.data(),
      32U);
  SpeakerAssetsEmission emission{};
  assert(begin_session.consume(route, first_begin, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_begin);
  assert(begin_session.phase() ==
         SpeakerAssetsSessionPhase::BeginAssembly);
  const auto begin_activity =
      begin_session.partial_activity_counter();
  emission = {};
  assert(begin_session.consume(route, first_begin, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_begin);
  assert(begin_session.partial_activity_counter() ==
         begin_activity);
  assert(!begin_session.expire_partial(unrelated));
  assert(begin_session.expire_partial(route));
  assert(begin_session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(!begin_session.action_pending());
  assert(!begin_session.route_bound());

  SpeakerAssetsSession data_session(0x1300U, 0x1400U);
  static_cast<void>(
      establish_ready_session(
          &data_session, route, plan, 721U, 0x74U));
  const auto cookie = data_session.session_cookie();
  std::array<std::uint8_t, 20> bytes{};
  bytes.fill(0xA5U);
  const auto first_data = make_bytes_request(
      SpeakerAssetsOpcode::Data,
      722U,
      cookie,
      0U,
      static_cast<std::uint8_t>(
          easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
          easy_input::speaker_assets::
              kSpeakerAssetsFlagFirstFragment),
      bytes.data(),
      bytes.size());
  emission = {};
  assert(data_session.consume(route, first_data, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_data);
  assert(data_session.phase() ==
         SpeakerAssetsSessionPhase::UnitAssembly);
  const auto data_activity =
      data_session.partial_activity_counter();
  emission = {};
  assert(data_session.consume(route, first_data, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_data);
  assert(data_session.partial_activity_counter() ==
         data_activity);
  assert(!data_session.expire_partial(unrelated));
  assert(data_session.expire_partial(route));
  assert(data_session.phase() == SpeakerAssetsSessionPhase::Ready);
  assert(data_session.route_bound());
  assert(data_session.session_cookie() == cookie);
  assert(!data_session.action_pending());
}

void resume_resolves_committed_and_unknown_terminal_outcomes() {
  const auto route = usb_route(71U);
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSoundTransactionIdBytes> transaction{};
  for (std::size_t index = 0U; index < transaction.size(); ++index) {
    transaction[index] =
        static_cast<std::uint8_t>(0x80U + index);
  }
  const auto make_resume = [&](std::uint32_t request_id) {
    auto frame = make_control_request(
        SpeakerAssetsOpcode::Resume, request_id, 0U);
    frame.body_length = static_cast<std::uint16_t>(
        transaction.size());
    std::copy(
        transaction.begin(), transaction.end(), frame.body.begin());
    return frame;
  };

  SpeakerAssetsSession committed_session(0x1500U, 0x1600U);
  const auto committed_request = make_resume(730U);
  SpeakerAssetsEmission emission{};
  assert(committed_session.consume(
             route, committed_request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  SpeakerAssetsActionCompletion completion{};
  completion.token = emission.action.token;
  completion.kind = emission.action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.identity_valid = true;
  completion.identity.generation = 9U;
  completion.identity.target_bank = SoundBankId::B;
  completion.identity.transaction_id = transaction;
  completion.outcome_valid = true;
  completion.outcome =
      SpeakerAssetsTransactionOutcome::Committed;
  completion.outcome_manifest_bytes = 84U;
  completion.outcome_payload_bytes = 4169U;
  emission = {};
  assert(committed_session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(
      emission, committed_request, SpeakerAssetsStatus::Ok);
  assert(emission.reply.session_cookie == 0U);
  assert(emission.reply.body_length == 36U);
  assert(emission.reply.body[1] ==
         static_cast<std::uint8_t>(
             SpeakerAssetsTransactionOutcome::Committed));
  assert(reply_contains_transaction_id(
      emission.reply, transaction));
  assert(!committed_session.route_bound());

  SpeakerAssetsSession unknown_session(0x1700U, 0x1800U);
  const auto unknown_request = make_resume(731U);
  emission = {};
  assert(unknown_session.consume(route, unknown_request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  completion = {};
  completion.token = emission.action.token;
  completion.kind = emission.action.kind;
  completion.result = SoundStoreResult::Ok;
  completion.identity_valid = true;
  completion.identity.transaction_id = transaction;
  completion.outcome_valid = true;
  completion.outcome = SpeakerAssetsTransactionOutcome::Unknown;
  emission = {};
  assert(unknown_session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, unknown_request, SpeakerAssetsStatus::Ok);
  assert(emission.reply.session_cookie == 0U);
  assert(emission.reply.body[1] ==
         static_cast<std::uint8_t>(
             SpeakerAssetsTransactionOutcome::Unknown));
  assert(emission.reply.body[2] == 0xFFU);
  assert(read_le64(emission.reply.body.data() + 4U) == 0U);
  assert(read_le32(emission.reply.body.data() + 28U) == 0U);
  assert(read_le32(emission.reply.body.data() + 32U) == 0U);
  assert(reply_contains_transaction_id(
      emission.reply, transaction));
  assert(!unknown_session.route_bound());
}

void recover_invalid_staging_completes_and_exactly_replays() {
  SpeakerAssetsSession session(0x1900U, 0x1A00U);
  const auto route = usb_route(81U);
  const auto plan = make_plan();
  constexpr std::uint32_t kRequestId = 740U;

  auto emission = drive_plan_to_final_emission(
      &session,
      route,
      plan,
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      kRequestId);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto action = emission.action;
  assert(action.kind ==
         SpeakerAssetsActionKind::DiscardInvalidStaging);
  assert(action.request_id == kRequestId);
  assert(action.session_cookie == 0U);
  assert(routes_equal(action.route, route));
  assert(action.plan != nullptr);
  assert(plans_equal(*action.plan, plan));
  assert(action.bytes == nullptr);
  assert(action.length == 0U);
  assert(action.token != 0U);
  assert(session.phase() == SpeakerAssetsSessionPhase::ActionPending);
  assert(session.action_pending());

  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  const auto request = make_control_request(
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      kRequestId,
      0U);
  assert_reply(emission, request, SpeakerAssetsStatus::Ok);
  assert(emission.reply.session_cookie == 0U);
  assert(emission.reply.body_length == 1U);
  const auto success_reply = emission.reply;
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(!session.action_pending());
  assert(session.progress() == nullptr);
  assert(session.replay_entry_count() == 1U);

  emission = drive_plan_to_final_emission(
      &session,
      route,
      plan,
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      kRequestId);
  assert(emission.kind == SpeakerAssetsEmissionKind::Reply);
  assert(frames_equal(emission.reply, success_reply));
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(!session.route_bound());
  assert(!session.action_pending());
  assert(session.replay_entry_count() == 1U);
}

void recover_invalid_staging_failure_maps_and_returns_idle() {
  SpeakerAssetsSession session(0x1B00U, 0x1C00U);
  const auto route = usb_route(82U);
  const auto plan = make_plan();
  constexpr std::uint32_t kRequestId = 741U;

  auto emission = drive_plan_to_final_emission(
      &session,
      route,
      plan,
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      kRequestId);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto action = emission.action;
  assert(action.kind ==
         SpeakerAssetsActionKind::DiscardInvalidStaging);

  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::IoError;
  emission = {};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  const auto request = make_control_request(
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      kRequestId,
      0U);
  assert_reply(
      emission,
      request,
      easy_input::speaker_assets::
          speaker_assets_status_from_store_result(
              SoundStoreResult::IoError));
  assert(emission.reply.body[0] ==
         static_cast<std::uint8_t>(
             SpeakerAssetsStatus::StorageUnavailable));
  assert(emission.reply.session_cookie == 0U);
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(!session.route_bound());
  assert(session.session_cookie() == 0U);
  assert(!session.action_pending());
  assert(session.progress() == nullptr);
  assert(session.replay_entry_count() == 0U);
}

void only_invalid_staging_requests_explicit_recovery() {
  assert(easy_input::speaker_assets::
             speaker_assets_status_from_store_result(
                 SoundStoreResult::InvalidStaging) ==
         SpeakerAssetsStatus::RecoveryRequired);
  assert(easy_input::speaker_assets::
             speaker_assets_status_from_store_result(
                 SoundStoreResult::InvalidBank) ==
         SpeakerAssetsStatus::IntegrityError);
}

void begin_and_recover_partial_assemblies_cannot_be_mixed() {
  const auto route = usb_route(83U);
  const auto plan = make_plan();
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);
  const auto first_flags = static_cast<std::uint8_t>(
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
      easy_input::speaker_assets::kSpeakerAssetsFlagFirstFragment);
  const auto middle_flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;

  SpeakerAssetsSession begin_owner(0x1D00U, 0x1E00U);
  const auto first_begin = make_bytes_request(
      SpeakerAssetsOpcode::Begin,
      742U,
      0U,
      0U,
      first_flags,
      encoded.data(),
      kUsbFragmentBytes);
  SpeakerAssetsEmission emission{};
  assert(begin_owner.consume(route, first_begin, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_begin);
  const auto mixed_recover = make_bytes_request(
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      742U,
      0U,
      static_cast<std::uint32_t>(kUsbFragmentBytes),
      middle_flags,
      encoded.data() + kUsbFragmentBytes,
      kUsbFragmentBytes);
  emission = {};
  assert(begin_owner.consume(route, mixed_recover, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, mixed_recover, SpeakerAssetsStatus::Busy);
  assert(begin_owner.phase() ==
         SpeakerAssetsSessionPhase::BeginAssembly);
  assert(begin_owner.expire_partial(route));
  assert(begin_owner.phase() == SpeakerAssetsSessionPhase::Idle);

  SpeakerAssetsSession recover_owner(0x1F00U, 0x2000U);
  const auto first_recover = make_bytes_request(
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      743U,
      0U,
      0U,
      first_flags,
      encoded.data(),
      kUsbFragmentBytes);
  emission = {};
  assert(recover_owner.consume(route, first_recover, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_recover);
  const auto mixed_begin = make_bytes_request(
      SpeakerAssetsOpcode::Begin,
      743U,
      0U,
      static_cast<std::uint32_t>(kUsbFragmentBytes),
      middle_flags,
      encoded.data() + kUsbFragmentBytes,
      kUsbFragmentBytes);
  emission = {};
  assert(recover_owner.consume(route, mixed_begin, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, mixed_begin, SpeakerAssetsStatus::Busy);
  assert(recover_owner.phase() ==
         SpeakerAssetsSessionPhase::BeginAssembly);
  assert(recover_owner.expire_partial(route));
  assert(recover_owner.phase() == SpeakerAssetsSessionPhase::Idle);
}

void ready_session_rejects_recover_invalid_staging() {
  SpeakerAssetsSession session(0x2100U, 0x2200U);
  const auto route = usb_route(84U);
  const auto plan = make_plan();
  static_cast<void>(
      establish_ready_session(&session, route, plan, 744U, 0x90U));
  const auto cookie = session.session_cookie();

  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);
  const auto request = make_bytes_request(
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      745U,
      0U,
      0U,
      static_cast<std::uint8_t>(
          easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
          easy_input::speaker_assets::
              kSpeakerAssetsFlagFirstFragment),
      encoded.data(),
      kUsbFragmentBytes);
  SpeakerAssetsEmission emission{};
  assert(session.consume(route, request, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_reply(emission, request, SpeakerAssetsStatus::Busy);
  assert(session.phase() == SpeakerAssetsSessionPhase::Ready);
  assert(session.route_bound());
  assert(routes_equal(session.route(), route));
  assert(session.session_cookie() == cookie);
  assert(!session.action_pending());
}

void all_routes_close_preserves_borrowed_write_until_completion() {
  SpeakerAssetsSession session(0x2900U, 0x2A00U);
  const auto route = usb_route(87U);
  const auto plan = make_plan();
  static_cast<void>(
      establish_ready_session(&session, route, plan, 748U, 0xA0U));
  const auto cookie = session.session_cookie();
  std::array<std::uint8_t, 84U> manifest{};
  for (std::size_t index = 0U; index < manifest.size(); ++index) {
    manifest[index] =
        static_cast<std::uint8_t>(0x53U ^ (index * 9U));
  }
  const auto action = drive_unit_to_action(
      &session,
      route,
      cookie,
      749U,
      SpeakerAssetsRegion::Manifest,
      0U,
      manifest.data(),
      manifest.size(),
      29U);
  assert(action.kind == SpeakerAssetsActionKind::WriteManifest);
  assert(action.bytes != nullptr);
  assert(action.length == manifest.size());

  session.all_routes_closed();
  assert(session.action_pending());
  assert(!session.route_bound());
  assert(std::equal(
      action.bytes,
      action.bytes + action.length,
      manifest.begin()));

  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  SpeakerAssetsEmission emission{};
  assert(session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::None);
  assert(!session.action_pending());
  assert(session.phase() == SpeakerAssetsSessionPhase::Idle);
}

void recover_partial_expiry_and_route_close_clear_only_ram() {
  const auto route = usb_route(85U);
  const auto unrelated = usb_route(86U);
  const auto plan = make_plan();
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);
  const auto first_recover = make_bytes_request(
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      746U,
      0U,
      0U,
      static_cast<std::uint8_t>(
          easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
          easy_input::speaker_assets::
              kSpeakerAssetsFlagFirstFragment),
      encoded.data(),
      kUsbFragmentBytes);

  SpeakerAssetsSession expiry_session(0x2300U, 0x2400U);
  SpeakerAssetsEmission emission{};
  assert(expiry_session.consume(route, first_recover, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_recover);
  assert(!expiry_session.expire_partial(unrelated));
  assert(expiry_session.expire_partial(route));
  assert(expiry_session.phase() == SpeakerAssetsSessionPhase::Idle);
  assert(!expiry_session.action_pending());
  assert(!expiry_session.route_bound());

  SpeakerAssetsSession partial_close_session(0x2500U, 0x2600U);
  emission = {};
  assert(partial_close_session.consume(
             route, first_recover, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert_fragment_reply(emission, first_recover);
  partial_close_session.route_closed(unrelated);
  assert(partial_close_session.phase() ==
         SpeakerAssetsSessionPhase::BeginAssembly);
  partial_close_session.route_closed(route);
  assert(partial_close_session.phase() ==
         SpeakerAssetsSessionPhase::Idle);
  assert(!partial_close_session.action_pending());
  assert(!partial_close_session.route_bound());

  SpeakerAssetsSession pending_close_session(0x2700U, 0x2800U);
  emission = drive_plan_to_final_emission(
      &pending_close_session,
      route,
      plan,
      SpeakerAssetsOpcode::RecoverInvalidStaging,
      747U);
  assert(emission.kind == SpeakerAssetsEmissionKind::Action);
  const auto action = emission.action;
  assert(action.kind ==
         SpeakerAssetsActionKind::DiscardInvalidStaging);
  assert(pending_close_session.replay_entry_count() == 0U);
  pending_close_session.route_closed(route);
  assert(pending_close_session.action_pending());
  assert(pending_close_session.pending_action_token() ==
         action.token);
  assert(!pending_close_session.route_bound());

  SpeakerAssetsActionCompletion completion{};
  completion.token = action.token;
  completion.kind = action.kind;
  completion.result = SoundStoreResult::Ok;
  emission = {};
  assert(pending_close_session.complete(completion, &emission) ==
         SpeakerAssetsSessionResult::Ok);
  assert(emission.kind == SpeakerAssetsEmissionKind::None);
  assert(pending_close_session.phase() ==
         SpeakerAssetsSessionPhase::Idle);
  assert(!pending_close_session.action_pending());
  assert(!pending_close_session.route_bound());
  assert(pending_close_session.session_cookie() == 0U);
  assert(pending_close_session.progress() == nullptr);
  assert(pending_close_session.replay_entry_count() == 0U);
}

}  // namespace

int main() {
  static_assert(sizeof(SpeakerAssetsSession) <= kSessionRamBudgetBytes);
  capabilities_are_immediate_and_do_not_bind();
  current_active_query_is_durable_replayable_and_usb_sized();
  begin_usb_fragments_wait_for_every_hole_and_return_identity();
  manifest_and_tail_payload_blocks_are_borrowed_until_completion();
  query_commit_and_abort_emit_actions_and_replay_terminal_replies();
  route_generation_change_revokes_cookie_and_requires_resume();
  route_close_orphans_pending_action_without_synthesizing_abort();
  stale_and_duplicate_completions_never_finish_the_wrong_action();
  mismatched_success_progress_fails_closed_without_issuing_cookie();
  begin_identity_disagreement_fails_closed();
  partial_assembly_timeout_only_clears_ram();
  resume_resolves_committed_and_unknown_terminal_outcomes();
  recover_invalid_staging_completes_and_exactly_replays();
  recover_invalid_staging_failure_maps_and_returns_idle();
  only_invalid_staging_requests_explicit_recovery();
  begin_and_recover_partial_assemblies_cannot_be_mixed();
  ready_session_rejects_recover_invalid_staging();
  all_routes_close_preserves_borrowed_write_until_completion();
  recover_partial_expiry_and_route_close_clear_only_ram();
  return 0;
}
