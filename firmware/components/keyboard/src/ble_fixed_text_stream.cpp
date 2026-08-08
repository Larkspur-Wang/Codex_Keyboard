#include "keyboard/ble_fixed_text_stream.h"

#include <algorithm>
#include <array>
#include <utility>

namespace ai_keyboard {

BleFixedTextStartStatus BleFixedTextStream::start(
    std::string_view text,
    BleOwnerToken owner) {
  if (pending()) {
    return BleFixedTextStartStatus::Busy;
  }
  if (text.empty()) {
    return BleFixedTextStartStatus::Empty;
  }
  if (!owner.valid()) {
    return BleFixedTextStartStatus::InvalidOwner;
  }
  const auto chunks = fixed_text_chunk_count(text.size());
  if (chunks == 0) {
    return BleFixedTextStartStatus::TooLarge;
  }

  text_.assign(text.data(), text.size());
  owner_ = owner;
  next_chunk_ = 0;
  total_chunks_ = chunks;
  return BleFixedTextStartStatus::Started;
}

BleFixedTextPumpResult BleFixedTextStream::pump(
    BleOwnerToken current_owner,
    HidReportQueue* queue,
    std::uint32_t queued_ms) {
  BleFixedTextPumpResult result;
  if (!pending()) {
    return result;
  }
  if (!current_owner.valid() || current_owner != owner_) {
    reset();
    result.owner_changed = true;
    return result;
  }
  if (queue == nullptr) {
    result.blocked = true;
    return result;
  }

  while (pending()) {
    if (queue->size() >= kBleFixedTextQueuedWindow) {
      result.blocked = true;
      return result;
    }
    const auto offset =
        static_cast<std::size_t>(next_chunk_) *
        kFixedTextAppCommandChunkDataLen;
    const auto chunk_len =
        std::min(kFixedTextAppCommandChunkDataLen, text_.size() - offset);
    std::array<std::uint8_t, kFixedTextAppCommandPayloadLen> report{};
    report[0] = kFixedTextAppCommandKind;
    report[1] = next_chunk_;
    report[2] = total_chunks_;
    report[3] = static_cast<std::uint8_t>(chunk_len);
    std::copy_n(
        reinterpret_cast<const std::uint8_t*>(text_.data()) + offset,
        chunk_len,
        report.data() + kFixedTextAppCommandHeaderLen);

    const auto pushed = queue->push_classified(
        kFixedTextAppCommandReportId,
        report.data(),
        report.size(),
        queued_ms,
        HidReportClass::AppCommand,
        owner_);
    if (!pushed.accepted()) {
      result.blocked = true;
      return result;
    }

    ++result.queued_chunks;
    ++next_chunk_;
    if (next_chunk_ == total_chunks_) {
      reset();
      result.completed = true;
      return result;
    }
  }
  return result;
}

void BleFixedTextStream::reset() {
  std::string empty;
  text_.swap(empty);
  owner_ = {};
  next_chunk_ = 0;
  total_chunks_ = 0;
}

bool BleFixedTextStream::pending() const {
  return total_chunks_ != 0;
}

BleOwnerToken BleFixedTextStream::owner() const {
  return owner_;
}

std::uint8_t BleFixedTextStream::next_chunk() const {
  return next_chunk_;
}

std::uint8_t BleFixedTextStream::total_chunks() const {
  return total_chunks_;
}

std::size_t BleFixedTextStream::remaining_bytes() const {
  if (!pending()) {
    return 0;
  }
  const auto offset =
      static_cast<std::size_t>(next_chunk_) *
      kFixedTextAppCommandChunkDataLen;
  return text_.size() - offset;
}

}  // namespace ai_keyboard
