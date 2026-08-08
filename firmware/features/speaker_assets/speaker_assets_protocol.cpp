#include "speaker_assets/speaker_assets_protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/sound_asset_crypto.h"

namespace easy_input::speaker_assets {
namespace {

constexpr std::array<std::uint8_t, 3> kFrameMagic{{
    'E', 'I', 'A',
}};
constexpr std::array<std::uint8_t, 4> kPlanMagic{{
    'E', 'I', 'B', 'P',
}};
constexpr std::size_t kFrameCrcOffset = 20U;
constexpr std::size_t kPlanCrcOffset = 636U;

std::uint16_t read_le16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(bytes[0]) |
      (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t read_le64(const std::uint8_t* bytes) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

void write_le16(std::uint8_t* bytes, std::uint16_t value) {
  bytes[0] = static_cast<std::uint8_t>(value);
  bytes[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_le32(std::uint8_t* bytes, std::uint32_t value) {
  bytes[0] = static_cast<std::uint8_t>(value);
  bytes[1] = static_cast<std::uint8_t>(value >> 8U);
  bytes[2] = static_cast<std::uint8_t>(value >> 16U);
  bytes[3] = static_cast<std::uint8_t>(value >> 24U);
}

void write_le64(std::uint8_t* bytes, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

bool bytes_equal(const std::uint8_t* first,
                 const std::uint8_t* second,
                 std::size_t length) {
  if (first == nullptr || second == nullptr) {
    return false;
  }
  std::uint8_t difference = 0U;
  for (std::size_t index = 0U; index < length; ++index) {
    difference = static_cast<std::uint8_t>(
        difference | (first[index] ^ second[index]));
  }
  return difference == 0U;
}

bool bytes_are_zero(const std::uint8_t* bytes, std::size_t length) {
  if (bytes == nullptr) {
    return false;
  }
  std::uint8_t combined = 0U;
  for (std::size_t index = 0U; index < length; ++index) {
    combined = static_cast<std::uint8_t>(combined | bytes[index]);
  }
  return combined == 0U;
}

bool digest_is_zero(const SoundSha256Digest& digest) {
  return bytes_are_zero(digest.data(), digest.size());
}

std::size_t payload_block_count(std::uint32_t payload_bytes) {
  return payload_bytes == 0U
             ? 0U
             : (static_cast<std::size_t>(payload_bytes) +
                kSoundPayloadBlockSize - 1U) /
                   kSoundPayloadBlockSize;
}

bool sound_bundle_plan_shape_is_valid(const SoundBundlePlan& plan) {
  if (plan.manifest_bytes < 32U ||
      plan.manifest_bytes > kSoundSectorSize ||
      plan.payload_bytes > kSoundPayloadMaxSize ||
      digest_is_zero(plan.manifest_sha256) ||
      digest_is_zero(plan.bundle_sha256)) {
    return false;
  }
  if ((plan.base_generation == 0U) !=
      digest_is_zero(plan.base_bundle_sha256)) {
    return false;
  }
  const auto block_count = payload_block_count(plan.payload_bytes);
  if (block_count > plan.payload_block_crc32.size()) {
    return false;
  }
  for (std::size_t index = block_count;
       index < plan.payload_block_crc32.size();
       ++index) {
    if (plan.payload_block_crc32[index] != 0U) {
      return false;
    }
  }
  return true;
}

bool region_is_valid(SpeakerAssetsRegion region) {
  return region == SpeakerAssetsRegion::Manifest ||
         region == SpeakerAssetsRegion::Payload;
}

bool route_is_valid(const SpeakerAssetsRouteToken& route) {
  return (route.transport == SpeakerAssetsTransport::Usb ||
          route.transport == SpeakerAssetsTransport::Wifi) &&
         route.generation != 0U &&
         (route.transport != SpeakerAssetsTransport::Usb ||
          route.route_id == 0U);
}

SpeakerAssetsProtocolResult validate_frame_for_encoding(
    const SpeakerAssetsFrame& frame,
    std::size_t maximum_body_bytes) {
  if (frame.request_id == 0U) {
    return SpeakerAssetsProtocolResult::InvalidRequestId;
  }
  if (frame.body_length > maximum_body_bytes) {
    return SpeakerAssetsProtocolResult::InvalidBodyLength;
  }
  if ((frame.flags & static_cast<std::uint8_t>(~kSpeakerAssetsKnownFlags)) !=
          0U ||
      ((frame.flags & kSpeakerAssetsFlagError) != 0U &&
       (frame.flags & kSpeakerAssetsFlagResponse) == 0U) ||
      ((frame.flags & kSpeakerAssetsFlagResponse) != 0U &&
       (frame.flags & kSpeakerAssetsFlagAckRequested) != 0U)) {
    return SpeakerAssetsProtocolResult::InvalidFlags;
  }
  return SpeakerAssetsProtocolResult::Ok;
}

template <std::size_t EncodedBytes>
void encode_frame_prefix(
    const SpeakerAssetsFrame& frame,
    std::array<std::uint8_t, EncodedBytes>* encoded) {
  encoded->fill(0U);
  std::copy(kFrameMagic.begin(), kFrameMagic.end(), encoded->begin());
  (*encoded)[3] = kSpeakerAssetsProtocolVersion;
  (*encoded)[4] = frame.opcode;
  (*encoded)[5] = frame.flags;
  write_le16(encoded->data() + 6U, frame.body_length);
  write_le32(encoded->data() + 8U, frame.request_id);
  write_le32(encoded->data() + 12U, frame.session_cookie);
  write_le32(encoded->data() + 16U, frame.object_offset);
  std::copy_n(frame.body.begin(),
              frame.body_length,
              encoded->begin() + kSpeakerAssetsFrameHeaderBytes);
}

SpeakerAssetsProtocolResult decode_frame_prefix(
    const std::uint8_t* encoded,
    std::size_t length,
    std::size_t maximum_body_bytes,
    bool fixed_size_with_padding,
    SpeakerAssetsFrame* frame) {
  if (encoded == nullptr || frame == nullptr) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (length < kSpeakerAssetsFrameHeaderBytes) {
    return SpeakerAssetsProtocolResult::InvalidLength;
  }
  if (!bytes_equal(encoded, kFrameMagic.data(), kFrameMagic.size())) {
    return SpeakerAssetsProtocolResult::InvalidMagic;
  }
  if (encoded[3] != kSpeakerAssetsProtocolVersion) {
    return SpeakerAssetsProtocolResult::UnsupportedVersion;
  }
  const auto flags = encoded[5];
  if ((flags & static_cast<std::uint8_t>(~kSpeakerAssetsKnownFlags)) != 0U ||
      ((flags & kSpeakerAssetsFlagError) != 0U &&
       (flags & kSpeakerAssetsFlagResponse) == 0U) ||
      ((flags & kSpeakerAssetsFlagResponse) != 0U &&
       (flags & kSpeakerAssetsFlagAckRequested) != 0U)) {
    return SpeakerAssetsProtocolResult::InvalidFlags;
  }
  const auto body_length = read_le16(encoded + 6U);
  if (body_length > maximum_body_bytes) {
    return SpeakerAssetsProtocolResult::InvalidBodyLength;
  }
  const auto exact_length =
      kSpeakerAssetsFrameHeaderBytes + static_cast<std::size_t>(body_length);
  if ((!fixed_size_with_padding && length != exact_length) ||
      (fixed_size_with_padding && length < exact_length)) {
    return SpeakerAssetsProtocolResult::InvalidLength;
  }
  if (fixed_size_with_padding &&
      !bytes_are_zero(encoded + exact_length, length - exact_length)) {
    return SpeakerAssetsProtocolResult::NonZeroPadding;
  }
  const auto request_id = read_le32(encoded + 8U);
  if (request_id == 0U) {
    return SpeakerAssetsProtocolResult::InvalidRequestId;
  }

  std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes>
      crc_input{};
  std::copy_n(encoded, exact_length, crc_input.begin());
  const auto expected_crc = read_le32(encoded + kFrameCrcOffset);
  std::fill_n(crc_input.begin() + kFrameCrcOffset, 4U, 0U);
  if (sound_crc32_iso_hdlc(crc_input.data(), exact_length) !=
      expected_crc) {
    return SpeakerAssetsProtocolResult::CrcMismatch;
  }

  SpeakerAssetsFrame decoded{};
  decoded.opcode = encoded[4];
  decoded.flags = flags;
  decoded.request_id = request_id;
  decoded.session_cookie = read_le32(encoded + 12U);
  decoded.object_offset = read_le32(encoded + 16U);
  decoded.body_length = body_length;
  std::copy_n(encoded + kSpeakerAssetsFrameHeaderBytes,
              body_length,
              decoded.body.begin());
  *frame = decoded;
  return SpeakerAssetsProtocolResult::Ok;
}

}  // namespace

bool speaker_assets_route_equal(
    const SpeakerAssetsRouteToken& first,
    const SpeakerAssetsRouteToken& second) {
  return first.transport == second.transport &&
         first.route_id == second.route_id &&
         first.generation == second.generation;
}

SpeakerAssetsProtocolResult encode_speaker_assets_usb_frame(
    const SpeakerAssetsFrame& frame,
    std::array<std::uint8_t, kSpeakerAssetsUsbFrameBytes>* encoded) {
  if (encoded == nullptr) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  const auto validation = validate_frame_for_encoding(
      frame, kSpeakerAssetsUsbFrameBodyBytes);
  if (validation != SpeakerAssetsProtocolResult::Ok) {
    return validation;
  }

  encode_frame_prefix(frame, encoded);
  const auto crc =
      sound_crc32_iso_hdlc(
          encoded->data(),
          kSpeakerAssetsFrameHeaderBytes + frame.body_length);
  write_le32(encoded->data() + kFrameCrcOffset, crc);
  return SpeakerAssetsProtocolResult::Ok;
}

SpeakerAssetsProtocolResult decode_speaker_assets_usb_frame(
    const std::uint8_t* encoded,
    std::size_t length,
    SpeakerAssetsFrame* frame) {
  if (length != kSpeakerAssetsUsbFrameBytes) {
    return SpeakerAssetsProtocolResult::InvalidLength;
  }
  return decode_frame_prefix(
      encoded,
      length,
      kSpeakerAssetsUsbFrameBodyBytes,
      true,
      frame);
}

SpeakerAssetsProtocolResult encode_speaker_assets_wifi_frame(
    const SpeakerAssetsFrame& frame,
    std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes>* encoded,
    std::size_t* encoded_length) {
  if (encoded == nullptr || encoded_length == nullptr) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  const auto validation = validate_frame_for_encoding(
      frame, kSpeakerAssetsWifiFrameBodyBytes);
  if (validation != SpeakerAssetsProtocolResult::Ok) {
    return validation;
  }
  encode_frame_prefix(frame, encoded);
  *encoded_length =
      kSpeakerAssetsFrameHeaderBytes + frame.body_length;
  const auto crc =
      sound_crc32_iso_hdlc(encoded->data(), *encoded_length);
  write_le32(encoded->data() + kFrameCrcOffset, crc);
  return SpeakerAssetsProtocolResult::Ok;
}

SpeakerAssetsProtocolResult decode_speaker_assets_wifi_frame(
    const std::uint8_t* encoded,
    std::size_t length,
    SpeakerAssetsFrame* frame) {
  if (length > kSpeakerAssetsWifiFrameMaxBytes) {
    return SpeakerAssetsProtocolResult::InvalidLength;
  }
  return decode_frame_prefix(
      encoded,
      length,
      kSpeakerAssetsWifiFrameBodyBytes,
      false,
      frame);
}

SpeakerAssetsProtocolResult encode_sound_bundle_plan_wire(
    const SoundBundlePlan& plan,
    std::array<std::uint8_t, kSpeakerAssetsPlanWireBytes>* encoded) {
  if (encoded == nullptr) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (!sound_bundle_plan_shape_is_valid(plan)) {
    return SpeakerAssetsProtocolResult::InvalidPlan;
  }

  encoded->fill(0U);
  std::copy(kPlanMagic.begin(), kPlanMagic.end(), encoded->begin());
  write_le16(encoded->data() + 4U, kSpeakerAssetsPlanVersion);
  write_le16(encoded->data() + 6U, kSpeakerAssetsPlanWireBytes);
  write_le64(encoded->data() + 8U, plan.base_generation);
  std::copy(plan.base_bundle_sha256.begin(),
            plan.base_bundle_sha256.end(),
            encoded->begin() + 16U);
  write_le32(encoded->data() + 48U, plan.manifest_bytes);
  write_le32(encoded->data() + 52U, plan.payload_bytes);
  write_le32(encoded->data() + 56U, plan.manifest_crc32);
  std::copy(plan.manifest_sha256.begin(),
            plan.manifest_sha256.end(),
            encoded->begin() + 60U);
  std::copy(plan.bundle_sha256.begin(),
            plan.bundle_sha256.end(),
            encoded->begin() + 92U);
  for (std::size_t index = 0U;
       index < plan.payload_block_crc32.size();
       ++index) {
    write_le32(encoded->data() + 124U + index * 4U,
               plan.payload_block_crc32[index]);
  }
  const auto crc =
      sound_crc32_iso_hdlc(encoded->data(), kPlanCrcOffset);
  write_le32(encoded->data() + kPlanCrcOffset, crc);
  return SpeakerAssetsProtocolResult::Ok;
}

SpeakerAssetsProtocolResult decode_sound_bundle_plan_wire(
    const std::uint8_t* encoded,
    std::size_t length,
    SoundBundlePlan* plan) {
  if (encoded == nullptr || plan == nullptr) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (length != kSpeakerAssetsPlanWireBytes) {
    return SpeakerAssetsProtocolResult::InvalidLength;
  }
  if (!bytes_equal(encoded, kPlanMagic.data(), kPlanMagic.size())) {
    return SpeakerAssetsProtocolResult::InvalidMagic;
  }
  if (read_le16(encoded + 4U) != kSpeakerAssetsPlanVersion) {
    return SpeakerAssetsProtocolResult::UnsupportedVersion;
  }
  if (read_le16(encoded + 6U) != kSpeakerAssetsPlanWireBytes) {
    return SpeakerAssetsProtocolResult::InvalidLength;
  }
  if (sound_crc32_iso_hdlc(encoded, kPlanCrcOffset) !=
      read_le32(encoded + kPlanCrcOffset)) {
    return SpeakerAssetsProtocolResult::CrcMismatch;
  }

  SoundBundlePlan decoded{};
  decoded.base_generation = read_le64(encoded + 8U);
  std::copy_n(encoded + 16U,
              decoded.base_bundle_sha256.size(),
              decoded.base_bundle_sha256.begin());
  decoded.manifest_bytes = read_le32(encoded + 48U);
  decoded.payload_bytes = read_le32(encoded + 52U);
  decoded.manifest_crc32 = read_le32(encoded + 56U);
  std::copy_n(encoded + 60U,
              decoded.manifest_sha256.size(),
              decoded.manifest_sha256.begin());
  std::copy_n(encoded + 92U,
              decoded.bundle_sha256.size(),
              decoded.bundle_sha256.begin());
  for (std::size_t index = 0U;
       index < decoded.payload_block_crc32.size();
       ++index) {
    decoded.payload_block_crc32[index] =
        read_le32(encoded + 124U + index * 4U);
  }
  if (!sound_bundle_plan_shape_is_valid(decoded)) {
    return SpeakerAssetsProtocolResult::InvalidPlan;
  }
  *plan = decoded;
  return SpeakerAssetsProtocolResult::Ok;
}

SpeakerAssetsProtocolResult SpeakerAssetsPlanAssembler::begin(
    const SpeakerAssetsRouteToken& owner,
    std::uint32_t begin_id) {
  if (!route_is_valid(owner) || begin_id == 0U) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (bound_) {
    return speaker_assets_route_equal(owner_, owner) &&
                   begin_id_ == begin_id
               ? SpeakerAssetsProtocolResult::Ok
               : SpeakerAssetsProtocolResult::WrongOwner;
  }
  owner_ = owner;
  begin_id_ = begin_id;
  bound_ = true;
  return SpeakerAssetsProtocolResult::Ok;
}

void SpeakerAssetsPlanAssembler::reset() {
  bytes_.fill(0U);
  received_.fill(0U);
  received_bytes_ = 0U;
  owner_ = {};
  begin_id_ = 0U;
  bound_ = false;
}

SpeakerAssetsProtocolResult SpeakerAssetsPlanAssembler::accept_fragment(
    const SpeakerAssetsRouteToken& owner,
    std::uint32_t begin_id,
    std::uint16_t offset,
    const std::uint8_t* data,
    std::size_t length) {
  if (!bound_ ||
      !speaker_assets_route_equal(owner_, owner) ||
      begin_id_ != begin_id) {
    return SpeakerAssetsProtocolResult::WrongOwner;
  }
  if (data == nullptr || length == 0U) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (length > kSpeakerAssetsWifiFrameBodyBytes ||
      offset > bytes_.size() ||
      length > bytes_.size() - offset) {
    return SpeakerAssetsProtocolResult::OutOfBounds;
  }

  for (std::size_t index = 0U; index < length; ++index) {
    const auto destination = static_cast<std::size_t>(offset) + index;
    if (byte_received(destination) &&
        bytes_[destination] != data[index]) {
      return SpeakerAssetsProtocolResult::Conflict;
    }
  }
  for (std::size_t index = 0U; index < length; ++index) {
    const auto destination = static_cast<std::size_t>(offset) + index;
    if (!byte_received(destination)) {
      bytes_[destination] = data[index];
      mark_byte_received(destination);
      ++received_bytes_;
    }
  }
  return SpeakerAssetsProtocolResult::Ok;
}

SpeakerAssetsProtocolResult SpeakerAssetsPlanAssembler::decode(
    SoundBundlePlan* plan) const {
  if (plan == nullptr) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (!complete()) {
    return SpeakerAssetsProtocolResult::Incomplete;
  }
  return decode_sound_bundle_plan_wire(bytes_.data(), bytes_.size(), plan);
}

bool SpeakerAssetsPlanAssembler::complete() const {
  return bound_ && received_bytes_ == bytes_.size();
}

bool SpeakerAssetsPlanAssembler::bound() const {
  return bound_;
}

const SpeakerAssetsRouteToken& SpeakerAssetsPlanAssembler::owner() const {
  return owner_;
}

std::uint32_t SpeakerAssetsPlanAssembler::begin_id() const {
  return begin_id_;
}

std::size_t SpeakerAssetsPlanAssembler::received_bytes() const {
  return received_bytes_;
}

std::size_t SpeakerAssetsPlanAssembler::first_missing_offset() const {
  for (std::size_t index = 0U; index < bytes_.size(); ++index) {
    if (!byte_received(index)) {
      return index;
    }
  }
  return bytes_.size();
}

SpeakerAssetsProtocolResult
SpeakerAssetsPlanAssembler::copy_received_bitmap(
    std::size_t bitmap_offset,
    std::uint8_t* destination,
    std::size_t length) const {
  if (destination == nullptr && length != 0U) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (bitmap_offset > received_.size() ||
      length > received_.size() - bitmap_offset) {
    return SpeakerAssetsProtocolResult::OutOfBounds;
  }
  if (length == 0U) {
    return SpeakerAssetsProtocolResult::Ok;
  }
  std::copy_n(
      received_.begin() + bitmap_offset,
      length,
      destination);
  return SpeakerAssetsProtocolResult::Ok;
}

bool SpeakerAssetsPlanAssembler::byte_received(std::size_t index) const {
  const auto mask = static_cast<std::uint8_t>(1U << (index % 8U));
  return (received_[index / 8U] & mask) != 0U;
}

void SpeakerAssetsPlanAssembler::mark_byte_received(std::size_t index) {
  const auto mask = static_cast<std::uint8_t>(1U << (index % 8U));
  received_[index / 8U] =
      static_cast<std::uint8_t>(received_[index / 8U] | mask);
}

SpeakerAssetsProtocolResult SpeakerAssetsBlockAssembler::begin(
    SpeakerAssetsRegion region,
    std::uint16_t unit_index,
    std::uint16_t expected_bytes) {
  if (!region_is_valid(region) ||
      expected_bytes == 0U ||
      expected_bytes > bytes_.size() ||
      (region == SpeakerAssetsRegion::Manifest && unit_index != 0U) ||
      (region == SpeakerAssetsRegion::Payload &&
       unit_index >= kSoundPayloadBlockCount)) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  reset();
  region_ = region;
  unit_index_ = unit_index;
  expected_bytes_ = expected_bytes;
  active_ = true;
  return SpeakerAssetsProtocolResult::Ok;
}

void SpeakerAssetsBlockAssembler::reset() {
  bytes_.fill(0U);
  received_.fill(0U);
  received_bytes_ = 0U;
  region_ = SpeakerAssetsRegion::Manifest;
  unit_index_ = 0U;
  expected_bytes_ = 0U;
  active_ = false;
}

SpeakerAssetsProtocolResult SpeakerAssetsBlockAssembler::accept_fragment(
    SpeakerAssetsRegion region,
    std::uint16_t unit_index,
    std::uint16_t offset,
    const std::uint8_t* data,
    std::size_t length) {
  if (!active_) {
    return SpeakerAssetsProtocolResult::Incomplete;
  }
  if (region != region_ || unit_index != unit_index_) {
    return SpeakerAssetsProtocolResult::WrongUnit;
  }
  if (data == nullptr || length == 0U) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  if (length > kSpeakerAssetsWifiFrameBodyBytes ||
      offset > expected_bytes_ ||
      length > static_cast<std::size_t>(expected_bytes_ - offset)) {
    return SpeakerAssetsProtocolResult::OutOfBounds;
  }

  for (std::size_t index = 0U; index < length; ++index) {
    const auto destination = static_cast<std::size_t>(offset) + index;
    if (byte_received(destination) &&
        bytes_[destination] != data[index]) {
      return SpeakerAssetsProtocolResult::Conflict;
    }
  }
  for (std::size_t index = 0U; index < length; ++index) {
    const auto destination = static_cast<std::size_t>(offset) + index;
    if (!byte_received(destination)) {
      bytes_[destination] = data[index];
      mark_byte_received(destination);
      ++received_bytes_;
    }
  }
  return SpeakerAssetsProtocolResult::Ok;
}

bool SpeakerAssetsBlockAssembler::active() const {
  return active_;
}

bool SpeakerAssetsBlockAssembler::complete() const {
  return active_ && received_bytes_ == expected_bytes_;
}

SpeakerAssetsRegion SpeakerAssetsBlockAssembler::region() const {
  return region_;
}

std::uint16_t SpeakerAssetsBlockAssembler::unit_index() const {
  return unit_index_;
}

std::uint16_t SpeakerAssetsBlockAssembler::expected_bytes() const {
  return expected_bytes_;
}

std::size_t SpeakerAssetsBlockAssembler::received_bytes() const {
  return received_bytes_;
}

std::size_t SpeakerAssetsBlockAssembler::first_missing_offset() const {
  if (!active_) {
    return 0U;
  }
  for (std::size_t index = 0U; index < expected_bytes_; ++index) {
    if (!byte_received(index)) {
      return index;
    }
  }
  return expected_bytes_;
}

SpeakerAssetsProtocolResult
SpeakerAssetsBlockAssembler::copy_received_bitmap(
    std::size_t bitmap_offset,
    std::uint8_t* destination,
    std::size_t length) const {
  if (destination == nullptr && length != 0U) {
    return SpeakerAssetsProtocolResult::InvalidArgument;
  }
  const auto bitmap_bytes =
      (static_cast<std::size_t>(expected_bytes_) + 7U) / 8U;
  if (!active_ ||
      bitmap_offset > bitmap_bytes ||
      length > bitmap_bytes - bitmap_offset) {
    return SpeakerAssetsProtocolResult::OutOfBounds;
  }
  if (length == 0U) {
    return SpeakerAssetsProtocolResult::Ok;
  }
  std::copy_n(
      received_.begin() + bitmap_offset,
      length,
      destination);
  return SpeakerAssetsProtocolResult::Ok;
}

const std::uint8_t* SpeakerAssetsBlockAssembler::data() const {
  return active_ ? bytes_.data() : nullptr;
}

bool SpeakerAssetsBlockAssembler::byte_received(std::size_t index) const {
  const auto mask = static_cast<std::uint8_t>(1U << (index % 8U));
  return (received_[index / 8U] & mask) != 0U;
}

void SpeakerAssetsBlockAssembler::mark_byte_received(std::size_t index) {
  const auto mask = static_cast<std::uint8_t>(1U << (index % 8U));
  received_[index / 8U] =
      static_cast<std::uint8_t>(received_[index / 8U] | mask);
}

}  // namespace easy_input::speaker_assets
