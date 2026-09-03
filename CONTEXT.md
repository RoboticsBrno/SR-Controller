# SR Controller

The SR Controller is a handheld transmitter that sends complete controller state snapshots to a selected robot and optionally displays returned diagnostics.

## Language

**SimpleRadio group**:
A logical 8-bit identifier carried in SimpleRadio packets and used to isolate robot/controller pairs sharing a Wi-Fi channel.
_Avoid_: Frequency, radio channel

**Wi-Fi channel**:
The physical 2.4 GHz channel used by ESP-NOW; communicating devices must use the same value.
_Avoid_: Group

**Controller snapshot**:
The complete current state of all joystick axes, buttons, and safety controls represented by one CONTROL packet.
_Avoid_: Button event, command delta

**Startup interlock**:
The safety state that prevents movement permission after startup until the operator deliberately passes the safety selector through SAFE.
_Avoid_: Startup delay

**SAFE**:
The safety-selector position in which communication continues but movement permission and emergency stop are both inactive.

**ARMED**:
The safety-selector position in which movement permission is active after the startup interlock has cleared.
_Avoid_: On

**Emergency stop**:
The safety-selector state requesting that the robot latch an emergency stop.
_Avoid_: Controller off
