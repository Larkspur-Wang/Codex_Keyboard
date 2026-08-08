#pragma once

#include <array>
#include <cstdint>

namespace ai_keyboard {

struct KeyPin {
  const char* name;
  std::uint8_t gpio;
};

#if defined(EASY_INPUT_BOARD_V2)

constexpr const char* kBoardName = "v2";
constexpr const char* kUsbSerialNumber = "easy-input-v2";

constexpr std::array<KeyPin, 8> kKeyPins{{
    {"KEY1", 2},
    {"KEY2", 47},
    {"KEY3", 38},
    {"KEY4", 41},
    {"KEY5", 1},
    {"KEY6", 6},
    {"KEY7", 7},
    {"KEY8", 48},
}};

constexpr std::uint8_t kEncoderPinA = 17;
constexpr std::uint8_t kEncoderPinB = 16;
constexpr std::uint8_t kEncoderPressPin = 18;
constexpr std::int8_t kEncoderDirectionMultiplier = -1;
constexpr std::uint8_t kWs2812Pin = 12;
constexpr std::uint8_t kWs2812Count = 5;
constexpr std::int8_t kBatterySenseEnablePin = 5;
constexpr std::int8_t kBatterySenseAdcPin = 4;
constexpr std::uint8_t kUsbDnPin = 19;
constexpr std::uint8_t kUsbDpPin = 20;
constexpr std::uint8_t kBoot0Pin = 0;
constexpr std::int8_t kUsbVbusSensePin = -1;
constexpr std::int8_t kExternalPowerSensePin = 40;
constexpr std::uint8_t kExternalPowerSenseActiveLevel = 0;
constexpr std::int8_t kChargeStatusPin = 39;
// V2 schematic truth table: with USB present, high means charging and low means full.
constexpr std::uint8_t kChargeStatusChargingLevel = 1;
constexpr std::int8_t kKeyWakePin = 21;
constexpr std::int8_t kPeripheralPowerEnablePin = 8;
constexpr std::uint8_t kPeripheralPowerEnableActiveLevel = 1;
constexpr std::int8_t kMicI2sBclkPin = 9;
constexpr std::int8_t kMicI2sWsPin = 10;
constexpr std::int8_t kMicI2sDataInPin = 11;
// V2 schematic nets: GPIO13=SPK_I2S_WS_OUT,
// GPIO14=SPK_I2S_BCK_OUT, GPIO15=SPK_I2S_SD_OUT.
constexpr std::int8_t kSpkI2sBclkPin = 14;
constexpr std::int8_t kSpkI2sWsPin = 13;
constexpr std::int8_t kSpkI2sDataOutPin = 15;

#else

constexpr const char* kBoardName = "v1";
constexpr const char* kUsbSerialNumber = "easy-input-v1";

constexpr std::array<KeyPin, 8> kKeyPins{{
    {"KEY1", 39},
    {"KEY2", 40},
    {"KEY3", 41},
    {"KEY4", 42},
    {"KEY5", 38},
    {"KEY6", 37},
    {"KEY7", 36},
    {"KEY8", 35},
}};

constexpr std::uint8_t kEncoderPinA = 9;
constexpr std::uint8_t kEncoderPinB = 10;
constexpr std::uint8_t kEncoderPressPin = 11;
constexpr std::int8_t kEncoderDirectionMultiplier = -1;
constexpr std::uint8_t kWs2812Pin = 18;
constexpr std::uint8_t kWs2812Count = 5;
constexpr std::int8_t kBatterySenseEnablePin = 6;
constexpr std::int8_t kBatterySenseAdcPin = 7;
constexpr std::uint8_t kUsbDnPin = 19;
constexpr std::uint8_t kUsbDpPin = 20;
constexpr std::uint8_t kBoot0Pin = 0;
constexpr std::int8_t kUsbVbusSensePin = -1;
constexpr std::int8_t kExternalPowerSensePin = -1;
constexpr std::uint8_t kExternalPowerSenseActiveLevel = 1;
constexpr std::int8_t kChargeStatusPin = -1;
constexpr std::uint8_t kChargeStatusChargingLevel = 1;
constexpr std::int8_t kKeyWakePin = -1;
constexpr std::int8_t kPeripheralPowerEnablePin = -1;
constexpr std::uint8_t kPeripheralPowerEnableActiveLevel = 1;
constexpr std::int8_t kMicI2sBclkPin = -1;
constexpr std::int8_t kMicI2sWsPin = -1;
constexpr std::int8_t kMicI2sDataInPin = -1;
constexpr std::int8_t kSpkI2sBclkPin = -1;
constexpr std::int8_t kSpkI2sWsPin = -1;
constexpr std::int8_t kSpkI2sDataOutPin = -1;

#endif

}  // namespace ai_keyboard
