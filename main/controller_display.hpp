#pragma once

#include "controller_state.hpp"
#include "esp_err.h"
#include "radio_controller.hpp"

#include <cstddef>
#include <cstdint>

typedef struct _dgx_screen_t dgx_screen_t;
typedef struct _dgx_bus_protocols_t dgx_bus_protocols_t;

class ControllerDisplay {
public:
    esp_err_t initialize();
    void render(uint8_t group, const controller::ControllerSnapshot &snapshot,
                const RadioDiagnostics &radio, int64_t now_us, bool calibrating);

private:
    void setLed(size_t index, bool enabled) const;

    dgx_bus_protocols_t *bus_ = nullptr;
    dgx_screen_t *screen_ = nullptr;
};
