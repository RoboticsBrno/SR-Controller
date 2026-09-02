#include <atomic>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "simple_radio.h"

#include "protocol.hpp"

namespace {
constexpr uint8_t kGroup = 0;
constexpr uint8_t kControllerId = 1;
constexpr uint8_t kRobotId = 2;
constexpr char kTag[] = "sr_receiver";
std::atomic<uint32_t> applied_sequence{0};
std::atomic<bool> has_control{false};
std::atomic<int64_t> last_control_us{0};
std::atomic<bool> deadman{false};
} // namespace

extern "C" void app_main() {
    sr_protocol::selfCheck();
    ESP_ERROR_CHECK(SimpleRadio.begin(kGroup));
    SimpleRadio.setOnBlobCallback([](std::span<const uint8_t> blob,
                                     PacketInfo info) {
        sr_protocol::Control control{};
        if (!sr_protocol::parseControl(blob, control) ||
            control.source_id != kControllerId ||
            (control.destination_id != kRobotId && control.destination_id != 0))
            return;
        if (has_control.load() &&
            static_cast<int32_t>(control.sequence - applied_sequence.load()) <=
                0)
            return;
        applied_sequence.store(control.sequence);
        has_control.store(true);
        last_control_us.store(esp_timer_get_time());
        deadman.store((control.safety & 0x01) != 0);
        ESP_LOGI(kTag,
                 "control seq=%lu pot=[%u,%u,%u,%u] switches=0x%03x "
                 "safety=0x%02x rssi=%d",
                 static_cast<unsigned long>(control.sequence), control.pots[0],
                 control.pots[1], control.pots[2], control.pots[3],
                 control.switches, control.safety, info.rssi);
    });

    uint32_t telemetry_sequence = 0;
    while (true) {
        const bool fresh =
            has_control.load() &&
            esp_timer_get_time() - last_control_us.load() <= 150000;
        if (!fresh)
            has_control.store(false);
        const sr_protocol::Telemetry telemetry{
            kRobotId,
            kControllerId,
            telemetry_sequence++,
            0,
            0,
            0,
            static_cast<uint16_t>(fresh && deadman.load() ? 0x0001 : 0x0002),
            applied_sequence.load(),
            0};
        const auto packet = sr_protocol::makeTelemetry(telemetry);
        SimpleRadio.sendBlob(packet.data(), packet.size());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
