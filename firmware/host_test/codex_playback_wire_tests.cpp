#include "keyboard/codex_playback_wire.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, easy_codex::kPlaybackRequestBytes>
    kRequestGolden{{
        0x45, 0x49, 0x50, 0x52, 0x01, 0x02, 0x00, 0x00,
        0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x0f, 0x90, 0x68, 0xe8, 0xdf, 0xef, 0x3f, 0x65,
        0x70, 0x5b, 0xc6, 0x69, 0x24, 0xa7, 0xa3, 0x5b,
    }};

constexpr std::array<std::uint8_t, easy_codex::kMailboxStatusBytes>
    kMailboxGolden{{
        0x45, 0x49, 0x4d, 0x42, 0x02, 0x05, 0x00, 0x00,
        0x44, 0x33, 0x22, 0x11, 0x07, 0x00, 0x02, 0x00,
        0x0e, 0xd9, 0x0e, 0x14, 0x2c, 0x64, 0x33, 0x4f,
        0xd4, 0xf0, 0xc1, 0x86, 0xc9, 0x49, 0x70, 0xa0,
    }};

std::uint8_t hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  return static_cast<std::uint8_t>(value - 'a' + 10);
}

std::vector<std::uint8_t> from_hex(std::string_view value) {
  assert(value.size() % 2U == 0U);
  std::vector<std::uint8_t> bytes(value.size() / 2U);
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(
        (hex_nibble(value[index * 2U]) << 4U) |
        hex_nibble(value[index * 2U + 1U]));
  }
  return bytes;
}

std::string to_hex(const std::uint8_t* bytes, std::size_t length) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string value(length * 2U, '0');
  for (std::size_t index = 0U; index < length; ++index) {
    value[index * 2U] = kHex[bytes[index] >> 4U];
    value[index * 2U + 1U] = kHex[bytes[index] & 0x0fU];
  }
  return value;
}

void request_matches_rust_golden_and_authenticates() {
  std::array<std::uint8_t, 32> key{};
  key.fill(0x11U);
  std::array<std::uint8_t, easy_codex::kPlaybackRequestBytes> encoded{};
  const easy_codex::PlaybackWireRequest request{
      2U,
      0x11223344U,
      0x55667788U,
      0x0102030405060708ULL,
  };
  assert(easy_codex::encode_playback_request(
      request, key, encoded.data(), encoded.size()));
  assert(encoded == kRequestGolden);

  easy_codex::PlaybackWireRequest decoded{};
  assert(easy_codex::decode_playback_request(
      encoded.data(), encoded.size(), key, &decoded));
  assert(decoded.slot == request.slot);
  assert(decoded.request_generation == request.request_generation);
  assert(decoded.connection_generation == request.connection_generation);
  assert(decoded.nonce == request.nonce);

  encoded[8] ^= 1U;
  assert(!easy_codex::decode_playback_request(
      encoded.data(), encoded.size(), key, &decoded));
}

void host_packets_decode_and_device_packets_encode() {
  std::array<std::uint8_t, 32> key{};
  key.fill(0x11U);
  const easy_codex::PlaybackWireIdentity identity{
      2U,
      0x11223344U,
      0x55667788U,
      0x0102030405060708ULL,
      0x1112131415161718ULL,
  };

  const auto begin_packet = from_hex(
      "45495042010200004433221188776655080706050403020118171615141312110108000001770100000000000004000028272625242322215eb7cb9543aaff9d9921aca91d17ac4c");
  easy_codex::PlaybackWireBegin begin{};
  assert(easy_codex::decode_playback_begin(
      begin_packet.data(), begin_packet.size(), key, &begin));
  assert(easy_codex::playback_wire_identity_equal(begin.identity, identity));
  assert(begin.total_bytes == 2049U);
  assert(begin.total_samples == 96001U);
  assert(begin.chunk_bytes == 1024U);
  assert(begin.request_nonce == 0x2122232425262728ULL);

  const auto data_packet = from_hex(
      "4549504401020000443322118877665508070605040302011817161514131211000400000500000072c72e7422760e9748f0f43d33155889a40b43f66e");
  assert(std::search(data_packet.begin(), data_packet.end(),
                     reinterpret_cast<const std::uint8_t*>("frame"),
                     reinterpret_cast<const std::uint8_t*>("frame") + 5U) ==
         data_packet.end());
  easy_codex::PlaybackWireData data{};
  std::array<std::uint8_t, easy_codex::kPlaybackChunkBytes> plaintext{};
  assert(easy_codex::decode_playback_data(
      data_packet.data(), data_packet.size(), begin.request_nonce, key,
      plaintext.data(), plaintext.size(), &data));
  assert(easy_codex::playback_wire_identity_equal(data.identity, identity));
  assert(data.offset == 1024U);
  assert(data.payload_length == 5U);
  assert(std::string_view(
             reinterpret_cast<const char*>(data.payload),
             data.payload_length) == "frame");
  assert(!easy_codex::decode_playback_data(
      data_packet.data(), data_packet.size(), begin.request_nonce + 1U, key,
      plaintext.data(), plaintext.size(), &data));

  const auto finished_ack_packet = from_hex(
      "4549504b01020000443322118877665508070605040302011817161514131211c698df6a9dcfb02955ab714203c52598");
  easy_codex::PlaybackWireIdentity acknowledged{};
  std::uint8_t status = 1U;
  assert(easy_codex::decode_playback_finished_ack(
      finished_ack_packet.data(), finished_ack_packet.size(), key,
      &acknowledged, &status));
  assert(easy_codex::playback_wire_identity_equal(acknowledged, identity));
  assert(status == 0U);

  const auto cancel_ack_packet = from_hex(
      "4549504b01020100443322118877665508070605040302011817161514131211d807630e0ebf8ef128cad06c93fefb67");
  assert(easy_codex::decode_playback_finished_ack(
      cancel_ack_packet.data(), cancel_ack_packet.size(), key,
      &acknowledged, &status));
  assert(easy_codex::playback_wire_identity_equal(acknowledged, identity));
  assert(status == 1U);

  std::array<std::uint8_t, easy_codex::kPlaybackAckBytes> ack{};
  assert(easy_codex::encode_playback_ack(
      {identity, 0U, 1029U}, key, ack.data(), ack.size()));
  assert(ack[0] == 'E' && ack[3] == 'A');
  assert(to_hex(ack.data(), ack.size()) ==
         "454950410102000044332211887766550807060504030201181716151413121105040000d80cbc8e56210eb573dd2ba2bedad79b");

  std::array<std::uint8_t, easy_codex::kPlaybackFinishedBytes> finished{};
  assert(easy_codex::encode_playback_finished(
      {identity, 96001U}, key, finished.data(), finished.size()));
  assert(finished[0] == 'E' && finished[3] == 'F');
  assert(to_hex(finished.data(), finished.size()) ==
         "45495046010200004433221188776655080706050403020118171615141312110177010000000000fe239befd07010e57b2e4fbfdf274034");
}

void replayed_data_must_match_the_received_prefix() {
  const std::array<std::uint8_t, 8> received{{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}};
  const std::array<std::uint8_t, 3> exact{{4U, 5U, 6U}};
  const std::array<std::uint8_t, 3> changed{{4U, 9U, 6U}};
  easy_codex::PlaybackWireData replay{};
  replay.offset = 3U;
  replay.payload = exact.data();
  replay.payload_length = exact.size();
  assert(easy_codex::playback_data_matches_received_prefix(
      replay, received.data(), received.size()));

  replay.payload = changed.data();
  assert(!easy_codex::playback_data_matches_received_prefix(
      replay, received.data(), received.size()));

  replay.payload = exact.data();
  replay.offset = 7U;
  assert(!easy_codex::playback_data_matches_received_prefix(
      replay, received.data(), received.size()));
}

void mailbox_status_matches_rust_golden_and_fails_closed() {
  std::array<std::uint8_t, 32> key{};
  key.fill(0x11U);
  easy_codex::MailboxWireStatus status{};
  assert(easy_codex::decode_mailbox_status(
      kMailboxGolden.data(), kMailboxGolden.size(), key, &status));
  assert(status.unread_slots == 0x05U);
  assert(status.heartbeat_sequence == 0x11223344U);
  assert((status.coverage_by_slot ==
          std::array<std::uint8_t, 4>{7U, 0U, 2U, 0U}));

  auto tampered = kMailboxGolden;
  tampered[12U] ^= 1U;
  assert(!easy_codex::decode_mailbox_status(
      tampered.data(), tampered.size(), key, &status));
}

}  // namespace

int main() {
  request_matches_rust_golden_and_authenticates();
  host_packets_decode_and_device_packets_encode();
  replayed_data_must_match_the_received_prefix();
  mailbox_status_matches_rust_golden_and_fails_closed();
  return 0;
}
