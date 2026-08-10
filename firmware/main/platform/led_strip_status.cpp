#include "platform/led_strip_status.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "keyboard/mailbox_led.h"

namespace easy_input {
namespace {

const char* const kTag = "status_led";
constexpr std::uint32_t kRmtResolutionHz = 20 * 1000 * 1000;
constexpr std::uint16_t kT0hTicks = 6;       // 300 ns at 20 MHz
constexpr std::uint16_t kT0lTicks = 18;      // 900 ns at 20 MHz
constexpr std::uint16_t kT1hTicks = 16;      // 800 ns at 20 MHz
constexpr std::uint16_t kT1lTicks = 12;      // 600 ns at 20 MHz
constexpr std::uint16_t kResetTicks = 6000;  // 300 us at 20 MHz
constexpr std::size_t kWs2812SymbolCount = (ai_keyboard::kWs2812Count * 24U) + 1U;
// Five WS2812 pixels finish in under 1 ms. The project tick is 10 ms, so a
// literal 1 ms timeout becomes zero ticks; allow two ticks for completion
// while still bounding a faulty transfer.
constexpr int kRmtFlushWaitMs = 2 * portTICK_PERIOD_MS;

Rgb to_rgb(ai_keyboard::FeedbackColor color) {
  return {color.red, color.green, color.blue};
}

Rgb scale_rgb(Rgb color, std::uint8_t numerator, std::uint8_t denominator) {
  return {
      static_cast<std::uint8_t>((static_cast<unsigned>(color.red) * numerator) / denominator),
      static_cast<std::uint8_t>((static_cast<unsigned>(color.green) * numerator) / denominator),
      static_cast<std::uint8_t>((static_cast<unsigned>(color.blue) * numerator) / denominator),
  };
}

bool deadline_pending(std::uint32_t now_ms, std::uint32_t deadline_ms) {
  return static_cast<std::int32_t>(deadline_ms - now_ms) > 0;
}

Rgb agent_status_color(ai_keyboard::AgentStatusState state) {
  switch (state) {
    case ai_keyboard::AgentStatusState::kRunning:
      return {0, 0, 28};
    case ai_keyboard::AgentStatusState::kWaitingUser:
      return {30, 12, 0};
    case ai_keyboard::AgentStatusState::kCompletedUnread:
      return {0, 24, 5};
    case ai_keyboard::AgentStatusState::kFailed:
      return {30, 0, 8};
    case ai_keyboard::AgentStatusState::kIdle:
      return {};
  }
  return {};
}

void set_scaled_pixel(std::array<Rgb, ai_keyboard::kWs2812Count>* leds,
                      std::size_t index,
                      Rgb color,
                      std::uint8_t numerator,
                      std::uint8_t denominator) {
  if (leds == nullptr || index >= leds->size()) {
    return;
  }
  (*leds)[index] = scale_rgb(color, numerator, denominator);
}

Rgb rainbow_color(std::uint8_t index) {
  constexpr std::array<Rgb, 7> kRainbow{{
      {46, 0, 5},
      {46, 17, 0},
      {38, 35, 0},
      {0, 43, 11},
      {0, 36, 43},
      {12, 0, 47},
      {42, 0, 36},
  }};
  return kRainbow[index % kRainbow.size()];
}

void encode_ws2812_byte(std::uint8_t value, rmt_symbol_word_t* out) {
  if (out == nullptr) {
    return;
  }
  for (std::uint8_t bit = 0; bit < 8; ++bit) {
    const bool one = (value & (1U << (7U - bit))) != 0;
    out[bit].level0 = 1;
    out[bit].duration0 = one ? kT1hTicks : kT0hTicks;
    out[bit].level1 = 0;
    out[bit].duration1 = one ? kT1lTicks : kT0lTicks;
  }
}

}  // namespace

esp_err_t StatusLedStrip::begin() {
  rmt_tx_channel_config_t rmt_config = {};
  rmt_config.gpio_num = static_cast<gpio_num_t>(ai_keyboard::kWs2812Pin);
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = kRmtResolutionHz;
  rmt_config.mem_block_symbols = 64;
  rmt_config.trans_queue_depth = 1;
  rmt_config.flags.invert_out = false;
  rmt_config.flags.with_dma = false;
  const esp_err_t channel_err = rmt_new_tx_channel(&rmt_config, &rmt_channel_);
  if (channel_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_new_tx_channel GPIO%u failed: %s",
             static_cast<unsigned>(ai_keyboard::kWs2812Pin),
             esp_err_to_name(channel_err));
    return channel_err;
  }

  rmt_copy_encoder_config_t encoder_config = {};
  const esp_err_t encoder_err = rmt_new_copy_encoder(&encoder_config, &rmt_copy_encoder_);
  if (encoder_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_new_copy_encoder failed: %s", esp_err_to_name(encoder_err));
    return encoder_err;
  }

  clear();

  ESP_LOGI(kTag,
           "configured WS2812 custom RMT GPIO%u count=%u rmt_hz=%lu timing=(%u,%u,%u,%u)",
           static_cast<unsigned>(ai_keyboard::kWs2812Pin),
           static_cast<unsigned>(ai_keyboard::kWs2812Count),
           static_cast<unsigned long>(kRmtResolutionHz),
           static_cast<unsigned>(kT0hTicks),
           static_cast<unsigned>(kT0lTicks),
           static_cast<unsigned>(kT1hTicks),
           static_cast<unsigned>(kT1lTicks));
  return ESP_OK;
}

bool StatusLedStrip::ready() const {
  return rmt_channel_ != nullptr && rmt_copy_encoder_ != nullptr;
}

void StatusLedStrip::show_raw_color(Rgb color) {
  active_feedback_ = {};
  effect_until_ms_ = 0;
  last_frame_ms_ = 0;
  set_all(color);
  if (ready()) {
    flush();
  }
  idle_rendered_ = false;
}

void StatusLedStrip::show_pixel(std::size_t index, Rgb color) {
  active_feedback_ = {};
  effect_until_ms_ = 0;
  last_frame_ms_ = 0;
  set_all({});
  if (index < leds_.size()) {
    leds_[index] = color;
  }
  if (ready()) {
    flush();
  }
  idle_rendered_ = false;
}

void StatusLedStrip::clear() {
  active_feedback_ = {};
  agent_status_ = {};
  agent_status_active_ = false;
  agent_status_rendered_ = false;
  agent_status_expires_ms_ = 0;
  mailbox_unread_slots_ = 0U;
  mailbox_coverage_by_slot_ = {};
  running_tasks_ = 0U;
  mailbox_status_active_ = false;
  set_all({});
  esp_err_t err = ESP_OK;
  if (ready()) {
    err = flush();
  }
  idle_rendered_ = true;
  (void)err;
}

esp_err_t StatusLedStrip::prepare_for_deep_sleep() {
  if (!ready()) {
    return ESP_ERR_INVALID_STATE;
  }
  set_all({});
  const esp_err_t err = flush();
  if (err != ESP_OK) {
    ESP_LOGE(kTag,
             "deep sleep black frame failed: %s",
             esp_err_to_name(err));
    return err;
  }
  // Preserve the logical effect/status state until deep sleep actually
  // starts. If a late blocker cancels shutdown, the next update can render
  // that state again instead of leaving a still-awake device artificially
  // blank.
  return ESP_OK;
}

void StatusLedStrip::set_agent_status(const ai_keyboard::AgentStatusCommand& command,
                                      std::uint32_t now_ms) {
  agent_status_ = command;
  if (command.state == ai_keyboard::AgentStatusState::kIdle || command.ttl_ms == 0) {
    agent_status_active_ = false;
    agent_status_rendered_ = false;
    agent_status_expires_ms_ = 0;
  } else {
    agent_status_active_ = true;
    agent_status_expires_ms_ = now_ms + command.ttl_ms;
  }

  idle_rendered_ = false;
  if (!active_feedback_.active) {
    render_background_status(now_ms);
    idle_rendered_ = true;
  }
}

void StatusLedStrip::set_mailbox_status(std::uint8_t unread_slots,
                                        const std::array<std::uint8_t, 4>& coverage_by_slot,
                                        std::uint8_t running_tasks,
                                        std::uint32_t now_ms) {
  bool valid = (unread_slots & 0xF0U) == 0U && running_tasks <= 4U;
  for (std::size_t index = 0U; index < coverage_by_slot.size(); ++index) {
    const bool unread = (unread_slots & (1U << index)) != 0U;
    valid = valid && unread == (coverage_by_slot[index] != 0U);
  }
  mailbox_unread_slots_ = valid ? unread_slots : 0U;
  mailbox_coverage_by_slot_ = valid ? coverage_by_slot
                                    : std::array<std::uint8_t, 4>{};
  running_tasks_ = valid ? running_tasks : 0U;
  mailbox_status_active_ = valid;
  idle_rendered_ = false;
  if (!active_feedback_.active) {
    render_background_status(now_ms);
    idle_rendered_ = true;
  }
}

void StatusLedStrip::show_scroll_event(std::int8_t vertical,
                                       std::int8_t horizontal,
                                       std::uint32_t now_ms) {
  if (vertical == 0 && horizontal == 0) {
    return;
  }

  const bool horizontal_scroll = horizontal != 0;
  const auto value = horizontal_scroll ? horizontal : vertical;
  show_feedback({true,
                 ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                 value > 0 ? ai_keyboard::FeedbackDirection::Right
                           : ai_keyboard::FeedbackDirection::Left,
                 horizontal_scroll ? ai_keyboard::FeedbackColor{0, 22, 22}
                                   : ai_keyboard::FeedbackColor{0, 0, 28},
                 160,
                 40},
                now_ms);
}

void StatusLedStrip::show_status_event(StatusLedEvent event, std::uint32_t now_ms) {
  switch (event) {
    case StatusLedEvent::BleConnected:
      show_feedback({true,
                     ai_keyboard::FeedbackEffectKind::LightBarRipple,
                     ai_keyboard::FeedbackDirection::None,
                     {0, 0, 28},
                     900,
                     180},
                    now_ms);
      return;
    case StatusLedEvent::BleDisconnected:
      show_feedback({true,
                     ai_keyboard::FeedbackEffectKind::ConfirmPulse,
                     ai_keyboard::FeedbackDirection::None,
                     {4, 4, 8},
                     420,
                     80},
                    now_ms);
      return;
    case StatusLedEvent::UsbConnected:
      show_feedback({true,
                     ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                     ai_keyboard::FeedbackDirection::Right,
                     {18, 18, 18},
                     700,
                     90},
                    now_ms);
      return;
    case StatusLedEvent::UsbDisconnected:
      show_feedback({true,
                     ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                     ai_keyboard::FeedbackDirection::Left,
                     {10, 10, 10},
                     360,
                     70},
                    now_ms);
      return;
    case StatusLedEvent::ConfigMode:
      show_feedback({true,
                     ai_keyboard::FeedbackEffectKind::RainbowMarquee,
                     ai_keyboard::FeedbackDirection::None,
                     {0, 0, 0},
                     1200,
                     110},
                    now_ms);
      return;
    case StatusLedEvent::PlatformMacOS:
      show_feedback({true, ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                     ai_keyboard::FeedbackDirection::Left, {24, 24, 24}, 1000, 100}, now_ms);
      return;
    case StatusLedEvent::PlatformWindows:
      show_feedback({true, ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                     ai_keyboard::FeedbackDirection::Right, {0, 8, 30}, 1000, 100}, now_ms);
      return;
    case StatusLedEvent::SaveFailed:
      show_feedback({true, ai_keyboard::FeedbackEffectKind::ConfirmPulse,
                     ai_keyboard::FeedbackDirection::None, {32, 0, 0}, 1000, 120}, now_ms);
      return;
  }
}

void StatusLedStrip::show_input_event(ai_keyboard::InputId input,
                                      ai_keyboard::InputPhase phase,
                                      std::uint32_t now_ms) {
  const auto feedback = ai_keyboard::feedback_for_input_event(input, phase);
  if (!feedback.active) {
    return;
  }

  show_feedback(feedback, now_ms);
}

void StatusLedStrip::show_feedback(const ai_keyboard::InputActivityFeedback& feedback,
                                   std::uint32_t now_ms) {
  if (!feedback.active) {
    return;
  }
  active_feedback_ = feedback;
  effect_until_ms_ = now_ms + feedback.duration_ms;
  last_frame_ms_ = now_ms;
  cursor_ = 0;
  render_active_effect();
}

void StatusLedStrip::update(std::uint32_t now_ms) {
  if (agent_status_active_ && !agent_status_valid(now_ms)) {
    agent_status_active_ = false;
    agent_status_rendered_ = false;
    idle_rendered_ = false;
  }

  if (active_feedback_.active && now_ms < effect_until_ms_) {
    if (active_feedback_.frame_interval_ms == 0) {
      return;
    }
    if (now_ms - last_frame_ms_ < active_feedback_.frame_interval_ms) {
      return;
    }
    last_frame_ms_ = now_ms;
    cursor_ = static_cast<std::uint8_t>((cursor_ + 1U) % ai_keyboard::kWs2812Count);
    render_active_effect();
    return;
  }

  active_feedback_ = {};
  if (!idle_rendered_) {
    render_background_status(now_ms);
    idle_rendered_ = true;
  }
}

bool StatusLedStrip::agent_status_valid(std::uint32_t now_ms) const {
  return agent_status_active_ && deadline_pending(now_ms, agent_status_expires_ms_);
}

void StatusLedStrip::render_background_status(std::uint32_t now_ms) {
  if (mailbox_status_active_) {
    render_mailbox_status();
    flush();
    return;
  }

  if (agent_status_valid(now_ms)) {
    render_agent_status();
    flush();
    agent_status_rendered_ = true;
    return;
  }

  agent_status_rendered_ = false;
  render_idle_status();
}

void StatusLedStrip::render_mailbox_status() {
  const auto frame = easy_codex::mailbox_frame_for_slots(
      mailbox_coverage_by_slot_, running_tasks_);
  for (std::size_t index = 0U; index < leds_.size(); ++index) {
    leds_[index] = to_rgb(frame[index]);
  }
}

void StatusLedStrip::render_agent_status() {
  set_all({});
  if (leds_.empty()) {
    return;
  }

  const auto color = agent_status_color(agent_status_.state);
  const auto center = leds_.size() / 2U;
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    const auto distance = index > center ? index - center : center - index;
    if (distance == 0U) {
      leds_[index] = color;
    } else if (distance == 1U) {
      leds_[index] = scale_rgb(color, 1, 2);
    } else {
      leds_[index] = scale_rgb(color, 1, 4);
    }
  }
}

void StatusLedStrip::render_idle_status() {
  set_all({});
  flush();
}

void StatusLedStrip::set_all(Rgb color) {
  for (auto& led : leds_) {
    led = color;
  }
}

void StatusLedStrip::render_active_effect() {
  agent_status_rendered_ = false;
  switch (active_feedback_.effect) {
    case ai_keyboard::FeedbackEffectKind::Solid:
      render_solid_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::LightBarRipple:
      render_light_bar_ripple_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::DirectionalFlow:
      render_directional_flow_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::ConfirmPulse:
      render_confirm_pulse_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::RainbowMarquee:
      render_rainbow_marquee_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::None:
      set_all({});
      break;
  }
  flush();
  idle_rendered_ = false;
}

void StatusLedStrip::render_solid_effect() {
  set_all(to_rgb(active_feedback_.color));
}

void StatusLedStrip::render_light_bar_ripple_effect() {
  const auto base = to_rgb(active_feedback_.color);
  set_all({});

  switch (cursor_ % 4U) {
    case 0:
      set_scaled_pixel(&leds_, 1, base, 1, 5);
      set_scaled_pixel(&leds_, 2, base, 1, 1);
      set_scaled_pixel(&leds_, 3, base, 1, 5);
      break;
    case 1:
      set_scaled_pixel(&leds_, 0, base, 1, 4);
      set_scaled_pixel(&leds_, 1, base, 1, 1);
      set_scaled_pixel(&leds_, 2, base, 1, 2);
      set_scaled_pixel(&leds_, 3, base, 1, 1);
      set_scaled_pixel(&leds_, 4, base, 1, 4);
      break;
    case 2:
      set_scaled_pixel(&leds_, 0, base, 1, 1);
      set_scaled_pixel(&leds_, 1, base, 1, 4);
      set_scaled_pixel(&leds_, 3, base, 1, 4);
      set_scaled_pixel(&leds_, 4, base, 1, 1);
      break;
    default:
      set_scaled_pixel(&leds_, 0, base, 1, 4);
      set_scaled_pixel(&leds_, 1, base, 1, 6);
      set_scaled_pixel(&leds_, 2, base, 1, 8);
      set_scaled_pixel(&leds_, 3, base, 1, 6);
      set_scaled_pixel(&leds_, 4, base, 1, 4);
      break;
  }
}

void StatusLedStrip::render_directional_flow_effect() {
  const auto count = static_cast<std::uint8_t>(ai_keyboard::kWs2812Count);
  const auto base = to_rgb(active_feedback_.color);
  const bool right = active_feedback_.direction == ai_keyboard::FeedbackDirection::Right;
  const auto offset = static_cast<std::uint8_t>(cursor_ % count);
  const auto head = right ? offset : static_cast<std::uint8_t>((count - 1U) - offset);
  constexpr std::array<std::uint8_t, ai_keyboard::kWs2812Count> kNumerators{{12, 7, 4, 2, 1}};

  for (std::uint8_t index = 0; index < count; ++index) {
    const auto distance = right
                              ? static_cast<std::uint8_t>((head + count - index) % count)
                              : static_cast<std::uint8_t>((index + count - head) % count);
    leds_[index] = scale_rgb(base, kNumerators[distance], 12);
  }
}

void StatusLedStrip::render_confirm_pulse_effect() {
  const auto base = to_rgb(active_feedback_.color);
  constexpr std::array<std::uint8_t, 5> kNumerators{{5, 12, 7, 3, 1}};
  const auto numerator = kNumerators[cursor_ % kNumerators.size()];
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    const bool center = index == leds_.size() / 2;
    const auto pixel_numerator = center ? numerator : static_cast<std::uint8_t>((numerator * 2) / 3);
    leds_[index] = scale_rgb(base, pixel_numerator, 12);
  }
}

void StatusLedStrip::render_rainbow_marquee_effect() {
  for (std::size_t index = 0; index < ai_keyboard::kWs2812Count; ++index) {
    leds_[index] = rainbow_color(static_cast<std::uint8_t>(index + cursor_));
  }
}

esp_err_t StatusLedStrip::flush() {
  if (!ready()) {
    return ESP_ERR_INVALID_STATE;
  }

  std::array<rmt_symbol_word_t, kWs2812SymbolCount> symbols = {};
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    const auto& color = leds_[index];
    auto* pixel = &symbols[index * 24U];
    encode_ws2812_byte(color.green, pixel);
    encode_ws2812_byte(color.red, pixel + 8U);
    encode_ws2812_byte(color.blue, pixel + 16U);
  }

  auto& reset = symbols[ai_keyboard::kWs2812Count * 24U];
  reset.level0 = 0;
  reset.duration0 = kResetTicks;
  reset.level1 = 0;
  reset.duration1 = kResetTicks;

  rmt_transmit_config_t tx_config = {};
  tx_config.loop_count = 0;

  const esp_err_t enable_err = rmt_enable(rmt_channel_);
  if (enable_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_enable GPIO%u failed: %s",
             static_cast<unsigned>(ai_keyboard::kWs2812Pin),
             esp_err_to_name(enable_err));
    return enable_err;
  }

  const esp_err_t transmit_err =
      rmt_transmit(rmt_channel_,
                   rmt_copy_encoder_,
                   symbols.data(),
                   symbols.size() * sizeof(symbols[0]),
                   &tx_config);
  if (transmit_err != ESP_OK) {
    ESP_LOGE(kTag, "transmit failed: %s", esp_err_to_name(transmit_err));
    const esp_err_t disable_err = rmt_disable(rmt_channel_);
    if (disable_err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_disable after transmit failure: %s",
               esp_err_to_name(disable_err));
    }
    return transmit_err;
  }

  const esp_err_t wait_err = rmt_tx_wait_all_done(rmt_channel_, kRmtFlushWaitMs);
  if (wait_err != ESP_OK) {
    ESP_LOGW(kTag, "wait tx done timed out/dropped frame: %s",
             esp_err_to_name(wait_err));
  }

  // rmt_enable() acquires a PM lock. The LED frame is complete (or has been
  // abandoned after timeout), so release it before returning to the idle loop.
  const esp_err_t disable_err = rmt_disable(rmt_channel_);
  if (disable_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_disable after frame: %s", esp_err_to_name(disable_err));
  }
  return wait_err != ESP_OK ? wait_err : disable_err;
}

}  // namespace easy_input
