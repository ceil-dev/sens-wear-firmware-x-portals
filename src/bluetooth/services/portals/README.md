# Portals service

With `CONFIG_SENSWEAR_PORTALS`, an app that speaks portals connects to the device,
enters the env `senswear` and reads or sets the properties below. As with the
other services, no pairing is needed.

## Wire format

Service `7cbf0001-c9bd-463f-8789-e53d718f0ac1`:

| Characteristic | UUID | Properties |
| --- | --- | --- |
| `input` | `7cbf0002-c9bd-463f-8789-e53d718f0ac1` | Write Without Response (Write is taken too): packets from the app |
| `output` | `7cbf0003-c9bd-463f-8789-e53d718f0ac1` | Notify: packets from the device |

The app subscribes to `output` before it writes. A frame, a portals frame packed
as CBOR, travels as one or more packets of at most the ATT MTU less 3 bytes.
A packet's first byte holds its place in the frame (bits 0 to 6, from 0) and, in
bit 7, whether the frame ends with it; the frame's bytes follow. A frame takes at
most 128 packets. The device takes frames of up to 512 bytes and sends frames of
up to 2,048. A packet out of place drops the frame being joined. What the other
side has not confirmed is sent again after a second in which nothing arrived.

## Properties

| Property | Value | Access | Needs |
| --- | --- | --- | --- |
| `firmware` | text, as Firmware Revision String `2a26` | read | |
| `led.color` | `#rrggbb`; `#000000` is off | read, set | `CONFIG_SENSWEAR_LP5562_DRIVER` |
| `battery.level`, `battery.voltage` | % and mV; 0 until the fuel gauge first reports | read | `CONFIG_SENSWEAR_BQ27427_DRIVER` |
| `imu.streams`, `imu.period` | true or false; ms above 0 | read, set | `CONFIG_SENSWEAR_BHI360_DRIVER` |
| `haptic.vibrate` | takes `amplitude` 0 to 255 and `duration` 1 to 65535 ms | call | the haptic shield |
| `haptic.stop`, `haptic.active` | stops the motor; whether it vibrates now | call, read | the haptic shield |

The device does not announce changes: an app reads a value again to see it change.

## Limits

- One app at a time per Bluetooth connection: two apps sharing one can disturb each other.
- The LED color and the IMU's streams and period are also kept by the LED and IMU
  services. Set through one service, the other can read back a stale value.
- The service's six attributes move the handles of the services after it up by 6.
  An app that keeps handles from an earlier image discovers the services again.

## Validation

So far compile-only: every preset builds, and nothing has run on hardware. To
validate, flash the base preset, then:

1. With a Bluetooth scanner app such as nRF Connect, find "Sens Wear (OOB)": the
   service and its two characteristics are listed, and the other services still work.
2. With an app that speaks portals, enter `senswear`, read `firmware` and `battery`,
   and set `led.color` to `#ff0000`: the LED turns red.
3. On a haptic build, call `haptic.vibrate` with amplitude 128 and duration 500: the
   motor runs for half a second.
