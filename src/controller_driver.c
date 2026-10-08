#include "controller_driver.h"
#include "bluebridge_state.h"
#include "btstack.h"
#include <string.h>
#include <stdio.h>

#define BB_DPAD_UP 1
#define BB_DPAD_DOWN 2
#define BB_DPAD_LEFT 4
#define BB_DPAD_RIGHT 8
#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_BUTTON 0x09
#define HID_USAGE_PAGE_GENERIC_DEVICE 0x06
#define HID_USAGE_X 0x30
#define HID_USAGE_Y 0x31
#define HID_USAGE_Z 0x32
#define HID_USAGE_RX 0x33
#define HID_USAGE_RY 0x34
#define HID_USAGE_RZ 0x35
#define HID_USAGE_HAT 0x39
#define HID_USAGE_BATTERY_STRENGTH 0x20

static uint8_t early_arcade_battery_report[2];
static bool have_early_arcade_battery_report;
static int16_t diagnostic_battery_request_status = -1;
static uint32_t diagnostic_battery_request_count;
static uint8_t diagnostic_battery_response[32];
static uint8_t diagnostic_battery_response_length;
static int16_t diagnostic_battery_status = -1;
static uint8_t diagnostic_report[64];
static uint16_t diagnostic_report_length;
static uint16_t diagnostic_report_original_length;
static uint32_t diagnostic_report_sequence;
static bb_controller_identity_t identity;
static const uint8_t* hid_descriptor;
static uint16_t hid_descriptor_len;
static bb_input_state_t last_input_state;
static bool have_last_input_state;
static uint8_t eightbitdo_axis_roles[6];
static bool eightbitdo_axis_roles_ready;
static uint32_t switch2_cal_sum[4];
static uint16_t switch2_cal_center[4];
static uint8_t switch2_cal_samples;
static uint16_t n64_center[2] = {2048, 2048};
static uint16_t n64_below[2] = {2047, 2047};
static uint16_t n64_above[2] = {2047, 2047};

static void submit_state(const bb_input_state_t* state) {
    if (!state) return;
    bb_input_state_t normalized = *state;
    if (identity.kind == BB_CONTROLLER_8BITDO && (strstr(identity.name, "Arcade Stick") || strstr(identity.name, "M30"))) {
        if (normalized.lt > 8) normalized.buttons |= 1u << 6;
        if (normalized.rt > 8) normalized.buttons |= 1u << 7;
        normalized.lt = normalized.rt = 0;
        if (strstr(identity.name, "Arcade Stick")) {
            normalized.buttons &= 0x000013ffu;
            bb_state_update_button_capabilities(0x000013ffu);
        }
    }
    last_input_state = normalized;
    have_last_input_state = true;
    bb_input_submit(&normalized);
}

static int16_t axis12_quantized(uint16_t value, uint16_t center, uint16_t range) {
    int32_t delta = (int32_t)value - (int32_t)center;
    const int32_t neutral = 48;
    if (!range || range <= neutral) return 0;
    if (delta >= -neutral && delta <= neutral) return 0;
    if (delta < 0) delta += neutral;
    else delta -= neutral;
    int32_t q = (delta * 127) / (int32_t)(range - neutral);
    if (q < -127) q = -127;
    if (q > 127) q = 127;
    return (int16_t)(q * 258);
}

static int16_t axis12(uint16_t value) {
    return axis12_quantized(value, 2048, 2047);
}

static int16_t axis12_switch_pro(uint16_t value) {
    int32_t delta = (int32_t)value - 2048;
    const int32_t neutral = 128;
    if (delta >= -neutral && delta <= neutral) return 0;
    if (delta < 0) delta += neutral;
    else delta -= neutral;
    int32_t q = (delta * 127) / (2047 - neutral);
    if (q < -127) q = -127;
    if (q > 127) q = 127;
    return (int16_t)(q * 258);
}

static int16_t wiiu_axis(uint16_t value) {
    int32_t centered = (int32_t)value - 2048;
    centered *= 16;
    if (centered < -32768) centered = -32768;
    if (centered > 32767) centered = 32767;
    return (int16_t)centered;
}

static int16_t generic_axis(int32_t value) {
    if (value >= -128 && value <= 127) return (int16_t)(value * 256);
    if (value >= 0 && value <= 255) return (int16_t)((value - 128) * 256);
    if (value >= -512 && value <= 511) return (int16_t)(value * 64);
    if (value >= 0 && value <= 1023) return (int16_t)((value - 512) * 64);
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

static int16_t hid_axis(int32_t value, int32_t minimum, int32_t maximum) {
    int64_t span = (int64_t)maximum - minimum;
    if (span <= 0) return 0;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    int64_t centered = (int64_t)value * 2 - minimum - maximum;
    if (centered >= -1 && centered <= 1) return 0;
    int64_t scaled = centered * 32767 / span;
    return (int16_t)scaled;
}

static int16_t dinput_axis(int32_t value) {
    int32_t centered;
    int32_t deadzone;
    int32_t limit;
    if (value >= 0 && value <= 255) {
        centered = value - 128;
        deadzone = 2;
        limit = 127;
    } else if (value >= 0 && value <= 1023) {
        centered = value - 512;
        deadzone = 8;
        limit = 511;
    } else if (value >= 0 && value <= 65535) {
        centered = value - 32768;
        deadzone = 512;
        limit = 32767;
    } else {
        return generic_axis(value);
    }
    if (centered >= -deadzone && centered <= deadzone) return 0;
    if (centered < 0) centered += deadzone;
    else centered -= deadzone;
    centered = (centered * 32767) / (limit - deadzone);
    if (centered < -32767) centered = -32767;
    if (centered > 32767) centered = 32767;
    return (int16_t)centered;
}

static int16_t dualsense_axis(uint8_t value) {
    int32_t centered = (int32_t)value - 128;
    if (centered >= -8 && centered <= 8) return 0;
    if (centered < 0) centered += 8;
    else centered -= 8;
    centered = (centered * 32767) / 119;
    if (centered < -32767) centered = -32767;
    if (centered > 32767) centered = 32767;
    return (int16_t)centered;
}

static uint16_t generic_trigger(int32_t value, int32_t minimum, int32_t maximum) {
    if (maximum <= minimum || value <= minimum) return 0;
    if (value >= maximum) return 255;
    return (uint16_t)(((int64_t)value - minimum) * 255 / ((int64_t)maximum - minimum));
}

static uint8_t hat_to_dpad(int32_t value) {
    switch (value) {
        case 0: return BB_DPAD_UP;
        case 1: return BB_DPAD_UP | BB_DPAD_RIGHT;
        case 2: return BB_DPAD_RIGHT;
        case 3: return BB_DPAD_RIGHT | BB_DPAD_DOWN;
        case 4: return BB_DPAD_DOWN;
        case 5: return BB_DPAD_DOWN | BB_DPAD_LEFT;
        case 6: return BB_DPAD_LEFT;
        case 7: return BB_DPAD_LEFT | BB_DPAD_UP;
        default: return 0;
    }
}

static bool dualsense_hat_to_dpad(uint8_t value, uint8_t* dpad) {
    if (!dpad) return false;
    if (value == 8) {
        *dpad = 0;
        return true;
    }
    if (value > 7) return false;
    *dpad = hat_to_dpad(value);
    return true;
}

static uint32_t crc32_update(uint32_t crc, uint8_t value) {
    crc ^= value;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    return crc;
}

static bool dualsense_crc_valid(const uint8_t* report, uint16_t length) {
    if (!report || length != 78) return false;
    uint32_t crc = 0xffffffffu;
    crc = crc32_update(crc, 0xa1);
    for (uint16_t i = 0; i < length - 4; ++i) crc = crc32_update(crc, report[i]);
    crc = ~crc;
    uint32_t expected = (uint32_t)report[length - 4] |
                        ((uint32_t)report[length - 3] << 8) |
                        ((uint32_t)report[length - 2] << 16) |
                        ((uint32_t)report[length - 1] << 24);
    return crc == expected;
}

static uint32_t canonicalize_buttons(uint32_t buttons) {
    if (identity.kind == BB_CONTROLLER_DUALSENSE || identity.kind == BB_CONTROLLER_DUALSENSE_EDGE || identity.kind == BB_CONTROLLER_DUALSHOCK4) {
        uint32_t out = buttons & 0xfffffff0u;
        if (buttons & (1u << 0)) out |= 1u << 2;
        if (buttons & (1u << 1)) out |= 1u << 0;
        if (buttons & (1u << 2)) out |= 1u << 1;
        if (buttons & (1u << 3)) out |= 1u << 3;
        return out;
    }
    return buttons;
}

static void map_button_bit(uint32_t src_buttons, uint32_t src_caps, uint8_t src_index, uint32_t* dst_buttons, uint32_t* dst_caps, uint8_t dst_index) {
    if (src_index < 32 && (src_buttons & (1u << src_index))) *dst_buttons |= 1u << dst_index;
    if (src_index < 32 && (src_caps & (1u << src_index))) *dst_caps |= 1u << dst_index;
}

static bool eightbitdo_name_has(const char* part) {
    size_t n = strlen(part);
    for (const char* name = identity.name; *name; ++name) {
        size_t i = 0;
        while (i < n && name[i]) {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != part[i]) break;
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

static void map_8bitdo_layout(bb_input_state_t* state, uint32_t* capabilities, const uint8_t* targets, uint8_t count) {
    uint32_t buttons = 0;
    uint32_t caps = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (targets[i] < BB_BUTTON_COUNT) map_button_bit(state->buttons, *capabilities, i, &buttons, &caps, targets[i]);
    }
    state->buttons = buttons;
    *capabilities = caps;
}

static void normalize_8bitdo_buttons(bb_input_state_t* state, uint32_t* capabilities, bool analog_lt, bool analog_rt) {
    if (!state || !capabilities) return;
    uint32_t src_buttons = state->buttons;
    uint32_t src_caps = *capabilities;
    uint32_t dst_buttons = 0;
    uint32_t dst_caps = 0;
    bool pro3 = strstr(identity.name, "Pro 3") || strstr(identity.name, "PRO 3") || strstr(identity.name, "pro 3");
    bool arcade = eightbitdo_name_has("arcade stick") || eightbitdo_name_has("nes30 arcade");
    if (arcade) {
        static const uint8_t xinput[] = {1,0,3,2,4,5,8,9,4,5,12,255,12};
        map_8bitdo_layout(state, capabilities, xinput, sizeof(xinput));
        return;
    }
    if (eightbitdo_name_has("micro")) {
        static const uint8_t layout[] = {1,0,255,3,2,255,4,5,6,7,8,9,12};
        map_8bitdo_layout(state, capabilities, layout, sizeof(layout));
        state->lt = state->rt = 0;
        return;
    }
    if (eightbitdo_name_has("m30")) {
        static const uint8_t layout[] = {1,0,12,3,2,255,4,5,6,7,8,9,12};
        map_8bitdo_layout(state, capabilities, layout, sizeof(layout));
        state->lt = state->rt = 0;
        return;
    }
    bool model = eightbitdo_name_has("m30") || eightbitdo_name_has("sn30") || eightbitdo_name_has("sf30") || eightbitdo_name_has("pro 2") || eightbitdo_name_has("ultimate") || eightbitdo_name_has("lite") || eightbitdo_name_has("zero");
    if (!pro3 && model) {
        static const uint8_t sequential[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13};
        static const uint8_t paddles[] = {0,1,17,2,3,16,4,5,6,7,8,9,12,10,11,13};
        if (src_caps & 0xffffc000u) map_8bitdo_layout(state, capabilities, paddles, sizeof(paddles));
        else map_8bitdo_layout(state, capabilities, sequential, sizeof(sequential));
        if (!analog_lt && (state->buttons & (1u << 6))) state->lt = 255;
        if (!analog_rt && (state->buttons & (1u << 7))) state->rt = 255;
        return;
    }
    map_button_bit(src_buttons, src_caps, 0, &dst_buttons, &dst_caps, 1);
    map_button_bit(src_buttons, src_caps, 1, &dst_buttons, &dst_caps, 0);
    map_button_bit(src_buttons, src_caps, 3, &dst_buttons, &dst_caps, 3);
    map_button_bit(src_buttons, src_caps, 4, &dst_buttons, &dst_caps, 2);
    map_button_bit(src_buttons, src_caps, 6, &dst_buttons, &dst_caps, 4);
    map_button_bit(src_buttons, src_caps, 7, &dst_buttons, &dst_caps, 5);
    map_button_bit(src_buttons, src_caps, 8, &dst_buttons, &dst_caps, 6);
    map_button_bit(src_buttons, src_caps, 9, &dst_buttons, &dst_caps, 7);
    map_button_bit(src_buttons, src_caps, 10, &dst_buttons, &dst_caps, 8);
    map_button_bit(src_buttons, src_caps, 11, &dst_buttons, &dst_caps, 9);
    if (pro3) {
        map_button_bit(src_buttons, src_caps, 13, &dst_buttons, &dst_caps, 10);
        map_button_bit(src_buttons, src_caps, 14, &dst_buttons, &dst_caps, 11);
        map_button_bit(src_buttons, src_caps, 12, &dst_buttons, &dst_caps, 12);
        map_button_bit(src_buttons, src_caps, 16, &dst_buttons, &dst_caps, 16);
        map_button_bit(src_buttons, src_caps, 17, &dst_buttons, &dst_caps, 17);
        map_button_bit(src_buttons, src_caps, 5, &dst_buttons, &dst_caps, 18);
        map_button_bit(src_buttons, src_caps, 2, &dst_buttons, &dst_caps, 19);
    } else {
        map_button_bit(src_buttons, src_caps, 12, &dst_buttons, &dst_caps, 10);
        map_button_bit(src_buttons, src_caps, 13, &dst_buttons, &dst_caps, 11);
        map_button_bit(src_buttons, src_caps, 2, &dst_buttons, &dst_caps, 12);
        map_button_bit(src_buttons, src_caps, 5, &dst_buttons, &dst_caps, 13);
        map_button_bit(src_buttons, src_caps, 14, &dst_buttons, &dst_caps, 14);
        map_button_bit(src_buttons, src_caps, 15, &dst_buttons, &dst_caps, 15);
        for (uint8_t i = 16; i < BB_BUTTON_COUNT; ++i) map_button_bit(src_buttons, src_caps, i, &dst_buttons, &dst_caps, i);
    }
    state->buttons = dst_buttons;
    *capabilities = dst_caps;
    if (!analog_lt && (dst_buttons & (1u << 6))) state->lt = 255;
    if (!analog_rt && (dst_buttons & (1u << 7))) state->rt = 255;
}

static uint16_t unpack12a(const uint8_t* p) {
    return (uint16_t)(p[0] | ((p[1] & 0x0f) << 8));
}

static uint16_t unpack12b(const uint8_t* p) {
    return (uint16_t)((p[1] >> 4) | (p[2] << 4));
}

bool bb_driver_set_n64_calibration(const uint8_t* data, uint16_t length) {
    if (!data || length < 9 || identity.kind != BB_CONTROLLER_NSO_N64) return false;
    uint16_t above[2] = {unpack12a(data), unpack12b(data)};
    uint16_t center[2] = {unpack12a(data + 3), unpack12b(data + 3)};
    uint16_t below[2] = {unpack12a(data + 6), unpack12b(data + 6)};
    for (unsigned i = 0; i < 2; ++i) {
        if (center[i] < 256 || center[i] > 3839 || below[i] < 128 || above[i] < 128 || below[i] > center[i] || above[i] > 4095u - center[i]) return false;
    }
    memcpy(n64_center, center, sizeof(center));
    memcpy(n64_below, below, sizeof(below));
    memcpy(n64_above, above, sizeof(above));
    return true;
}

static int16_t n64_axis(uint16_t value, unsigned axis) {
    int32_t delta = (int32_t)value - n64_center[axis];
    uint16_t range = delta < 0 ? n64_below[axis] : n64_above[axis];
    return axis12_quantized(value, n64_center[axis], range);
}

static bool parse_n64_full(const uint8_t* p, uint16_t len) {
    if (len < 13) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    uint8_t r = p[3], m = p[4], l = p[5];
    if (r & 0x08) s.buttons |= 1u << 0;
    if (r & 0x04) s.buttons |= 1u << 1;
    if (r & 0x02) s.buttons |= 1u << 2;
    if (m & 0x01) s.buttons |= 1u << 3;
    if (l & 0x40) s.buttons |= 1u << 4;
    if (r & 0x40) s.buttons |= 1u << 5;
    if (l & 0x80) s.buttons |= 1u << 6;
    if (r & 0x80) s.buttons |= 1u << 7;
    if (m & 0x02) s.buttons |= 1u << 9;
    if (m & 0x08) s.buttons |= 1u << 10;
    if (r & 0x01) s.buttons |= 1u << 11;
    if (m & 0x10) s.buttons |= 1u << 12;
    if (m & 0x20) s.buttons |= 1u << 13;
    if (l & 0x02) s.dpad |= BB_DPAD_UP;
    if (l & 0x01) s.dpad |= BB_DPAD_DOWN;
    if (l & 0x08) s.dpad |= BB_DPAD_LEFT;
    if (l & 0x04) s.dpad |= BB_DPAD_RIGHT;
    s.x = n64_axis(unpack12a(p + 6), 0);
    s.y = (int16_t)-n64_axis(unpack12b(p + 6), 1);
    s.lt = (l & 0x80) ? 255 : 0;
    s.rt = (m & 0x08) ? 255 : 0;
    uint8_t battery = p[2] >> 4;
    uint8_t level = battery & 0x0e;
    bb_state_update_battery(true, level >= 8 ? 100 : level * 100u / 8u, (battery & 1) ? 2 : 1);
    bb_state_update_button_capabilities(0x00003effu);
    submit_state(&s);
    return true;
}

static bool parse_n64_simple(const uint8_t* report, uint16_t length) {
    if (length != 12 || report[0] != 0x3f) return false;
    bb_input_state_t state;
    bb_input_clear(&state);
    uint16_t buttons = little_endian_read_16(report, 1);
    static const uint8_t target[] = {1,0,11,2,4,5,6,7,3,9,10,255,12,13};
    for (uint8_t i = 0; i < sizeof(target); ++i) {
        if (target[i] != 255 && (buttons & (1u << i))) state.buttons |= 1u << target[i];
    }
    uint8_t hat = report[3] & 15;
    if (hat < 8) state.dpad = hat_to_dpad(hat);
    state.x = hid_axis(little_endian_read_16(report, 4), 0, 65535);
    state.y = hid_axis(little_endian_read_16(report, 6), 0, 65535);
    state.lt = (state.buttons & (1u << 6)) ? 255 : 0;
    state.rt = (state.buttons & (1u << 10)) ? 255 : 0;
    bb_state_update_button_capabilities(0x00003effu);
    submit_state(&state);
    return true;
}

static bool parse_nso_retro(const uint8_t* report, uint16_t length) {
    if (!report || length < 12) return false;
    bool full = report[0] == 0x30;
    if (!full && report[0] != 0x3f) return false;
    bb_input_state_t state;
    bb_input_clear(&state);
    uint32_t bits = full ? ((uint32_t)report[3] | ((uint32_t)report[4] << 8) | ((uint32_t)report[5] << 16)) : ((uint32_t)report[1] | ((uint32_t)report[2] << 8));
    uint8_t sources[14];
    memset(sources, 255, sizeof(sources));
    if (identity.kind == BB_CONTROLLER_NSO_GENESIS) {
        sources[0] = full ? 2 : 0;
        sources[1] = full ? 6 : 5;
        sources[2] = full ? 3 : 1;
        sources[3] = full ? 1 : 6;
        sources[4] = full ? 0 : 2;
        sources[5] = full ? 22 : 4;
        sources[8] = 7;
        sources[9] = 9;
        sources[12] = 12;
        sources[13] = 13;
    } else if (identity.kind == BB_CONTROLLER_NSO_SNES) {
        sources[0] = full ? 2 : 0;
        sources[1] = full ? 3 : 1;
        sources[2] = full ? 0 : 2;
        sources[3] = full ? 1 : 3;
        sources[4] = full ? 22 : 4;
        sources[5] = full ? 6 : 5;
        sources[6] = full ? 23 : 6;
        sources[7] = full ? 7 : 15;
        sources[8] = 8;
        sources[9] = 9;
    } else if (identity.kind == BB_CONTROLLER_NSO_NES) {
        sources[0] = full ? 2 : 1;
        sources[1] = full ? 3 : 0;
        sources[4] = full ? 22 : 4;
        sources[5] = full ? 6 : 5;
        sources[8] = 8;
        sources[9] = 9;
    } else {
        return false;
    }
    for (uint8_t i = 0; i < sizeof(sources); ++i) {
        if (sources[i] < 24 && (bits & (1u << sources[i]))) state.buttons |= 1u << i;
    }
    if (full) {
        if (bits & (1u << 17)) state.dpad |= BB_DPAD_UP;
        if (bits & (1u << 16)) state.dpad |= BB_DPAD_DOWN;
        if (bits & (1u << 19)) state.dpad |= BB_DPAD_LEFT;
        if (bits & (1u << 18)) state.dpad |= BB_DPAD_RIGHT;
        uint8_t battery = report[2] >> 4;
        uint8_t level = battery & 0x0e;
        bb_state_update_battery(true, level >= 8 ? 100 : level * 100u / 8u, (battery & 1) ? 2 : 1);
    } else {
        uint8_t hat = report[3] & 0x0f;
        if (hat <= 7) state.dpad = hat_to_dpad(hat);
    }
    bb_state_update_button_capabilities(bb_controller_capabilities(identity.kind, identity.name).buttons);
    submit_state(&state);
    return true;
}

static bool parse_switch_full(const uint8_t* p, uint16_t len) {
    if (len < 12) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    uint8_t r = p[3];
    uint8_t m = p[4];
    uint8_t l = p[5];
    if (r & 0x04) s.buttons |= 1u << 0;
    if (r & 0x08) s.buttons |= 1u << 1;
    if (r & 0x01) s.buttons |= 1u << 2;
    if (r & 0x02) s.buttons |= 1u << 3;
    if (l & 0x40) s.buttons |= 1u << 4;
    if (r & 0x40) s.buttons |= 1u << 5;
    if (l & 0x80) s.buttons |= 1u << 6;
    if (r & 0x80) s.buttons |= 1u << 7;
    if (m & 0x01) s.buttons |= 1u << 8;
    if (m & 0x02) s.buttons |= 1u << 9;
    if (m & 0x08) s.buttons |= 1u << 10;
    if (m & 0x04) s.buttons |= 1u << 11;
    if (m & 0x10) s.buttons |= 1u << 12;
    if (m & 0x20) s.buttons |= 1u << 13;
    if (l & 0x02) s.dpad |= BB_DPAD_UP;
    if (l & 0x01) s.dpad |= BB_DPAD_DOWN;
    if (l & 0x08) s.dpad |= BB_DPAD_LEFT;
    if (l & 0x04) s.dpad |= BB_DPAD_RIGHT;
    s.x = axis12_switch_pro(unpack12a(&p[6]));
    s.y = (int16_t)-axis12_switch_pro(unpack12b(&p[6]));
    s.rx = axis12_switch_pro(unpack12a(&p[9]));
    s.ry = (int16_t)-axis12_switch_pro(unpack12b(&p[9]));
    s.lt = (l & 0x80) ? 255 : 0;
    s.rt = (r & 0x80) ? 255 : 0;
    uint8_t battery = (uint8_t)(p[2] >> 4);
    uint8_t level = (uint8_t)(battery & 0x0eu);
    uint8_t percent = level >= 8 ? 100 : (uint8_t)((level * 100u) / 8u);
    bb_state_update_battery(true, percent, (battery & 0x01u) ? 2 : 1);
    bb_state_update_button_capabilities(0x00003fffu);
    submit_state(&s);
    return true;
}

static int16_t axis16(const uint8_t* p) {
    uint16_t raw = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    int32_t centered = (int32_t)raw - 32768;
    const int32_t deadzone = 64;
    if (centered >= -deadzone && centered <= deadzone) return 0;
    if (centered < 0) centered += deadzone;
    else centered -= deadzone;
    centered = (centered * 32767) / (32767 - deadzone);
    if (centered < -32767) centered = -32767;
    if (centered > 32767) centered = 32767;
    return (int16_t)centered;
}

static bool parse_switch_simple(const uint8_t* p, uint16_t len) {
    if (len < 12) return false;
    bb_input_state_t s;
    bb_input_clear(&s);

    uint8_t buttons0 = p[1];
    uint8_t buttons1 = p[2];
    uint8_t hat = (uint8_t)(p[3] & 0x0f);

    if (buttons0 & 0x01) s.buttons |= 1u << 0;
    if (buttons0 & 0x02) s.buttons |= 1u << 1;
    if (buttons0 & 0x04) s.buttons |= 1u << 2;
    if (buttons0 & 0x08) s.buttons |= 1u << 3;
    if (buttons0 & 0x10) s.buttons |= 1u << 4;
    if (buttons0 & 0x20) s.buttons |= 1u << 5;
    if (buttons0 & 0x40) s.buttons |= 1u << 6;
    if (buttons0 & 0x80) s.buttons |= 1u << 7;

    if (buttons1 & 0x01) s.buttons |= 1u << 8;
    if (buttons1 & 0x02) s.buttons |= 1u << 9;
    if (buttons1 & 0x04) s.buttons |= 1u << 10;
    if (buttons1 & 0x08) s.buttons |= 1u << 11;
    if (buttons1 & 0x10) s.buttons |= 1u << 12;
    if (buttons1 & 0x20) s.buttons |= 1u << 13;
    if (buttons1 & 0x40) s.buttons |= 1u << 14;
    if (buttons1 & 0x80) s.buttons |= 1u << 15;

    if (hat <= 7) s.dpad = hat_to_dpad(hat);

    s.x = axis16(&p[4]);
    s.y = (int16_t)-axis16(&p[6]);
    s.rx = axis16(&p[8]);
    s.ry = (int16_t)-axis16(&p[10]);
    s.lt = (s.buttons & (1u << 6)) ? 255 : 0;
    s.rt = (s.buttons & (1u << 7)) ? 255 : 0;
    bb_state_update_button_capabilities(0x0000ffffu);
    submit_state(&s);
    return true;
}

static bool parse_dualsense_bt_simple(const uint8_t* p, uint16_t len) {
    if (!p || len < 10 || p[0] != 0x01) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    s.x = dualsense_axis(p[1]);
    s.y = dualsense_axis(p[2]);
    s.rx = dualsense_axis(p[3]);
    s.ry = dualsense_axis(p[4]);
    uint8_t b0 = p[5];
    uint8_t b1 = p[6];
    uint8_t b2 = p[7];
    if (!dualsense_hat_to_dpad((uint8_t)(b0 & 0x0f), &s.dpad)) return true;
    if (b0 & 0x20) s.buttons |= 1u << 0;
    if (b0 & 0x40) s.buttons |= 1u << 1;
    if (b0 & 0x10) s.buttons |= 1u << 2;
    if (b0 & 0x80) s.buttons |= 1u << 3;
    if (b1 & 0x01) s.buttons |= 1u << 4;
    if (b1 & 0x02) s.buttons |= 1u << 5;
    if (b1 & 0x04) s.buttons |= 1u << 6;
    if (b1 & 0x08) s.buttons |= 1u << 7;
    if (b1 & 0x10) s.buttons |= 1u << 8;
    if (b1 & 0x20) s.buttons |= 1u << 9;
    if (b1 & 0x40) s.buttons |= 1u << 10;
    if (b1 & 0x80) s.buttons |= 1u << 11;
    if (b2 & 0x01) s.buttons |= 1u << 12;
    if (b2 & 0x02) s.buttons |= 1u << 13;
    s.lt = p[8];
    s.rt = p[9];
    submit_state(&s);
    return true;
}

static bool parse_dualsense_bt_full(const uint8_t* p, uint16_t len) {
    if (!p || len != 78 || p[0] != 0x31) return false;
    if (!dualsense_crc_valid(p, len)) return true;
    bb_input_state_t s;
    bb_input_clear(&s);
    s.x = dualsense_axis(p[2]);
    s.y = dualsense_axis(p[3]);
    s.rx = dualsense_axis(p[4]);
    s.ry = dualsense_axis(p[5]);
    s.lt = p[6];
    s.rt = p[7];
    uint8_t b0 = p[9];
    uint8_t b1 = p[10];
    uint8_t b2 = p[11];
    if (!dualsense_hat_to_dpad((uint8_t)(b0 & 0x0f), &s.dpad)) return true;
    if (b0 & 0x20) s.buttons |= 1u << 0;
    if (b0 & 0x40) s.buttons |= 1u << 1;
    if (b0 & 0x10) s.buttons |= 1u << 2;
    if (b0 & 0x80) s.buttons |= 1u << 3;
    if (b1 & 0x01) s.buttons |= 1u << 4;
    if (b1 & 0x02) s.buttons |= 1u << 5;
    if (b1 & 0x04) s.buttons |= 1u << 6;
    if (b1 & 0x08) s.buttons |= 1u << 7;
    if (b1 & 0x10) s.buttons |= 1u << 8;
    if (b1 & 0x20) s.buttons |= 1u << 9;
    if (b1 & 0x40) s.buttons |= 1u << 10;
    if (b1 & 0x80) s.buttons |= 1u << 11;
    if (b2 & 0x01) s.buttons |= 1u << 12;
    if (b2 & 0x02) s.buttons |= 1u << 13;
    if (b2 & 0x04) s.buttons |= 1u << 14;
    uint8_t battery = p[54];
    uint8_t level = battery & 0x0f;
    uint8_t charge = (battery >> 4) & 0x0f;
    if (charge == 0 || charge == 1) bb_state_update_battery(true, (uint8_t)((level * 10u + 5u) > 100u ? 100u : (level * 10u + 5u)), charge == 1 ? 2 : 1);
    else if (charge == 2) bb_state_update_battery(true, 100, 3);
    else bb_state_update_battery(false, 0, 0);
    submit_state(&s);
    return true;
}

static bool parse_ds4_common(const uint8_t* p, uint16_t len, uint8_t base, bool battery) {
    if (!p || len < (uint16_t)(base + 9)) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    s.x = dualsense_axis(p[base + 0]);
    s.y = dualsense_axis(p[base + 1]);
    s.rx = dualsense_axis(p[base + 2]);
    s.ry = dualsense_axis(p[base + 3]);
    uint8_t b0 = p[base + 4];
    uint8_t b1 = p[base + 5];
    uint8_t b2 = p[base + 6];
    if (!dualsense_hat_to_dpad((uint8_t)(b0 & 0x0f), &s.dpad)) return true;
    if (b0 & 0x20) s.buttons |= 1u << 0;
    if (b0 & 0x40) s.buttons |= 1u << 1;
    if (b0 & 0x10) s.buttons |= 1u << 2;
    if (b0 & 0x80) s.buttons |= 1u << 3;
    if (b1 & 0x01) s.buttons |= 1u << 4;
    if (b1 & 0x02) s.buttons |= 1u << 5;
    if (b1 & 0x04) s.buttons |= 1u << 6;
    if (b1 & 0x08) s.buttons |= 1u << 7;
    if (b1 & 0x10) s.buttons |= 1u << 8;
    if (b1 & 0x20) s.buttons |= 1u << 9;
    if (b1 & 0x40) s.buttons |= 1u << 10;
    if (b1 & 0x80) s.buttons |= 1u << 11;
    if (b2 & 0x01) s.buttons |= 1u << 12;
    if (b2 & 0x02) s.buttons |= 1u << 13;
    s.lt = p[base + 7];
    s.rt = p[base + 8];
    bb_state_update_button_capabilities(0x00003fffu);
    if (battery && len > 32) {
        uint8_t v = p[32];
        uint8_t level = v & 0x0f;
        uint8_t pct = level >= 10 ? 100 : (uint8_t)(level * 10u + 5u);
        bb_state_update_battery(true, pct, (v & 0x10) ? 2 : 1);
    }
    submit_state(&s);
    return true;
}

static bool parse_ds4_bt(const uint8_t* p, uint16_t len) {
    if (!p || !len) return false;
    if (p[0] == 0x11 && len >= 12) return parse_ds4_common(p, len, 3, true);
    if (p[0] == 0x01 && len >= 10) return parse_ds4_common(p, len, 1, false);
    return false;
}

static bool parse_ds3_bt(const uint8_t* p, uint16_t len) {
    if (!p || len < 10 || p[0] != 0x01) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    uint8_t a = p[2];
    uint8_t b = p[3];
    uint8_t c = p[4];
    if (b & 0x40) s.buttons |= 1u << 0;
    if (b & 0x20) s.buttons |= 1u << 1;
    if (b & 0x80) s.buttons |= 1u << 2;
    if (b & 0x10) s.buttons |= 1u << 3;
    if (b & 0x04) s.buttons |= 1u << 4;
    if (b & 0x08) s.buttons |= 1u << 5;
    if (b & 0x01) s.buttons |= 1u << 6;
    if (b & 0x02) s.buttons |= 1u << 7;
    if (a & 0x01) s.buttons |= 1u << 8;
    if (a & 0x08) s.buttons |= 1u << 9;
    if (a & 0x02) s.buttons |= 1u << 10;
    if (a & 0x04) s.buttons |= 1u << 11;
    if (c & 0x01) s.buttons |= 1u << 12;
    if (a & 0x10) s.dpad |= BB_DPAD_UP;
    if (a & 0x40) s.dpad |= BB_DPAD_DOWN;
    if (a & 0x80) s.dpad |= BB_DPAD_LEFT;
    if (a & 0x20) s.dpad |= BB_DPAD_RIGHT;
    s.x = dualsense_axis(p[6]);
    s.y = dualsense_axis(p[7]);
    s.rx = dualsense_axis(p[8]);
    s.ry = dualsense_axis(p[9]);
    s.lt = (s.buttons & (1u << 6)) ? 255 : 0;
    s.rt = (s.buttons & (1u << 7)) ? 255 : 0;
    bb_state_update_button_capabilities(0x00001fffu);
    submit_state(&s);
    return true;
}

static bool parse_wiiu_pro(const uint8_t* p, uint16_t len) {
    if (!p || len < 14 || p[0] != 0x34) return false;
    const uint8_t* e = &p[3];
    bb_input_state_t s;
    bb_input_clear(&s);
    uint16_t lx = (uint16_t)e[0] | ((uint16_t)e[1] << 8);
    uint16_t rx = (uint16_t)e[2] | ((uint16_t)e[3] << 8);
    uint16_t ly = (uint16_t)e[4] | ((uint16_t)e[5] << 8);
    uint16_t ry = (uint16_t)e[6] | ((uint16_t)e[7] << 8);
    uint8_t b0 = e[8];
    uint8_t b1 = e[9];
    uint8_t b2 = e[10];
    s.x = wiiu_axis(lx);
    s.y = (int16_t)-wiiu_axis(ly);
    s.rx = wiiu_axis(rx);
    s.ry = (int16_t)-wiiu_axis(ry);
    if (!(b1 & 0x40)) s.buttons |= 1u << 0;
    if (!(b1 & 0x10)) s.buttons |= 1u << 1;
    if (!(b1 & 0x20)) s.buttons |= 1u << 2;
    if (!(b1 & 0x08)) s.buttons |= 1u << 3;
    if (!(b0 & 0x20)) s.buttons |= 1u << 4;
    if (!(b0 & 0x02)) s.buttons |= 1u << 5;
    if (!(b1 & 0x80)) s.buttons |= 1u << 6;
    if (!(b1 & 0x04)) s.buttons |= 1u << 7;
    if (!(b0 & 0x10)) s.buttons |= 1u << 8;
    if (!(b0 & 0x04)) s.buttons |= 1u << 9;
    if (!(b2 & 0x02)) s.buttons |= 1u << 10;
    if (!(b2 & 0x01)) s.buttons |= 1u << 11;
    if (!(b0 & 0x08)) s.buttons |= 1u << 12;
    if (!(b1 & 0x01)) s.dpad |= BB_DPAD_UP;
    if (!(b0 & 0x40)) s.dpad |= BB_DPAD_DOWN;
    if (!(b1 & 0x02)) s.dpad |= BB_DPAD_LEFT;
    if (!(b0 & 0x80)) s.dpad |= BB_DPAD_RIGHT;
    s.lt = (s.buttons & (1u << 6)) ? 255 : 0;
    s.rt = (s.buttons & (1u << 7)) ? 255 : 0;
    uint8_t level = (uint8_t)((b2 >> 4) & 0x07u);
    bool charging = (b2 & 0x04u) == 0;
    bb_state_update_battery(true, (uint8_t)((level * 100u) / 7u), charging ? 2 : 1);
    bb_state_update_button_capabilities(0x00001fffu);
    submit_state(&s);
    return true;
}

static bool parse_stadia_aux(const uint8_t* p, uint16_t len) {
    if (!p || len < 11 || p[0] != 0x03) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    uint8_t hat = p[1];
    uint8_t b1 = p[2];
    uint8_t b2 = p[3];
    if (hat <= 7) s.dpad = hat_to_dpad(hat);
    if (b2 & 0x40) s.buttons |= 1u << 0;
    if (b2 & 0x20) s.buttons |= 1u << 1;
    if (b2 & 0x10) s.buttons |= 1u << 2;
    if (b2 & 0x08) s.buttons |= 1u << 3;
    if (b2 & 0x04) s.buttons |= 1u << 4;
    if (b2 & 0x02) s.buttons |= 1u << 5;
    if (b1 & 0x04) s.buttons |= 1u << 6;
    if (b1 & 0x08) s.buttons |= 1u << 7;
    if (b1 & 0x40) s.buttons |= 1u << 8;
    if (b1 & 0x20) s.buttons |= 1u << 9;
    if (b2 & 0x01) s.buttons |= 1u << 10;
    if (b1 & 0x80) s.buttons |= 1u << 11;
    if (b1 & 0x10) s.buttons |= 1u << 12;
    if (b1 & 0x02) s.buttons |= 1u << 13;
    if (b1 & 0x01) s.buttons |= 1u << 14;
    s.x = dualsense_axis(p[4]);
    s.y = dualsense_axis(p[5]);
    s.rx = dualsense_axis(p[6]);
    s.ry = dualsense_axis(p[7]);
    s.lt = p[8];
    s.rt = p[9];
    bb_state_update_button_capabilities(0x00007fffu);
    submit_state(&s);
    return true;
}


static bool axis_looks_centered(int32_t value) {
    if (value >= 0 && value <= 255) return value >= 48 && value <= 207;
    if (value >= 0 && value <= 1023) return value >= 192 && value <= 831;
    if (value >= 0 && value <= 65535) return value >= 12288 && value <= 53247;
    if (value >= -32768 && value <= 32767) return value >= -16384 && value <= 16384;
    return false;
}

static bool axis_looks_trigger_rest(int32_t value) {
    if (value >= 0 && value <= 255) return value <= 24;
    if (value >= 0 && value <= 1023) return value <= 96;
    if (value >= 0 && value <= 65535) return value <= 6144;
    return false;
}

static void learn_8bitdo_axis_roles(const int32_t values[6], const bool present[6]) {
    if (eightbitdo_axis_roles_ready) return;
    uint8_t centered[4];
    uint8_t trigger[4];
    uint8_t centered_count = 0;
    uint8_t trigger_count = 0;
    for (uint8_t i = 2; i < 6; ++i) {
        if (!present[i]) continue;
        if (axis_looks_trigger_rest(values[i])) trigger[trigger_count++] = i;
        else if (axis_looks_centered(values[i])) centered[centered_count++] = i;
    }
    if (centered_count >= 2 && trigger_count >= 2) {
        eightbitdo_axis_roles[centered[0]] = 1;
        eightbitdo_axis_roles[centered[1]] = 2;
        eightbitdo_axis_roles[trigger[0]] = 3;
        eightbitdo_axis_roles[trigger[1]] = 4;
        eightbitdo_axis_roles_ready = true;
        return;
    }
    bool pro3 = strstr(identity.name, "Pro 3") || strstr(identity.name, "PRO 3") || strstr(identity.name, "pro 3");
    if (pro3 && present[2] && present[3] && present[4] && present[5]) {
        eightbitdo_axis_roles[2] = 1;
        eightbitdo_axis_roles[3] = 2;
        eightbitdo_axis_roles[4] = 3;
        eightbitdo_axis_roles[5] = 4;
        eightbitdo_axis_roles_ready = true;
    }
}

static void apply_8bitdo_axes(bb_input_state_t* state, const int32_t values[6], const bool present[6], const int32_t minimum[6], const int32_t maximum[6], bool* analog_lt, bool* analog_rt) {
    if (!state || !analog_lt || !analog_rt) return;
    if (present[0]) state->x = dinput_axis(values[0]);
    if (present[1]) state->y = dinput_axis(values[1]);
    learn_8bitdo_axis_roles(values, present);
    if (!eightbitdo_axis_roles_ready) {
        if (present[3]) state->rx = dinput_axis(values[3]);
        if (present[4]) state->ry = dinput_axis(values[4]);
        if (present[2]) { state->lt = generic_trigger(values[2], minimum[2], maximum[2]); *analog_lt = true; }
        if (present[5]) { state->rt = generic_trigger(values[5], minimum[5], maximum[5]); *analog_rt = true; }
        return;
    }
    for (uint8_t i = 2; i < 6; ++i) {
        if (!present[i]) continue;
        if (eightbitdo_axis_roles[i] == 1) state->rx = dinput_axis(values[i]);
        else if (eightbitdo_axis_roles[i] == 2) state->ry = dinput_axis(values[i]);
        else if (eightbitdo_axis_roles[i] == 3) { state->lt = generic_trigger(values[i], minimum[i], maximum[i]); *analog_lt = true; }
        else if (eightbitdo_axis_roles[i] == 4) { state->rt = generic_trigger(values[i], minimum[i], maximum[i]); *analog_rt = true; }
    }
}

static bool parse_8bitdo_enhanced(const uint8_t* report, uint16_t length) {
    bool pro3 = strstr(identity.name, "Pro 3") || strstr(identity.name, "PRO 3") || strstr(identity.name, "pro 3");
    if (!pro3 || !report || length < 15) return false;
    if (report[0] != 0x01 && report[0] != 0x03 && report[0] != 0x04) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    s.dpad = hat_to_dpad(report[1]);
    s.x = dinput_axis(report[2]);
    s.y = dinput_axis(report[3]);
    s.rx = dinput_axis(report[4]);
    s.ry = dinput_axis(report[5]);
    s.rt = report[6];
    s.lt = report[7];
    if (report[8] & 0x01) s.buttons |= 1u << 1;
    if (report[8] & 0x02) s.buttons |= 1u << 0;
    if (report[8] & 0x08) s.buttons |= 1u << 3;
    if (report[8] & 0x10) s.buttons |= 1u << 2;
    if (report[8] & 0x40) s.buttons |= 1u << 4;
    if (report[8] & 0x80) s.buttons |= 1u << 5;
    if (report[9] & 0x04) s.buttons |= 1u << 8;
    if (report[9] & 0x08) s.buttons |= 1u << 9;
    if (report[9] & 0x20) s.buttons |= 1u << 10;
    if (report[9] & 0x40) s.buttons |= 1u << 11;
    if (report[9] & 0x10) s.buttons |= 1u << 12;
    if (report[10] & 0x01) s.buttons |= 1u << 16;
    if (report[10] & 0x02) s.buttons |= 1u << 17;
    if (report[8] & 0x20) s.buttons |= 1u << 18;
    if (report[8] & 0x04) s.buttons |= 1u << 19;
    if (s.lt > 8) s.buttons |= 1u << 6;
    if (s.rt > 8) s.buttons |= 1u << 7;
    uint8_t level = report[14] & 0x7fu;
    uint8_t charge = report[14] >> 7;
    if (level <= 100) bb_state_update_battery(true, level, level == 100 ? 3 : charge ? 2 : 1);
    bb_controller_capabilities_t caps = bb_controller_capabilities(identity.kind, identity.name);
    bb_state_update_button_capabilities(caps.buttons);
    submit_state(&s);
    return true;
}

static bool parse_generic(const uint8_t* report, uint16_t length) {
    if (!hid_descriptor || !hid_descriptor_len || !report || !length) return false;
    btstack_hid_parser_t parser;
    bb_input_state_t s;
    bb_input_clear(&s);
    uint32_t button_capabilities = 0;
    bool arcade = identity.kind == BB_CONTROLLER_8BITDO && eightbitdo_name_has("arcade stick");
    bool m30 = identity.kind == BB_CONTROLLER_8BITDO && eightbitdo_name_has("m30");
    bool micro = identity.kind == BB_CONTROLLER_8BITDO && eightbitdo_name_has("micro");
    if (arcade || m30 || micro) {
        bool numbered = btstack_hid_report_id_declared(hid_descriptor, hid_descriptor_len);
        int expected = btstack_hid_get_report_size_for_id(numbered ? report[0] : HID_REPORT_ID_UNDEFINED, HID_REPORT_TYPE_INPUT, hid_descriptor, hid_descriptor_len);
        if (expected <= 0 || length < expected + numbered) return false;
    }
    bool home_present = false;
    bool home_pressed = false;
    bool primary_input = false;
    int32_t axes[6] = {0};
    bool axis_present[6] = {false};
    int32_t axis_minimum[6] = {0};
    int32_t axis_maximum[6] = {0};
    bool analog_lt = false;
    bool analog_rt = false;
    btstack_hid_parser_init(&parser, hid_descriptor, hid_descriptor_len, HID_REPORT_TYPE_INPUT, report, length);
    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t page = 0;
        uint16_t usage = 0;
        int32_t value = 0;
        int32_t minimum = m30 ? parser.descriptor_usage_item.global_logical_minimum : parser.usage_iterator.global_logical_minimum;
        int32_t maximum = parser.usage_iterator.global_logical_maximum;
        btstack_hid_parser_get_field(&parser, &page, &usage, &value);
        if (arcade && ((page == HID_USAGE_PAGE_GENERIC_DESKTOP && usage == 0x85) || (page == 0x0c && usage == 0x0223))) {
            home_present = true;
            home_pressed |= value != 0;
            continue;
        }
        if (page == HID_USAGE_PAGE_BUTTON) {
            if (arcade && (usage == 11 || usage == 13)) {
                home_present = true;
                home_pressed |= value != 0;
            } else primary_input = true;
            if (usage >= 1 && usage <= BB_BUTTON_COUNT) {
                button_capabilities |= 1u << (usage - 1);
                if (value) s.buttons |= 1u << (usage - 1);
            }
            continue;
        }
        if (page == HID_USAGE_PAGE_GENERIC_DEVICE && usage == HID_USAGE_BATTERY_STRENGTH) {
            uint8_t percent = value <= 0 ? 0 : value <= 100 ? (uint8_t)value : value <= 255 ? (uint8_t)((value * 100) / 255) : 100;
            bb_state_update_battery(true, percent, 1);
            continue;
        }
        if (page != HID_USAGE_PAGE_GENERIC_DESKTOP) continue;
        if (usage >= HID_USAGE_X && usage <= HID_USAGE_HAT) primary_input = true;
        if (identity.kind == BB_CONTROLLER_8BITDO) {
            int index = -1;
            if (usage == HID_USAGE_X) index = 0;
            else if (usage == HID_USAGE_Y) index = 1;
            else if (usage == HID_USAGE_Z) index = 2;
            else if (usage == HID_USAGE_RX) index = 3;
            else if (usage == HID_USAGE_RY) index = 4;
            else if (usage == HID_USAGE_RZ) index = 5;
            if (index >= 0) {
                axes[index] = value;
                axis_minimum[index] = minimum;
                axis_maximum[index] = maximum;
                axis_present[index] = true;
                continue;
            }
        }
        switch (usage) {
            case HID_USAGE_X: s.x = hid_axis(value, minimum, maximum); break;
            case HID_USAGE_Y: s.y = hid_axis(value, minimum, maximum); break;
            case HID_USAGE_RX: s.rx = hid_axis(value, minimum, maximum); break;
            case HID_USAGE_RY: s.ry = hid_axis(value, minimum, maximum); break;
            case HID_USAGE_Z: s.lt = generic_trigger(value, minimum, maximum); analog_lt = true; break;
            case HID_USAGE_RZ: s.rt = generic_trigger(value, minimum, maximum); analog_rt = true; break;
            case HID_USAGE_HAT:
                if (arcade || m30 || micro) s.dpad = value >= minimum && value <= maximum ? hat_to_dpad(value - minimum) : 0;
                else s.dpad = hat_to_dpad(value);
                break;
            default: break;
        }
    }
    if (identity.kind == BB_CONTROLLER_8BITDO) {
        if (m30 || micro) {
            const uint8_t pairs[][2] = {{0,1},{2,5},{3,4}};
            for (uint8_t pair = 0; pair < 3; ++pair) {
                uint8_t x = pairs[pair][0];
                uint8_t y = pairs[pair][1];
                if (!axis_present[x] || !axis_present[y]) continue;
                int16_t dx = hid_axis(axes[x], axis_minimum[x], axis_maximum[x]);
                int16_t dy = hid_axis(axes[y], axis_minimum[y], axis_maximum[y]);
                if (dx < -16384) s.dpad |= 4;
                if (dx > 16384) s.dpad |= 8;
                if (dy < -16384) s.dpad |= 1;
                if (dy > 16384) s.dpad |= 2;
            }
        } else apply_8bitdo_axes(&s, axes, axis_present, axis_minimum, axis_maximum, &analog_lt, &analog_rt);
        normalize_8bitdo_buttons(&s, &button_capabilities, analog_lt, analog_rt);
    }
    if (identity.kind == BB_CONTROLLER_8BITDO && (eightbitdo_name_has("m30") || eightbitdo_name_has("arcade stick"))) {
        if (s.lt > 8) s.buttons |= 1u << 6;
        if (s.rt > 8) s.buttons |= 1u << 7;
        if (analog_lt) button_capabilities |= 1u << 6;
        if (analog_rt) button_capabilities |= 1u << 7;
        s.lt = s.rt = 0;
    }
    if (arcade && home_present) {
        if (!primary_input && have_last_input_state) s = last_input_state;
        s.buttons &= ~(1u << 12);
        if (home_pressed) s.buttons |= 1u << 12;
        button_capabilities |= 1u << 12;
    } else if (arcade && have_last_input_state) {
        s.buttons |= last_input_state.buttons & (1u << 12);
    }
    s.buttons = canonicalize_buttons(s.buttons);
    bb_state_update_button_capabilities(canonicalize_buttons(button_capabilities));
    submit_state(&s);
    return true;
}

static int16_t battery_report_for_type(hid_report_type_t type) {
    if (!hid_descriptor || !hid_descriptor_len) return -1;
    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, hid_descriptor, hid_descriptor_len, type);
    while (btstack_hid_usage_iterator_has_more(&iterator)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);
        if (item.usage_page == HID_USAGE_PAGE_GENERIC_DEVICE && item.usage == HID_USAGE_BATTERY_STRENGTH) {
            return item.report_id == HID_REPORT_ID_UNDEFINED ? 0 : (int16_t)item.report_id;
        }
    }
    return -1;
}

int16_t bb_driver_battery_report_id(void) {
    int16_t id = battery_report_for_type(HID_REPORT_TYPE_INPUT);
    return id >= 0 ? id : battery_report_for_type(HID_REPORT_TYPE_FEATURE);
}

uint8_t bb_driver_battery_report_type(void) {
    if (battery_report_for_type(HID_REPORT_TYPE_INPUT) >= 0) return HID_REPORT_TYPE_INPUT;
    return battery_report_for_type(HID_REPORT_TYPE_FEATURE) >= 0 ? HID_REPORT_TYPE_FEATURE : 0;
}

bool bb_driver_handle_battery_report(const uint8_t* report, uint16_t length) {
    int16_t id = bb_driver_battery_report_id();
    hid_report_type_t type = (hid_report_type_t)bb_driver_battery_report_type();
    if (id < 0 || !report || !length || (id && report[0] != id)) return false;
    int expected = btstack_hid_get_report_size_for_id(id ? (uint16_t)id : HID_REPORT_ID_UNDEFINED, type, hid_descriptor, hid_descriptor_len);
    if (expected <= 0 || length < expected + (id != 0)) return false;
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, hid_descriptor, hid_descriptor_len, type, report, length);
    bool found = false;
    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t page, usage;
        int32_t value;
        int32_t minimum = parser.usage_iterator.global_logical_minimum;
        int32_t maximum = parser.usage_iterator.global_logical_maximum;
        btstack_hid_parser_get_field(&parser, &page, &usage, &value);
        if (page != HID_USAGE_PAGE_GENERIC_DEVICE || usage != HID_USAGE_BATTERY_STRENGTH || maximum <= minimum) continue;
        int64_t percent = ((int64_t)value - minimum) * 100 / ((int64_t)maximum - minimum);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        bb_state_update_battery(true, (uint8_t)percent, percent == 100 ? 3 : 1);
        found = true;
    }
    return found;
}

static void record_input_report(const uint8_t* report, uint16_t length) {
    diagnostic_report_original_length = length;
    diagnostic_report_length = length < sizeof(diagnostic_report) ? length : sizeof(diagnostic_report);
    memcpy(diagnostic_report, report, diagnostic_report_length);
    ++diagnostic_report_sequence;
}

static bool parse_8bitdo_arcade(const uint8_t* report, uint16_t length) {

    if (report[0] == 0x30) return parse_switch_full(report, length);
    if (report[0] == 0x3f) return parse_switch_simple(report, length);
    if (report[0] == 0x21) return true;
    int16_t battery_id = bb_driver_battery_report_id();
    if (battery_id > 0 && bb_driver_battery_report_type() == HID_REPORT_TYPE_INPUT && report[0] == battery_id) {
        btstack_hid_usage_iterator_t iterator;
        btstack_hid_usage_iterator_init(&iterator, hid_descriptor, hid_descriptor_len, HID_REPORT_TYPE_INPUT);
        bool battery_only = true;
        while (btstack_hid_usage_iterator_has_more(&iterator)) {
            btstack_hid_usage_item_t item;
            btstack_hid_usage_iterator_get_item(&iterator, &item);
            if (item.report_id == (uint16_t)battery_id &&
                (item.usage_page != HID_USAGE_PAGE_GENERIC_DEVICE || item.usage != HID_USAGE_BATTERY_STRENGTH)) battery_only = false;
        }
        if (battery_only) return bb_driver_handle_battery_report(report, length);
    }
    return parse_generic(report, length);
}

void bb_driver_reset(void) {
    memset(&identity, 0, sizeof(identity));
    hid_descriptor = NULL;
    hid_descriptor_len = 0;
    diagnostic_report_length = 0;
    diagnostic_report_original_length = 0;
    diagnostic_report_sequence = 0;
    diagnostic_battery_response_length = 0;
    diagnostic_battery_status = -1;
    diagnostic_battery_request_status = -1;
    diagnostic_battery_request_count = 0;
    have_early_arcade_battery_report = false;
    memset(&last_input_state, 0, sizeof(last_input_state));
    have_last_input_state = false;
    memset(eightbitdo_axis_roles, 0, sizeof(eightbitdo_axis_roles));
    eightbitdo_axis_roles_ready = false;
    memset(switch2_cal_sum, 0, sizeof(switch2_cal_sum));
    memset(switch2_cal_center, 0, sizeof(switch2_cal_center));
    switch2_cal_samples = 0;
    for (unsigned i = 0; i < 2; ++i) {
        n64_center[i] = 2048;
        n64_below[i] = n64_above[i] = 2047;
    }
    bb_state_update_battery(false, 0, 0);
    bb_state_set_rumble_supported(false);
}

void bb_driver_set_identity(const bb_controller_identity_t* value) {
    bool preserve_battery = value && identity.kind == BB_CONTROLLER_8BITDO && eightbitdo_name_has("arcade stick") &&
        value->kind == BB_CONTROLLER_8BITDO && memcmp(identity.address, value->address, sizeof(identity.address)) == 0;
    if (!preserve_battery) have_early_arcade_battery_report = false;
    if (value) identity = *value;
    else memset(&identity, 0, sizeof(identity));
    memset(eightbitdo_axis_roles, 0, sizeof(eightbitdo_axis_roles));
    eightbitdo_axis_roles_ready = false;
    memset(switch2_cal_sum, 0, sizeof(switch2_cal_sum));
    memset(switch2_cal_center, 0, sizeof(switch2_cal_center));
    switch2_cal_samples = 0;
    for (unsigned i = 0; i < 2; ++i) {
        n64_center[i] = 2048;
        n64_below[i] = n64_above[i] = 2047;
    }
    if (!preserve_battery) bb_state_update_battery(false, 0, 0);
    bool rumble = identity.kind == BB_CONTROLLER_DUALSENSE || identity.kind == BB_CONTROLLER_DUALSENSE_EDGE || identity.kind == BB_CONTROLLER_DUALSHOCK4 || identity.kind == BB_CONTROLLER_DUALSHOCK3 || identity.kind == BB_CONTROLLER_SWITCH_PRO || identity.kind == BB_CONTROLLER_JOYCON_LEFT || identity.kind == BB_CONTROLLER_JOYCON_RIGHT || identity.kind == BB_CONTROLLER_WII_U_PRO || identity.kind == BB_CONTROLLER_8BITDO;
    if (identity.kind == BB_CONTROLLER_8BITDO && (eightbitdo_name_has("arcade stick") || eightbitdo_name_has("m30") || eightbitdo_name_has("micro"))) rumble = false;
    bb_state_set_rumble_supported(rumble);
}

void bb_driver_set_hid_descriptor(const uint8_t* descriptor, uint16_t length) {
    hid_descriptor = descriptor;
    hid_descriptor_len = length;
    if (have_early_arcade_battery_report && bb_driver_battery_report_id() == 4 && bb_driver_battery_report_type() == HID_REPORT_TYPE_INPUT) {
        bb_driver_handle_battery_report(early_arcade_battery_report, sizeof(early_arcade_battery_report));
    }
}

const bb_controller_identity_t* bb_driver_identity(void) {
    return &identity;
}

static bool descriptor_has(const uint8_t* descriptor, uint16_t length, const uint8_t* pattern, uint16_t pattern_len) {
    if (!descriptor || !pattern || !pattern_len || length < pattern_len) return false;
    for (uint16_t i = 0; i <= (uint16_t)(length - pattern_len); ++i) {
        if (memcmp(&descriptor[i], pattern, pattern_len) == 0) return true;
    }
    return false;
}

bool bb_driver_refine_identity_from_hid_descriptor(const uint8_t* descriptor, uint16_t length, bb_controller_identity_t* value) {
    if (!descriptor || !value || value->kind != BB_CONTROLLER_GENERIC || length < 32) return false;
    static const uint8_t switch_full[] = {0x85,0x30};
    static const uint8_t switch_simple[] = {0x85,0x3f};
    if (descriptor_has(descriptor, length, switch_full, sizeof(switch_full)) && descriptor_has(descriptor, length, switch_simple, sizeof(switch_simple))) {
        value->kind = BB_CONTROLLER_SWITCH_PRO;
        memcpy(value->name, "Switch Pro Controller", sizeof("Switch Pro Controller"));
        return true;
    }
    static const uint8_t ds5_output[] = {0x85,0x31,0x09,0x31,0x91,0x02};
    static const uint8_t ds5_output_alt[] = {0x85,0x31};
    static const uint8_t ds4_output[] = {0x85,0x11};
    static const uint8_t sony_gamepad[] = {0x05,0x01,0x09,0x05,0xa1,0x01};
    if (!descriptor_has(descriptor, length, sony_gamepad, sizeof(sony_gamepad))) return false;
    if (descriptor_has(descriptor, length, ds5_output, sizeof(ds5_output)) || (length >= 250 && descriptor_has(descriptor, length, ds5_output_alt, sizeof(ds5_output_alt)))) {
        value->vendor_id = 0x054c;
        value->product_id = 0x0ce6;
        value->kind = BB_CONTROLLER_DUALSENSE;
        memcpy(value->name, "DualSense", sizeof("DualSense"));
        return true;
    }
    if (descriptor_has(descriptor, length, ds4_output, sizeof(ds4_output))) {
        value->vendor_id = 0x054c;
        value->product_id = 0x09cc;
        value->kind = BB_CONTROLLER_DUALSHOCK4;
        memcpy(value->name, "DualShock 4", sizeof("DualShock 4"));
        return true;
    }
    return false;
}

bool bb_driver_handle_xbox_ble_report(const uint8_t* report, uint16_t length) {
    if (identity.kind != BB_CONTROLLER_XBOX_SERIES && identity.kind != BB_CONTROLLER_XBOX_ONE) return false;
    if (!report || !length) return false;
    if (length != 17 || report[0] != 1) return false;
    record_input_report(report, length);
    bb_input_state_t state;
    bb_input_clear(&state);
    int16_t* axes[4] = {&state.x, &state.y, &state.rx, &state.ry};
    for (uint8_t i = 0; i < 4; ++i) {
        uint16_t value = little_endian_read_16(report, 1 + i * 2);
        int32_t delta = (int32_t)value - 32768;
        int32_t magnitude = delta < 0 ? -delta : delta;
        const int32_t neutral = 512;
        if (magnitude <= neutral) {
            *axes[i] = 0;
        } else {
            int32_t limit = delta < 0 ? 32768 : 32767;
            int32_t scaled = (magnitude - neutral) * limit / (limit - neutral);
            *axes[i] = (int16_t)(delta < 0 ? -scaled : scaled);
        }
    }
    state.lt = generic_trigger(little_endian_read_16(report, 9), 0, 1023);
    state.rt = generic_trigger(little_endian_read_16(report, 11), 0, 1023);
    uint8_t hat = report[13];
    if (hat >= 1 && hat <= 8) state.dpad = hat_to_dpad(hat - 1);
    uint16_t buttons = little_endian_read_16(report, 14);
    const uint8_t source[11] = {0, 1, 3, 4, 6, 7, 10, 11, 13, 14, 12};
    const uint8_t target[11] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12};
    for (uint8_t i = 0; i < 11; ++i) {
        if (buttons & (1u << source[i])) state.buttons |= 1u << target[i];
    }
    if (state.lt > 10) state.buttons |= 1u << 6;
    if (state.rt > 10) state.buttons |= 1u << 7;
    uint32_t capabilities = 0x1fff;
    if (identity.kind == BB_CONTROLLER_XBOX_SERIES) {
        capabilities |= 1u << 13;
        if (report[16] & 1) state.buttons |= 1u << 13;
    }
    bb_state_update_button_capabilities(capabilities);
    submit_state(&state);
    return true;
}

bool bb_driver_handle_hid_report(const uint8_t* report, uint16_t length) {
    if (!report || !length) return false;
    record_input_report(report, length);
    if (identity.kind == BB_CONTROLLER_8BITDO && eightbitdo_name_has("arcade stick") && length == 2 && report[0] == 4) {
        memcpy(early_arcade_battery_report, report, sizeof(early_arcade_battery_report));
        have_early_arcade_battery_report = true;
    }
    uint8_t id = report[0];
    if (identity.kind == BB_CONTROLLER_DUALSENSE || identity.kind == BB_CONTROLLER_DUALSENSE_EDGE) {
        if (id == 0x31) return parse_dualsense_bt_full(report, length);
        if (id == 0x01) return parse_dualsense_bt_simple(report, length);
        return false;
    }
    if (identity.kind == BB_CONTROLLER_DUALSHOCK4) {
        if (parse_ds4_bt(report, length)) return true;
    }
    if (identity.kind == BB_CONTROLLER_DUALSHOCK3) {
        if (parse_ds3_bt(report, length)) return true;
    }
    if (identity.kind == BB_CONTROLLER_WII_U_PRO) {
        if (id == 0x34) return parse_wiiu_pro(report, length);
        return false;
    }
    if (identity.kind == BB_CONTROLLER_STADIA && id == 0x03) {
        if (parse_stadia_aux(report, length)) return true;
    }
    if (identity.kind == BB_CONTROLLER_NSO_N64) {
        if (id == 0x30) return parse_n64_full(report, length);
        if (id == 0x3f) return parse_n64_simple(report, length);
        return id == 0x21;
    }
    if (identity.kind == BB_CONTROLLER_8BITDO && eightbitdo_name_has("arcade stick")) return parse_8bitdo_arcade(report, length);
    if (identity.kind == BB_CONTROLLER_8BITDO && parse_8bitdo_enhanced(report, length)) return true;
    if (identity.kind == BB_CONTROLLER_NSO_NES || identity.kind == BB_CONTROLLER_NSO_SNES || identity.kind == BB_CONTROLLER_NSO_GENESIS) {
        if (id == 0x21) return true;
        return parse_nso_retro(report, length);
    }
    if (identity.kind == BB_CONTROLLER_SWITCH_PRO || identity.kind == BB_CONTROLLER_JOYCON_LEFT || identity.kind == BB_CONTROLLER_JOYCON_RIGHT || identity.kind == BB_CONTROLLER_8BITDO) {
        if (id == 0x30) return parse_switch_full(report, length);
        if (id == 0x3f) return parse_switch_simple(report, length);
        if (id == 0x21) return true;
    }
    return parse_generic(report, length);
}

bool bb_driver_handle_nintendo_ble_report(const uint8_t* report, uint16_t length, bool gamecube, bool switch2_pro) {
    if (!report || length < 14) return false;
    bb_input_state_t s;
    bb_input_clear(&s);
    if (switch2_pro) {
        if (length < 11) return false;
        uint8_t power = report[1];
        uint8_t a = report[2];
        uint8_t b = report[3];
        uint8_t c = report[4];
        if (a & 0x01) s.buttons |= 1u << 0;
        if (a & 0x02) s.buttons |= 1u << 1;
        if (a & 0x04) s.buttons |= 1u << 2;
        if (a & 0x08) s.buttons |= 1u << 3;
        if (b & 0x10) s.buttons |= 1u << 4;
        if (a & 0x10) s.buttons |= 1u << 5;
        if (b & 0x20) s.buttons |= 1u << 6;
        if (a & 0x20) s.buttons |= 1u << 7;
        if (b & 0x40) s.buttons |= 1u << 8;
        if (a & 0x40) s.buttons |= 1u << 9;
        if (b & 0x80) s.buttons |= 1u << 10;
        if (a & 0x80) s.buttons |= 1u << 11;
        if (c & 0x01) s.buttons |= 1u << 12;
        if (c & 0x02) s.buttons |= 1u << 13;
        if (c & 0x08) s.buttons |= 1u << 14;
        if (c & 0x04) s.buttons |= 1u << 15;
        if (c & 0x10) s.buttons |= 1u << 16;
        if (b & 0x08) s.dpad |= BB_DPAD_UP;
        if (b & 0x01) s.dpad |= BB_DPAD_DOWN;
        if (b & 0x04) s.dpad |= BB_DPAD_LEFT;
        if (b & 0x02) s.dpad |= BB_DPAD_RIGHT;
        uint16_t raw_lx = unpack12a(&report[5]);
        uint16_t raw_ly = unpack12b(&report[5]);
        uint16_t raw_rx = unpack12a(&report[8]);
        uint16_t raw_ry = unpack12b(&report[8]);
        if (switch2_cal_samples < 4) {
            switch2_cal_sum[0] += raw_lx;
            switch2_cal_sum[1] += raw_ly;
            switch2_cal_sum[2] += raw_rx;
            switch2_cal_sum[3] += raw_ry;
            switch2_cal_samples++;
            if (switch2_cal_samples == 4) {
                for (uint8_t i = 0; i < 4; ++i) switch2_cal_center[i] = (uint16_t)(switch2_cal_sum[i] / 4u);
            }
            return true;
        }
        s.x = axis12_quantized(raw_lx, switch2_cal_center[0], 1610);
        s.y = (int16_t)-axis12_quantized(raw_ly, switch2_cal_center[1], 1610);
        s.rx = axis12_quantized(raw_rx, switch2_cal_center[2], 1610);
        s.ry = (int16_t)-axis12_quantized(raw_ry, switch2_cal_center[3], 1610);
        s.lt = (b & 0x20) ? 255 : 0;
        s.rt = (a & 0x20) ? 255 : 0;
        uint8_t level = (uint8_t)((power >> 2) & 0x0f);
        if (level > 9) level = 9;
        uint8_t percent = (uint8_t)((level * 100u + 4u) / 9u);
        uint8_t state = (power & 0x02u) ? 2u : ((level == 9 && (power & 0x01u)) ? 3u : 1u);
        bb_state_update_battery(true, percent, state);
        bb_state_update_button_capabilities(0x0001ffffu);
        submit_state(&s);
        return true;
    }
    uint8_t a = report[2];
    uint8_t b = report[3];
    uint8_t c = report[4];
    if (a & 0x02) s.buttons |= 1u << 0;
    if (a & 0x01) s.buttons |= 1u << 1;
    if (a & 0x08) s.buttons |= 1u << 2;
    if (a & 0x04) s.buttons |= 1u << 3;
    if (gamecube) {
        if (b & 0x20) s.buttons |= 1u << 4;
        if (a & 0x20) s.buttons |= 1u << 5;
        if (b & 0x10) s.buttons |= 1u << 6;
        if (a & 0x10) s.buttons |= 1u << 7;
        if (c & 0x10) s.buttons |= 1u << 14;
    } else {
        if (b & 0x10) s.buttons |= 1u << 4;
        if (a & 0x10) s.buttons |= 1u << 5;
        if (b & 0x20) s.buttons |= 1u << 6;
        if (a & 0x20) s.buttons |= 1u << 7;
        if (c & 0x08) s.buttons |= 1u << 14;
        if (c & 0x04) s.buttons |= 1u << 15;
    }
    if (b & 0x80) s.buttons |= 1u << 8;
    if (a & 0x80) s.buttons |= 1u << 9;
    if (b & 0x40) s.buttons |= 1u << 10;
    if (a & 0x40) s.buttons |= 1u << 11;
    if (c & 0x01) s.buttons |= 1u << 12;
    if (c & 0x02) s.buttons |= 1u << 13;
    if (b & 0x08) s.dpad |= BB_DPAD_UP;
    if (b & 0x01) s.dpad |= BB_DPAD_DOWN;
    if (b & 0x04) s.dpad |= BB_DPAD_LEFT;
    if (b & 0x02) s.dpad |= BB_DPAD_RIGHT;
    s.x = axis12(unpack12a(&report[5]));
    s.y = (int16_t)-axis12(unpack12b(&report[5]));
    s.rx = axis12(unpack12a(&report[8]));
    s.ry = (int16_t)-axis12(unpack12b(&report[8]));
    if (gamecube) {
        s.lt = report[12];
        s.rt = report[13];
    } else {
        s.lt = (b & 0x20) ? 255 : 0;
        s.rt = (a & 0x20) ? 255 : 0;
    }
    if (gamecube) {
        s.buttons &= 0x000072ffu;
        bb_state_update_button_capabilities(0x000072ffu);
    }
    submit_state(&s);
    return true;
}

void bb_driver_record_battery_request(uint8_t status) {
    diagnostic_battery_request_status = status;
    ++diagnostic_battery_request_count;
}

void bb_driver_record_battery_response(uint8_t status, const uint8_t* report, uint16_t length) {
    diagnostic_battery_status = status;
    diagnostic_battery_response_length = report ? (length < sizeof(diagnostic_battery_response) ? length : sizeof(diagnostic_battery_response)) : 0;
    if (diagnostic_battery_response_length) memcpy(diagnostic_battery_response, report, diagnostic_battery_response_length);
}

bool bb_driver_hid_diagnostics(uint16_t offset, char* out, size_t size) {
    if (!out || size < 1024 || offset > hid_descriptor_len) return false;
    char descriptor_hex[257];
    char report_hex[129];
    char battery_hex[65];
    uint16_t count = hid_descriptor_len - offset;
    if (count > 128) count = 128;
    static const char digits[] = "0123456789abcdef";
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t value = hid_descriptor[offset + i];
        descriptor_hex[2 * i] = digits[value >> 4];
        descriptor_hex[2 * i + 1] = digits[value & 15];
    }
    descriptor_hex[2 * count] = 0;
    for (uint16_t i = 0; i < diagnostic_report_length; ++i) {
        uint8_t value = diagnostic_report[i];
        report_hex[2 * i] = digits[value >> 4];
        report_hex[2 * i + 1] = digits[value & 15];
    }
    report_hex[2 * diagnostic_report_length] = 0;
    for (uint8_t i = 0; i < diagnostic_battery_response_length; ++i) {
        uint8_t value = diagnostic_battery_response[i];
        battery_hex[2 * i] = digits[value >> 4];
        battery_hex[2 * i + 1] = digits[value & 15];
    }
    battery_hex[2 * diagnostic_battery_response_length] = 0;
    snprintf(out, size, "{\"vid\":%u,\"pid\":%u,\"kind\":%u,\"descriptor_length\":%u,\"offset\":%u,\"descriptor_hex\":\"%s\",\"report_hex\":\"%s\",\"report_length\":%u,\"report_sequence\":%lu,\"battery_report_id\":%d,\"battery_report_type\":%u,\"battery_response_status\":%d,\"battery_response_hex\":\"%s\",\"battery_request_status\":%d,\"battery_request_count\":%lu}", identity.vendor_id, identity.product_id, (unsigned)identity.kind, hid_descriptor_len, offset, descriptor_hex, report_hex, diagnostic_report_original_length, (unsigned long)diagnostic_report_sequence, bb_driver_battery_report_id(), bb_driver_battery_report_type(), diagnostic_battery_status, battery_hex, diagnostic_battery_request_status, (unsigned long)diagnostic_battery_request_count);
    return true;
}
