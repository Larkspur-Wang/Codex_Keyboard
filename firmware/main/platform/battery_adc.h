#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"

namespace easy_input {

struct BatterySample {
  int raw = 0;
  int sense_mv = 0;
  int rail_mv = 0;
  bool calibrated = false;
  esp_err_t error = ESP_OK;
};

class BatteryAdc {
 public:
  esp_err_t begin();
  BatterySample read();
  bool ready() const;

 private:
  adc_unit_t unit_ = ADC_UNIT_1;
  adc_channel_t channel_ = ADC_CHANNEL_0;
  adc_oneshot_unit_handle_t adc_handle_ = nullptr;
  adc_cali_handle_t cali_handle_ = nullptr;
  bool calibrated_ = false;
};

}  // namespace easy_input
