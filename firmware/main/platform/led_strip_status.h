#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "keyboard/agent_status.h"
#include "keyboard/board_pins.h"
#include "keyboard/input_feedback.h"
#include "keyboard/keymap.h"

namespace easy_input {

struct Rgb {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

enum class StatusLedEvent {
  BleConnected,
  BleDisconnected,
  UsbConnected,
  UsbDisconnected,
  ConfigMode,
  PlatformMacOS,
  PlatformWindows,
  SaveFailed,
};

class StatusLedStrip {
 public:
  esp_err_t begin();
  bool ready() const;

  void clear();
  esp_err_t prepare_for_deep_sleep();
  void show_raw_color(Rgb color);
  void show_pixel(std::size_t index, Rgb color);
  void set_agent_status(const ai_keyboard::AgentStatusCommand& command,
                        std::uint32_t now_ms);
  void set_mailbox_status(std::uint8_t unread_slots,
                          const std::array<std::uint8_t, 4>& coverage_by_slot,
                          std::uint32_t now_ms);
  void show_scroll_event(std::int8_t vertical,
                         std::int8_t horizontal,
                         std::uint32_t now_ms);
  void show_status_event(StatusLedEvent event, std::uint32_t now_ms);
  void show_input_event(ai_keyboard::InputId input,
                        ai_keyboard::InputPhase phase,
                        std::uint32_t now_ms);
  void update(std::uint32_t now_ms);

 private:
  void show_feedback(const ai_keyboard::InputActivityFeedback& feedback, std::uint32_t now_ms);
  bool agent_status_valid(std::uint32_t now_ms) const;
  void render_background_status(std::uint32_t now_ms);
  void render_agent_status();
  void render_mailbox_status();
  void render_idle_status();
  void set_all(Rgb color);
  void render_active_effect();
  void render_solid_effect();
  void render_light_bar_ripple_effect();
  void render_directional_flow_effect();
  void render_confirm_pulse_effect();
  void render_rainbow_marquee_effect();
  esp_err_t flush();

  rmt_channel_handle_t rmt_channel_ = nullptr;
  rmt_encoder_handle_t rmt_copy_encoder_ = nullptr;
  std::array<Rgb, ai_keyboard::kWs2812Count> leds_ = {};
  ai_keyboard::InputActivityFeedback active_feedback_;
  std::uint32_t effect_until_ms_ = 0;
  std::uint32_t last_frame_ms_ = 0;
  std::uint8_t cursor_ = 0;
  ai_keyboard::AgentStatusCommand agent_status_;
  std::uint32_t agent_status_expires_ms_ = 0;
  bool agent_status_active_ = false;
  bool agent_status_rendered_ = false;
  std::uint8_t mailbox_unread_slots_ = 0U;
  std::array<std::uint8_t, 4> mailbox_coverage_by_slot_{};
  bool mailbox_status_active_ = false;
  bool idle_rendered_ = false;
};

}  // namespace easy_input
