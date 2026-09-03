# SR Robot Protocol Without Security

Version: 1
Status: implementation design

This document specifies a simple bidirectional binary protocol for
`SimpleRadio.sendBlob()`. The controller sends input state to the robot and the
robot returns telemetry. This variant deliberately contains no PSK, HMAC,
encryption, or nonce handling. It is appropriate only for development,
teaching, or an isolated radio environment.

> Any participant in the same SimpleRadio group can create a packet that the
> robot accepts as control. `source_id` and `destination_id` are addressing,
> not protection. The robot must always have a local timeout and emergency stop.

## Principles

- Every `CONTROL` packet carries the complete current state. A lost packet is
  therefore replaced by the next packet.
- The same envelope serves controller and robot; packet type determines the
  direction.
- Extensions are TLV entries. Older firmware skips an unknown type using its
  length.
- SimpleRadio/ESP-NOW already checks transmitted-frame integrity, so this
  variant adds no checksum.
- The robot stops motors itself when it stops receiving fresh valid control.

Multi-byte integers are little-endian. Bits are numbered from the
least-significant bit; bit 0 has mask `0x01`.

## Packet envelope

The header is 11 bytes. It is followed by a TLV payload through the end of the
blob.

| Bytes | Field | Description |
|---:|---|---|
| 0-1 | magic | ASCII `SR` (`0x53`, `0x52`) |
| 2 | version | Protocol version; `0x01` in v1 |
| 3 | packet_type | Packet type |
| 4 | packet_flags | Generic flags |
| 5 | source_id | Sending device ID |
| 6 | destination_id | Target device ID; `0` means broadcast |
| 7-10 | sequence | Sequence number (`u32`) |
| 11...n-1 | payload | Sequence of TLV entries |

The minimum packet size is 11 bytes. Reject an unknown major version, invalid
magic value, or a packet shorter than the header.

### Packet type (`packet_type`)

| Value | Name | Use |
|---:|---|---|
| `0x01` | `HELLO` | Optional device-presence announcement |
| `0x02` | `CONTROL` | Controller -> robot control state |
| `0x03` | `TELEMETRY` | Robot -> controller measurements and state |
| `0x04` | `RESPONSE` | Optional response to a diagnostic request |
| `0x05-0x7f` | reserved | Future core types |
| `0x80-0xff` | extension | Application/vendor-defined types |

### Packet flags (`packet_flags`, byte 4)

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | `0x01` | Sender requests a response |
| 1 | `0x02` | Packet is a response |
| 2 | `0x04` | Sender reports an error or fault |
| 3-7 | `0xf8` | Reserved; must be zero in v1 |

## Packet ordering

Each sender increments `sequence` for every packet. The receiver tracks the
last number for each `source_id` and accepts only a newer value:
`int32_t(new - last) > 0`. This discards delayed duplicates during an active
link.

After 150 ms without a newer valid `CONTROL` packet, the robot stops motors
and clears the controller's stored sequence number. The next valid `CONTROL`
can then restart the link even after a controller reboot whose counter starts
at zero. This is not replay protection: after timeout, an attacker can replay
an old packet. Do not use this variant where untrusted people can access radio.

`HELLO` is for presence display and diagnostics; it must never affect motors.

## TLV format

The payload is a sequence of entries:

| Entry byte | Field |
|---:|---|
| 0 | `type` |
| 1 | `length` |
| 2 through `length + 1` | `value` |

Before reading an entry, the parser verifies that its complete `value` is in
the payload. It skips unknown types by exactly `length` bytes; a known type
with invalid length rejects the entire packet. Types `0x01-0x3f` are core,
`0x40-0x7f` are optional, and `0x80-0xff` are local extensions.

## Control payload (`CONTROL`)

The current profile requires safety state, joystick axes, and switches.
Potentiometers are optional and the current sender does not transmit them.
With four joystick axes, the packet is 31 bytes.

### `0x01` — safety state

The value is one byte containing `safety_flags:u8`.

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | `0x01` | Deadman is held; movement is permitted |
| 1 | `0x02` | Emergency-stop request |
| 2-7 | `0xfc` | Reserved |

The robot may drive wheels only with a fresh valid `CONTROL` packet and the
deadman bit set. It latches emergency stop locally; clearing it must require a
safe local procedure or a separate explicit command.

### `0x02` — joystick axes

The value is `present_mask:u8` followed by one little-endian `i16` for each
set bit in ascending channel order. The range is `-32768..32767`, with center
at `0`. Bits 0-3 identify axes 1-4; bits 4-7 are reserved. The example uses
mask `0x0f`, giving a value length of 9 bytes.

### `0x03` — potentiometers (optional)

The value is `present_mask:u8` followed by one little-endian `u16` in the
range `0..65535` for each set bit. Only values for set bits are present, in
ascending channel order, so the length is `1 + 2 × popcount(present_mask)`.

### `0x04` — switches

The value is `present_mask:u16`, followed by `pressed_mask:u16`. A set bit in
`pressed_mask` means pressed and must also be set in `present_mask`. Switches
are level state rather than events.

| Bit | Mask | Input |
|---:|---:|---|
| 0-8 | `0x0001` through `0x0100` | Button 1 through Button 9 |
| 9-15 | `0xfe00` | Reserved |

## Telemetry payload (`TELEMETRY`)

| TLV | Length | Value |
|---:|---:|---|
| `0x20` | 2 | Battery voltage in mV (`u16`) |
| `0x21` | 8 | Encoder positions: left `i32`, right `i32` |
| `0x22` | 2 | Motor status (`u16`) |
| `0x23` | 4 | Last applied `CONTROL.sequence` (`u32`) |
| `0x24` | 1 | RSSI of the last control packet in dBm (`i8`) |

### Motor status (`0x22`)

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | `0x0001` | Motors are armed |
| 1 | `0x0002` | Control timeout / failsafe active |
| 2 | `0x0004` | Emergency stop is latched |
| 3 | `0x0008` | Motor-driver fault |
| 4 | `0x0010` | Over-current fault |
| 5 | `0x0020` | Under-voltage fault |
| 6-15 | `0xffc0` | Reserved |

## Recommended timing

- The controller sends complete `CONTROL` state at about 50 Hz.
- The robot sends `TELEMETRY` at about 10 Hz and immediately when a fault
  changes.
- The robot stops wheels after 150 ms without a newer valid `CONTROL` packet.
- The controller should use TLV `0x23` and motor status to display which
  command the robot actually applied.

## Size example

A current-profile control packet has an 11-byte header, a 3-byte safety TLV,
an 11-byte four-axis joystick TLV, and a 6-byte switch TLV: 31 bytes total.
The current sender omits the optional potentiometer TLV. The packet is far
below the SimpleRadio 1490-byte blob payload limit.

## TypeScript compatibility

Every numeric value is at most `u32`, so it fits exactly in a TypeScript
`number`. Use `Uint8Array` for the packet and `DataView` with the
little-endian argument set to `true` for `getUint16`, `getUint32`, `setUint16`,
and `setUint32`. The modular sequence comparison is
`((next - last) | 0) > 0`.
