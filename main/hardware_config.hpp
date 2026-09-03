#pragma once

#include "controller_state.hpp"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#include <array>
#include <cstdint>

namespace hardware {

constexpr std::array<gpio_num_t, 4> kAxisPins{GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7};
constexpr std::array<adc_channel_t, 4> kAxisChannels{ADC_CHANNEL_3, ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6};
constexpr std::array<gpio_num_t, 7> kButtonPins{
    GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10};

// Swap these two constants if the physical selector contacts are reversed.
constexpr gpio_num_t kEmergencyContactPin = GPIO_NUM_11;
constexpr gpio_num_t kArmedContactPin = GPIO_NUM_12;

constexpr std::array<gpio_num_t, 3> kLedPins{GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_21};
constexpr gpio_num_t kDisplayChipSelectPin = GPIO_NUM_47; // ST7920 RS/CS
constexpr gpio_num_t kDisplayDataPin = GPIO_NUM_48;       // ST7920 RW/SID
constexpr gpio_num_t kDisplayClockPin = GPIO_NUM_38;      // ST7920 EN/SCLK

constexpr bool kInputsActiveLow = true;
constexpr bool kLedsActiveHigh = true;
constexpr uint8_t kControllerId = 1;
constexpr uint8_t kRobotId = 2;
constexpr uint8_t kWifiChannel = 1;
constexpr uint32_t kControlPeriodMs = 20;
constexpr uint32_t kDisplayPeriodMs = 100;
constexpr uint32_t kTelemetryFreshMs = 500;
constexpr uint32_t kGroupSettleMs = 100;
constexpr uint8_t kDebounceSamples = 2;
constexpr uint8_t kAdcAverageSamples = 4;
// Calibrated from the connected controller. Order matches GPIO4 through GPIO7.
constexpr std::array<controller::AxisCalibration, 4> kAxisCalibration{{
    {919, 1769, 3057, 20},
    {815, 1805, 2975, 20},
    {775, 1941, 2869, 20},
    {938, 1916, 2869, 20},
}};
constexpr uint16_t kDiagnosticAxisThreshold = 256;

} // namespace hardware
