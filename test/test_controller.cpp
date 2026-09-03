#include "controller_state.hpp"
#include "protocol.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

void testGroupUsesButtonOneAsLeastSignificantBit() {
    assert(controller::computeGroup(0b0000001) == 1);
    assert(controller::computeGroup(0b0000101) == 5);
    assert(controller::computeGroup(0b1111111) == 127);
}

void testAxisCalibrationMapsEndsAndCenterToSignedRange() {
    constexpr controller::AxisCalibration calibration{1000, 2000, 3000, 20};
    static_assert(controller::isValidCalibration(calibration, 1000));
    assert(controller::normalizeAxis(0, calibration) == -32768);
    assert(controller::normalizeAxis(1000, calibration) == -32768);
    assert(controller::normalizeAxis(1980, calibration) == 0);
    assert(controller::normalizeAxis(2000, calibration) == 0);
    assert(controller::normalizeAxis(2020, calibration) == 0);
    assert(controller::normalizeAxis(3000, calibration) == 32767);
    assert(controller::normalizeAxis(4095, calibration) == 32767);
    assert(controller::normalizeAxis(1500, calibration) < 0);
    assert(controller::normalizeAxis(2500, calibration) > 0);
    static_assert(!controller::isValidCalibration({2000, 1000, 3000, 20}, 1000));
    static_assert(!controller::isValidCalibration({1000, 2000, 2500, 20}, 2000));
}

void testDebouncerRequiresStableSamples() {
    controller::Debouncer debounce(false, 2);
    assert(!debounce.update(true));
    assert(debounce.update(true));
    assert(debounce.value());
    assert(debounce.update(false));
    assert(!debounce.update(false));
    assert(!debounce.value());
}

void testSafetyInterlockRequiresSafeBeforeArmed() {
    controller::SafetyInterlock interlock(2);
    auto state = interlock.update(false, true);
    assert(state.startup_inhibited);
    assert(!state.deadman && !state.emergency_stop);

    interlock.update(false, false);
    state = interlock.update(false, false);
    assert(!state.startup_inhibited);
    assert(!state.deadman && !state.emergency_stop);

    interlock.update(false, true);
    state = interlock.update(false, true);
    assert(state.deadman && !state.emergency_stop);
}

void testEmergencyAndInvalidSelectorFailSafeImmediately() {
    controller::SafetyInterlock interlock(2);
    auto state = interlock.update(true, false);
    assert(state.emergency_stop && !state.deadman);
    state = interlock.update(true, true);
    assert(state.emergency_stop && !state.deadman);
}

void testControlPacketRoundTripAndWireLayout() {
    const sr_protocol::Control input{1, 2, 0x78563412, {-32768, -1, 0, 32767}, 0x0155, 0x01};
    const auto packet = sr_protocol::makeControl(input);
    static_assert(packet.size() == 31);
    assert(packet[0] == 'S' && packet[1] == 'R');
    assert(packet[7] == 0x12 && packet[8] == 0x34 && packet[9] == 0x56 && packet[10] == 0x78);
    assert(packet[11] == 0x01 && packet[12] == 1 && packet[13] == 0x01);
    assert(packet[14] == 0x02 && packet[15] == 9 && packet[16] == 0x0f);
    assert(packet[17] == 0x00 && packet[18] == 0x80);
    assert(packet[19] == 0xff && packet[20] == 0xff);
    assert(packet[21] == 0x00 && packet[22] == 0x00);
    assert(packet[23] == 0xff && packet[24] == 0x7f);
    assert(packet[25] == 0x04 && packet[26] == 4);

    sr_protocol::Control output{};
    assert(sr_protocol::parseControl(packet, output));
    assert(output.source_id == input.source_id);
    assert(output.destination_id == input.destination_id);
    assert(output.sequence == input.sequence);
    assert(output.joystick_axes == input.joystick_axes);
    assert(output.potentiometer_present == 0);
    assert(output.switches == input.switches);
    assert(output.safety == input.safety);
}

void testInputChangeDetectionFiltersAdcNoise() {
    controller::ControllerSnapshot previous{};
    controller::ControllerSnapshot current{};
    current.axes[0] = 99;
    assert(!controller::hasMeaningfulInputChange(previous, current, 100));
    current.axes[0] = 100;
    assert(controller::hasMeaningfulInputChange(previous, current, 100));
    current = previous;
    current.switches = 1;
    assert(controller::hasMeaningfulInputChange(previous, current, 100));
    current = previous;
    current.safety.deadman = true;
    assert(controller::hasMeaningfulInputChange(previous, current, 100));
}

void testTelemetryValidationRejectsMalformedPacket() {
    std::array<uint8_t, 11> truncated{'S', 'R', 1, 3, 0, 2, 1, 0, 0, 0, 0};
    sr_protocol::Telemetry telemetry{};
    assert(!sr_protocol::parseTelemetry(truncated, telemetry));
}

} // namespace

int main() {
    testGroupUsesButtonOneAsLeastSignificantBit();
    testAxisCalibrationMapsEndsAndCenterToSignedRange();
    testDebouncerRequiresStableSamples();
    testSafetyInterlockRequiresSafeBeforeArmed();
    testEmergencyAndInvalidSelectorFailSafeImmediately();
    testControlPacketRoundTripAndWireLayout();
    testInputChangeDetectionFiltersAdcNoise();
    testTelemetryValidationRejectsMalformedPacket();
    std::cout << "controller tests passed\n";
}
