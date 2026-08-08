#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "speaker_assets/speaker_assets_protocol.h"
#include "speaker_assets/sound_asset_crypto.h"

namespace {

using easy_input::speaker_assets::SoundBundlePlan;
using easy_input::speaker_assets::SpeakerAssetsBlockAssembler;
using easy_input::speaker_assets::SpeakerAssetsFrame;
using easy_input::speaker_assets::SpeakerAssetsPlanAssembler;
using easy_input::speaker_assets::SpeakerAssetsProtocolResult;
using easy_input::speaker_assets::SpeakerAssetsRegion;
using easy_input::speaker_assets::SpeakerAssetsRouteToken;
using easy_input::speaker_assets::SpeakerAssetsTransport;

SoundBundlePlan make_plan(std::uint32_t payload_bytes = 6872U) {
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
  plan.manifest_bytes = 84U;
  plan.payload_bytes = payload_bytes;
  plan.manifest_crc32 = 0x78563412U;
  const auto blocks =
      (static_cast<std::size_t>(payload_bytes) +
       easy_input::speaker_assets::kSoundPayloadBlockSize - 1U) /
      easy_input::speaker_assets::kSoundPayloadBlockSize;
  for (std::size_t index = 0U; index < blocks; ++index) {
    plan.payload_block_crc32[index] =
        static_cast<std::uint32_t>(0xA0B00000U + index);
  }
  return plan;
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

void frame_round_trip_and_padding_are_canonical() {
  SpeakerAssetsFrame frame{};
  frame.opcode = static_cast<std::uint8_t>(
      easy_input::speaker_assets::SpeakerAssetsOpcode::Data);
  frame.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested |
      easy_input::speaker_assets::kSpeakerAssetsFlagLastFragment;
  frame.request_id = 0x78563412U;
  frame.session_cookie = 0x11223344U;
  frame.object_offset = 0xA0B0C0D0U;
  frame.body_length =
      easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes;
  for (std::size_t index = 0U; index < frame.body.size(); ++index) {
    frame.body[index] = static_cast<std::uint8_t>(index * 3U + 1U);
  }

  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsUsbFrameBytes> encoded{};
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             frame, &encoded) == SpeakerAssetsProtocolResult::Ok);
  assert(encoded[0] == 'E' && encoded[1] == 'I' && encoded[2] == 'A');
  assert(encoded[3] ==
         easy_input::speaker_assets::kSpeakerAssetsProtocolVersion);
  assert(encoded[6] == frame.body_length);

  SpeakerAssetsFrame decoded{};
  assert(easy_input::speaker_assets::decode_speaker_assets_usb_frame(
             encoded.data(), encoded.size(), &decoded) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(decoded.opcode == frame.opcode);
  assert(decoded.flags == frame.flags);
  assert(decoded.request_id == frame.request_id);
  assert(decoded.session_cookie == frame.session_cookie);
  assert(decoded.object_offset == frame.object_offset);
  assert(decoded.body_length == frame.body_length);
  assert(std::equal(
      decoded.body.begin(),
      decoded.body.begin() + decoded.body_length,
      frame.body.begin()));
  assert(std::all_of(
      decoded.body.begin() + decoded.body_length,
      decoded.body.end(),
      [](std::uint8_t value) { return value == 0U; }));

  frame.body_length = 3U;
  frame.body.fill(0xA5U);
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             frame, &encoded) == SpeakerAssetsProtocolResult::Ok);
  assert(encoded[24] == 0xA5U);
  assert(encoded[25] == 0xA5U);
  assert(encoded[26] == 0xA5U);
  assert(std::all_of(
      encoded.begin() + 27U,
      encoded.end(),
      [](std::uint8_t value) { return value == 0U; }));

  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsWifiFrameMaxBytes> wifi{};
  std::size_t wifi_length = 0U;
  frame.body_length =
      easy_input::speaker_assets::kSpeakerAssetsWifiFrameBodyBytes;
  assert(easy_input::speaker_assets::encode_speaker_assets_wifi_frame(
             frame, &wifi, &wifi_length) == SpeakerAssetsProtocolResult::Ok);
  assert(wifi_length ==
         easy_input::speaker_assets::kSpeakerAssetsWifiFrameMaxBytes);
  decoded = {};
  assert(easy_input::speaker_assets::decode_speaker_assets_wifi_frame(
             wifi.data(), wifi_length, &decoded) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(decoded.body_length == frame.body_length);
  assert(decoded.body == frame.body);

  frame.body_length = 3U;
  assert(easy_input::speaker_assets::encode_speaker_assets_wifi_frame(
             frame, &wifi, &wifi_length) == SpeakerAssetsProtocolResult::Ok);
  assert(wifi_length ==
         easy_input::speaker_assets::kSpeakerAssetsFrameHeaderBytes + 3U);
  assert(easy_input::speaker_assets::decode_speaker_assets_wifi_frame(
             wifi.data(), wifi_length + 1U, &decoded) ==
         SpeakerAssetsProtocolResult::InvalidLength);
}

void frame_rejects_malformed_input_and_every_single_bit_flip() {
  SpeakerAssetsFrame frame{};
  frame.opcode = static_cast<std::uint8_t>(
      easy_input::speaker_assets::SpeakerAssetsOpcode::Capabilities);
  frame.request_id = 1U;
  frame.body_length = 2U;
  frame.body[0] = 0xA5U;
  frame.body[1] = 0x5AU;
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsUsbFrameBytes> encoded{};
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             frame, &encoded) == SpeakerAssetsProtocolResult::Ok);

  SpeakerAssetsFrame decoded{};
  for (std::size_t byte = 0U; byte < encoded.size(); ++byte) {
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      auto corrupted = encoded;
      corrupted[byte] ^= static_cast<std::uint8_t>(1U << bit);
      assert(easy_input::speaker_assets::decode_speaker_assets_usb_frame(
                 corrupted.data(), corrupted.size(), &decoded) !=
             SpeakerAssetsProtocolResult::Ok);
    }
  }

  assert(easy_input::speaker_assets::decode_speaker_assets_usb_frame(
             encoded.data(), encoded.size() - 1U, &decoded) ==
         SpeakerAssetsProtocolResult::InvalidLength);
  assert(easy_input::speaker_assets::decode_speaker_assets_usb_frame(
             nullptr, encoded.size(), &decoded) ==
         SpeakerAssetsProtocolResult::InvalidArgument);

  frame.request_id = 0U;
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             frame, &encoded) ==
         SpeakerAssetsProtocolResult::InvalidRequestId);
  frame.request_id = 1U;
  frame.flags = easy_input::speaker_assets::kSpeakerAssetsFlagError;
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             frame, &encoded) == SpeakerAssetsProtocolResult::InvalidFlags);
  frame.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagResponse |
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             frame, &encoded) == SpeakerAssetsProtocolResult::InvalidFlags);
}

void exact_golden_vectors_are_stable_for_app_implementations() {
  SpeakerAssetsFrame frame{};
  frame.opcode = static_cast<std::uint8_t>(
      easy_input::speaker_assets::SpeakerAssetsOpcode::Capabilities);
  frame.flags =
      easy_input::speaker_assets::kSpeakerAssetsFlagAckRequested;
  frame.request_id = 0x78563412U;
  frame.session_cookie = 0x11223344U;
  frame.object_offset = 0xA0B0C0D0U;
  frame.body_length = 3U;
  frame.body[0] = 0xA5U;
  frame.body[1] = 0x5AU;
  frame.body[2] = 0x00U;

  constexpr std::array<std::uint8_t, 63> kUsbGolden{{
      0x45, 0x49, 0x41, 0x01, 0x01, 0x04, 0x03, 0x00,
      0x12, 0x34, 0x56, 0x78, 0x44, 0x33, 0x22, 0x11,
      0xD0, 0xC0, 0xB0, 0xA0, 0x4D, 0x52, 0xD9, 0x65,
      0xA5, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  }};
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsUsbFrameBytes> usb{};
  assert(easy_input::speaker_assets::encode_speaker_assets_usb_frame(
             frame, &usb) == SpeakerAssetsProtocolResult::Ok);
  assert(usb == kUsbGolden);

  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsWifiFrameMaxBytes> wifi{};
  std::size_t wifi_length = 0U;
  assert(easy_input::speaker_assets::encode_speaker_assets_wifi_frame(
             frame, &wifi, &wifi_length) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(wifi_length == 27U);
  assert(std::equal(
      wifi.begin(), wifi.begin() + wifi_length, kUsbGolden.begin()));
  const auto plan = make_plan();
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> plan_wire{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &plan_wire) == SpeakerAssetsProtocolResult::Ok);
  assert(plan_wire[636] == 0xDCU);
  assert(plan_wire[637] == 0x96U);
  assert(plan_wire[638] == 0x00U);
  assert(plan_wire[639] == 0xE6U);
  constexpr easy_input::speaker_assets::SoundSha256Digest
      kPlanWireSha256{{
          0x5E, 0x65, 0x23, 0xD2, 0xFE, 0x98, 0xDB, 0xE7,
          0xAF, 0x0D, 0xC3, 0xE7, 0x98, 0xA9, 0xD9, 0x57,
          0xD9, 0xBE, 0xDF, 0xD0, 0x7A, 0x2C, 0x91, 0x4C,
          0xE4, 0x25, 0x94, 0x40, 0x4A, 0xD6, 0x0F, 0x5A,
      }};
  easy_input::speaker_assets::SoundSha256 plan_hash;
  assert(plan_hash.update(plan_wire.data(), plan_wire.size()));
  assert(plan_hash.finish() == kPlanWireSha256);
}

void plan_round_trip_and_mutation_detection() {
  const auto plan = make_plan();
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);
  assert(encoded[0] == 'E' && encoded[1] == 'I' &&
         encoded[2] == 'B' && encoded[3] == 'P');

  SoundBundlePlan decoded{};
  assert(easy_input::speaker_assets::decode_sound_bundle_plan_wire(
             encoded.data(), encoded.size(), &decoded) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(plans_equal(plan, decoded));

  for (std::size_t byte = 0U; byte < encoded.size(); ++byte) {
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      auto corrupted = encoded;
      corrupted[byte] ^= static_cast<std::uint8_t>(1U << bit);
      assert(easy_input::speaker_assets::decode_sound_bundle_plan_wire(
                 corrupted.data(), corrupted.size(), &decoded) !=
             SpeakerAssetsProtocolResult::Ok);
    }
  }
}

void plan_rejects_invalid_shapes() {
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  auto plan = make_plan();

  plan.manifest_bytes = 31U;
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::InvalidPlan);

  plan = make_plan();
  plan.payload_bytes =
      easy_input::speaker_assets::kSoundPayloadMaxSize + 1U;
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::InvalidPlan);

  plan = make_plan();
  plan.base_generation = 0U;
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::InvalidPlan);

  plan = make_plan();
  plan.bundle_sha256.fill(0U);
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::InvalidPlan);

  plan = make_plan();
  plan.payload_block_crc32[2] = 1U;
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::InvalidPlan);

  plan = make_plan(0U);
  plan.payload_block_crc32.fill(0U);
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);
}

void plan_assembler_accepts_out_of_order_duplicates_and_rejects_conflicts() {
  const auto plan = make_plan(
      easy_input::speaker_assets::kSoundPayloadMaxSize);
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSpeakerAssetsPlanWireBytes> encoded{};
  assert(easy_input::speaker_assets::encode_sound_bundle_plan_wire(
             plan, &encoded) == SpeakerAssetsProtocolResult::Ok);

  SpeakerAssetsPlanAssembler assembler;
  const SpeakerAssetsRouteToken owner{
      SpeakerAssetsTransport::Usb, 0U, 17U};
  const SpeakerAssetsRouteToken other_owner{
      SpeakerAssetsTransport::Usb, 0U, 18U};
  constexpr std::uint32_t kBeginId = 0x12345678U;
  assert(!assembler.complete());
  SoundBundlePlan decoded{};
  assert(assembler.decode(&decoded) ==
         SpeakerAssetsProtocolResult::Incomplete);
  assert(assembler.accept_fragment(
             owner, kBeginId, 0U, encoded.data(), 1U) ==
         SpeakerAssetsProtocolResult::WrongOwner);
  assert(assembler.begin(owner, kBeginId) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(assembler.begin(owner, kBeginId) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(assembler.begin(other_owner, kBeginId) ==
         SpeakerAssetsProtocolResult::WrongOwner);
  assert(assembler.begin(owner, kBeginId + 1U) ==
         SpeakerAssetsProtocolResult::WrongOwner);
  assert(assembler.bound());
  assert(easy_input::speaker_assets::speaker_assets_route_equal(
      assembler.owner(), owner));
  assert(assembler.begin_id() == kBeginId);

  std::vector<std::pair<std::uint16_t, std::size_t>> fragments;
  for (std::size_t offset = 0U; offset < encoded.size();) {
    const auto length = std::min(
        easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes,
        encoded.size() - offset);
    fragments.emplace_back(static_cast<std::uint16_t>(offset), length);
    offset += length;
  }
  std::reverse(fragments.begin(), fragments.end());
  for (const auto& fragment : fragments) {
    assert(assembler.accept_fragment(
               owner,
               kBeginId,
               fragment.first,
               encoded.data() + fragment.first,
               fragment.second) == SpeakerAssetsProtocolResult::Ok);
    assert(assembler.accept_fragment(
               owner,
               kBeginId,
               fragment.first,
               encoded.data() + fragment.first,
               fragment.second) == SpeakerAssetsProtocolResult::Ok);
  }
  assert(assembler.complete());
  assert(assembler.received_bytes() == encoded.size());
  assert(assembler.first_missing_offset() == encoded.size());
  assert(assembler.decode(&decoded) == SpeakerAssetsProtocolResult::Ok);
  assert(plans_equal(plan, decoded));

  auto conflict = encoded;
  conflict[20] ^= 0x01U;
  assert(assembler.accept_fragment(
             owner,
             kBeginId,
             20U,
             conflict.data() + 20U,
             1U) ==
         SpeakerAssetsProtocolResult::Conflict);
  assert(assembler.received_bytes() == encoded.size());
  assert(assembler.decode(&decoded) == SpeakerAssetsProtocolResult::Ok);

  std::array<std::uint8_t, 10> bitmap_window{};
  assert(assembler.copy_received_bitmap(
             70U,
             bitmap_window.data(),
             bitmap_window.size()) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(std::all_of(
      bitmap_window.begin(),
      bitmap_window.end(),
      [](std::uint8_t value) { return value == 0xFFU; }));

  assert(assembler.accept_fragment(
             owner,
             kBeginId,
             639U,
             encoded.data() + 639U,
             2U) ==
         SpeakerAssetsProtocolResult::OutOfBounds);
  assert(assembler.accept_fragment(
             owner, kBeginId, 0U, encoded.data(), 0U) ==
         SpeakerAssetsProtocolResult::InvalidArgument);
  assembler.reset();
  assert(!assembler.complete());
  assert(!assembler.bound());
  assert(assembler.received_bytes() == 0U);

  // The second byte is accepted first. A later fragment contains one unseen
  // byte followed by a conflicting byte; the whole call must be atomic, so
  // the unseen byte cannot leak into state.
  assert(assembler.begin(owner, kBeginId) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(assembler.accept_fragment(
             owner, kBeginId, 1U, encoded.data() + 1U, 1U) ==
         SpeakerAssetsProtocolResult::Ok);
  const auto received_before_conflict = assembler.received_bytes();
  std::array<std::uint8_t, 2> mixed{{
      encoded[0], static_cast<std::uint8_t>(encoded[1] ^ 0x01U)}};
  assert(assembler.accept_fragment(
             owner, kBeginId, 0U, mixed.data(), mixed.size()) ==
         SpeakerAssetsProtocolResult::Conflict);
  assert(assembler.received_bytes() == received_before_conflict);
  assert(assembler.first_missing_offset() == 0U);
  assert(assembler.accept_fragment(
             owner, kBeginId, 0U, encoded.data(), 1U) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(assembler.received_bytes() == 2U);
}

void block_assembler_handles_full_and_tail_units_atomically() {
  std::array<
      std::uint8_t,
      easy_input::speaker_assets::kSoundPayloadBlockSize> block{};
  for (std::size_t index = 0U; index < block.size(); ++index) {
    block[index] = static_cast<std::uint8_t>(index * 17U + 3U);
  }

  SpeakerAssetsBlockAssembler assembler;
  assert(assembler.begin(
             SpeakerAssetsRegion::Payload,
             127U,
             static_cast<std::uint16_t>(block.size())) ==
         SpeakerAssetsProtocolResult::Ok);
  std::vector<std::pair<std::uint16_t, std::size_t>> fragments;
  for (std::size_t offset = 0U; offset < block.size();) {
    const auto length = std::min(
        easy_input::speaker_assets::kSpeakerAssetsWifiFrameBodyBytes,
        block.size() - offset);
    fragments.emplace_back(static_cast<std::uint16_t>(offset), length);
    offset += length;
  }
  std::reverse(fragments.begin(), fragments.end());
  for (const auto& fragment : fragments) {
    assert(assembler.accept_fragment(
               SpeakerAssetsRegion::Payload,
               127U,
               fragment.first,
               block.data() + fragment.first,
               fragment.second) == SpeakerAssetsProtocolResult::Ok);
  }
  assert(assembler.complete());
  assert(assembler.received_bytes() == block.size());
  assert(assembler.first_missing_offset() == block.size());
  assert(std::equal(
      block.begin(), block.end(), assembler.data()));
  std::array<std::uint8_t, 16> block_bitmap{};
  assert(assembler.copy_received_bitmap(
             496U,
             block_bitmap.data(),
             block_bitmap.size()) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(std::all_of(
      block_bitmap.begin(),
      block_bitmap.end(),
      [](std::uint8_t value) { return value == 0xFFU; }));

  auto conflicting = block;
  conflicting[100] ^= 0x80U;
  assert(assembler.accept_fragment(
             SpeakerAssetsRegion::Payload,
             127U,
             100U,
             conflicting.data() + 100U,
             1U) == SpeakerAssetsProtocolResult::Conflict);
  assert(assembler.complete());
  assert(std::equal(
      block.begin(), block.end(), assembler.data()));

  assert(assembler.accept_fragment(
             SpeakerAssetsRegion::Manifest,
             0U,
             0U,
             block.data(),
             1U) == SpeakerAssetsProtocolResult::WrongUnit);

  constexpr std::uint16_t kTailBytes = 2776U;
  assert(assembler.begin(
             SpeakerAssetsRegion::Payload, 1U, kTailBytes) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(!assembler.complete());
  assert(assembler.accept_fragment(
             SpeakerAssetsRegion::Payload,
             1U,
             kTailBytes - 1U,
             block.data(),
             2U) == SpeakerAssetsProtocolResult::OutOfBounds);
  assert(assembler.accept_fragment(
             SpeakerAssetsRegion::Payload,
             1U,
             0U,
             block.data(),
             easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes) ==
         SpeakerAssetsProtocolResult::Ok);
  assert(assembler.received_bytes() ==
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes);
  assert(assembler.first_missing_offset() ==
         easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes);

  assert(assembler.begin(
             SpeakerAssetsRegion::Manifest, 1U, 84U) ==
         SpeakerAssetsProtocolResult::InvalidArgument);
  assert(assembler.begin(
             static_cast<SpeakerAssetsRegion>(9U), 0U, 84U) ==
         SpeakerAssetsProtocolResult::InvalidArgument);
  assembler.reset();
  assert(!assembler.active());
  assert(assembler.data() == nullptr);
}

}  // namespace

int main() {
  static_assert(
      easy_input::speaker_assets::kSpeakerAssetsFrameHeaderBytes == 24U);
  static_assert(
      easy_input::speaker_assets::kSpeakerAssetsUsbFrameBodyBytes == 39U);
  static_assert(
      easy_input::speaker_assets::kSpeakerAssetsWifiFrameBodyBytes == 101U);
  static_assert(
      sizeof(SpeakerAssetsPlanAssembler) <= 768U);
  static_assert(
      sizeof(SpeakerAssetsBlockAssembler) <= 4704U);

  frame_round_trip_and_padding_are_canonical();
  frame_rejects_malformed_input_and_every_single_bit_flip();
  exact_golden_vectors_are_stable_for_app_implementations();
  plan_round_trip_and_mutation_detection();
  plan_rejects_invalid_shapes();
  plan_assembler_accepts_out_of_order_duplicates_and_rejects_conflicts();
  block_assembler_handles_full_and_tail_units_atomically();
  return 0;
}
