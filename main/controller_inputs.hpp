#pragma once

#include "controller_state.hpp"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

#include <array>
#include <cstdint>
#include <mutex>

class ControllerInputs {
public:
    esp_err_t initialize();
    uint8_t readGroup() const;
    controller::ControllerSnapshot sample();

    void beginCalibration();
    bool captureCalibrationCenter();
    esp_err_t saveCalibration();
    void cancelCalibration();
    esp_err_t resetCalibration();
    void printCalibration() const;
    bool isCalibrating() const;

private:
    static bool isActive(int level);
    esp_err_t loadCalibration();
    esp_err_t persistCalibration(const std::array<controller::AxisCalibration, 4> &calibration);

    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    std::array<controller::Debouncer, 7> button_debouncers_{};
    controller::SafetyInterlock safety_interlock_{};
    std::array<int16_t, 4> last_axes_{};
    std::array<uint16_t, 4> last_raw_{};
    std::array<controller::AxisCalibration, 4> calibration_{};
    bool calibration_active_ = false;
    bool calibration_center_captured_ = false;
    mutable std::mutex mutex_;
    int64_t last_adc_warning_us_ = 0;
};
