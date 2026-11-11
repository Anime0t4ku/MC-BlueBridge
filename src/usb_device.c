#include "usb_device.h"
#include "usb_descriptors.h"
#include "xinput_device.h"
#include "dualsense_device.h"
#include "xbox360_auth.h"
#include "hid_feedback.h"
#include "cdc_protocol.h"
#include "bt_host.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include <string.h>

static critical_section_t usb_lock;
static volatile bool usb_lock_ready;
#define BB_USB_QUEUE_DEPTH 8
typedef struct {
    bb_gamepad_report_t reports[BB_USB_QUEUE_DEPTH];
    uint8_t head;
    uint8_t count;
} bb_usb_queue_t;
static bb_usb_queue_t usb_queues[BB_MAX_SLOTS];
static bb_gamepad_report_t latest_slots[BB_MAX_SLOTS];
static bool press_latch_enabled[BB_MAX_SLOTS];
static uint16_t press_latched_buttons[BB_MAX_SLOTS];
static uint16_t press_previous_buttons[BB_MAX_SLOTS];
static uint8_t usb_slot;
static bool reenumerate;
static volatile bool usb_mounted;
static uint32_t switch_last_send;
static uint32_t dualsense_last_send;

static bool same_digital_state(const bb_gamepad_report_t* a, const bb_gamepad_report_t* b) {
    return a->buttons == b->buttons && a->hat == b->hat;
}

static bool queue_push_locked(uint8_t slot, const bb_gamepad_report_t* report) {
    bb_usb_queue_t* q = &usb_queues[slot];
    latest_slots[slot] = *report;
    if (!usb_mounted) {
        q->head = 0;
        q->count = 1;
        q->reports[0] = *report;
        return false;
    }
    if (q->count) {
        uint8_t last = (uint8_t)((q->head + q->count - 1u) % BB_USB_QUEUE_DEPTH);
        if (same_digital_state(&q->reports[last], report)) {
            q->reports[last] = *report;
            return false;
        }
    }
    bool overflow = q->count == BB_USB_QUEUE_DEPTH;
    if (overflow) {
        q->head = (uint8_t)((q->head + 1u) % BB_USB_QUEUE_DEPTH);
        --q->count;
    }
    uint8_t tail = (uint8_t)((q->head + q->count) % BB_USB_QUEUE_DEPTH);
    q->reports[tail] = *report;
    ++q->count;
    return overflow;
}

void bb_usb_submit(const bb_gamepad_report_t* report) {
    uint8_t slot = bb_slot_current();
    bool overflow;
    critical_section_enter_blocking(&usb_lock);
    if (press_latch_enabled[slot]) {
        uint16_t buttons = report->buttons;
        press_latched_buttons[slot] |= (uint16_t)(buttons & (uint16_t)~press_previous_buttons[slot]);
        press_previous_buttons[slot] = buttons;
    }
    overflow = queue_push_locked(slot, report);
    critical_section_exit(&usb_lock);
    if (overflow) bb_state_note_slot_usb_report(slot, false);
}

void bb_usb_set_press_latch(bool enabled) {
    uint8_t slot = bb_slot_current();
    if (!usb_lock_ready) {
        press_latch_enabled[slot] = enabled;
        press_latched_buttons[slot] = 0;
        press_previous_buttons[slot] = 0;
        return;
    }
    critical_section_enter_blocking(&usb_lock);
    press_latch_enabled[slot] = enabled;
    press_latched_buttons[slot] = 0;
    press_previous_buttons[slot] = 0;
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
    if ((mode == BB_OUTPUT_XINPUT || mode == BB_OUTPUT_XBOX360) ? !bb_xinput_ready() : !tud_hid_n_ready(usb_slot)) return;
    bb_gamepad_report_t report;
    uint16_t latched_for_send = 0;
    bool send = false;
    bool queued = false;
    critical_section_enter_blocking(&usb_lock);
    bb_usb_queue_t* q = &usb_queues[usb_slot];
    if (q->count) {
        report = q->reports[q->head];
        q->head = (uint8_t)((q->head + 1u) % BB_USB_QUEUE_DEPTH);
        --q->count;
        send = true;
        queued = true;
    } else if (mode == BB_OUTPUT_XBOX360) {
        report = latest_slots[usb_slot];
        send = true;
    } else if (mode == BB_OUTPUT_DUALSENSE && (uint32_t)(to_ms_since_boot(get_absolute_time()) - dualsense_last_send) >= 4u) {
        // A real DualSense streams input reports continuously.
        report = latest_slots[usb_slot];
        send = true;
    } else if (mode == BB_OUTPUT_SWITCH && (uint32_t)(to_ms_since_boot(get_absolute_time()) - switch_last_send) >= 15u) {
        report = latest_slots[usb_slot];
        send = true;
    }
    if (send && press_latch_enabled[usb_slot]) {
        latched_for_send = press_latched_buttons[usb_slot];
        report.buttons |= latched_for_send;
        press_latched_buttons[usb_slot] &= (uint16_t)~latched_for_send;
    }
    critical_section_exit(&usb_lock);
    if (!send) return;
    bool ok = false;
    if (mode == BB_OUTPUT_XINPUT || mode == BB_OUTPUT_XBOX360) ok = send_xinput(&report);
    else if (mode == BB_OUTPUT_DUALSENSE) {
        uint8_t body[BB_DS5_INPUT_SIZE];
        bb_ds5_build_input(&report, body);
        ok = tud_hid_n_report(usb_slot, 0x01, body, sizeof(body));
        if (ok) dualsense_last_send = to_ms_since_boot(get_absolute_time());
    }
    else if (mode == BB_OUTPUT_SWITCH) {
        ok = send_switch_state(&report);
        if (ok) switch_last_send = to_ms_since_boot(get_absolute_time());
    } else if (mode == BB_OUTPUT_GENERIC_HID && tud_hid_n_ready(usb_slot)) {
        uint8_t gamepad[9] = {
            (uint8_t)report.buttons, (uint8_t)(report.buttons >> 8),
            report.hat >= 1 && report.hat <= 8 ? (uint8_t)(report.hat - 1) : 8,
            (uint8_t)report.x, (uint8_t)report.y,
            (uint8_t)report.rx, (uint8_t)report.ry,
            report.lt, report.rt
        };
        ok = tud_hid_n_report(usb_slot, REPORT_ID_GAMEPAD, gamepad, sizeof(gamepad));
    } else if (tud_hid_n_ready(usb_slot)) {
        hid_gamepad_report_t gamepad = {0};
        gamepad.x = report.x;
        gamepad.y = report.y;
        gamepad.z = report.rx;
        gamepad.rz = report.ry;
        gamepad.rx = (int8_t)((int)report.lt - 128);
        gamepad.ry = (int8_t)((int)report.rt - 128);
        gamepad.hat = report.hat;
        gamepad.buttons = report.buttons;
        ok = tud_hid_n_report(usb_slot, REPORT_ID_GAMEPAD, &gamepad, sizeof(gamepad));
    }
    bb_state_note_slot_usb_report(usb_slot, ok);
    if (!ok && latched_for_send) {
        critical_section_enter_blocking(&usb_lock);
        press_latched_buttons[usb_slot] |= latched_for_send;
        critical_section_exit(&usb_lock);
    }
    if (!ok && queued) {
        critical_section_enter_blocking(&usb_lock);
        q = &usb_queues[usb_slot];
        if (q->count < BB_USB_QUEUE_DEPTH) {
            q->head = (uint8_t)((q->head + BB_USB_QUEUE_DEPTH - 1u) % BB_USB_QUEUE_DEPTH);
            q->reports[q->head] = report;
            ++q->count;
        }
        critical_section_exit(&usb_lock);
    }
}

void bb_usb_service_pending(void) {
    for (usb_slot = 0; usb_slot < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++usb_slot) send_pending();
    usb_slot = 0;
}

static void feedback_reset_all(void) {
    for (uint8_t i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) {
        bb_feedback_select(i);
        bb_feedback_reset();
    }
    bb_feedback_select(0);
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
    feedback_reset_all();
    tud_disconnect();
    sleep_ms(120);
    switch_last_send = 0;
    dualsense_last_send = 0;
    bb_ds5_reset();
    if (bb_output_mode() == BB_OUTPUT_XBOX360) bb_xbox360_auth_init();
    tud_connect();
}

void bb_usb_core1(void) {
    flash_safe_execute_core_init();
    critical_section_init(&usb_lock);
    usb_lock_ready = true;
    if (bb_output_mode() == BB_OUTPUT_XBOX360) bb_xbox360_auth_init();
    tusb_init();
    while (true) {
        if (bb_output_mode() == BB_OUTPUT_MISTER) {
            for (uint8_t i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) {
                bb_feedback_select(i);
                bb_feedback_poll(to_ms_since_boot(get_absolute_time()));
            }
        }
        tud_task();
        if (bb_output_mode() == BB_OUTPUT_XBOX360) bb_xbox360_auth_task();
        if (bb_output_mode() == BB_OUTPUT_DUALSENSE) bb_ds5_task();
        bb_usb_service_pending();
        bb_cdc_poll();
        handle_reenumeration();
        bb_usb_service_pending();
        bb_config_flush_pending();
        if (bb_mode_restart_pending()) {
            sleep_ms(300);
            watchdog_reboot(0, 0, 0);
            while (true) tight_loop_contents();
        }
        tight_loop_contents();
    }
}

void tud_mount_cb(void) {
    feedback_reset_all();
    usb_mounted = true;
    bb_state_set_usb_ready(true);
    critical_section_enter_blocking(&usb_lock);
    for (unsigned i = 0; i < BB_MAX_SLOTS; ++i) {
        usb_queues[i].head = 0;
        usb_queues[i].count = 1;
        usb_queues[i].reports[0] = latest_slots[i];
    }
    critical_section_exit(&usb_lock);
}

void tud_umount_cb(void) {
    feedback_reset_all();
    usb_mounted = false;
    bb_state_set_usb_ready(false);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    for (uint8_t i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) { bb_feedback_select(i); bb_feedback_stop(); }
}

void tud_resume_cb(void) {
    usb_mounted = tud_mounted();
    bb_state_set_usb_ready(usb_mounted);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    if (bb_output_mode() == BB_OUTPUT_DUALSENSE && !bb_multi_mode()) {
        if (instance != 0 || report_type != HID_REPORT_TYPE_FEATURE) return 0;
        return bb_ds5_get_feature(report_id, buffer, reqlen);
    }
    if (instance >= (bb_multi_mode() ? BB_MAX_SLOTS : 1) || bb_output_mode() != BB_OUTPUT_MISTER) return 0;
    bb_feedback_select(instance);
    return bb_feedback_get(report_id, report_type == HID_REPORT_TYPE_FEATURE, buffer, reqlen);
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (bb_output_mode() == BB_OUTPUT_DUALSENSE && !bb_multi_mode()) {
        // Output reports arrive on the interrupt OUT endpoint (report_id 0, ID in
        // the first byte) or through SET_REPORT (ID already removed).
        if (instance == 0 && (report_type == HID_REPORT_TYPE_OUTPUT || report_type == HID_REPORT_TYPE_INVALID))
            bb_ds5_handle_output(report_id, buffer, bufsize);
        return;
    }
    if (instance >= (bb_multi_mode() ? BB_MAX_SLOTS : 1) || bb_output_mode() != BB_OUTPUT_MISTER || !buffer) return;
    if (report_type != HID_REPORT_TYPE_OUTPUT && report_type != HID_REPORT_TYPE_FEATURE) return;
    if (!report_id) {
        if (!bufsize) return;
        report_id = *buffer++;
        --bufsize;
    }
    bb_feedback_select(instance);
    bb_feedback_set(report_id, report_type == HID_REPORT_TYPE_FEATURE, buffer, bufsize);
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if (bb_output_mode() == BB_OUTPUT_XBOX360 && bb_xbox360_auth_control(rhport, stage, request)) return true;
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
