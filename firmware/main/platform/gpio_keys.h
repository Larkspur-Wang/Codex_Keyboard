#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard/board_pins.h"
#include "keyboard/debounce.h"
#include "keyboard/encoder.h"
#include "keyboard/keymap.h"

namespace easy_input {

struct InputEvent {
  ai_keyboard::InputId input = ai_keyboard::InputId::Count;
  ai_keyboard::InputPhase phase = ai_keyboard::InputPhase::Pressed;
  int encoder_step = 0;
};

struct InputDiagnostics {
  std::uint32_t raw_edges = 0;
  std::uint32_t edge_queue_drops = 0;
  std::uint32_t emitted_events = 0;
  std::uint32_t filtered_transitions = 0;
  std::uint32_t encoder_edges = 0;
  std::uint32_t encoder_steps = 0;
  std::uint32_t encoder_invalid_transitions = 0;
  std::uint32_t encoder_partial_resets = 0;
  std::uint32_t encoder_queue_drops = 0;
};

using InputEventCallback = void (*)(const InputEvent& event, void* context);

class GpioInputScanner {
 public:
  GpioInputScanner();

  void set_notify_task(TaskHandle_t task);
  esp_err_t begin(std::uint32_t now_ms);
  void poll(std::uint32_t now_ms, InputEventCallback callback, void* context);
  std::uint32_t recover_pressed_after_light_sleep(std::uint32_t now_ms,
                                                  InputEventCallback callback,
                                                  void* context);
  std::uint32_t take_input_edge_count();
  std::uint32_t take_wake_edge_count();
  std::uint32_t wake_edge_count() const;
  std::uint32_t encoder_edge_count() const;
  InputDiagnostics diagnostics() const;
  std::uint32_t active_input_mask() const;
  bool activity_pending() const;

  bool any_input_active() const;
  bool low_active_pressed(std::uint8_t gpio) const;
  std::uint8_t encoder_state() const;

 private:
  struct InputEdgeSnapshot {
    std::uint32_t timestamp_ms = 0;
    std::uint32_t active_mask = 0;
  };

  static constexpr std::size_t kInputEdgeQueueCapacity = 64;

  static void input_gpio_isr(void* arg);
  static void encoder_gpio_isr(void* arg);
  static void key_wake_gpio_isr(void* arg);

  void notify_task_from_isr();
  void handle_input_edge_from_isr();
  void handle_wake_edge_from_isr();
  void handle_encoder_edge_from_isr();
  bool take_input_edge_snapshot(InputEdgeSnapshot* snapshot);
  std::uint32_t take_input_edge_drop_count();
  void process_input_snapshot(const InputEdgeSnapshot& snapshot,
                              InputEventCallback callback,
                              void* context);
  bool take_pending_encoder_steps(int* steps);
  void emit(const InputEvent& event, InputEventCallback callback, void* context);

  std::array<ai_keyboard::DebouncedInput, ai_keyboard::kKeyPins.size()> key_debouncers_;
  ai_keyboard::DebouncedInput encoder_press_debouncer_;
  ai_keyboard::EncoderDecoder encoder_decoder_;
  mutable portMUX_TYPE encoder_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE wake_mux_ = portMUX_INITIALIZER_UNLOCKED;
  ai_keyboard::EncoderStepQueue pending_encoder_steps_;
  std::array<InputEdgeSnapshot, kInputEdgeQueueCapacity> input_edge_queue_{};
  std::size_t input_edge_queue_head_ = 0;
  std::size_t input_edge_queue_tail_ = 0;
  std::size_t input_edge_queue_size_ = 0;
  std::uint32_t pending_input_edge_drops_ = 0;
  std::uint32_t input_edge_drop_count_ = 0;
  std::uint32_t pending_input_edges_ = 0;
  std::uint32_t input_edge_count_ = 0;
  std::uint32_t input_event_count_ = 0;
  std::uint32_t filtered_transition_count_ = 0;
  std::uint32_t pending_wake_edges_ = 0;
  std::uint32_t wake_edge_count_ = 0;
  std::uint32_t encoder_edge_count_ = 0;
  std::uint32_t encoder_step_count_ = 0;
  std::uint32_t encoder_queue_drop_count_ = 0;
  std::uint8_t last_encoder_state_ = 0;
  TaskHandle_t notify_task_ = nullptr;
};

}  // namespace easy_input
