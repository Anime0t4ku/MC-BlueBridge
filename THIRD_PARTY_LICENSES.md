# Third-party software

## libxsm3

MC BlueBridge uses libxsm3 for Xbox 360 console authentication.

- Source: https://github.com/InvoxiPlayGames/libxsm3
- License: LGPL-2.1-or-later
- Copyright: InvoxiPlayGames and contributors

libxsm3 includes ExCrypt components under the BSD-3-Clause license. The dependency is fetched from its upstream repository when building MC BlueBridge. Upstream source and license terms remain available from the source URL above.

## DualSense USB descriptors

The DualSense output mode uses the USB HID report descriptor and firmware-info feature report of a genuine DualSense controller, as published in joypad-os, which took them from DS5Dongle. The mode's design follows joypad-os's DualSense mode.

- Source: https://github.com/joypad-ai/joypad-os (`src/usb/usbd/descriptors/dualsense_descriptors.h`, SPDX-License-Identifier: MIT; `src/usb/usbd/modes/dualsense_mode.c`, Apache-2.0)
- Original capture: https://github.com/awalol/DS5Dongle
- Copyright: the joypad-ai project and contributors; awalol and contributors
