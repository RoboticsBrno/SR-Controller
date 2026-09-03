# SR protocol examples

`sr-protocol-sender` is a controller-side ESP-IDF project. It sends a neutral
four-axis joystick control state at 50 Hz and logs robot telemetry. Its
31-byte CONTROL packet omits the optional potentiometer TLV.

`sr-protocol-receiver` is a robot-side ESP-IDF project. It validates and logs
control packets, reports simulated telemetry at 10 Hz, and reports failsafe
after 150 ms without a fresh control packet. It intentionally contains no
motor-driver code.

Both use SimpleRadio group `0`; the sender ID is `1` and receiver ID is `2`.
Build and flash each project independently after exporting ESP-IDF:

```sh
idf.py -C examples/sr-protocol-sender set-target esp32
idf.py -C examples/sr-protocol-sender flash monitor

idf.py -C examples/sr-protocol-receiver set-target esp32
idf.py -C examples/sr-protocol-receiver flash monitor
```

The shared `sr-protocol-common/protocol.hpp` encodes the unsecured protocol.
