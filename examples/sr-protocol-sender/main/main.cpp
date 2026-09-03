#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "simple_radio.h"

#include "protocol.hpp"

namespace {
constexpr uint8_t kGroup = 0;
constexpr uint8_t kControllerId = 1;
constexpr uint8_t kRobotId = 2;
constexpr char kTag[] = "sr_sender";
}

extern "C" void app_main() {
    sr_protocol::selfCheck();
    ESP_ERROR_CHECK(SimpleRadio.begin(kGroup));
    SimpleRadio.setOnBlobCallback([](std::span<const uint8_t> blob, PacketInfo) {
        sr_protocol::Telemetry telemetry{};
        if (sr_protocol::parseTelemetry(blob, telemetry) &&
            (telemetry.destination_id == kControllerId || telemetry.destination_id == 0)) {
            ESP_LOGI(kTag, "telemetry seq=%lu battery=%umV motor=0x%04x applied=%lu rssi=%d",
                static_cast<unsigned long>(telemetry.sequence), telemetry.battery_mv, telemetry.motor_status,
                static_cast<unsigned long>(telemetry.applied_control_sequence), telemetry.rssi_dbm);
        }
    });

    uint32_t sequence = 0;
    const sr_protocol::Control state{kControllerId, kRobotId, 0, {32768, 32768, 32768, 32768}, 0, 0};
    while (true) {
        auto control = state;
        control.sequence = sequence++;
        const auto packet = sr_protocol::makeControl(control);
        SimpleRadio.sendBlob(packet.data(), packet.size());
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
