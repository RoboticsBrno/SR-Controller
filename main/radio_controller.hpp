#pragma once

#include "controller_state.hpp"
#include "esp_err.h"
#include "protocol.hpp"

#include <cstdint>
#include <mutex>

struct RadioDiagnostics {
    bool has_telemetry = false;
    int64_t received_at_us = 0;
    sr_protocol::Telemetry telemetry{};
};

class RadioController {
public:
    esp_err_t initialize(uint8_t group);
    void send(const controller::ControllerSnapshot &snapshot);
    [[nodiscard]] RadioDiagnostics diagnostics() const;

private:
    void receive(std::span<const uint8_t> blob);

    mutable std::mutex mutex_;
    RadioDiagnostics diagnostics_{};
    uint32_t sequence_ = 0;
};
