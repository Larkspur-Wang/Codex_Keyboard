#include "keyboard/encoder.h"

namespace ai_keyboard {
namespace {

int transition_delta(std::uint8_t previous, std::uint8_t current) {
  const auto transition = static_cast<std::uint8_t>(((previous & 0x03) << 2) | (current & 0x03));
  switch (transition) {
    case 0b0001:
    case 0b0111:
    case 0b1110:
    case 0b1000:
      return 1;
    case 0b0010:
    case 0b1011:
    case 0b1101:
    case 0b0100:
      return -1;
    default:
      return 0;
  }
}

}  // namespace

bool EncoderStepQueue::push(int step) {
  if (step == 0) {
    return true;
  }

  if (size_ > 0) {
    const auto tail = (head_ + size_ - 1) % steps_.size();
    const bool same_direction = (steps_[tail] > 0) == (step > 0);
    if (same_direction) {
      steps_[tail] += step;
      return true;
    }
  }

  if (size_ >= steps_.size()) {
    return false;
  }
  const auto tail = (head_ + size_) % steps_.size();
  steps_[tail] = step;
  ++size_;
  return true;
}

bool EncoderStepQueue::pop(int* step) {
  if (step == nullptr || size_ == 0) {
    return false;
  }
  *step = steps_[head_];
  steps_[head_] = 0;
  head_ = (head_ + 1) % steps_.size();
  --size_;
  return true;
}

void EncoderStepQueue::clear() {
  steps_ = {};
  head_ = 0;
  size_ = 0;
}

bool EncoderStepQueue::empty() const {
  return size_ == 0;
}

std::size_t EncoderStepQueue::size() const {
  return size_;
}

void EncoderDecoder::reset(std::uint8_t state) {
  previous_state_ = state & 0x03;
  accumulator_ = 0;
  armed_ = previous_state_ == 0;
}

int EncoderDecoder::update(std::uint8_t state) {
  state &= 0x03;
  if (state == previous_state_) {
    return 0;
  }

  const auto delta = transition_delta(previous_state_, state);
  previous_state_ = state;
  if (delta == 0) {
    ++invalid_transition_count_;
    if (accumulator_ != 0) {
      ++partial_reset_count_;
    }
    accumulator_ = 0;
    if (state == 0) {
      armed_ = true;
    }
    return 0;
  }

  if (!armed_) {
    if (state == 0) {
      armed_ = true;
      accumulator_ = 0;
    }
    return 0;
  }

  accumulator_ += delta;
  if (state != 0) {
    return 0;
  }

  const auto completed_step = accumulator_;
  accumulator_ = 0;
  if (completed_step >= 4) {
    return 1;
  }
  if (completed_step <= -4) {
    return -1;
  }
  if (completed_step != 0) {
    ++partial_reset_count_;
  }
  return 0;
}

std::uint32_t EncoderDecoder::invalid_transition_count() const {
  return invalid_transition_count_;
}

std::uint32_t EncoderDecoder::partial_reset_count() const {
  return partial_reset_count_;
}

}  // namespace ai_keyboard
