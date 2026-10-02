# MC BlueBridge

MC BlueBridge is a Raspberry Pi Pico 2 W Bluetooth controller bridge and customization platform designed primarily for MiSTer.

Bluetooth controllers connect to a Pico 2 W and are translated into a normalized BlueBridge controller state before profiles, mappings, and other behavior are applied. The normalized state is then emitted through the selected USB output driver. The supported output modes are MiSTer, X-Input, Generic HID, and Nintendo Switch.

## Architecture

- Raspberry Pi Pico 2 W only
- BTstack for Bluetooth Classic and Bluetooth Low Energy transport
- BlueBridge-native controller identification and input parsing
- Generic HID descriptor parsing for standards-compliant Bluetooth gamepads
- Dedicated controller protocol handling where generic HID is not sufficient
- Bluetooth Classic HID support
- Bluetooth Low Energy HID-over-GATT support
- Nintendo custom BLE support for newer Nintendo controllers
- One active Bluetooth controller at a time
- Independent USB output drivers for MiSTer, X-Input, Generic HID, and Nintendo Switch
- USB CDC management channel for Companion Remote and the BlueBridge configuration website
- Persistent controller-based profile configuration
- Remembered controllers matched by Bluetooth identity with native VID/PID metadata when available
- Multiple profiles per controller with button remapping, duplication, deadzones, axis inversion, turbo, and macros
- Separate MiSTer controller mapping per profile by default
- Optional shared MiSTer mapping between profiles of the same controller
- Configuration export/import through the CDC protocol
- Onboard LED status indication
- No Wi-Fi, DHCP, DNS, or Pico-hosted web interface

## Controller support

BlueBridge uses its own controller layer instead of relying on a third-party controller abstraction library. Standard HID controllers use the generic HID path where possible, while controllers with proprietary initialization or report formats use dedicated BlueBridge handling.

The controller layer is designed to cover major Bluetooth controller families including Sony, Nintendo, Microsoft/Xbox, 8BitDo, Steam, Stadia, SteelSeries, OUYA, Atari, iCade, Android-style gamepads, and other standards-compliant HID controllers.

Nintendo support includes the Switch Pro Controller, Joy-Con controllers, the Nintendo Switch Online retro-controller family, the Nintendo Switch 2 Pro Controller, and the Nintendo GameCube Controller for Nintendo Switch Online. Older Switch-era controllers and newer Nintendo BLE controllers are handled as separate protocol families.

Compatibility remains hardware-dependent, and individual models, firmware revisions, and operating modes may require controller-specific adjustments.

## Controller profiles

Each remembered controller has its own set of profiles. New profiles use a separate MiSTer controller identity by default so mappings configured on MiSTer can remain profile-specific.

Profiles can optionally share the same MiSTer mapping when the user wants multiple BlueBridge profiles to use one MiSTer controller configuration. Profiles can also be duplicated and tuned independently with stick and trigger deadzones, axis inversion, turbo, and button-chord macros.

The controller's native VID/PID and Bluetooth identity are used internally by BlueBridge for identification and compatibility handling. BlueBridge presents its own USB identity to MiSTer.

## LED states

- Solid: controller connected
- Slow blink: waiting for a controller
- Fast blink: explicit pairing mode
- Double blink: profile changed
- Rapid blink: error or recovery state

## MiSTer default mapping

MC BlueBridge normalizes supported controllers into one positional layout before profiles are applied: South, East, West, North, L1, R1, L2, R2, Select, Start, L3, R3 and Home. This keeps the default MiSTer behavior consistent across PlayStation, Xbox, Nintendo and other controller families while still allowing every profile to override the mapping.

BlueBridge does not automatically create or overwrite MiSTer controller mappings. Each separate profile identity remains user-configurable inside MiSTer, including menu navigation, confirm/back behavior, and core-specific mappings. Profiles that share a MiSTer identity reuse the same MiSTer-side mapping.

## Build

Initialize the Pico SDK and its submodules, then build for Pico 2 W:

```bash
export PICO_SDK_PATH="$HOME/pico/pico-sdk"
cd "$PICO_SDK_PATH"
git submodule update --init lib/btstack
cd /path/to/MC-BlueBridge
rm -rf build && mkdir build && cd build
cmake -G Ninja -DPICO_BOARD=pico2_w ..
ninja
```

A separate BTstack checkout can be supplied with `BTSTACK_ROOT` when needed. The Pico SDK-pinned BTstack version is recommended.

## USB output modes

MC BlueBridge exposes four output modes:

- MiSTer: BlueBridge's native MiSTer-focused HID identity with per-profile MiSTer identity support
- X-Input: Xbox 360-style USB controller output with analog triggers and rumble feedback
- Generic HID: standards-based USB HID gamepad output
- Nintendo Switch: wired USB HID controller output for Nintendo Switch and Nintendo Switch 2

Output selection is persistent and changing modes re-enumerates the USB device with the appropriate identity and protocol. Controller input is normalized before output translation, so controller-specific report layouts do not leak into the USB output drivers. Analog input triggers are preserved for outputs that support them and converted by the output driver when the target protocol uses digital trigger semantics.

Each output mode includes a CDC management interface alongside its controller interface. Availability on the host depends on USB driver binding.

## Development and compatibility

Controller compatibility and output behavior are validated through hardware testing. Support can vary by controller model, firmware revision, operating mode, and host platform.

Motion sensors, touchpads, adaptive triggers, and controller-specific lighting are areas for further development.

## Acknowledgments

[JoypadOS](https://github.com/joypad-ai/joypad-os) was consulted as a reference when I encountered controller compatibility and Bluetooth and USB protocol issues that the project had already solved. MC BlueBridge has its own implementation; JoypadOS helped me understand the expected behavior and troubleshoot these issues.

Thank you to the JoypadOS contributors for making their work publicly available.

## License and third-party components

MC BlueBridge project code is licensed under the GNU General Public License v3.0 (GPL-3.0).

MC BlueBridge also uses third-party components that remain subject to their own licenses:

- BTstack is used as the Bluetooth transport. Raspberry Pi provides supplemental BTstack licensing for use with supported Raspberry Pi Pico wireless hardware, including Pico 2 W. The applicable upstream and Raspberry Pi license terms must be preserved when redistributing builds that include BTstack.
- Raspberry Pi Pico SDK components remain subject to their respective upstream licenses.
- TinyUSB and any other bundled or linked third-party components remain subject to their respective upstream licenses.

The GPL-3.0 license applies to MC BlueBridge's own project code and does not replace or remove the license terms of third-party components. When redistributing MC BlueBridge source or binaries, retain the license notices required by those dependencies.
