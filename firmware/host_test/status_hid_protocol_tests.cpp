#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

#include "keyboard/config_receiver.h"
#include "keyboard/status_hid_protocol.h"

namespace {

std::array<std::uint8_t, ai_keyboard::kStatusRequestPayloadLen> request_payload(
    std::uint32_t request_id,
    std::uint8_t flags = ai_keyboard::kStatusRequestFlagsNone) {
  std::array<std::uint8_t, ai_keyboard::kStatusRequestPayloadLen> payload{};
  payload[0] = 'S';
  payload[1] = '3';
  payload[2] = 'R';
  payload[3] = ai_keyboard::kStatusHidProtocolVersion;
  payload[4] = static_cast<std::uint8_t>(request_id & 0xFF);
  payload[5] = static_cast<std::uint8_t>((request_id >> 8) & 0xFF);
  payload[6] = static_cast<std::uint8_t>((request_id >> 16) & 0xFF);
  payload[7] = static_cast<std::uint8_t>((request_id >> 24) & 0xFF);
  payload[8] = flags;
  return payload;
}

std::uint16_t read_u16_le(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t read_u32_le(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

void decodes_payload_with_or_without_report_id() {
  constexpr std::uint32_t kRequestId = 0x78563412;
  auto payload = request_payload(kRequestId);
  ai_keyboard::StatusHidRequest request;
  assert(ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));
  assert(request.request_id == kRequestId);
  assert(request.flags == ai_keyboard::kStatusRequestFlagsNone);

  payload = request_payload(kRequestId, ai_keyboard::kStatusRequestFlagFresh);
  request = {};
  assert(ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));
  assert(request.request_id == kRequestId);
  assert(request.flags == ai_keyboard::kStatusRequestFlagFresh);

  std::array<std::uint8_t, ai_keyboard::kStatusRequestPayloadLen + 1> prefixed{};
  prefixed[0] = ai_keyboard::kStatusRequestReportId;
  std::copy(payload.begin(), payload.end(), prefixed.begin() + 1);
  request = {};
  assert(ai_keyboard::decode_status_hid_request(
      prefixed.data(), prefixed.size(), &request));
  assert(request.request_id == kRequestId);
  assert(request.flags == ai_keyboard::kStatusRequestFlagFresh);
}

void rejects_malformed_requests() {
  auto payload = request_payload(1);
  ai_keyboard::StatusHidRequest request;
  assert(!ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size() - 1, &request));

  payload = request_payload(1);
  payload[0] = 'X';
  assert(!ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));

  payload = request_payload(1);
  payload[3] = 2;
  assert(!ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));

  payload = request_payload(0);
  assert(!ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));

  payload = request_payload(1, 0x02);
  assert(!ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));

  payload = request_payload(1, 0x03);
  assert(!ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));

  payload = request_payload(1);
  payload[15] = 1;
  assert(!ai_keyboard::decode_status_hid_request(
      payload.data(), payload.size(), &request));
}

void encodes_exact_status_chunks_and_reassembles_json() {
  constexpr std::uint32_t kRequestId = 0x12345678;
  const std::string json(512, 'j');
  const auto total_chunks =
      ai_keyboard::status_hid_response_chunk_count(json.size());
  assert(total_chunks == 11);
  std::string reassembled;

  for (std::uint8_t index = 0; index < total_chunks; ++index) {
    std::array<std::uint8_t, ai_keyboard::kStatusAppCommandPayloadLen> report{};
    assert(ai_keyboard::encode_status_hid_response_chunk(
        kRequestId, json, index, &report));
    assert(report[0] == ai_keyboard::kStatusResponseCommandKind);
    assert(report[1] == index);
    assert(report[2] == total_chunks);
    const auto expected_json_len = std::min<std::size_t>(
        ai_keyboard::kStatusResponseJsonPerChunk,
        json.size() -
            static_cast<std::size_t>(index) *
                ai_keyboard::kStatusResponseJsonPerChunk);
    assert(report[3] ==
           ai_keyboard::kStatusResponseMetadataLen + expected_json_len);
    assert(report[4] == ai_keyboard::kStatusHidProtocolVersion);
    assert(read_u32_le(report.data() + 5) == kRequestId);
    assert(read_u16_le(report.data() + 9) == json.size());
    assert(read_u16_le(report.data() + 11) ==
           ai_keyboard::crc16_ccitt(
               reinterpret_cast<const std::uint8_t*>(json.data()), json.size()));
    const auto json_len =
        report[3] - ai_keyboard::kStatusResponseMetadataLen;
    reassembled.append(
        reinterpret_cast<const char*>(report.data() + 13), json_len);
  }
  assert(reassembled == json);
}

void handles_chunk_boundaries_and_rejects_invalid_responses() {
  assert(ai_keyboard::status_hid_response_chunk_count(0) == 0);
  assert(ai_keyboard::status_hid_response_chunk_count(1) == 1);
  assert(ai_keyboard::status_hid_response_chunk_count(50) == 1);
  assert(ai_keyboard::status_hid_response_chunk_count(51) == 2);
  static_assert(ai_keyboard::kStatusResponseMaxJsonLen == 512);
  assert(ai_keyboard::status_hid_response_chunk_count(512) == 11);
  assert(ai_keyboard::status_hid_response_chunk_count(513) == 0);

  std::array<std::uint8_t, ai_keyboard::kStatusAppCommandPayloadLen> report{};
  assert(!ai_keyboard::encode_status_hid_response_chunk(
      0, "{}", 0, &report));
  assert(!ai_keyboard::encode_status_hid_response_chunk(
      1, "", 0, &report));
  assert(!ai_keyboard::encode_status_hid_response_chunk(
      1, "{}", 1, &report));

  const std::string json(51, 'x');
  assert(ai_keyboard::encode_status_hid_response_chunk(
      1, json, 1, &report));
  assert(report[3] == ai_keyboard::kStatusResponseMetadataLen + 1);
  for (std::size_t index = 14; index < report.size(); ++index) {
    assert(report[index] == 0);
  }
}

void response_stream_supersedes_stale_request_and_restarts_at_chunk_zero() {
  ai_keyboard::StatusHidResponseStream stream;
  std::array<std::uint8_t, ai_keyboard::kStatusAppCommandPayloadLen> report{};

  assert(stream.replace(0x11111111, std::string(80, 'a')));
  assert(stream.pending());
  assert(stream.total_chunks() == 2);
  assert(stream.encode_next(&report));
  assert(read_u32_le(report.data() + 5) == 0x11111111);
  assert(report[1] == 0);
  assert(!stream.mark_next_sent());
  assert(stream.next_chunk() == 1);

  // A retry carries a new ID. It must replace stale chunks rather than being
  // consumed behind them, and the new stream must begin at chunk zero.
  assert(stream.replace(0x22222222, std::string(51, 'b')));
  assert(stream.request_id() == 0x22222222);
  assert(stream.next_chunk() == 0);
  assert(stream.total_chunks() == 2);
  assert(stream.encode_next(&report));
  assert(read_u32_le(report.data() + 5) == 0x22222222);
  assert(report[1] == 0);
  assert(!stream.mark_next_sent());
  assert(stream.encode_next(&report));
  assert(report[1] == 1);
  assert(stream.mark_next_sent());
  assert(!stream.pending());

  assert(!stream.replace(0, "{}"));
  assert(!stream.replace(1, std::string(513, 'x')));
  assert(!stream.pending());
}

}  // namespace

int main() {
  decodes_payload_with_or_without_report_id();
  rejects_malformed_requests();
  encodes_exact_status_chunks_and_reassembles_json();
  handles_chunk_boundaries_and_rejects_invalid_responses();
  response_stream_supersedes_stale_request_and_restarts_at_chunk_zero();
  return 0;
}
