#include "radio_controller.hpp"

#include "esp_timer.h"
#include "hardware_config.hpp"
#include "simple_radio.h"

esp_err_t RadioController::initialize(uint8_t group) {
    auto config = SimpleRadioImpl::DEFAULT_CONFIG;
    config.channel = hardware::kWifiChannel;
    const esp_err_t error = SimpleRadio.begin(group, config);
    if (error != ESP_OK) return error;

    SimpleRadio.setOnBlobCallback([this](std::span<const uint8_t> blob, PacketInfo) { receive(blob); });
    return ESP_OK;
}

void RadioController::send(const controller::ControllerSnapshot &snapshot) {
    uint8_t safety = 0;
    if (snapshot.safety.deadman) safety |= 0x01;
    if (snapshot.safety.emergency_stop) safety |= 0x02;
    const sr_protocol::Control control{
        hardware::kControllerId,
        hardware::kRobotId,
        sequence_++,
        snapshot.axes,
        snapshot.switches,
        safety,
    };
    const auto packet = sr_protocol::makeControl(control);
    SimpleRadio.sendBlob(packet);
}

RadioDiagnostics RadioController::diagnostics() const {
    std::lock_guard lock(mutex_);
    return diagnostics_;
}

void RadioController::receive(std::span<const uint8_t> blob) {
    sr_protocol::Telemetry telemetry{};
    if (!sr_protocol::parseTelemetry(blob, telemetry) || telemetry.source_id != hardware::kRobotId ||
        (telemetry.destination_id != hardware::kControllerId && telemetry.destination_id != 0)) {
        return;
    }
    std::lock_guard lock(mutex_);
    diagnostics_.has_telemetry = true;
    diagnostics_.received_at_us = esp_timer_get_time();
    diagnostics_.telemetry = telemetry;
}
