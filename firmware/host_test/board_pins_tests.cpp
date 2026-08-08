#include <cassert>
#include <string_view>

#include "keyboard/board_pins.h"

void board_pin_map_matches_reference() {
  static_assert(ai_keyboard::kKeyPins.size() == 8);

#if defined(EASY_INPUT_BOARD_V2)
  assert(ai_keyboard::kBoardName == std::string_view("v2"));
  assert(ai_keyboard::kKeyPins[0].gpio == 2);
  assert(ai_keyboard::kKeyPins[1].gpio == 47);
  assert(ai_keyboard::kKeyPins[2].gpio == 38);
  assert(ai_keyboard::kKeyPins[3].gpio == 41);
  assert(ai_keyboard::kKeyPins[4].gpio == 1);
  assert(ai_keyboard::kKeyPins[5].gpio == 6);
  assert(ai_keyboard::kKeyPins[6].gpio == 7);
  assert(ai_keyboard::kKeyPins[7].gpio == 48);

  assert(ai_keyboard::kEncoderPinA == 17);
  assert(ai_keyboard::kEncoderPinB == 16);
  assert(ai_keyboard::kEncoderPressPin == 18);
  assert(ai_keyboard::kWs2812Pin == 12);
  assert(ai_keyboard::kBatterySenseEnablePin == 5);
  assert(ai_keyboard::kBatterySenseAdcPin == 4);
  assert(ai_keyboard::kExternalPowerSensePin == 40);
  assert(ai_keyboard::kExternalPowerSenseActiveLevel == 0);
  assert(ai_keyboard::kChargeStatusPin == 39);
  assert(ai_keyboard::kChargeStatusChargingLevel == 1);
  assert(ai_keyboard::kKeyWakePin == 21);
  assert(ai_keyboard::kPeripheralPowerEnablePin == 8);
  assert(ai_keyboard::kPeripheralPowerEnableActiveLevel == 1);
  assert(ai_keyboard::kMicI2sBclkPin == 9);
  assert(ai_keyboard::kMicI2sWsPin == 10);
  assert(ai_keyboard::kMicI2sDataInPin == 11);
  assert(ai_keyboard::kSpkI2sWsPin == 13);
  assert(ai_keyboard::kSpkI2sBclkPin == 14);
  assert(ai_keyboard::kSpkI2sDataOutPin == 15);
#else
  assert(ai_keyboard::kBoardName == std::string_view("v1"));
  assert(ai_keyboard::kKeyPins[0].gpio == 39);
  assert(ai_keyboard::kKeyPins[1].gpio == 40);
  assert(ai_keyboard::kKeyPins[2].gpio == 41);
  assert(ai_keyboard::kKeyPins[3].gpio == 42);
  assert(ai_keyboard::kKeyPins[4].gpio == 38);
  assert(ai_keyboard::kKeyPins[5].gpio == 37);
  assert(ai_keyboard::kKeyPins[6].gpio == 36);
  assert(ai_keyboard::kKeyPins[7].gpio == 35);

  assert(ai_keyboard::kEncoderPinA == 9);
  assert(ai_keyboard::kEncoderPinB == 10);
  assert(ai_keyboard::kEncoderPressPin == 11);
  assert(ai_keyboard::kEncoderDirectionMultiplier == -1);
  assert(ai_keyboard::kWs2812Pin == 18);
  assert(ai_keyboard::kWs2812Count == 5);
  assert(ai_keyboard::kUsbVbusSensePin == -1);
  assert(ai_keyboard::kExternalPowerSensePin == -1);
  assert(ai_keyboard::kChargeStatusPin == -1);
#endif
}

int main() {
  board_pin_map_matches_reference();
  return 0;
}
