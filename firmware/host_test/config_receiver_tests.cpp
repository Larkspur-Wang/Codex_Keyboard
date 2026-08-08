#include <cassert>
#include <string>
#include <vector>

#include "keyboard/config_receiver.h"

using ai_keyboard::ConfigReceiveStatus;

namespace {

std::vector<std::uint8_t> report(std::uint8_t index,
                                 std::uint8_t total_chunks,
                                 std::uint16_t total_len,
                                 std::uint8_t chunk_len,
                                 std::uint16_t crc,
                                 const std::string& chunk) {
  std::vector<std::uint8_t> packet(11 + chunk.size(), 0);
  packet[0] = 'S';
  packet[1] = '3';
  packet[2] = 'C';
  packet[3] = 1;
  packet[4] = index;
  packet[5] = total_chunks;
  packet[6] = static_cast<std::uint8_t>(total_len);
  packet[7] = static_cast<std::uint8_t>(total_len >> 8);
  packet[8] = chunk_len;
  packet[9] = static_cast<std::uint8_t>(crc);
  packet[10] = static_cast<std::uint8_t>(crc >> 8);
  for (std::size_t i = 0; i < chunk.size(); ++i) {
    packet[11 + i] = static_cast<std::uint8_t>(chunk[i]);
  }
  return packet;
}

}  // namespace

void receives_single_chunk_json() {
  const std::string json = R"({"schema":"ai_keyboard.v1"})";
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()),
      json.size());
  ai_keyboard::ConfigReceiver receiver;

  const auto packet = report(0, 1, json.size(), json.size(), crc, json);
  const auto result = receiver.receive(packet.data(), packet.size());

  assert(result.status == ConfigReceiveStatus::Complete);
  assert(result.json == json);
}

void accepts_hid_report_id_prefix() {
  const std::string json = R"({"ptt_hotkey":"F12"})";
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()),
      json.size());
  auto packet = report(0, 1, json.size(), json.size(), crc, json);
  packet.insert(packet.begin(), 0x10);
  ai_keyboard::ConfigReceiver receiver;

  const auto result = receiver.receive(packet.data(), packet.size());

  assert(result.status == ConfigReceiveStatus::Complete);
  assert(result.json == json);
}

void accepts_padded_gatt_write() {
  const std::string json = R"({"ptt_hotkey":"F13"})";
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()),
      json.size());
  auto packet = report(0, 1, json.size(), json.size(), crc, json);
  packet.resize(63, 0);
  ai_keyboard::ConfigReceiver receiver;

  const auto result = receiver.receive(packet.data(), packet.size());

  assert(result.status == ConfigReceiveStatus::Complete);
  assert(result.json == json);
}

void accepts_padded_hid_feature_report() {
  const std::string json = R"({"ptt_hotkey":"F14"})";
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()),
      json.size());
  auto packet = report(0, 1, json.size(), json.size(), crc, json);
  packet.insert(packet.begin(), 0x10);
  packet.resize(64, 0);
  ai_keyboard::ConfigReceiver receiver;

  const auto result = receiver.receive(packet.data(), packet.size());

  assert(result.status == ConfigReceiveStatus::Complete);
  assert(result.json == json);
}

void rejects_bad_crc() {
  const std::string json = R"({"schema":"ai_keyboard.v1"})";
  ai_keyboard::ConfigReceiver receiver;

  const auto packet = report(0, 1, json.size(), json.size(), 0x1234, json);
  const auto result = receiver.receive(packet.data(), packet.size());

  assert(result.status == ConfigReceiveStatus::CrcMismatch);
  assert(result.json.empty());
}

void rejects_out_of_order_chunks() {
  const std::string json = "abcdefghijklmnopqrstuvwxyz";
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()),
      json.size());
  ai_keyboard::ConfigReceiver receiver;

  const auto packet = report(1, 2, json.size(), 13, crc, json.substr(13));
  const auto result = receiver.receive(packet.data(), packet.size());

  assert(result.status == ConfigReceiveStatus::OutOfOrder);
  assert(result.json.empty());
}

void endpoint_change_discards_partial_message() {
  const std::string json = "abcdefghijklmnopqrstuvwxyz";
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()),
      json.size());
  const auto first = report(0, 2, json.size(), 13, crc, json.substr(0, 13));
  const auto second = report(1, 2, json.size(), 13, crc, json.substr(13));
  ai_keyboard::EndpointBoundConfigReceiver receiver;

  const auto pending = receiver.receive(41, first.data(), first.size());
  assert(pending.status == ConfigReceiveStatus::Pending);
  assert(receiver.endpoint_epoch() == 41);

  // Host B cannot finish Host A's partially assembled config, even if it
  // presents the exact continuation chunk.
  const auto cross_endpoint = receiver.receive(42, second.data(), second.size());
  assert(cross_endpoint.status == ConfigReceiveStatus::OutOfOrder);
  assert(cross_endpoint.json.empty());
  assert(receiver.endpoint_epoch() == 42);

  // A complete message entirely within Host B's lifetime is still accepted.
  const auto restarted = receiver.receive(42, first.data(), first.size());
  assert(restarted.status == ConfigReceiveStatus::Pending);
  const auto complete = receiver.receive(42, second.data(), second.size());
  assert(complete.status == ConfigReceiveStatus::Complete);
  assert(complete.json == json);
}

void endpoint_bound_receiver_rejects_zero_epoch() {
  const std::string json = R"({"schema":"ai_keyboard.v1"})";
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()),
      json.size());
  const auto packet = report(0, 1, json.size(), json.size(), crc, json);
  ai_keyboard::EndpointBoundConfigReceiver receiver;

  const auto result = receiver.receive(0, packet.data(), packet.size());
  assert(result.status == ConfigReceiveStatus::InvalidReport);
  assert(result.json.empty());
  assert(receiver.endpoint_epoch() == 0);
}

void accepts_exactly_the_2048_byte_limit() {
  static_assert(ai_keyboard::kConfigMaxJsonLen == 2048);
  const std::string json(2048, 'x');
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
  const auto total_chunks = static_cast<std::uint8_t>(
      (json.size() + ai_keyboard::kConfigMaxChunkData - 1) /
      ai_keyboard::kConfigMaxChunkData);
  ai_keyboard::ConfigReceiver receiver;
  ai_keyboard::ConfigReceiveResult result;

  for (std::uint8_t index = 0; index < total_chunks; ++index) {
    const auto offset = static_cast<std::size_t>(index) *
                        ai_keyboard::kConfigMaxChunkData;
    const auto chunk = json.substr(offset, ai_keyboard::kConfigMaxChunkData);
    const auto packet = report(index,
                               total_chunks,
                               static_cast<std::uint16_t>(json.size()),
                               static_cast<std::uint8_t>(chunk.size()),
                               crc,
                               chunk);
    result = receiver.receive(packet.data(), packet.size());
    assert(result.status == (index + 1 == total_chunks
                                 ? ConfigReceiveStatus::Complete
                                 : ConfigReceiveStatus::Pending));
  }
  assert(result.json == json);
}

void rejects_2049_bytes() {
  const std::string json(2049, 'x');
  const auto crc = ai_keyboard::crc16_ccitt(
      reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
  const auto total_chunks = static_cast<std::uint8_t>(
      (json.size() + ai_keyboard::kConfigMaxChunkData - 1) /
      ai_keyboard::kConfigMaxChunkData);
  const auto chunk = json.substr(0, ai_keyboard::kConfigMaxChunkData);
  const auto packet = report(0,
                             total_chunks,
                             static_cast<std::uint16_t>(json.size()),
                             static_cast<std::uint8_t>(chunk.size()),
                             crc,
                             chunk);
  ai_keyboard::ConfigReceiver receiver;

  const auto result = receiver.receive(packet.data(), packet.size());
  assert(result.status == ConfigReceiveStatus::MalformedChunk);
  assert(result.json.empty());
}

int main() {
  receives_single_chunk_json();
  accepts_hid_report_id_prefix();
  accepts_padded_gatt_write();
  accepts_padded_hid_feature_report();
  rejects_bad_crc();
  rejects_out_of_order_chunks();
  endpoint_change_discards_partial_message();
  endpoint_bound_receiver_rejects_zero_epoch();
  accepts_exactly_the_2048_byte_limit();
  rejects_2049_bytes();
  return 0;
}
