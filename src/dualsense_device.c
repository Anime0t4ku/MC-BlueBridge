// DualSense output mode: presents BlueBridge as a wired DualSense controller.
//
// macOS supports the DualSense natively over USB (Game Controller framework,
// including rumble) without any authentication, and so do SDL/Steam on every
// platform. Only a PS5 console would require controller authentication.
//
// Report layout follows the Linux hid-playstation driver's dualsense input and
// output reports; the approach is modelled on joypad-os's DualSense mode.

#include "dualsense_device.h"
#include "dualsense_descriptors.h"
#include "bt_host.h"
#include "pico/stdlib.h"
#include <string.h>

#define DS5_HAT_NEUTRAL 0x08
#define DS5_RUMBLE_REFRESH_MS 1000u

// Offsets inside the input report body (after the report ID).
#define DS5_IN_LX 0
#define DS5_IN_LY 1
#define DS5_IN_RX 2
#define DS5_IN_RY 3
#define DS5_IN_L2 4
#define DS5_IN_R2 5
#define DS5_IN_SEQ 6
#define DS5_IN_BUTTONS0 7
#define DS5_IN_BUTTONS1 8
#define DS5_IN_BUTTONS2 9
#define DS5_IN_ACCEL_Z 25
#define DS5_IN_SENSOR_TIME 27
#define DS5_IN_TOUCH1 32
#define DS5_IN_TOUCH2 36
#define DS5_IN_STATUS 52

// Offsets inside the output report body (after the report ID).
#define DS5_OUT_RUMBLE_RIGHT 2
#define DS5_OUT_RUMBLE_LEFT 3

static uint8_t sequence;
static uint8_t rumble_left;
static uint8_t rumble_right;
static uint32_t rumble_refresh_at;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint8_t stick(int8_t v) {
    return (uint8_t)((int)v + 128);
}

static void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void bb_ds5_build_input(const bb_gamepad_report_t* r, uint8_t out[BB_DS5_INPUT_SIZE]) {
    memset(out, 0, BB_DS5_INPUT_SIZE);
    out[DS5_IN_LX] = stick(r->x);
    out[DS5_IN_LY] = stick(r->y);
    out[DS5_IN_RX] = stick(r->rx);
    out[DS5_IN_RY] = stick(r->ry);
    out[DS5_IN_L2] = r->lt ? r->lt : (r->buttons & (1u << 6)) ? 255 : 0;
    out[DS5_IN_R2] = r->rt ? r->rt : (r->buttons & (1u << 7)) ? 255 : 0;
    out[DS5_IN_SEQ] = sequence;

    // BlueBridge hat: 0 neutral, 1..8 clockwise from up; DualSense: 0..7, 8 neutral.
    uint8_t hat = r->hat >= 1 && r->hat <= 8 ? (uint8_t)(r->hat - 1) : DS5_HAT_NEUTRAL;
    uint8_t b0 = hat;
    if (r->buttons & (1u << 2)) b0 |= 0x10; // West: square
    if (r->buttons & (1u << 0)) b0 |= 0x20; // South: cross
    if (r->buttons & (1u << 1)) b0 |= 0x40; // East: circle
    if (r->buttons & (1u << 3)) b0 |= 0x80; // North: triangle
    out[DS5_IN_BUTTONS0] = b0;

    uint8_t b1 = 0;
    if (r->buttons & (1u << 4)) b1 |= 0x01;               // L1
    if (r->buttons & (1u << 5)) b1 |= 0x02;               // R1
    if (out[DS5_IN_L2] >= 64) b1 |= 0x04;                 // L2 digital
    if (out[DS5_IN_R2] >= 64) b1 |= 0x08;                 // R2 digital
    if (r->buttons & (1u << 8)) b1 |= 0x10;               // Select: Create
    if (r->buttons & (1u << 9)) b1 |= 0x20;               // Start: Options
    if (r->buttons & (1u << 10)) b1 |= 0x40;              // L3
    if (r->buttons & (1u << 11)) b1 |= 0x80;              // R3
    out[DS5_IN_BUTTONS1] = b1;

    uint8_t b2 = 0;
    if (r->buttons & (1u << 12)) b2 |= 0x01;              // Home: PS
    if (r->buttons & (1u << 13)) b2 |= 0x02;              // touchpad click
    if (r->buttons & (1u << 14)) b2 |= 0x04;              // mute
    out[DS5_IN_BUTTONS2] = b2;

    // Level and still: +1 g on the accelerometer Z axis (8192 per g).
    out[DS5_IN_ACCEL_Z] = 0x00;
    out[DS5_IN_ACCEL_Z + 1] = 0x20;
    put_u32le(&out[DS5_IN_SENSOR_TIME], now_ms() * 3000u);
    // No fingers on the touchpad (bit 7 set = not touching).
    out[DS5_IN_TOUCH1] = 0x80;
    out[DS5_IN_TOUCH2] = 0x80;
    // Battery full, wired.
    out[DS5_IN_STATUS] = 0x2a;

    ++sequence;
}

void bb_ds5_handle_output(uint8_t report_id, const uint8_t* buffer, uint16_t length) {
    if (!buffer || !length) return;
    if (!report_id) {
        report_id = buffer[0];
        ++buffer;
        --length;
    }
    if (report_id != BB_DS5_REPORT_ID_OUTPUT || length <= DS5_OUT_RUMBLE_LEFT) return;
    uint8_t left = buffer[DS5_OUT_RUMBLE_LEFT];
    uint8_t right = buffer[DS5_OUT_RUMBLE_RIGHT];
    // Hosts stream identical output reports; only act on changes here and let
    // bb_ds5_task() keep a sustained rumble alive.
    if (left == rumble_left && right == rumble_right) return;
    rumble_left = left;
    rumble_right = right;
    bb_bt_host_set_output_rumble(left, right);
    rumble_refresh_at = now_ms() + DS5_RUMBLE_REFRESH_MS;
}

uint16_t bb_ds5_get_feature(uint8_t report_id, uint8_t* buffer, uint16_t length) {
    if (report_id != BB_DS5_REPORT_ID_FIRMWARE || !buffer) return 0;
    uint16_t n = length < sizeof(bb_ds5_feature_20) ? length : (uint16_t)sizeof(bb_ds5_feature_20);
    memcpy(buffer, bb_ds5_feature_20, n);
    return n;
}

void bb_ds5_task(void) {
    if (!rumble_left && !rumble_right) return;
    uint32_t now = now_ms();
    if ((int32_t)(now - rumble_refresh_at) < 0) return;
    bb_bt_host_set_output_rumble(rumble_left, rumble_right);
    rumble_refresh_at = now + DS5_RUMBLE_REFRESH_MS;
}

void bb_ds5_reset(void) {
    if (rumble_left || rumble_right) bb_bt_host_set_output_rumble(0, 0);
    rumble_left = 0;
    rumble_right = 0;
    sequence = 0;
}
