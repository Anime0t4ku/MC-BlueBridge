# DS4 mouse and light-bar support

This contribution is based on Anime0t4ku/MC-BlueBridge main at e3896aa
(upstream firmware 1.1.0, configuration schema 13). It extracts the working DS4
features from the Soopahfly hardware fork. It adds no OLED/encoder, trackball,
Steam Controller, audio or Bluetooth reconnection changes.

## Mouse

Mouse output is off by default. Enable it in the Companion Remote BlueBridge
page, save, and allow the adapter to restart. It adds a relative USB HID mouse
alongside the gamepad in single-controller MiSTer output mode. Slide the DS4
touchpad to move and press it for left-click. The click is consumed by the mouse
in this mode. Controller Tester captures input and suppresses mouse output;
leave the tester and use a mouse-capable core to check movement.

Gyro mouse is independently off by default. Settings include sensitivity
1-200%, activation while touching the pad, while holding L2, or always; axis
inversion; a hold-to-pause button; and optional R2 mouse fire. Horizontal
inversion defaults on. The pause button defaults to L3; hold it while still
for at least half a second and 32 fresh samples to estimate resting gyro bias.
The chosen pause button and optional R2 fire are consumed from gamepad input
while gyro is enabled. Touchpad and gyro motion can contribute together.
Gyro uses nominal sensor scaling, not factory calibration feature reports.

Only complete, CRC-valid 78-byte Bluetooth reports feed the pointer/gyro.
Relative movement is retained until a successful USB send. Capture, disconnect
and stale input release mouse buttons. Mouse/gyro settings are boot-applied;
light-bar changes below apply live.

## Light bar

Nintendo defaults to P1 red/P2 green. Sega uses blue/orange. Custom allows RGB
colours. Switching themes changes P1/P2 and retains P3/P4. Per-controller
settings select Auto, a fixed P1-P4 preset, or custom RGB. Automatic selection
follows BlueBridge connection slots (two per adapter), not MiSTer-assigned player
numbers. P3/P4 are explicit presets for setups that need them. Factory P3/P4 are
green/pink. Upstream maintainers can choose different palette defaults.

Below 20% battery while discharging, the DS4 blinks its selected colour one
second on/one second off. Charging, full, unknown battery data, or recovery to
25% clears the warning. Rumble preserves colour and does not restart the blink
phase. A black custom colour remains unlit. Other controllers are unaffected.

## Integration and configuration

Capability flags: touch_mouse, ds4_gyro, ds4_lightbar. CDC commands:

- TOUCH_MOUSE_GET; TOUCH_MOUSE_SET|enabled
- DS4_AIM_GET; DS4_AIM_SET|enabled|sensitivity|activation|flags|pause_button|mouse_enabled
- LIGHTBAR_GET; LIGHTBAR_PALETTE_SET|theme|12_RGB_bytes
- LIGHTBAR_SET|controller_index|controller_id|player|custom_enabled|r|g|b

Activation: 0 always, 1 touchpad, 2 L2. Flags: 1 invert X, 2 invert Y,
4 R2 fire. Pause uses canonical button indexes 0-13 or 255 for none. Theme:
0 Custom, 1 Sega, 2 Nintendo; player: 0 Auto or 1-4 fixed preset. Inputs are
validated before saving. Controller IDs reject writes for stale selections.

Schema 14 appends only the mouse flag (1 byte), DS4 gyro settings (5 bytes)
and light-bar settings (53 bytes) to the exact upstream schema-13 prefix.
Schemas 8-13 load/import with defaults for the appended settings. Upstream
version numbering is intentionally unchanged for the maintainer to choose.
This schema 14 is not compatible with schema 14-20 from the independent OLED
fork. Do not flash this contribution over that fork or import its backups;
use an upstream adapter/configuration for testing.

The native Companion Remote web GUI integration is proposed separately:
https://github.com/Anime0t4ku/mister-companion/pull/173
It adds the mouse/gyro settings and light-bar controls directly to the bundled
web daemon. No installer is included in this firmware contribution. Both PRs
use the capability flags and CDC commands above, so the GUI remains compatible
with older adapters and the independent hardware fork. The desktop controller
panel is outside the Companion Remote web change.

## Validation

    python tests/run.py

CC selects a native C compiler. Tests cover packet CRC/bounds/history, contact
changes, click/queue edges and rejected sends; gyro gating, inversion, timing,
bias zeroing and fractional motion; colours, rumble and blink; persistence,
record reuse, upstream schema-13 byte preservation, import validation/reset;
and rejected imports that must not trigger a restart. Companion Remote API/UI
tests are included with the separate web GUI PR.

The extracted upstream branch builds for Pico 2 W with Pico SDK 2.3.0 and GCC
15.2.1. The same underlying features were physically tested in the user's
no-OLED fork: DS4 touchpad in the Amiga core, gyro aiming and Nintendo red.
The extracted branch has not been flashed or physically tested. Low-battery
blinking is verified with synthetic battery values, not a depleted controller.

The first patch fixes an existing missing bluebridge_state.h include in
bluebridge_platform.c exposed by GCC 15. It is separate from the DS4 changes.
DS4 report flags and timing reference:
https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux-stable/+/master/drivers/hid/hid-playstation.c
