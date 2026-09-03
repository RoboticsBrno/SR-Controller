#include "controller_display.hpp"

#include "bus/dgx_spi_esp32.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_screen.h"
#include "driver/spi_master.h"
#include "drivers/st7920.h"
#include "esp_log.h"
#include "fonts/TerminusTTFMedium12.h"
#include "hardware_config.hpp"

#include <cstdio>

namespace {
constexpr char kTag[] = "controller_display";

const char *safetyLabel(const controller::SafetyState &safety) {
    if (safety.emergency_stop) return "E-STOP";
    if (safety.startup_inhibited) return "INTERLOCK";
    if (safety.deadman) return "ARMED";
    return "SAFE";
}
}

esp_err_t ControllerDisplay::initialize() {
    uint64_t led_mask = 0;
    for (const auto pin : hardware::kLedPins) led_mask |= 1ULL << pin;
    const gpio_config_t led_config{
        .pin_bit_mask = led_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&led_config);
    if (error != ESP_OK) return error;
    setLed(0, true);
    setLed(1, false);
    setLed(2, false);

    bus_ = dgx_spi_init(SPI2_HOST, SPI_DMA_CH_AUTO, hardware::kDisplayDataPin, GPIO_NUM_NC,
                        hardware::kDisplayClockPin, GPIO_NUM_NC, GPIO_NUM_NC, 1000000, 0);
    if (bus_ == nullptr) {
        ESP_LOGW(kTag, "ST7920 SPI initialization failed; continuing without display");
        return ESP_OK;
    }
    screen_ = dgx_st7920_init(bus_, GPIO_NUM_NC, hardware::kDisplayChipSelectPin);
    if (screen_ == nullptr) {
        ESP_LOGW(kTag, "ST7920 initialization failed; continuing without display");
        return ESP_OK;
    }
    // The ST7920 driver is write-only and otherwise logs every refreshed region.
    esp_log_level_set("DGX ST7920", ESP_LOG_WARN);
    return ESP_OK;
}

void ControllerDisplay::setLed(size_t index, bool enabled) const {
    const int active = hardware::kLedsActiveHigh ? enabled : !enabled;
    gpio_set_level(hardware::kLedPins[index], active);
}

void ControllerDisplay::render(uint8_t group, const controller::ControllerSnapshot &snapshot,
                               const RadioDiagnostics &radio, int64_t now_us, bool calibrating) {
    const bool telemetry_fresh = radio.has_telemetry &&
        now_us - radio.received_at_us <= static_cast<int64_t>(hardware::kTelemetryFreshMs) * 1000;
    setLed(0, true);
    setLed(1, snapshot.safety.deadman);
    const bool emergency_blink = snapshot.safety.emergency_stop && ((now_us / 250000) % 2 == 0);
    setLed(2, snapshot.safety.emergency_stop ? emergency_blink : snapshot.safety.startup_inhibited || telemetry_fresh);

    if (screen_ == nullptr) return;

    char line[32];
    dgx_screen_progress_up(screen_);
    dgx_fill_rectangle(screen_, 0, 0, screen_->width, screen_->height, 0);

    std::snprintf(line, sizeof(line), "G:%03u %-9s", group,
                  calibrating ? "CALIBRATE" : safetyLabel(snapshot.safety));
    dgx_font_string_utf8_screen(screen_, 0, 10, line, 1, DgxOutputNormal, 1, TerminusTTFMedium12(), nullptr, nullptr);
    std::snprintf(line, sizeof(line), "A1:%6d A2:%6d", snapshot.axes[0], snapshot.axes[1]);
    dgx_font_string_utf8_screen(screen_, 0, 21, line, 1, DgxOutputNormal, 1, TerminusTTFMedium12(), nullptr, nullptr);
    std::snprintf(line, sizeof(line), "A3:%6d A4:%6d", snapshot.axes[2], snapshot.axes[3]);
    dgx_font_string_utf8_screen(screen_, 0, 32, line, 1, DgxOutputNormal, 1, TerminusTTFMedium12(), nullptr, nullptr);
    std::snprintf(line, sizeof(line), "BTN:%c%c%c%c%c%c%c", (snapshot.switches & 1) ? '1' : '0',
                  (snapshot.switches & 2) ? '1' : '0', (snapshot.switches & 4) ? '1' : '0',
                  (snapshot.switches & 8) ? '1' : '0', (snapshot.switches & 16) ? '1' : '0',
                  (snapshot.switches & 32) ? '1' : '0', (snapshot.switches & 64) ? '1' : '0');
    dgx_font_string_utf8_screen(screen_, 0, 43, line, 1, DgxOutputNormal, 1, TerminusTTFMedium12(), nullptr, nullptr);
    if (telemetry_fresh) {
        std::snprintf(line, sizeof(line), "LINK %umV RSSI:%d", radio.telemetry.battery_mv, radio.telemetry.rssi_dbm);
    } else {
        std::snprintf(line, sizeof(line), "NO LINK");
    }
    dgx_font_string_utf8_screen(screen_, 0, 54, line, 1, DgxOutputNormal, 1, TerminusTTFMedium12(), nullptr, nullptr);
    if (dgx_screen_progress_down(screen_) == 0) {
        screen_->update_screen(screen_, 0, screen_->width - 1, 0, screen_->height - 1);
    }
}
