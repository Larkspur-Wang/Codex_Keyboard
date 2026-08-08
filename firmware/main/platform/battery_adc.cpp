#include "platform/battery_adc.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard/board_pins.h"

namespace easy_input {
namespace {

constexpr const char* kTag = "battery_adc";
constexpr int kBatteryDividerScale = 2;
constexpr int kBatterySampleCount = 16;
constexpr int kBatteryTrimCount = 2;
constexpr adc_atten_t kAttenuation = ADC_ATTEN_DB_12;
constexpr adc_bitwidth_t kBitwidth = ADC_BITWIDTH_DEFAULT;

static_assert(kBatterySampleCount > kBatteryTrimCount * 2);

TickType_t delay_ticks(std::uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks == 0 ? 1 : ticks;
}

void set_battery_sense_enabled(bool enabled) {
  if constexpr (ai_keyboard::kBatterySenseEnablePin >= 0) {
    gpio_set_level(static_cast<gpio_num_t>(ai_keyboard::kBatterySenseEnablePin),
                   enabled ? 1 : 0);
  }
}

}  // namespace

esp_err_t BatteryAdc::begin() {
  if constexpr (ai_keyboard::kBatterySenseAdcPin < 0) {
    ESP_LOGW(kTag,
             "battery ADC disabled for board=%s; no confirmed SEN_VBAT GPIO in pin map",
             ai_keyboard::kBoardName);
    return ESP_OK;
  }

  esp_err_t err = ESP_OK;
  if constexpr (ai_keyboard::kBatterySenseEnablePin >= 0) {
    gpio_config_t enable_config = {};
    enable_config.pin_bit_mask = 1ULL << ai_keyboard::kBatterySenseEnablePin;
    enable_config.mode = GPIO_MODE_OUTPUT;
    enable_config.pull_up_en = GPIO_PULLUP_DISABLE;
    enable_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    enable_config.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&enable_config);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "enable GPIO config failed: %s", esp_err_to_name(err));
      return err;
    }
    set_battery_sense_enabled(false);
  }

  err = adc_oneshot_io_to_channel(ai_keyboard::kBatterySenseAdcPin, &unit_, &channel_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "GPIO%d is not an ADC channel: %s",
             static_cast<int>(ai_keyboard::kBatterySenseAdcPin),
             esp_err_to_name(err));
    return err;
  }

  adc_oneshot_unit_init_cfg_t unit_config = {};
  unit_config.unit_id = unit_;
  err = adc_oneshot_new_unit(&unit_config, &adc_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
    return err;
  }

  adc_oneshot_chan_cfg_t channel_config = {};
  channel_config.atten = kAttenuation;
  channel_config.bitwidth = kBitwidth;
  err = adc_oneshot_config_channel(adc_handle_, channel_, &channel_config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "adc channel config failed: %s", esp_err_to_name(err));
    return err;
  }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali_config = {};
  cali_config.unit_id = unit_;
  cali_config.chan = channel_;
  cali_config.atten = kAttenuation;
  cali_config.bitwidth = kBitwidth;
  err = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle_);
  if (err == ESP_OK) {
    calibrated_ = true;
  } else {
    ESP_LOGW(kTag, "ADC calibration unavailable: %s", esp_err_to_name(err));
  }
#else
  ESP_LOGW(kTag, "ADC curve fitting calibration not supported by this target");
#endif

  ESP_LOGI(kTag,
           "configured battery sense enable=GPIO%d adc=GPIO%d unit=%d channel=%d calibrated=%s",
           static_cast<int>(ai_keyboard::kBatterySenseEnablePin),
           static_cast<int>(ai_keyboard::kBatterySenseAdcPin),
           static_cast<int>(unit_),
           static_cast<int>(channel_),
           calibrated_ ? "true" : "false");
  return ESP_OK;
}

BatterySample BatteryAdc::read() {
  BatterySample sample;
  if (adc_handle_ == nullptr) {
    sample.error = ESP_ERR_INVALID_STATE;
    return sample;
  }

  set_battery_sense_enabled(true);
  vTaskDelay(delay_ticks(5));

  std::array<int, kBatterySampleCount> raw_values = {};
  int raw_count = 0;
  for (int index = 0; index < kBatterySampleCount; ++index) {
    int raw = 0;
    sample.error = adc_oneshot_read(adc_handle_, channel_, &raw);
    if (sample.error != ESP_OK) {
      break;
    }
    raw_values[raw_count++] = raw;
    if (index + 1 < kBatterySampleCount) {
      vTaskDelay(delay_ticks(1));
    }
  }

  set_battery_sense_enabled(false);
  if (sample.error != ESP_OK || raw_count == 0) {
    return sample;
  }

  std::sort(raw_values.begin(), raw_values.begin() + raw_count);
  const int trim_count = raw_count > kBatteryTrimCount * 2 ? kBatteryTrimCount : 0;
  std::int64_t raw_total = 0;
  for (int index = trim_count; index < raw_count - trim_count; ++index) {
    raw_total += raw_values[index];
  }
  const int averaged_count = raw_count - trim_count * 2;
  sample.raw = static_cast<int>((raw_total + averaged_count / 2) / averaged_count);
  if (sample.error == ESP_OK && calibrated_) {
    sample.error = adc_cali_raw_to_voltage(cali_handle_, sample.raw, &sample.sense_mv);
    if (sample.error == ESP_OK) {
      sample.calibrated = true;
      sample.rail_mv = sample.sense_mv * kBatteryDividerScale;
    }
  }
  return sample;
}

bool BatteryAdc::ready() const {
  return adc_handle_ != nullptr;
}

}  // namespace easy_input
