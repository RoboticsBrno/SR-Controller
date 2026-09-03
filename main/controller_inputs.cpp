#include "controller_inputs.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware_config.hpp"
#include "nvs.h"

#include <algorithm>

namespace {
constexpr char kTag[] = "controller_inputs";
constexpr char kNvsNamespace[] = "controller";
constexpr char kCalibrationKey[] = "axis_cal_v1";
constexpr uint32_t kCalibrationMagic = 0x5343414c; // SCAL
constexpr uint16_t kMinimumCalibrationSpan = 500;

struct StoredCalibration {
    uint32_t magic;
    std::array<controller::AxisCalibration, 4> axes;
};
}

bool ControllerInputs::isActive(int level) {
    return hardware::kInputsActiveLow ? level == 0 : level != 0;
}

esp_err_t ControllerInputs::initialize() {
    calibration_ = hardware::kAxisCalibration;
    const esp_err_t calibration_error = loadCalibration();
    if (calibration_error != ESP_OK && calibration_error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "Could not load calibration: %s; using defaults", esp_err_to_name(calibration_error));
    }

    uint64_t input_mask = 0;
    for (const auto pin : hardware::kButtonPins) input_mask |= 1ULL << pin;
    input_mask |= 1ULL << hardware::kEmergencyContactPin;
    input_mask |= 1ULL << hardware::kArmedContactPin;

    const gpio_config_t input_config{
        .pin_bit_mask = input_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&input_config);
    if (error != ESP_OK) return error;

    const adc_oneshot_unit_init_cfg_t unit_config{
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    error = adc_oneshot_new_unit(&unit_config, &adc_handle_);
    if (error != ESP_OK) return error;

    const adc_oneshot_chan_cfg_t channel_config{
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    for (const auto channel : hardware::kAxisChannels) {
        error = adc_oneshot_config_channel(adc_handle_, channel, &channel_config);
        if (error != ESP_OK) return error;
    }

    vTaskDelay(pdMS_TO_TICKS(hardware::kGroupSettleMs));
    return ESP_OK;
}

uint8_t ControllerInputs::readGroup() const {
    uint8_t active_buttons = 0;
    for (size_t index = 0; index < hardware::kButtonPins.size(); ++index) {
        if (isActive(gpio_get_level(hardware::kButtonPins[index]))) {
            active_buttons |= static_cast<uint8_t>(1U << index);
        }
    }
    return controller::computeGroup(active_buttons);
}

controller::ControllerSnapshot ControllerInputs::sample() {
    std::lock_guard lock(mutex_);
    controller::ControllerSnapshot snapshot{};

    for (size_t axis = 0; axis < hardware::kAxisChannels.size(); ++axis) {
        uint32_t sum = 0;
        uint8_t successful_reads = 0;
        for (uint8_t sample = 0; sample < hardware::kAdcAverageSamples; ++sample) {
            int raw = 0;
            if (adc_oneshot_read(adc_handle_, hardware::kAxisChannels[axis], &raw) == ESP_OK) {
                sum += static_cast<uint32_t>(raw);
                ++successful_reads;
            }
        }
        if (successful_reads > 0) {
            last_raw_[axis] = static_cast<uint16_t>(sum / successful_reads);
            if (calibration_active_ && calibration_center_captured_) {
                calibration_[axis].minimum = std::min(calibration_[axis].minimum, last_raw_[axis]);
                calibration_[axis].maximum = std::max(calibration_[axis].maximum, last_raw_[axis]);
            }
            last_axes_[axis] = controller::normalizeAxis(last_raw_[axis], calibration_[axis]);
        } else {
            const int64_t now = esp_timer_get_time();
            if (now - last_adc_warning_us_ >= 1000000) {
                ESP_LOGW(kTag, "ADC read failed; retaining previous axis values");
                last_adc_warning_us_ = now;
            }
        }
    }
    snapshot.axes = last_axes_;

    for (size_t index = 0; index < hardware::kButtonPins.size(); ++index) {
        const bool pressed = button_debouncers_[index].update(isActive(gpio_get_level(hardware::kButtonPins[index])));
        if (pressed) snapshot.switches |= static_cast<uint16_t>(1U << index);
    }

    const bool emergency_contact = isActive(gpio_get_level(hardware::kEmergencyContactPin));
    const bool armed_contact = isActive(gpio_get_level(hardware::kArmedContactPin));
    snapshot.safety = safety_interlock_.update(emergency_contact, armed_contact);
    if (calibration_active_) {
        snapshot.safety.deadman = false;
        snapshot.safety.startup_inhibited = true;
    }
    if (emergency_contact) snapshot.switches |= 1U << 7;
    if (armed_contact) snapshot.switches |= 1U << 8;
    return snapshot;
}

void ControllerInputs::beginCalibration() {
    std::lock_guard lock(mutex_);
    calibration_active_ = true;
    calibration_center_captured_ = false;
    ESP_LOGW(kTag, "CALIBRATION started; radio motion enable is inhibited");
    ESP_LOGI(kTag, "Release all joysticks to center, then enter: center");
}

bool ControllerInputs::captureCalibrationCenter() {
    std::lock_guard lock(mutex_);
    if (!calibration_active_) return false;
    for (size_t axis = 0; axis < calibration_.size(); ++axis) {
        calibration_[axis] = {last_raw_[axis], last_raw_[axis], last_raw_[axis],
                              hardware::kAxisCalibration[axis].center_deadzone};
    }
    calibration_center_captured_ = true;
    ESP_LOGI(kTag, "Centers captured: [%u,%u,%u,%u]", last_raw_[0], last_raw_[1],
             last_raw_[2], last_raw_[3]);
    ESP_LOGI(kTag, "Move every joystick axis repeatedly to both limits, then enter: save");
    return true;
}

esp_err_t ControllerInputs::saveCalibration() {
    std::lock_guard lock(mutex_);
    if (!calibration_active_ || !calibration_center_captured_) return ESP_ERR_INVALID_STATE;
    for (size_t axis = 0; axis < calibration_.size(); ++axis) {
        if (!controller::isValidCalibration(calibration_[axis], kMinimumCalibrationSpan)) {
            ESP_LOGE(kTag, "Axis %u has invalid range: min=%u center=%u max=%u", axis + 1,
                     calibration_[axis].minimum, calibration_[axis].center,
                     calibration_[axis].maximum);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    const esp_err_t error = persistCalibration(calibration_);
    if (error == ESP_OK) {
        calibration_active_ = false;
        calibration_center_captured_ = false;
        ESP_LOGI(kTag, "Calibration saved to NVS");
    }
    return error;
}

void ControllerInputs::cancelCalibration() {
    std::lock_guard lock(mutex_);
    calibration_ = hardware::kAxisCalibration;
    const esp_err_t error = loadCalibration();
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "Could not restore saved calibration: %s", esp_err_to_name(error));
    }
    calibration_active_ = false;
    calibration_center_captured_ = false;
    ESP_LOGI(kTag, "Calibration canceled");
}

esp_err_t ControllerInputs::resetCalibration() {
    std::lock_guard lock(mutex_);
    const esp_err_t error = persistCalibration(hardware::kAxisCalibration);
    if (error == ESP_OK) {
        calibration_ = hardware::kAxisCalibration;
        calibration_active_ = false;
        calibration_center_captured_ = false;
        ESP_LOGI(kTag, "Calibration reset to firmware defaults");
    }
    return error;
}

void ControllerInputs::printCalibration() const {
    std::lock_guard lock(mutex_);
    for (size_t axis = 0; axis < calibration_.size(); ++axis) {
        const auto &value = calibration_[axis];
        ESP_LOGI(kTag, "A%u min=%u center=%u max=%u deadzone=%u", axis + 1,
                 value.minimum, value.center, value.maximum, value.center_deadzone);
    }
}

bool ControllerInputs::isCalibrating() const {
    std::lock_guard lock(mutex_);
    return calibration_active_;
}

esp_err_t ControllerInputs::loadCalibration() {
    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    StoredCalibration stored{};
    size_t size = sizeof(stored);
    error = nvs_get_blob(handle, kCalibrationKey, &stored, &size);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    if (size != sizeof(stored) || stored.magic != kCalibrationMagic) return ESP_ERR_INVALID_VERSION;
    for (const auto &axis : stored.axes) {
        if (!controller::isValidCalibration(axis, kMinimumCalibrationSpan)) return ESP_ERR_INVALID_ARG;
    }
    calibration_ = stored.axes;
    ESP_LOGI(kTag, "Loaded joystick calibration from NVS");
    return ESP_OK;
}

esp_err_t ControllerInputs::persistCalibration(
    const std::array<controller::AxisCalibration, 4> &calibration) {
    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    const StoredCalibration stored{kCalibrationMagic, calibration};
    error = nvs_set_blob(handle, kCalibrationKey, &stored, sizeof(stored));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}
