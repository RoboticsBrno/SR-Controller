# SR Robot Protocol

Version: 1
Status: implementation design

This document specifies a binary protocol carried by `SimpleRadio.sendBlob()`.
It sends controller state to a robot and robot telemetry back to the
controller. It is intended for small, frequent messages: wheel control,
potentiometers, buttons, motor state, and link diagnostics.

## Design principles

- Every message carries the complete current input state. Losing one message
  therefore does not lose an event; the next state replaces it.
- Both directions use the same envelope. Packet type and source/destination
  identifiers determine direction, rather than separate wire formats.
- New data is added as TLV entries. Older firmware safely skips unknown
  entries.
- Every message is authenticated with a pre-shared key (PSK). The PSK is never
  placed in a packet.
- The robot makes safety decisions locally: it stops motors when valid control
  is lost, even if the controller still displays an old state.

SimpleRadio already separates blobs from its other message types and ESP-NOW
checks frame integrity. The protocol therefore adds no extra checksum. HMAC
protects message origin and contents; it does not encrypt traffic or prevent
radio jamming.

## Numeric conventions

Multi-byte numbers are little-endian. Unsigned integers are `u8`, `u16`, and
`u32`; signed integers use two's complement (`i8`, `i32`). Bits are numbered
from the least-significant bit, so bit 0 has mask `0x01`. A nonce is not a
number: it is exactly eight random bytes.

## Packet envelope

Every packet has the following 28-byte header, followed by a TLV payload and
a 16-byte authentication tag.

| Bytes | Field | Description |
|---:|---|---|
| 0-1 | magic | ASCII `SR` (`0x53`, `0x52`) |
| 2 | version | Protocol version; `0x01` in version 1 |
| 3 | packet_type | Packet type |
| 4 | packet_flags | Generic packet flags |
| 5 | source_id | ID of the sending device |
| 6 | destination_id | Target device ID; `0` means broadcast |
| 7 | key_id | Active PSK identifier; initially `0` |
| 8-15 | sender_nonce | Sender's current random session nonce (`byte[8]`) |
| 16-23 | recipient_nonce | Recipient's current nonce (`byte[8]`) |
| 24-27 | sequence | Sequence number within that session (`u32`) |
| 28...n-17 | payload | Sequence of TLV entries |
| n-16...n-1 | auth_tag | First 16 bytes of HMAC-SHA-256 |

Calculate `auth_tag` as `HMAC-SHA-256(PSK[key_id], packet[0..n-17])[:16]`.
The receiver compares the tag in constant time. The minimum packet size is 44
bytes: header plus tag, with no payload.

### Packet type (`packet_type`)

| Value | Name | Use |
|---:|---|---|
| `0x01` | `HELLO` | Announces a new session and nonce |
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

## Sessions, nonces, and replay protection

Each device generates a cryptographically random `sender_nonce` using the
ESP32 hardware random-number generator at boot and whenever it restores a
session. The nonce is eight opaque bytes and is compared byte-for-byte, not as
a little-endian integer. Each device periodically broadcasts an authenticated `HELLO` (for
example, once per second). A `HELLO` has a zero `recipient_nonce`; its
`sender_nonce` is the new session value.

The receiver stores a verified peer nonce, but a `HELLO` must never directly
enable motor movement. A normal packet is accepted only when all conditions
below hold:

1. `destination_id` equals the local ID or is broadcast.
2. `key_id` names a configured key and the HMAC is valid.
3. `recipient_nonce` equals the current local nonce.
4. `sender_nonce` equals the most recently discovered session of that source.
5. `sequence` is newer than the last accepted number for that source and nonce.

After a new `sender_nonce`, sequence numbering begins at zero. Compare sequence
numbers with the modular check `int32_t(new - last) > 0`. The recipient nonce
prevents replay of a packet from a previous session after a robot reboot; the
sequence number prevents repeating a packet during an active session.

A captured `HELLO` can at most temporarily affect link availability. It cannot
authorize movement or bypass HMAC; a current periodic `HELLO` restores the
session. A PSK cannot prevent radio jamming or flooding with replayed valid
frames, which is why the local motor timeout remains mandatory.

## TLV format

The payload is a sequence of entries:

| Entry byte | Field |
|---:|---|
| 0 | `type` |
| 1 | `length` |
| 2 through `length + 1` | `value` |

Before reading an entry, the parser must verify that the complete `value` lies
within the payload. Unknown types are skipped by exactly `length`; a known type
with an invalid length rejects the whole packet. Types `0x01-0x3f` are core,
`0x40-0x7f` are optional, and `0x80-0xff` are local extensions.

## Control payload (`CONTROL`)

### `0x01` — potentiometers

The value starts with `present_mask:u8`, followed by one `u16` for each set
bit, in ascending channel order. Values are normalized to `0..65535`; the
robot maps them to throttle, steering, or another function through its own
control profile. With four potentiometers, the entry length is 9 bytes.

| Bit | Mask | Channel |
|---:|---:|---|
| 0 | `0x01` | Potentiometer 1 |
| 1 | `0x02` | Potentiometer 2 |
| 2 | `0x04` | Potentiometer 3 |
| 3 | `0x08` | Potentiometer 4 |
| 4-7 | `0xf0` | Reserved |

### `0x02` — switches

The value is `present_mask:u16`, followed by `pressed_mask:u16`. A set bit in
`pressed_mask` means pressed; a clear bit means released. Switches are state,
not edge, values, so a lost packet cannot lose a press.

| Bit | Mask | Input |
|---:|---:|---|
| 0-8 | `0x0001` through `0x0100` | Button 1 through Button 9 |
| 9-15 | `0xfe00` | Reserved |

### `0x03` — safety state

The value length is one byte.

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | `0x01` | Deadman is held; movement is permitted |
| 1 | `0x02` | Emergency-stop request |
| 2-7 | `0xfc` | Reserved |

The robot may drive wheels only with a fresh, valid `CONTROL` packet and the
deadman bit set. It latches an emergency stop locally; clearing it must require
a safe local procedure or a separately designed, explicit authenticated command.

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

## Recommended timing and safety behavior

- The controller sends complete `CONTROL` state at about 50 Hz.
- The robot sends `TELEMETRY` at about 10 Hz, and may immediately send an
  additional packet when a fault changes.
- The robot stops motors after 150 ms without a newer valid `CONTROL` packet.
- Telemetry is informative. The controller must not assume movement continues
  until it receives acknowledgement in `0x23` and motor state.
- Use a separate random 32-byte PSK for every robot where possible. Never put
  keys in the repository or logs; production devices should use protected
  storage.
- A `SimpleRadio` group is not a security boundary. Correct `destination_id`,
  HMAC, nonces, and timeout are required even when groups are used.

## Size example

A control packet carrying four potentiometers, nine buttons, and the safety
state uses 28 header bytes, an 11-byte potentiometer TLV (type, length, and
9-byte value), a 6-byte switch TLV, a 3-byte safety TLV, and a 16-byte tag:
64 bytes total. This is far below the
SimpleRadio blob payload limit of 1490 bytes.

## TypeScript compatibility

The protocol contains no `u64`, because a JavaScript `number` exactly
represents integers only through `2^53 - 1`. An implementation uses `Uint8Array`
for the complete packet and `DataView` with its little-endian argument set to
`true` for `getUint16`, `getUint32`, `setUint16`, and `setUint32`. A nonce is an
8-byte `Uint8Array`, never converted to a `number` or `bigint`. HMAC consumes
and produces bytes, so both Web Crypto and Node `crypto` can operate directly
on the packet envelope. The `u32` sequence number is in TypeScript's safe
`number` range; modular comparison is `((next - last) | 0) > 0`.
