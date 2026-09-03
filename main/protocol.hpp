#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sr_protocol {

constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 11;
constexpr size_t kControlPacketSize = 31;
constexpr size_t kTelemetryPacketSize = 38;

enum class PacketType : uint8_t { Hello = 1, Control = 2, Telemetry = 3, Response = 4 };

struct Control {
    uint8_t source_id = 0;
    uint8_t destination_id = 0;
    uint32_t sequence = 0;
    std::array<int16_t, 4> joystick_axes{};
    uint16_t switches = 0;
    uint8_t safety = 0;
    uint8_t potentiometer_present = 0;
    std::array<uint16_t, 4> potentiometers{};
};

struct Telemetry {
    uint8_t source_id = 0;
    uint8_t destination_id = 0;
    uint32_t sequence = 0;
    uint16_t battery_mv = 0;
    int32_t left_encoder = 0;
    int32_t right_encoder = 0;
    uint16_t motor_status = 0;
    uint32_t applied_control_sequence = 0;
    int8_t rssi_dbm = 0;
};

inline void put16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}
inline void put32(uint8_t *out, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
inline uint16_t get16(const uint8_t *in) {
    return static_cast<uint16_t>(in[0]) | static_cast<uint16_t>(in[1] << 8);
}
inline uint32_t get32(const uint8_t *in) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[i]) << (8 * i);
    return value;
}

inline void writeHeader(uint8_t *out, PacketType type, uint8_t source, uint8_t destination, uint32_t sequence) {
    out[0] = 'S'; out[1] = 'R'; out[2] = kVersion; out[3] = static_cast<uint8_t>(type);
    out[4] = 0; out[5] = source; out[6] = destination; put32(out + 7, sequence);
}

inline bool validHeader(std::span<const uint8_t> packet, PacketType type) {
    return packet.size() >= kHeaderSize && packet[0] == 'S' && packet[1] == 'R' &&
           packet[2] == kVersion && packet[3] == static_cast<uint8_t>(type) &&
           (packet[4] & 0xf8) == 0 && packet[5] != 0;
}

inline std::array<uint8_t, kControlPacketSize> makeControl(const Control &control) {
    std::array<uint8_t, kControlPacketSize> packet{};
    writeHeader(packet.data(), PacketType::Control, control.source_id, control.destination_id, control.sequence);
    size_t offset = kHeaderSize;
    packet[offset++] = 0x01; packet[offset++] = 1;
    packet[offset++] = static_cast<uint8_t>(control.safety & 0x03);
    packet[offset++] = 0x02; packet[offset++] = 9; packet[offset++] = 0x0f;
    for (int16_t axis : control.joystick_axes) {
        put16(packet.data() + offset, static_cast<uint16_t>(axis));
        offset += 2;
    }
    packet[offset++] = 0x04; packet[offset++] = 4;
    put16(packet.data() + offset, 0x01ff); offset += 2;
    put16(packet.data() + offset, static_cast<uint16_t>(control.switches & 0x01ff));
    return packet;
}

inline bool parseControl(std::span<const uint8_t> packet, Control &out) {
    if (!validHeader(packet, PacketType::Control)) return false;
    Control result{};
    result.source_id = packet[5];
    result.destination_id = packet[6];
    result.sequence = get32(packet.data() + 7);
    size_t offset = kHeaderSize;
    bool joysticks = false, switches = false, safety = false, potentiometers = false;
    while (offset < packet.size()) {
        if (offset + 2 > packet.size()) return false;
        const uint8_t type = packet[offset++], length = packet[offset++];
        if (offset + length > packet.size()) return false;
        const uint8_t *value = packet.data() + offset;
        if (type == 0x01) {
            if (safety || length != 1 || (value[0] & 0xfc)) return false;
            result.safety = value[0]; safety = true;
        } else if (type == 0x02) {
            if (joysticks || length != 9 || value[0] != 0x0f) return false;
            for (size_t i = 0; i < 4; ++i) {
                result.joystick_axes[i] = static_cast<int16_t>(get16(value + 1 + 2 * i));
            }
            joysticks = true;
        } else if (type == 0x03) {
            if (potentiometers || length < 1 || length != 1 + 2 * std::popcount(value[0]) ||
                (value[0] & 0xf0)) return false;
            result.potentiometer_present = value[0];
            size_t input_offset = 1;
            for (size_t i = 0; i < result.potentiometers.size(); ++i) {
                if (value[0] & (1U << i)) {
                    result.potentiometers[i] = get16(value + input_offset);
                    input_offset += 2;
                }
            }
            potentiometers = true;
        } else if (type == 0x04) {
            if (switches || length != 4 || get16(value) != 0x01ff || (get16(value + 2) & ~0x01ff)) return false;
            result.switches = get16(value + 2); switches = true;
        }
        offset += length;
    }
    if (!joysticks || !switches || !safety) return false;
    out = result;
    return true;
}

inline bool parseTelemetry(std::span<const uint8_t> packet, Telemetry &out) {
    if (!validHeader(packet, PacketType::Telemetry)) return false;
    Telemetry result{};
    result.source_id = packet[5]; result.destination_id = packet[6]; result.sequence = get32(packet.data() + 7);
    size_t offset = kHeaderSize;
    bool battery = false, encoders = false, motors = false, applied = false, rssi = false;
    while (offset < packet.size()) {
        if (offset + 2 > packet.size()) return false;
        const uint8_t type = packet[offset++], length = packet[offset++];
        if (offset + length > packet.size()) return false;
        const uint8_t *value = packet.data() + offset;
        switch (type) {
        case 0x20: if (battery || length != 2) return false; result.battery_mv = get16(value); battery = true; break;
        case 0x21: if (encoders || length != 8) return false; result.left_encoder = static_cast<int32_t>(get32(value)); result.right_encoder = static_cast<int32_t>(get32(value + 4)); encoders = true; break;
        case 0x22: if (motors || length != 2) return false; result.motor_status = get16(value); motors = true; break;
        case 0x23: if (applied || length != 4) return false; result.applied_control_sequence = get32(value); applied = true; break;
        case 0x24: if (rssi || length != 1) return false; result.rssi_dbm = static_cast<int8_t>(value[0]); rssi = true; break;
        default: break;
        }
        offset += length;
    }
    if (!battery || !encoders || !motors || !applied || !rssi) return false;
    out = result;
    return true;
}

} // namespace sr_protocol
