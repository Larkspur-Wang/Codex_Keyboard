#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/sound_asset_store.h"

namespace easy_input::speaker_assets {

inline constexpr std::uint8_t kSpeakerAssetsProtocolVersion = 1U;
inline constexpr std::size_t kSpeakerAssetsFrameHeaderBytes = 24U;
inline constexpr std::size_t kSpeakerAssetsUsbFrameBytes = 63U;
inline constexpr std::size_t kSpeakerAssetsUsbFrameBodyBytes =
    kSpeakerAssetsUsbFrameBytes - kSpeakerAssetsFrameHeaderBytes;
// Wi-Fi carries an exact-length EIA logical frame with a 101-byte body.
inline constexpr std::size_t kSpeakerAssetsWifiFrameMaxBytes =
    125U;
inline constexpr std::size_t kSpeakerAssetsWifiFrameBodyBytes =
    kSpeakerAssetsWifiFrameMaxBytes -
    kSpeakerAssetsFrameHeaderBytes;
inline constexpr std::size_t kSpeakerAssetsPlanWireBytes = 640U;
inline constexpr std::uint16_t kSpeakerAssetsPlanVersion = 1U;

inline constexpr std::uint8_t kSpeakerAssetsCapabilityUsb = 0x01U;
// Capability bit 0x02 is retired with the removed legacy bulk carrier.
inline constexpr std::uint8_t kSpeakerAssetsCapabilityResume = 0x04U;
inline constexpr std::uint8_t kSpeakerAssetsCapabilityProgress = 0x08U;
inline constexpr std::uint8_t kSpeakerAssetsCapabilityRecovery = 0x10U;
inline constexpr std::uint8_t kSpeakerAssetsCapabilityCurrentActive = 0x20U;
inline constexpr std::uint8_t kSpeakerAssetsCapabilityWifi = 0x40U;
inline constexpr std::uint8_t kSpeakerAssetsCapabilities =
    kSpeakerAssetsCapabilityUsb |
    kSpeakerAssetsCapabilityResume |
    kSpeakerAssetsCapabilityProgress |
    kSpeakerAssetsCapabilityRecovery |
    kSpeakerAssetsCapabilityCurrentActive |
    kSpeakerAssetsCapabilityWifi;

inline constexpr std::uint8_t kSpeakerAssetsFlagResponse = 0x01U;
inline constexpr std::uint8_t kSpeakerAssetsFlagError = 0x02U;
inline constexpr std::uint8_t kSpeakerAssetsFlagAckRequested = 0x04U;
inline constexpr std::uint8_t kSpeakerAssetsFlagFirstFragment = 0x08U;
inline constexpr std::uint8_t kSpeakerAssetsFlagLastFragment = 0x10U;
inline constexpr std::uint8_t kSpeakerAssetsFlagPayloadRegion = 0x20U;
inline constexpr std::uint8_t kSpeakerAssetsKnownFlags =
    kSpeakerAssetsFlagResponse |
    kSpeakerAssetsFlagError |
    kSpeakerAssetsFlagAckRequested |
    kSpeakerAssetsFlagFirstFragment |
    kSpeakerAssetsFlagLastFragment |
    kSpeakerAssetsFlagPayloadRegion;

enum class SpeakerAssetsOpcode : std::uint8_t {
  Capabilities = 0x01U,
  Begin = 0x02U,
  Resume = 0x03U,
  Data = 0x04U,
  Query = 0x05U,
  Commit = 0x06U,
  Abort = 0x07U,
  // Explicit maintenance operation. The App sends the same complete plan as
  // BEGIN; the Store may erase only a torn/invalid inactive staging bank that
  // this plan deterministically targets. Valid staging and committed banks
  // remain protected.
  RecoverInvalidStaging = 0x08U,
  // Read-only durable reconciliation query. The response exposes the current
  // committed generation and bundle SHA without opening or rebinding a session.
  CurrentActive = 0x09U,
};

enum class SpeakerAssetsRegion : std::uint8_t {
  Manifest = 0U,
  Payload = 1U,
};

enum class SpeakerAssetsProtocolResult : std::uint8_t {
  Ok,
  InvalidArgument,
  InvalidLength,
  InvalidMagic,
  UnsupportedVersion,
  InvalidFlags,
  InvalidRequestId,
  InvalidBodyLength,
  NonZeroPadding,
  CrcMismatch,
  InvalidPlan,
  Incomplete,
  Conflict,
  OutOfBounds,
  WrongUnit,
  WrongOwner,
};

// Stable wire status values returned to the App. Platform/storage-specific
// errors are deliberately collapsed into actionable protocol categories.
enum class SpeakerAssetsStatus : std::uint8_t {
  Ok = 0U,
  BadRequest = 1U,
  UnsupportedVersion = 2U,
  IntegrityError = 3U,
  Busy = 4U,
  NotReady = 5U,
  StaleBase = 6U,
  TransactionMismatch = 7U,
  Incomplete = 8U,
  StorageUnavailable = 9U,
  BankPinned = 10U,
  GenerationExhausted = 11U,
  SplitBrain = 12U,
  PausedForInput = 13U,
  InternalError = 14U,
  // A torn/non-FF inactive staging header was detected. Nothing was erased;
  // the App may offer the explicit RecoverInvalidStaging flow.
  RecoveryRequired = 15U,
};

struct SpeakerAssetsFrame {
  std::uint8_t opcode = 0U;
  std::uint8_t flags = 0U;
  std::uint32_t request_id = 0U;
  // Zero for pre-session CAPABILITIES/BEGIN. The supervisor issues a random
  // non-zero cookie only after durable BEGIN/QUERY and binds it to one exact
  // USB or Wi-Fi route lifetime.
  std::uint32_t session_cookie = 0U;
  // BEGIN plan offset, DATA manifest/payload offset, or response progress.
  std::uint32_t object_offset = 0U;
  std::uint16_t body_length = 0U;
  std::array<std::uint8_t, kSpeakerAssetsWifiFrameBodyBytes> body{};
};

enum class SpeakerAssetsTransport : std::uint8_t {
  Usb = 1U,
  // Value 2 remains reserved for the removed legacy bulk carrier so stale
  // route records can never be reinterpreted as Wi-Fi.
  ReservedLegacy = 2U,
  Wifi = 3U,
};

struct SpeakerAssetsRouteToken {
  SpeakerAssetsTransport transport = SpeakerAssetsTransport::Usb;
  // USB leaves route_id zero and uses generation as the endpoint epoch.
  // Wi-Fi uses a non-zero connection-local route_id and generation.
  std::uint32_t route_id = 0U;
  std::uint32_t generation = 0U;
};

bool speaker_assets_route_equal(const SpeakerAssetsRouteToken& first,
                                const SpeakerAssetsRouteToken& second);

[[nodiscard]] SpeakerAssetsProtocolResult encode_speaker_assets_usb_frame(
    const SpeakerAssetsFrame& frame,
    std::array<std::uint8_t, kSpeakerAssetsUsbFrameBytes>* encoded);

[[nodiscard]] SpeakerAssetsProtocolResult decode_speaker_assets_usb_frame(
    const std::uint8_t* encoded,
    std::size_t length,
    SpeakerAssetsFrame* frame);

[[nodiscard]] SpeakerAssetsProtocolResult encode_speaker_assets_wifi_frame(
    const SpeakerAssetsFrame& frame,
    std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes>* encoded,
    std::size_t* encoded_length);

[[nodiscard]] SpeakerAssetsProtocolResult decode_speaker_assets_wifi_frame(
    const std::uint8_t* encoded,
    std::size_t length,
    SpeakerAssetsFrame* frame);

[[nodiscard]] SpeakerAssetsProtocolResult encode_sound_bundle_plan_wire(
    const SoundBundlePlan& plan,
    std::array<std::uint8_t, kSpeakerAssetsPlanWireBytes>* encoded);

[[nodiscard]] SpeakerAssetsProtocolResult decode_sound_bundle_plan_wire(
    const std::uint8_t* encoded,
    std::size_t length,
    SoundBundlePlan* plan);

// Reassembles the canonical 640-byte BEGIN plan. Exact duplicates are
// idempotent, while overlapping bytes with different values fail atomically.
// The session owner must reset this object at every new BEGIN boundary.
class SpeakerAssetsPlanAssembler {
 public:
  [[nodiscard]] SpeakerAssetsProtocolResult begin(
      const SpeakerAssetsRouteToken& owner,
      std::uint32_t begin_id);
  void reset();
  [[nodiscard]] SpeakerAssetsProtocolResult accept_fragment(
      const SpeakerAssetsRouteToken& owner,
      std::uint32_t begin_id,
      std::uint16_t offset,
      const std::uint8_t* data,
      std::size_t length);
  [[nodiscard]] SpeakerAssetsProtocolResult decode(
      SoundBundlePlan* plan) const;

  bool complete() const;
  bool bound() const;
  const SpeakerAssetsRouteToken& owner() const;
  std::uint32_t begin_id() const;
  std::size_t received_bytes() const;
  std::size_t first_missing_offset() const;
  [[nodiscard]] SpeakerAssetsProtocolResult copy_received_bitmap(
      std::size_t bitmap_offset,
      std::uint8_t* destination,
      std::size_t length) const;

 private:
  bool byte_received(std::size_t index) const;
  void mark_byte_received(std::size_t index);

  std::array<std::uint8_t, kSpeakerAssetsPlanWireBytes> bytes_{};
  std::array<std::uint8_t, kSpeakerAssetsPlanWireBytes / 8U> received_{};
  std::size_t received_bytes_ = 0U;
  SpeakerAssetsRouteToken owner_{};
  std::uint32_t begin_id_ = 0U;
  bool bound_ = false;
};

// Holds exactly one manifest or payload unit. Payload units map one-to-one to
// the store's 4 KiB blocks; the final block may be shorter. The future runtime
// supervisor owns this object and never invokes Flash from a transport
// callback.
class SpeakerAssetsBlockAssembler {
 public:
  [[nodiscard]] SpeakerAssetsProtocolResult begin(
      SpeakerAssetsRegion region,
      std::uint16_t unit_index,
      std::uint16_t expected_bytes);
  void reset();
  [[nodiscard]] SpeakerAssetsProtocolResult accept_fragment(
      SpeakerAssetsRegion region,
      std::uint16_t unit_index,
      std::uint16_t offset,
      const std::uint8_t* data,
      std::size_t length);

  bool active() const;
  bool complete() const;
  SpeakerAssetsRegion region() const;
  std::uint16_t unit_index() const;
  std::uint16_t expected_bytes() const;
  std::size_t received_bytes() const;
  std::size_t first_missing_offset() const;
  [[nodiscard]] SpeakerAssetsProtocolResult copy_received_bitmap(
      std::size_t bitmap_offset,
      std::uint8_t* destination,
      std::size_t length) const;
  const std::uint8_t* data() const;

 private:
  bool byte_received(std::size_t index) const;
  void mark_byte_received(std::size_t index);

  std::array<std::uint8_t, kSoundPayloadBlockSize> bytes_{};
  std::array<std::uint8_t, kSoundPayloadBlockSize / 8U> received_{};
  std::size_t received_bytes_ = 0U;
  SpeakerAssetsRegion region_ = SpeakerAssetsRegion::Manifest;
  std::uint16_t unit_index_ = 0U;
  std::uint16_t expected_bytes_ = 0U;
  bool active_ = false;
};

}  // namespace easy_input::speaker_assets
