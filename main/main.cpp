#include "controller_display.hpp"
#include "controller_inputs.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware_config.hpp"
#include "nvs_flash.h"
#include "radio_controller.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
constexpr char kTag[] = "sr_controller";

struct SharedState {
    std::mutex mutex;
    controller::ControllerSnapshot snapshot{};
};

ControllerInputs inputs;
RadioController radio;
ControllerDisplay display;
SharedState shared_state;
uint8_t selected_group = 0;

const char *safetyName(const controller::SafetyState &safety) {
    if (safety.emergency_stop) return "E-STOP";
    if (safety.startup_inhibited) return "INTERLOCK";
    if (safety.deadman) return "ARMED";
    return "SAFE";
}

void logInputChange(const controller::ControllerSnapshot &snapshot) {
    ESP_LOGI(kTag, "INPUT axes=[%d,%d,%d,%d] buttons=0x%03x safety=%s",
             snapshot.axes[0], snapshot.axes[1], snapshot.axes[2], snapshot.axes[3],
             snapshot.switches, safetyName(snapshot.safety));
}

void consoleTask(void *) {
    char command[64];
    ESP_LOGI(kTag, "Console commands: calibrate, center, save, cancel, calibration show, calibration reset");
    while (true) {
        if (std::fgets(command, sizeof(command), stdin) == nullptr) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        command[strcspn(command, "\r\n")] = '\0';
        if (std::strcmp(command, "calibrate") == 0) {
            inputs.beginCalibration();
        } else if (std::strcmp(command, "center") == 0) {
            if (!inputs.captureCalibrationCenter()) {
                ESP_LOGW(kTag, "Enter 'calibrate' before 'center'");
            }
        } else if (std::strcmp(command, "save") == 0) {
            const esp_err_t error = inputs.saveCalibration();
            if (error != ESP_OK) ESP_LOGE(kTag, "Calibration not saved: %s", esp_err_to_name(error));
        } else if (std::strcmp(command, "cancel") == 0) {
            inputs.cancelCalibration();
        } else if (std::strcmp(command, "calibration show") == 0) {
            inputs.printCalibration();
        } else if (std::strcmp(command, "calibration reset") == 0) {
            const esp_err_t error = inputs.resetCalibration();
            if (error != ESP_OK) ESP_LOGE(kTag, "Calibration reset failed: %s", esp_err_to_name(error));
        } else if (command[0] != '\0') {
            ESP_LOGW(kTag, "Unknown command: %s", command);
        }
    }
}

void displayTask(void *) {
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        controller::ControllerSnapshot snapshot;
        {
            std::lock_guard lock(shared_state.mutex);
            snapshot = shared_state.snapshot;
        }
        display.render(selected_group, snapshot, radio.diagnostics(), esp_timer_get_time(),
                       inputs.isCalibrating());
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(hardware::kDisplayPeriodMs));
    }
}
}

extern "C" void app_main() {
    esp_err_t nvs_error = nvs_flash_init();
    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES || nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_error);
    ESP_ERROR_CHECK(inputs.initialize());
    selected_group = inputs.readGroup();
    ESP_ERROR_CHECK(display.initialize());
    ESP_ERROR_CHECK(radio.initialize(selected_group));

    auto snapshot = inputs.sample();
    {
        std::lock_guard lock(shared_state.mutex);
        shared_state.snapshot = snapshot;
    }

    const BaseType_t display_task_created =
        xTaskCreate(displayTask, "controller_display", 4096, nullptr, 2, nullptr);
    ESP_ERROR_CHECK(display_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    const BaseType_t console_task_created =
        xTaskCreate(consoleTask, "controller_console", 4096, nullptr, 2, nullptr);
    ESP_ERROR_CHECK(console_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(kTag, "Sender ready: group=%u channel=%u controller=%u robot=%u period=%ums",
             selected_group, hardware::kWifiChannel, hardware::kControllerId,
             hardware::kRobotId, hardware::kControlPeriodMs);

    logInputChange(snapshot);
    auto last_logged_snapshot = snapshot;
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        snapshot = inputs.sample();
        radio.send(snapshot);
        if (controller::hasMeaningfulInputChange(last_logged_snapshot, snapshot,
                                                 hardware::kDiagnosticAxisThreshold)) {
            logInputChange(snapshot);
            last_logged_snapshot = snapshot;
        }
        {
            std::lock_guard lock(shared_state.mutex);
            shared_state.snapshot = snapshot;
        }
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(hardware::kControlPeriodMs));
    }
}
