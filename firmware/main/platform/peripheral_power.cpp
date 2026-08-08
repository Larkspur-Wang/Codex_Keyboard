#include "platform/peripheral_power.h"

#include <array>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "keyboard/board_pins.h"

namespace easy_input {
namespace {

const char* const kTag = "peripheral_power";

// The factory has confirmed that commands must start only after the shared
// rail is stable. Keep this conservative boot-only delay centralized until
// scope measurements establish a smaller production minimum.
constexpr std::uint32_t kPeripheralPowerSettleUs = 20U * 1000U;

constexpr std::array<std::int8_t, 6> kSharedRailCommandOutputPins{{
    ai_keyboard::kMicI2sBclkPin,
    ai_keyboard::kMicI2sWsPin,
    static_cast<std::int8_t>(ai_keyboard::kWs2812Pin),
    ai_keyboard::kSpkI2sWsPin,
    ai_keyboard::kSpkI2sBclkPin,
    ai_keyboard::kSpkI2sDataOutPin,
}};

}  // namespace

esp_err_t PeripheralPowerController::begin_awake() {
  if (ready_) {
    return ESP_OK;
  }

  if constexpr (ai_keyboard::kPeripheralPowerEnablePin < 0) {
    initialized_ = true;
    ready_ = true;
    power_enabled_ = true;
    return ESP_OK;
  }

  gpio_config_t power_config = {};
  power_config.pin_bit_mask =
      1ULL << ai_keyboard::kPeripheralPowerEnablePin;
  power_config.mode = GPIO_MODE_OUTPUT;
  power_config.pull_up_en = GPIO_PULLUP_DISABLE;
  power_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  power_config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t config_err = gpio_config(&power_config);
  if (config_err != ESP_OK) {
    ESP_LOGE(kTag,
             "GPIO%d config failed: %s",
             static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
             esp_err_to_name(config_err));
    return config_err;
  }

  // ESP-IDF's S3 light-sleep workaround otherwise switches every GPIO to an
  // isolated sleep configuration. GPIO8 must keep its active output through
  // each controlled 30 ms light sleep; the board pulldown would turn it off.
  const esp_err_t sleep_select_err = gpio_sleep_sel_dis(
      static_cast<gpio_num_t>(ai_keyboard::kPeripheralPowerEnablePin));
  if (sleep_select_err != ESP_OK) {
    ESP_LOGE(kTag,
             "GPIO%d light-sleep preservation failed: %s",
             static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
             esp_err_to_name(sleep_select_err));
    return sleep_select_err;
  }

  // Keep downstream output commands inactive-low (and MIC DIN floating)
  // before the shared rail rises. Drivers may attach these pins only after
  // the 20 ms settle barrier.
  const esp_err_t safe_io_err =
      configure_command_pins_safe_for_rail_transition();
  if (safe_io_err != ESP_OK) {
    ESP_LOGE(kTag,
             "shared-rail startup command-pin setup failed: %s",
             esp_err_to_name(safe_io_err));
    return safe_io_err;
  }

  power_leases_.clear();
  if (!power_leases_.acquire(
          ai_keyboard::PeripheralPowerOwner::DeviceAwake)) {
    return ESP_ERR_INVALID_STATE;
  }
  initialized_ = true;
  const esp_err_t power_err = apply_power_state();
  if (power_err != ESP_OK) {
    initialized_ = false;
    power_leases_.clear();
    return power_err;
  }

  esp_rom_delay_us(kPeripheralPowerSettleUs);
  ready_ = true;
  ESP_LOGI(kTag,
           "shared rail ready GPIO%d settle_us=%lu light_sleep_hold=active",
           static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
           static_cast<unsigned long>(kPeripheralPowerSettleUs));
  return ESP_OK;
}

bool PeripheralPowerController::ready() const {
  return ready_ && power_enabled_;
}

bool PeripheralPowerController::power_enabled() const {
  return power_enabled_;
}

esp_err_t PeripheralPowerController::set_audio_power_hold(bool enabled) {
  return set_owner_hold(ai_keyboard::PeripheralPowerOwner::KeyboardMic,
                        enabled,
                        "audio");
}

esp_err_t PeripheralPowerController::set_speaker_power_hold(bool enabled) {
  return set_owner_hold(ai_keyboard::PeripheralPowerOwner::Speaker,
                        enabled,
                        "speaker");
}

esp_err_t PeripheralPowerController::prepare_for_deep_sleep() {
  if (!initialized_ || !ready_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (power_leases_.held(
          ai_keyboard::PeripheralPowerOwner::KeyboardMic) ||
      power_leases_.held(ai_keyboard::PeripheralPowerOwner::Speaker)) {
    ESP_LOGW(kTag,
             "deep sleep power-off rejected held_mask=0x%02x",
             static_cast<unsigned>(power_leases_.held_mask()));
    return ESP_ERR_INVALID_STATE;
  }

  // From here onward GPIO mux state may be destructively changed. Any error
  // must be treated by the caller as fail-closed restart, not as a retry from
  // the still-running application.
  ready_ = false;
  const esp_err_t safe_io_err =
      configure_command_pins_safe_for_rail_transition();
  if (safe_io_err != ESP_OK) {
    return safe_io_err;
  }

  power_leases_.clear();
  const esp_err_t power_err = apply_power_state();
  if (power_err != ESP_OK) {
    return power_err;
  }
  return ESP_OK;
}

esp_err_t PeripheralPowerController::set_owner_hold(
    ai_keyboard::PeripheralPowerOwner owner,
    bool enabled,
    const char* label) {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  const bool changed = enabled ? power_leases_.acquire(owner)
                               : power_leases_.release(owner);
  if (!changed) {
    return ESP_OK;
  }
  ESP_LOGI(kTag,
           "%s activity hold %s mask=0x%02x",
           label == nullptr ? "peripheral" : label,
           enabled ? "enabled" : "disabled",
           static_cast<unsigned>(power_leases_.held_mask()));
  return apply_power_state();
}

esp_err_t
PeripheralPowerController::configure_command_pins_safe_for_rail_transition() {
  if constexpr (ai_keyboard::kPeripheralPowerEnablePin < 0) {
    return ESP_OK;
  }

  std::uint64_t output_mask = 0;
  for (const auto pin : kSharedRailCommandOutputPins) {
    if (pin < 0) {
      continue;
    }
    const esp_err_t latch_err =
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    if (latch_err != ESP_OK) {
      return latch_err;
    }
    output_mask |= 1ULL << static_cast<unsigned>(pin);
  }

  gpio_config_t output_config = {};
  output_config.pin_bit_mask = output_mask;
  output_config.mode = GPIO_MODE_OUTPUT;
  output_config.pull_up_en = GPIO_PULLUP_DISABLE;
  output_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  output_config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t output_err = gpio_config(&output_config);
  if (output_err != ESP_OK) {
    return output_err;
  }
  for (const auto pin : kSharedRailCommandOutputPins) {
    if (pin < 0) {
      continue;
    }
    const esp_err_t level_err =
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    if (level_err != ESP_OK) {
      return level_err;
    }
  }

  if constexpr (ai_keyboard::kMicI2sDataInPin >= 0) {
    gpio_config_t input_config = {};
    input_config.pin_bit_mask =
        1ULL << ai_keyboard::kMicI2sDataInPin;
    input_config.mode = GPIO_MODE_DISABLE;
    input_config.pull_up_en = GPIO_PULLUP_DISABLE;
    input_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    input_config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t input_err = gpio_config(&input_config);
    if (input_err != ESP_OK) {
      return input_err;
    }
  }
  return ESP_OK;
}

esp_err_t PeripheralPowerController::apply_power_state() {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  return set_power_enabled(power_leases_.power_required());
}

esp_err_t PeripheralPowerController::set_power_enabled(bool enabled) {
  if (power_enabled_ == enabled) {
    return ESP_OK;
  }
  if constexpr (ai_keyboard::kPeripheralPowerEnablePin < 0) {
    power_enabled_ = enabled;
    return ESP_OK;
  }

  const auto active = static_cast<std::uint8_t>(
      ai_keyboard::kPeripheralPowerEnableActiveLevel == 0 ? 0 : 1);
  const auto inactive = static_cast<std::uint8_t>(active == 0 ? 1 : 0);
  const auto level = enabled ? active : inactive;
  ESP_LOGI(kTag,
           "set shared rail GPIO%d level=%u enabled=%d",
           static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
           static_cast<unsigned>(level),
           enabled ? 1 : 0);
  const esp_err_t err = gpio_set_level(
      static_cast<gpio_num_t>(ai_keyboard::kPeripheralPowerEnablePin),
      level);
  if (err != ESP_OK) {
    ESP_LOGE(kTag,
             "GPIO%d write failed: %s",
             static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
             esp_err_to_name(err));
    return err;
  }
  power_enabled_ = enabled;
  return ESP_OK;
}

}  // namespace easy_input
