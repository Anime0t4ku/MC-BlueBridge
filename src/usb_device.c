#include "usb_device.h"
#include "usb_descriptors.h"
#include "xinput_device.h"
#include "hid_feedback.h"
#include "cdc_protocol.h"
#include "bt_host.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include <string.h>

static critical_section_t usb_lock;
static bb_gamepad_report_t pending;
static bool dirty;
static bool reenumerate;
static volatile bool usb_mounted;
static uint32_t switch_last_send;

void bb_usb_submit(const bb_gamepad_report_t* report) {
    critical_section_enter_blocking(&usb_lock);
    pending = *report;
    dirty = true;
    critical_section_exit(&usb_lock);
}

bool bb_usb_ready(void) {
    return usb_mounted;
}

void bb_usb_request_reenumeration(void) {
    critical_section_enter_blocking(&usb_lock);
    reenumerate = true;
    critical_section_exit(&usb_lock);
}

static uint8_t hat_to_xinput(uint8_t hat) {
    switch (hat) {
        case 1: return 0x01;
        case 2: return 0x09;
        case 3: return 0x08;
        case 4: return 0x0a;
        case 5: return 0x02;
        case 6: return 0x06;
        case 7: return 0x04;
        case 8: return 0x05;
        default: return 0;
    }
}

static uint16_t xinput_buttons(const bb_gamepad_report_t* r) {
    uint16_t b = hat_to_xinput(r->hat);
    if (r->buttons & (1u << 8)) b |= 0x0020;
    if (r->buttons & (1u << 9)) b |= 0x0010;
    if (r->buttons & (1u << 10)) b |= 0x0040;
    if (r->buttons & (1u << 11)) b |= 0x0080;
    if (r->buttons & (1u << 4)) b |= 0x0100;
    if (r->buttons & (1u << 5)) b |= 0x0200;
    if (r->buttons & (1u << 12)) b |= 0x0400;
    if (r->buttons & (1u << 0)) b |= 0x1000;
    if (r->buttons & (1u << 1)) b |= 0x2000;
    if (r->buttons & (1u << 2)) b |= 0x4000;
    if (r->buttons & (1u << 3)) b |= 0x8000;
    return b;
}

static void put_i16le(uint8_t* p, int16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static int16_t xinput_axis(int8_t v, bool invert) {
    int32_t x = v;
    if (invert) x = -x;
    x *= 256;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return (int16_t)x;
}

static bool send_xinput(const bb_gamepad_report_t* r) {
    if (!bb_xinput_ready()) return false;
    uint8_t out[20] = {0};
    uint16_t b = xinput_buttons(r);
    out[0] = 0x00;
    out[1] = 0x14;
    out[2] = (uint8_t)b;
    out[3] = (uint8_t)(b >> 8);
    out[4] = r->lt ? r->lt : (r->buttons & (1u << 6)) ? 255 : 0;
    out[5] = r->rt ? r->rt : (r->buttons & (1u << 7)) ? 255 : 0;
    put_i16le(&out[6], xinput_axis(r->x, false));
    put_i16le(&out[8], xinput_axis(r->y, true));
    put_i16le(&out[10], xinput_axis(r->rx, false));
    put_i16le(&out[12], xinput_axis(r->ry, true));
    return bb_xinput_send(out);
}

static bool send_switch_state(const bb_gamepad_report_t* r) {
    if (!tud_hid_ready()) return false;
    uint16_t buttons = r->buttons & 0x3ff0u;
    if (r->buttons & 1u) buttons |= 2u;
    if (r->buttons & 2u) buttons |= 4u;
    if (r->buttons & 4u) buttons |= 1u;
    if (r->buttons & 8u) buttons |= 8u;
    if (r->lt >= 128) buttons |= 0x40;
    if (r->rt >= 128) buttons |= 0x80;
    uint8_t out[8] = {
        (uint8_t)buttons, (uint8_t)(buttons >> 8),
        r->hat >= 1 && r->hat <= 8 ? (uint8_t)(r->hat - 1) : 8,
        (uint8_t)((int)r->x + 128), (uint8_t)((int)r->y + 128),
        (uint8_t)((int)r->rx + 128), (uint8_t)((int)r->ry + 128), 0
    };
    return tud_hid_report(0, out, sizeof(out));
}

static void send_pending(void) {
    bb_output_mode_t mode = bb_output_mode();
    bb_gamepad_report_t report;
    bool send = false;
    critical_section_enter_blocking(&usb_lock);
    if (dirty) {
        report = pending;
        dirty = false;
        send = true;
    } else if (mode == BB_OUTPUT_SWITCH && (uint32_t)(to_ms_since_boot(get_absolute_time()) - switch_last_send) >= 15u) {
        report = pending;
        send = true;
    }
    critical_section_exit(&usb_lock);
    if (!send) return;
    bool ok = false;
    if (mode == BB_OUTPUT_XINPUT) ok = send_xinput(&report);
    else if (mode == BB_OUTPUT_SWITCH) {
        ok = send_switch_state(&report);
        if (ok) switch_last_send = to_ms_since_boot(get_absolute_time());
    } else if (tud_hid_ready()) {
        hid_gamepad_report_t gamepad = {0};
        gamepad.x = report.x;
        gamepad.y = report.y;
        gamepad.z = report.rx;
        gamepad.rz = report.ry;
        gamepad.rx = (int8_t)((int)report.lt - 128);
        gamepad.ry = (int8_t)((int)report.rt - 128);
        gamepad.hat = report.hat;
        gamepad.buttons = report.buttons;
        ok = tud_hid_report(REPORT_ID_GAMEPAD, &gamepad, sizeof(gamepad));
    }
    bb_state_note_usb_report(ok);
    if (!ok) {
        critical_section_enter_blocking(&usb_lock);
        dirty = true;
        critical_section_exit(&usb_lock);
    }
}

static void handle_reenumeration(void) {
    bool requested = false;
    critical_section_enter_blocking(&usb_lock);
    if (reenumerate) {
        reenumerate = false;
        requested = true;
    }
    critical_section_exit(&usb_lock);
    if (!requested) return;
    usb_mounted = false;
    bb_state_set_usb_ready(false);
    bb_feedback_reset();
    tud_disconnect();
    sleep_ms(120);
    switch_last_send = 0;
    tud_connect();
}

void bb_usb_core1(void) {
    flash_safe_execute_core_init();
    critical_section_init(&usb_lock);
    tusb_init();
    while (true) {
        if (bb_output_mode() == BB_OUTPUT_MISTER) bb_feedback_poll(to_ms_since_boot(get_absolute_time()));
        tud_task();
        bb_cdc_poll();
        handle_reenumeration();
        send_pending();
        bb_config_flush_pending();
        tight_loop_contents();
    }
}

void tud_mount_cb(void) {
    bb_feedback_reset();
    usb_mounted = true;
    bb_state_set_usb_ready(true);
    critical_section_enter_blocking(&usb_lock);
    dirty = true;
    critical_section_exit(&usb_lock);
}

void tud_umount_cb(void) {
    bb_feedback_reset();
    usb_mounted = false;
    bb_state_set_usb_ready(false);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    bb_feedback_stop();
}

void tud_resume_cb(void) {
    usb_mounted = tud_mounted();
    bb_state_set_usb_ready(usb_mounted);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    if (instance != 0 || bb_output_mode() != BB_OUTPUT_MISTER) return 0;
    return bb_feedback_get(report_id, report_type == HID_REPORT_TYPE_FEATURE, buffer, reqlen);
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (instance != 0 || bb_output_mode() != BB_OUTPUT_MISTER || !buffer) return;
    if (report_type != HID_REPORT_TYPE_OUTPUT && report_type != HID_REPORT_TYPE_FEATURE) return;
    if (!report_id) {
        if (!bufsize) return;
        report_id = *buffer++;
        --bufsize;
    }
    bb_feedback_set(report_id, report_type == HID_REPORT_TYPE_FEATURE, buffer, bufsize);
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    static const uint8_t compatible_id[40] = {
        40,0,0,0,0,1,4,0,1,0,0,0,0,0,0,0,
        0,1,'X','U','S','B','1','0',0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0
    };
    if (bb_output_mode() != BB_OUTPUT_XINPUT || request->bmRequestType != 0xc0 ||
        request->bRequest != 0x20 || request->wIndex != 4 || request->wValue != 0) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;
    return tud_control_xfer(rhport, request, (void*)compatible_id, sizeof(compatible_id));
}
