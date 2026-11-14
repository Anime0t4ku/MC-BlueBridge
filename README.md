# MC BlueBridge

MC BlueBridge turns a **Raspberry Pi Pico 2 W** into a low-latency Bluetooth controller adapter designed primarily for **MiSTer**, with additional support for PC, Nintendo Switch and Nintendo Switch 2.

Controllers connect to BlueBridge over Bluetooth, while BlueBridge presents them to the host as a wired USB controller. Controller-specific Bluetooth protocols are handled internally, giving you one consistent system for pairing, profiles, remapping and customization.

MC BlueBridge is free and open source.

## Features

- Bluetooth Classic and Bluetooth Low Energy controller support
- Support for many PlayStation, Xbox, Nintendo, 8BitDo and standard HID controllers
- MiSTer, X-Input, Generic HID, Nintendo Switch, Xbox 360 console and DualSense USB output modes
- Single-controller and two-controller modes
- Remembered controllers with automatic reconnect
- Multiple profiles per controller
- Button remapping and duplication
- Stick and trigger deadzones
- Axis inversion
- Turbo
- Button-chord macros
- Per-profile MiSTer controller identities
- Optional shared MiSTer mappings between profiles
- Rumble where supported
- Controller blocking and management
- Configuration export and import
- Built-in diagnostics
- Persistent settings stored directly on the adapter

BlueBridge does **not use Wi-Fi on the Pico**. Bluetooth is dedicated to controllers, while management is handled over USB or, when connected to MiSTer, through the Companion Remote daemon.

## Requirements

MC BlueBridge requires:

- **Raspberry Pi Pico 2 W**
- USB data connection to the host
- Supported Bluetooth controller

Other Raspberry Pi Pico models are not supported.

Pre-built adapters and DIY installations use the same BlueBridge firmware.

## Managing BlueBridge

There are three main ways to configure BlueBridge.

### MiSTer Companion Desktop

**MiSTer Companion Desktop** provides the most complete BlueBridge management experience.

It can manage an adapter:

- Directly over USB
- Remotely through a MiSTer running Companion Remote
- Without requiring the adapter to be connected to a MiSTer

MiSTer Companion can manage controllers, profiles, mappings and adapter settings, view diagnostics, and **install or update BlueBridge firmware**.

### Companion Remote

**Companion Remote** is a daemon that runs on the MiSTer itself.

When BlueBridge is connected to the MiSTer, Companion Remote communicates with the adapter over USB and exposes BlueBridge over the network.

You can manage BlueBridge through Companion Remote in two ways:

- From **MiSTer Companion Desktop**
- Directly from a web browser on a phone, tablet or computer

Open:

**http://MISTERIP:9191**

Replace `MISTERIP` with the IP address of your MiSTer.

This allows BlueBridge to remain connected to the MiSTer while you configure it remotely from another device.

### BlueBridge website

BlueBridge can also be configured directly over USB using:

**https://bluebridge.mistercompanion.org**

The website communicates directly with the adapter through the browser and can be used without MiSTer Companion or Companion Remote.

It can manage controllers, profiles and adapter settings, but it **does not install or update BlueBridge firmware**.

Firmware updates should be performed using MiSTer Companion or by manually flashing a UF2 file.

## Pairing controllers

Connect BlueBridge to a supported USB host and start pairing from MiSTer Companion, Companion Remote or the BlueBridge website.

Put your controller into Bluetooth pairing mode and select **Start Pairing**.

Once paired, the controller is remembered and normally reconnects automatically when it is turned on again.

### Pairing with the BOOTSEL button

You can also start pairing directly from the adapter.

During normal operation:

1. Hold **BOOTSEL for about one second**.
2. Release it when the onboard LED starts blinking rapidly.
3. Put your controller into Bluetooth pairing mode.
4. BlueBridge stops pairing automatically after a controller connects.

This is especially useful when the adapter is connected to a Nintendo Switch or Switch 2.

Holding BOOTSEL while plugging the Pico into USB still enters the normal Raspberry Pi bootloader.

## Controller support

BlueBridge contains its own controller identification and input system.

Standard Bluetooth HID controllers use the generic HID implementation where possible, while controllers requiring special initialization, report formats or Bluetooth behavior use dedicated support.

Supported controller families include devices from:

- Sony / PlayStation
- Microsoft / Xbox
- Nintendo
- 8BitDo
- Valve / Steam
- Google Stadia
- SteelSeries
- Atari
- OUYA
- iCade
- Android-style HID controllers
- Other standards-compliant Bluetooth gamepads

Nintendo support includes Switch controllers, Joy-Con, Nintendo Switch Online retro controllers, newer Nintendo BLE controllers and the Switch 2 Pro Controller.

Compatibility can vary between controller models, firmware revisions and controller operating modes.

## Profiles

Every remembered controller can have multiple profiles.

A profile can contain its own:

- Button mapping
- Stick deadzones
- Trigger deadzones
- Axis inversion
- Turbo configuration
- Macros
- MiSTer controller identity

Profiles can be duplicated and adjusted independently.

By default, separate profiles receive separate MiSTer USB identities. This allows MiSTer to keep a different controller mapping for each profile.

Profiles can instead share one MiSTer identity when you want multiple BlueBridge profiles to use the same MiSTer-side mapping.

## USB output modes

BlueBridge supports six USB output modes.

### MiSTer

The default and recommended mode for MiSTer.

It supports BlueBridge's full profile system and separate MiSTer controller identities for different profiles.

### X-Input

Presents BlueBridge as an Xbox-style X-Input controller.

Useful for Windows and other software designed around Xbox controllers.

Analog triggers and rumble are supported where available.

### Generic HID

Presents BlueBridge as a standard USB HID gamepad.

Useful for hosts and applications that work best with a conventional USB controller.

### Nintendo Switch

Presents BlueBridge as a compatible wired controller for Nintendo Switch and Nintendo Switch 2.

The required wired controller / USB controller option must be enabled on the console.

### Xbox 360 (Console)

Presents BlueBridge as a wired Xbox 360 controller and uses XSM3 authentication for retail Xbox 360 consoles. This mode is separate from the normal X-Input mode used for PC compatibility.

The native Xbox 360 USB layout does not expose the BlueBridge management interface. Configure and pair the adapter before switching to this mode. To return to MiSTer mode without a management connection, hold **BOOTSEL for 5 seconds** during normal operation.

### DualSense

Presents BlueBridge as a wired DualSense (PlayStation 5) controller.

This is the recommended mode on **macOS**: macOS supports the DualSense natively over USB, so it works with games and apps that use Apple's Game Controller framework as well as with SDL-based apps such as Moonlight, and rumble is supported. It does not work on a PS5 console, which requires controller authentication.

Buttons follow BlueBridge's positional layout: South is Cross, East is Circle, West is Square, North is Triangle, Select is Create, Start is Options, Home is the PS button and Capture is the touchpad button. The motion sensors report a controller lying still, and the touchpad reports no touches.

Select the mode by its number (`6`) with the management command `BB1|1|OUTPUT_SET|6`. BlueBridge also exposes its management interface next to the controller in this mode, under the DualSense USB IDs (054C:0CE6). To return to MiSTer mode without a management connection, hold **BOOTSEL for 5 seconds** during normal operation.

Changing USB output mode is persistent and automatically re-enumerates BlueBridge with the appropriate USB identity.

## Nintendo Switch and Switch 2

BlueBridge works as a wired USB controller on both Nintendo Switch and Nintendo Switch 2.

However, **the BlueBridge management interface cannot be accessed while the adapter is connected directly to a Switch or Switch 2**.

When using BlueBridge on a Switch, you therefore need to either:

- Use a controller that is already paired with the adapter
- Start pairing manually using the BOOTSEL button

For configuration, profile changes or other management, connect the adapter to a computer or MiSTer and use MiSTer Companion, Companion Remote or the BlueBridge website.

## Multi-controller mode

BlueBridge can expose **two simultaneous controllers** through a single adapter.

Each connected controller keeps its own:

- Active profile
- Button mapping
- Analog settings
- Supported rumble behavior

MiSTer decides which controller belongs to each player. BlueBridge's internal connection slots are not player numbers.

Pair controllers one at a time. After one finishes pairing, start pairing again to add another.

Multi-controller mode has a hard limit of **two connected controllers**. A third connection is refused while both slots are occupied.

Because two Bluetooth controllers share one adapter, multi-controller mode may have slightly higher latency than single-controller mode.

Turbo and macros are available in single-controller mode. Their saved settings are preserved when switching between modes.

## MiSTer controller mapping

BlueBridge normalizes supported controllers into a common positional layout before profile settings are applied:

**South, East, West, North, L1, R1, L2, R2, Select, Start, L3, R3 and Home**

This gives controllers from different manufacturers a predictable starting point.

BlueBridge does not automatically create or overwrite MiSTer's controller mappings.

Menu navigation, confirm/back behavior and core-specific mappings remain configurable inside MiSTer.

Profiles using separate MiSTer identities can have separate MiSTer mappings, while profiles sharing an identity reuse the same mapping.

## LED status

The onboard Pico LED shows the current BlueBridge state:

- **Solid** — controller connected
- **Slow blink** — waiting for a remembered controller
- **Fast blink** — pairing mode
- **Double blink** — profile changed
- **Rapid blink** — error or recovery state

## Managing controllers

BlueBridge keeps remembered controllers and their profiles stored directly on the adapter.

From the management interface you can:

- View connected and remembered controllers
- Rename controllers
- Select the active profile
- Create, duplicate and remove profiles
- Change button mappings
- Configure analog settings
- Configure turbo and macros
- Disconnect a controller
- Remove a remembered controller
- Block or unblock Bluetooth devices
- Export and import configuration
- View diagnostics

Controller identity is tracked using Bluetooth information and native controller metadata where available, so two controllers with the same displayed name can remain separate devices.

## Firmware updates

BlueBridge firmware can be installed and updated through **MiSTer Companion Desktop**.

Firmware can also be flashed manually by placing the Pico 2 W into BOOTSEL mode and copying the BlueBridge UF2 file to it.

The direct USB BlueBridge website does **not** install or update firmware.

## Diagnostics

BlueBridge includes diagnostics intended to make controller compatibility and Bluetooth problems easier to troubleshoot.

Diagnostic information can be viewed or exported from supported BlueBridge management interfaces and can be useful when reporting:

- Pairing problems
- Reconnection problems
- Incorrect mappings
- Analog input problems
- Rumble issues
- Controller identification issues
- Bluetooth transport problems

## Building from source

MC BlueBridge uses the Raspberry Pi Pico SDK and its bundled BTstack version.

```bash
export PICO_SDK_PATH="$HOME/pico/pico-sdk"

cd "$PICO_SDK_PATH"
git submodule update --init lib/btstack

cd /path/to/MC-BlueBridge
rm -rf build
mkdir build
cd build

cmake -G Ninja -DPICO_BOARD=pico2_w ..
ninja
```

A separate BTstack checkout can be supplied through `BTSTACK_ROOT` when needed, although the Pico SDK-pinned version is recommended.

## Limitations

Controller support depends on the hardware, controller firmware and Bluetooth mode being used.

Some controller-specific features may not currently be available, including:

- Motion sensors
- Touchpads
- Adaptive triggers
- Controller-specific lighting

Rumble availability also depends on both the connected controller and the selected USB output mode.

Multi-controller mode is limited to two simultaneous controllers and may have slightly higher latency than single-controller mode.

Xbox 360 (Console) and DualSense output are available in single-controller mode only. The management interface is unavailable while the native Xbox 360 USB mode is active.

## Acknowledgments

[JoypadOS](https://github.com/joypad-ai/joypad-os) was consulted as a behavioral reference while troubleshooting controller compatibility, Bluetooth initialization, controller report formats and USB output behavior.

No JoypadOS source code is used in MC BlueBridge.

MC BlueBridge has its own controller handling, input pipeline, profile storage, management protocol and USB output implementation. JoypadOS was useful as a reference for understanding expected controller and protocol behavior when investigating compatibility issues.

Thank you to the JoypadOS contributors for making their work publicly available.

## License

MC BlueBridge project code is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

MC BlueBridge also uses third-party components including:

- Raspberry Pi Pico SDK
- BTstack
- TinyUSB
- libxsm3 (LGPL-2.1-or-later) for Xbox 360 XSM3 authentication
- ExCrypt components included by libxsm3 (BSD-3-Clause)

These components remain subject to their own upstream license terms. See `THIRD_PARTY_LICENSES.md` for the Xbox 360 authentication dependency information.

The GPL-3.0 license applies to MC BlueBridge's own project code and does not replace or override the licenses of third-party components.
