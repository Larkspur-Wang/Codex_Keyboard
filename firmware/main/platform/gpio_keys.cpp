#include "platform/gpio_keys.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace easy_input {
namespace {

// Keep debounce short enough for repeated shortcut keys. 20ms was safe for
// bounce, but it can merge very fast release/press cycles into one key hold.
constexpr std::uint32_t kInputPressDebounceMs = 5;
constexpr std::uint32_t kInputReleaseDebounceMs = 3;
const char* const kTag = "gpio_keys";

constexpr std::array<ai_keyboard::InputId, ai_keyboard::kKeyPins.size()> kKeyInputs{{
    ai_keyboard::InputId::Key1,
    ai_keyboard::InputId::Key2,
    ai_keyboard::InputId::Key3,
    ai_keyboard::InputId::Key4,
    ai_keyboard::InputId::Key5,
    ai_keyboard::InputId::Key6,
    ai_keyboard::InputId::Key7,
    ai_keyboard::InputId::Key8,
}};

std::uint64_t input_pin_mask() {
  std::uint64_t mask = 0;
  for (const auto& key : ai_keyboard::kKeyPins) {
    mask |= 1ULL << key.gpio;
  }
  mask |= 1ULL << ai_keyboard::kEncoderPinA;
  mask |= 1ULL << ai_keyboard::kEncoderPinB;
  mask |= 1ULL << ai_keyboard::kEncoderPressPin;
  return mask;
}

}  // namespace

GpioInputScanner::GpioInputScanner()
    : key_debouncers_{{
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
          ai_keyboard::DebouncedInput(kInputPressDebounceMs, kInputReleaseDebounceMs),
      }},
      encoder_press_debouncer_(kInputPressDebounceMs, kInputReleaseDebounceMs) {}

void GpioInputScanner::set_notify_task(TaskHandle_t task) {
  notify_task_ = task;
}

esp_err_t GpioInputScanner::begin(std::uint32_t now_ms) {
  gpio_config_t config = {};
  config.pin_bit_mask = input_pin_mask();
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;

  const esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "gpio_config failed: %s", esp_err_to_name(err));
    return err;
  }

  for (std::size_t index = 0; index < ai_keyboard::kKeyPins.size(); ++index) {
    key_debouncers_[index].reset(low_active_pressed(ai_keyboard::kKeyPins[index].gpio), now_ms);
  }
  encoder_press_debouncer_.reset(low_active_pressed(ai_keyboard::kEncoderPressPin), now_ms);
  last_encoder_state_ = encoder_state();
  encoder_decoder_.reset(last_encoder_state_);
  pending_encoder_steps_.clear();

  esp_err_t isr_err = gpio_install_isr_service(0);
  if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_err));
    return isr_err;
  }

  const gpio_num_t encoder_a = static_cast<gpio_num_t>(ai_keyboard::kEncoderPinA);
  const gpio_num_t encoder_b = static_cast<gpio_num_t>(ai_keyboard::kEncoderPinB);
  for (const auto& key : ai_keyboard::kKeyPins) {
    const gpio_num_t key_gpio = static_cast<gpio_num_t>(key.gpio);
    ESP_ERROR_CHECK(gpio_set_intr_type(key_gpio, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(key_gpio, input_gpio_isr, this));
  }
  const gpio_num_t encoder_press = static_cast<gpio_num_t>(ai_keyboard::kEncoderPressPin);
  ESP_ERROR_CHECK(gpio_set_intr_type(encoder_press, GPIO_INTR_ANYEDGE));
  ESP_ERROR_CHECK(gpio_isr_handler_add(encoder_press, input_gpio_isr, this));
  ESP_ERROR_CHECK(gpio_set_intr_type(encoder_a, GPIO_INTR_ANYEDGE));
  ESP_ERROR_CHECK(gpio_set_intr_type(encoder_b, GPIO_INTR_ANYEDGE));
  ESP_ERROR_CHECK(gpio_isr_handler_add(encoder_a, encoder_gpio_isr, this));
  ESP_ERROR_CHECK(gpio_isr_handler_add(encoder_b, encoder_gpio_isr, this));

  if constexpr (ai_keyboard::kKeyWakePin >= 0) {
    const gpio_num_t key_wake = static_cast<gpio_num_t>(ai_keyboard::kKeyWakePin);
    ESP_ERROR_CHECK(gpio_set_intr_type(key_wake, GPIO_INTR_NEGEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(key_wake, key_wake_gpio_isr, this));
  }

  ESP_LOGI(kTag,
           "configured active-low inputs keys=%u encoder_a=GPIO%u encoder_b=GPIO%u press=GPIO%u key_wake=GPIO%d keys=poll encoder=edge_isr",
           static_cast<unsigned>(ai_keyboard::kKeyPins.size()),
           static_cast<unsigned>(ai_keyboard::kEncoderPinA),
           static_cast<unsigned>(ai_keyboard::kEncoderPinB),
           static_cast<unsigned>(ai_keyboard::kEncoderPressPin),
           static_cast<int>(ai_keyboard::kKeyWakePin));
  return ESP_OK;
}

void GpioInputScanner::poll(std::uint32_t now_ms, InputEventCallback callback, void* context) {
  InputEdgeSnapshot snapshot;
  while (take_input_edge_snapshot(&snapshot)) {
    process_input_snapshot(snapshot, callback, context);
  }

  process_input_snapshot({now_ms, active_input_mask()}, callback, context);

  const auto dropped = take_input_edge_drop_count();
  if (dropped > 0) {
    ESP_LOGW(kTag,
             "input edge queue overflow dropped=%lu capacity=%u",
             static_cast<unsigned long>(dropped),
             static_cast<unsigned>(kInputEdgeQueueCapacity));
  }

  int encoder_steps = 0;
  while (take_pending_encoder_steps(&encoder_steps)) {
    emit({encoder_steps > 0 ? ai_keyboard::InputId::EncoderRight
                            : ai_keyboard::InputId::EncoderLeft,
          ai_keyboard::InputPhase::Pressed,
          encoder_steps},
         callback,
         context);
  }
}

void GpioInputScanner::process_input_snapshot(const InputEdgeSnapshot& snapshot,
                                              InputEventCallback callback,
                                              void* context) {
  for (std::size_t index = 0; index < ai_keyboard::kKeyPins.size(); ++index) {
    const bool pressed = (snapshot.active_mask & (1UL << index)) != 0;
    const auto result = key_debouncers_[index].update(pressed, snapshot.timestamp_ms);
    if (result.filtered_transition) {
      ++filtered_transition_count_;
    }
    if (!result.changed) {
      continue;
    }
    emit({kKeyInputs[index],
          result.state ? ai_keyboard::InputPhase::Pressed : ai_keyboard::InputPhase::Released,
          0},
         callback,
         context);
  }

  const std::uint32_t encoder_press_mask = 1UL << ai_keyboard::kKeyPins.size();
  const bool encoder_pressed = (snapshot.active_mask & encoder_press_mask) != 0;
  const auto press_result =
      encoder_press_debouncer_.update(encoder_pressed, snapshot.timestamp_ms);
  if (press_result.filtered_transition) {
    ++filtered_transition_count_;
  }
  if (press_result.changed) {
    emit({ai_keyboard::InputId::EncoderPress,
          press_result.state ? ai_keyboard::InputPhase::Pressed
                             : ai_keyboard::InputPhase::Released,
          0},
         callback,
         context);
  }
}

std::uint32_t GpioInputScanner::recover_pressed_after_light_sleep(
    std::uint32_t now_ms,
    InputEventCallback callback,
    void* context) {
  std::uint32_t recovered_mask = 0;
  for (std::size_t index = 0; index < ai_keyboard::kKeyPins.size(); ++index) {
    const auto& key = ai_keyboard::kKeyPins[index];
    if (!low_active_pressed(key.gpio)) {
      continue;
    }
    const auto result = key_debouncers_[index].latch_wake_state(true, now_ms);
    if (!result.changed) {
      continue;
    }
    recovered_mask |= 1UL << index;
    emit({kKeyInputs[index], ai_keyboard::InputPhase::Pressed, 0}, callback, context);
  }

  if (low_active_pressed(ai_keyboard::kEncoderPressPin)) {
    const auto result = encoder_press_debouncer_.latch_wake_state(true, now_ms);
    if (result.changed) {
      recovered_mask |= 1UL << ai_keyboard::kKeyPins.size();
      emit({ai_keyboard::InputId::EncoderPress, ai_keyboard::InputPhase::Pressed, 0},
           callback,
           context);
    }
  }
  return recovered_mask;
}

bool GpioInputScanner::any_input_active() const {
  return active_input_mask() != 0;
}

bool GpioInputScanner::activity_pending() const {
  portENTER_CRITICAL(&wake_mux_);
  const bool input_pending = pending_input_edges_ != 0 ||
                             pending_wake_edges_ != 0 ||
                             input_edge_queue_size_ != 0;
  portEXIT_CRITICAL(&wake_mux_);

  portENTER_CRITICAL(&encoder_mux_);
  const bool encoder_pending = !pending_encoder_steps_.empty();
  portEXIT_CRITICAL(&encoder_mux_);
  return input_pending || encoder_pending;
}

std::uint32_t GpioInputScanner::take_input_edge_count() {
  portENTER_CRITICAL(&wake_mux_);
  const std::uint32_t edges = pending_input_edges_;
  pending_input_edges_ = 0;
  portEXIT_CRITICAL(&wake_mux_);
  return edges;
}

std::uint32_t GpioInputScanner::active_input_mask() const {
  std::uint32_t mask = 0;
  for (std::size_t index = 0; index < ai_keyboard::kKeyPins.size(); ++index) {
    if (low_active_pressed(ai_keyboard::kKeyPins[index].gpio)) {
      mask |= 1UL << index;
    }
  }
  // Encoder A/B are quadrature phases, not held buttons. A normal resting
  // encoder position may leave either phase low; treating those levels as
  // active would keep the firmware out of idle/deep-idle forever.
  if (low_active_pressed(ai_keyboard::kEncoderPressPin)) {
    mask |= 1UL << ai_keyboard::kKeyPins.size();
  }
  return mask;
}

std::uint32_t GpioInputScanner::take_wake_edge_count() {
  portENTER_CRITICAL(&wake_mux_);
  const std::uint32_t edges = pending_wake_edges_;
  pending_wake_edges_ = 0;
  portEXIT_CRITICAL(&wake_mux_);
  return edges;
}

std::uint32_t GpioInputScanner::wake_edge_count() const {
  portENTER_CRITICAL(&wake_mux_);
  const std::uint32_t edges = wake_edge_count_;
  portEXIT_CRITICAL(&wake_mux_);
  return edges;
}

std::uint32_t GpioInputScanner::encoder_edge_count() const {
  portENTER_CRITICAL(&wake_mux_);
  const std::uint32_t edges = encoder_edge_count_;
  portEXIT_CRITICAL(&wake_mux_);
  return edges;
}

InputDiagnostics GpioInputScanner::diagnostics() const {
  InputDiagnostics result;
  portENTER_CRITICAL(&wake_mux_);
  result.raw_edges = input_edge_count_;
  result.edge_queue_drops = input_edge_drop_count_;
  result.encoder_edges = encoder_edge_count_;
  portEXIT_CRITICAL(&wake_mux_);

  portENTER_CRITICAL(&encoder_mux_);
  result.encoder_steps = encoder_step_count_;
  result.encoder_invalid_transitions = encoder_decoder_.invalid_transition_count();
  result.encoder_partial_resets = encoder_decoder_.partial_reset_count();
  result.encoder_queue_drops = encoder_queue_drop_count_;
  portEXIT_CRITICAL(&encoder_mux_);

  result.emitted_events = input_event_count_;
  result.filtered_transitions = filtered_transition_count_;
  return result;
}

bool GpioInputScanner::low_active_pressed(std::uint8_t gpio) const {
  return gpio_get_level(static_cast<gpio_num_t>(gpio)) == 0;
}

std::uint8_t GpioInputScanner::encoder_state() const {
  return (low_active_pressed(ai_keyboard::kEncoderPinA) ? 0x01 : 0x00) |
         (low_active_pressed(ai_keyboard::kEncoderPinB) ? 0x02 : 0x00);
}

void GpioInputScanner::input_gpio_isr(void* arg) {
  if (arg == nullptr) {
    return;
  }
  static_cast<GpioInputScanner*>(arg)->handle_input_edge_from_isr();
}

void GpioInputScanner::encoder_gpio_isr(void* arg) {
  if (arg == nullptr) {
    return;
  }
  static_cast<GpioInputScanner*>(arg)->handle_encoder_edge_from_isr();
}

void GpioInputScanner::key_wake_gpio_isr(void* arg) {
  if (arg == nullptr) {
    return;
  }
  static_cast<GpioInputScanner*>(arg)->handle_wake_edge_from_isr();
}

void GpioInputScanner::notify_task_from_isr() {
  if (notify_task_ == nullptr) {
    return;
  }
  BaseType_t higher_priority_task_woken = pdFALSE;
  vTaskNotifyGiveFromISR(notify_task_, &higher_priority_task_woken);
  if (higher_priority_task_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

void GpioInputScanner::handle_input_edge_from_isr() {
  const InputEdgeSnapshot snapshot{
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000), active_input_mask()};
  portENTER_CRITICAL_ISR(&wake_mux_);
  if (input_edge_queue_size_ == kInputEdgeQueueCapacity) {
    input_edge_queue_head_ = (input_edge_queue_head_ + 1) % kInputEdgeQueueCapacity;
    --input_edge_queue_size_;
    ++pending_input_edge_drops_;
    ++input_edge_drop_count_;
  }
  input_edge_queue_[input_edge_queue_tail_] = snapshot;
  input_edge_queue_tail_ = (input_edge_queue_tail_ + 1) % kInputEdgeQueueCapacity;
  ++input_edge_queue_size_;
  ++pending_input_edges_;
  ++input_edge_count_;
  portEXIT_CRITICAL_ISR(&wake_mux_);
  notify_task_from_isr();
}

bool GpioInputScanner::take_input_edge_snapshot(InputEdgeSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return false;
  }
  portENTER_CRITICAL(&wake_mux_);
  if (input_edge_queue_size_ == 0) {
    portEXIT_CRITICAL(&wake_mux_);
    return false;
  }
  *snapshot = input_edge_queue_[input_edge_queue_head_];
  input_edge_queue_head_ = (input_edge_queue_head_ + 1) % kInputEdgeQueueCapacity;
  --input_edge_queue_size_;
  portEXIT_CRITICAL(&wake_mux_);
  return true;
}

std::uint32_t GpioInputScanner::take_input_edge_drop_count() {
  portENTER_CRITICAL(&wake_mux_);
  const auto dropped = pending_input_edge_drops_;
  pending_input_edge_drops_ = 0;
  portEXIT_CRITICAL(&wake_mux_);
  return dropped;
}

void GpioInputScanner::handle_wake_edge_from_isr() {
  portENTER_CRITICAL_ISR(&wake_mux_);
  ++pending_wake_edges_;
  ++wake_edge_count_;
  portEXIT_CRITICAL_ISR(&wake_mux_);
  notify_task_from_isr();
}

void GpioInputScanner::handle_encoder_edge_from_isr() {
  portENTER_CRITICAL_ISR(&wake_mux_);
  ++encoder_edge_count_;
  portEXIT_CRITICAL_ISR(&wake_mux_);

  const std::uint8_t next_encoder_state = encoder_state();
  if (next_encoder_state == last_encoder_state_) {
    notify_task_from_isr();
    return;
  }

  portENTER_CRITICAL_ISR(&encoder_mux_);
  const int raw_step = encoder_decoder_.update(next_encoder_state);
  last_encoder_state_ = next_encoder_state;
  if (raw_step != 0) {
    const int logical_step = raw_step * ai_keyboard::kEncoderDirectionMultiplier;
    if (pending_encoder_steps_.push(logical_step)) {
      ++encoder_step_count_;
    } else {
      ++encoder_queue_drop_count_;
    }
  }
  portEXIT_CRITICAL_ISR(&encoder_mux_);
  notify_task_from_isr();
}

bool GpioInputScanner::take_pending_encoder_steps(int* steps) {
  portENTER_CRITICAL(&encoder_mux_);
  const bool available = pending_encoder_steps_.pop(steps);
  portEXIT_CRITICAL(&encoder_mux_);
  return available;
}

void GpioInputScanner::emit(const InputEvent& event,
                            InputEventCallback callback,
                            void* context) {
  ++input_event_count_;
  if (callback != nullptr) {
    callback(event, context);
  }
}

}  // namespace easy_input
