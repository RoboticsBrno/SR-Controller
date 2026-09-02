#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sr_protocol {

// All packets begin with this fixed, little-endian wire header:
// "SR", version, type, flags, source ID, destination ID, and sequence number.
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 11;
constexpr size_t kControlPacketSize = 31;
constexpr size_t kTelemetryPacketSize = 38;

enum class PacketType : uint8_t {
    Hello = 1,
    Control = 2,
    Telemetry = 3,
    Response = 4
};

struct Control {
    uint8_t source_id;
    uint8_t destination_id;
    uint32_t sequence;
    std::array<uint16_t, 4> pots;
    uint16_t switches;
    uint8_t safety;
};

struct Telemetry {
    uint8_t source_id;
    uint8_t destination_id;
    uint32_t sequence;
    uint16_t battery_mv;
    int32_t left_encoder;
    int32_t right_encoder;
    uint16_t motor_status;
    uint32_t applied_control_sequence;
    int8_t rssi_dbm;
};

// Encode explicitly instead of using packed structs, whose layout is not
// portable.
inline void put16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

inline void put32(uint8_t *out, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
        out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline uint16_t get16(const uint8_t *in) {
    return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8);
}

inline uint32_t get32(const uint8_t *in) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(in[i]) << (8 * i);
    return value;
}

inline void writeHeader(uint8_t *out, PacketType type, uint8_t source,
                        uint8_t destination, uint32_t sequence) {
    out[0] = 'S';
    out[1] = 'R';
    out[2] = kVersion;
    out[3] = static_cast<uint8_t>(type);
    out[4] = 0;
    out[5] = source;
    out[6] = destination;
    put32(out + 7, sequence);
}

inline bool validHeader(std::span<const uint8_t> packet, PacketType type) {
    return packet.size() >= kHeaderSize && packet[0] == 'S' &&
           packet[1] == 'R' && packet[2] == kVersion &&
           packet[3] == static_cast<uint8_t>(type) && (packet[4] & 0xf8) == 0 &&
           packet[5] != 0;
}

inline std::array<uint8_t, kControlPacketSize>
makeControl(const Control &control) {
    std::array<uint8_t, kControlPacketSize> packet{};
    writeHeader(packet.data(), PacketType::Control, control.source_id,
                control.destination_id, control.sequence);
    size_t offset = kHeaderSize;
    // TLV 0x01: all four potentiometers, normalized to 0..65535.
    packet[offset++] = 0x01;
    packet[offset++] = 9;
    packet[offset++] = 0x0f;
    for (uint16_t pot : control.pots) {
        put16(packet.data() + offset, pot);
        offset += 2;
    }
    // TLV 0x02: present and pressed bitmasks for nine switches.
    packet[offset++] = 0x02;
    packet[offset++] = 4;
    put16(packet.data() + offset, 0x01ff);
    offset += 2;
    put16(packet.data() + offset,
          static_cast<uint16_t>(control.switches & 0x01ff));
    offset += 2;
    // TLV 0x03: deadman and emergency-stop state bits.
    packet[offset++] = 0x03;
    packet[offset++] = 1;
    packet[offset++] = static_cast<uint8_t>(control.safety & 0x03);
    return packet;
}

inline bool parseControl(std::span<const uint8_t> packet, Control &out) {
    if (!validHeader(packet, PacketType::Control))
        return false;
    Control result{packet[5], packet[6], get32(packet.data() + 7), {}, 0, 0};
    size_t offset = kHeaderSize;
    bool pots = false, switches = false, safety = false;
    while (offset < packet.size()) {
        // Each entry is type, length, value. Bounds checks make malformed blobs
        // safe.
        if (offset + 2 > packet.size())
            return false;
        const uint8_t type = packet[offset++], length = packet[offset++];
        if (offset + length > packet.size())
            return false;
        const uint8_t *value = packet.data() + offset;
        if (type == 0x01) {
            if (pots || length != 9 || value[0] != 0x0f)
                return false;
            for (size_t i = 0; i < result.pots.size(); ++i)
                result.pots[i] = get16(value + 1 + 2 * i);
            pots = true;
        } else if (type == 0x02) {
            if (switches || length != 4 || get16(value) != 0x01ff ||
                (get16(value + 2) & ~0x01ff))
                return false;
            result.switches = get16(value + 2);
            switches = true;
        } else if (type == 0x03) {
            if (safety || length != 1 || (value[0] & 0xfc))
                return false;
            result.safety = value[0];
            safety = true;
        }
        // Unknown TLVs are deliberately skipped, preserving protocol
        // extensibility.
        offset += length;
    }
    if (!pots || !switches || !safety)
        return false;
    out = result;
    return true;
}

inline std::array<uint8_t, kTelemetryPacketSize>
makeTelemetry(const Telemetry &telemetry) {
    std::array<uint8_t, kTelemetryPacketSize> packet{};
    writeHeader(packet.data(), PacketType::Telemetry, telemetry.source_id,
                telemetry.destination_id, telemetry.sequence);
    size_t offset = kHeaderSize;
    // Standard telemetry TLVs: battery, encoders, motor state, applied control,
    // RSSI.
    packet[offset++] = 0x20;
    packet[offset++] = 2;
    put16(packet.data() + offset, telemetry.battery_mv);
    offset += 2;
    packet[offset++] = 0x21;
    packet[offset++] = 8;
    put32(packet.data() + offset,
          static_cast<uint32_t>(telemetry.left_encoder));
    offset += 4;
    put32(packet.data() + offset,
          static_cast<uint32_t>(telemetry.right_encoder));
    offset += 4;
    packet[offset++] = 0x22;
    packet[offset++] = 2;
    put16(packet.data() + offset, telemetry.motor_status);
    offset += 2;
    packet[offset++] = 0x23;
    packet[offset++] = 4;
    put32(packet.data() + offset, telemetry.applied_control_sequence);
    offset += 4;
    packet[offset++] = 0x24;
    packet[offset++] = 1;
    packet[offset++] = static_cast<uint8_t>(telemetry.rssi_dbm);
    return packet;
}

inline bool parseTelemetry(std::span<const uint8_t> packet, Telemetry &out) {
    if (!validHeader(packet, PacketType::Telemetry))
        return false;
    Telemetry result{packet[5], packet[6], get32(packet.data() + 7), 0, 0, 0, 0,
                     0,         0};
    size_t offset = kHeaderSize;
    bool battery = false, encoders = false, motors = false, applied = false,
         rssi = false;
    while (offset < packet.size()) {
        if (offset + 2 > packet.size())
            return false;
        const uint8_t type = packet[offset++], length = packet[offset++];
        if (offset + length > packet.size())
            return false;
        const uint8_t *value = packet.data() + offset;
        if (type == 0x20) {
            if (battery || length != 2)
                return false;
            result.battery_mv = get16(value);
            battery = true;
        } else if (type == 0x21) {
            if (encoders || length != 8)
                return false;
            result.left_encoder = static_cast<int32_t>(get32(value));
            result.right_encoder = static_cast<int32_t>(get32(value + 4));
            encoders = true;
        } else if (type == 0x22) {
            if (motors || length != 2)
                return false;
            result.motor_status = get16(value);
            motors = true;
        } else if (type == 0x23) {
            if (applied || length != 4)
                return false;
            result.applied_control_sequence = get32(value);
            applied = true;
        } else if (type == 0x24) {
            if (rssi || length != 1)
                return false;
            result.rssi_dbm = static_cast<int8_t>(value[0]);
            rssi = true;
        }
        offset += length;
    }
    if (!battery || !encoders || !motors || !applied || !rssi)
        return false;
    out = result;
    return true;
}

inline void selfCheck() {
    // Small host- and device-runnable round trip check for the control codec.
    const Control input{1, 2, 7, {1, 2, 3, 4}, 0x0101, 0x01};
    Control output{};
    const auto packet = makeControl(input);
    assert(packet.size() == kControlPacketSize && parseControl(packet, output));
    assert(output.sequence == input.sequence && output.pots == input.pots &&
           output.switches == input.switches);
}

} // namespace sr_protocol
