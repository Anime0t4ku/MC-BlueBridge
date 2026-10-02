#include "cdc_protocol.h"
#include "bluebridge_state.h"
#include "bluebridge_platform.h"
#include "usb_device.h"
#include "led_status.h"
#include "controller_input.h"
#include "bt_host.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BB_CDC_LINE_MAX 2048
#define BB_IMPORT_CHUNK_MAX 128

static char line[BB_CDC_LINE_MAX];
static size_t used;
static bb_config_t import_cfg;
static uint8_t* import_bytes = (uint8_t*)&import_cfg;
static size_t import_received;
static uint32_t import_checksum;
static bool import_active;
static bb_config_t export_cfg;

static void write_text(const char* text) {
    size_t len = strlen(text);
    size_t off = 0;
    uint32_t idle = 0;
    while (off < len && idle < 2000) {
        uint32_t n = tud_cdc_write(text + off, (uint32_t)(len - off));
        if (n) {
            off += n;
            idle = 0;
            tud_cdc_write_flush();
        } else {
            tud_task();
            sleep_us(100);
            idle++;
        }
    }
}

static void send_result(const char* id, const char* status, const char* payload) {
    char out[2048];
    snprintf(out, sizeof(out), "BB1|%s|%s|%s\n", id && id[0] ? id : "0", status, payload ? payload : "");
    write_text(out);
}

static void send_ok(const char* id, const char* payload) {
    send_result(id, "OK", payload);
}

static void send_err(const char* id, const char* payload) {
    send_result(id, "ERR", payload);
}

static void send_status(const char* id) {
    bb_runtime_state_t state;
    bb_profile_t profile;
    bb_state_get(&state);
    bb_active_profile_get(&profile);
    uint8_t active_profile = bb_active_profile_index();
    uint8_t output_mode = bb_output_mode_value();
    char out[1024];
    snprintf(out, sizeof(out), "{\"protocol\":%d,\"firmware\":\"%s\",\"hardware\":\"Pico 2 W\",\"bt_ready\":%s,\"usb_ready\":%s,\"controller_connected\":%s,\"pairing\":%s,\"controller\":\"%s\",\"controller_index\":%d,\"active_profile\":%u,\"profile\":\"%s\",\"output_mode\":%u,\"output\":\"%s\",\"bt_reports\":%lu,\"usb_reports\":%lu,\"dropped_usb_reports\":%lu,\"raw_buttons\":%lu,\"button_capabilities\":%lu,\"battery_supported\":%s,\"battery_percent\":%u,\"battery_state\":%u,\"rumble_supported\":%s,\"capture_mode\":%u}", BLUEBRIDGE_PROTOCOL_VERSION, BLUEBRIDGE_VERSION, state.bt_ready ? "true" : "false", state.usb_ready ? "true" : "false", state.controller_connected ? "true" : "false", state.pairing ? "true" : "false", state.controller_name, state.controller_index, active_profile, profile.name, output_mode, bb_output_mode_name((bb_output_mode_t)output_mode), (unsigned long)state.bt_reports, (unsigned long)state.usb_reports, (unsigned long)state.dropped_usb_reports, (unsigned long)state.raw_buttons, (unsigned long)state.button_capabilities, state.battery_supported ? "true" : "false", state.battery_percent, state.battery_state, state.rumble_supported ? "true" : "false", state.capture_mode);
    send_ok(id, out);
}

static int shared_with_index(const bb_controller_t* controller, uint8_t index) {
    if (!controller || index >= controller->profile_count) return -1;
    uint16_t identity = controller->profiles[index].mister_identity;
    for (uint8_t i = 0; i < controller->profile_count; ++i) {
        if (i != index && controller->profiles[i].mister_identity == identity) return i;
    }
    return -1;
}

static void send_controllers(const char* id) {
    bb_config_t cfg;
    bb_runtime_state_t runtime;
    bb_config_get(&cfg);
    bb_state_get(&runtime);
    char out[2048];
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "{\"active\":%u,\"connected_index\":%d,\"controllers\":[", bb_selected_controller_index(), runtime.controller_connected ? runtime.controller_index : -1);
    for (uint8_t i = 0; i < cfg.controller_count && pos + 220 < sizeof(out); ++i) {
        const bb_controller_t* c = &cfg.controllers[i];
        bb_controller_kind_t kind = bb_controller_identify(c->native_vendor_id, c->native_product_id, c->name);
        pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s{\"index\":%u,\"id\":%lu,\"name\":\"%s\",\"kind\":%u,\"kind_name\":\"%s\",\"native_vid\":%u,\"native_pid\":%u,\"profiles\":%u,\"active_profile\":%u,\"connected\":%s}", i ? "," : "", i, (unsigned long)c->id, c->name, (unsigned)kind, bb_controller_kind_name(kind), c->native_vendor_id, c->native_product_id, c->profile_count, c->active_profile, runtime.controller_connected && runtime.controller_index == i ? "true" : "false");
    }
    snprintf(out + pos, sizeof(out) - pos, "]}");
    send_ok(id, out);
}

static void send_profiles(const char* id) {
    bb_controller_t controller_value;
    uint8_t controller_index = 0;
    bb_active_controller_get(&controller_value, &controller_index);
    const bb_controller_t* controller = &controller_value;
    if (!controller->profile_count) {
        send_err(id, "no_controller_profile_store");
        return;
    }
    bb_controller_kind_t kind = bb_controller_identify(controller->native_vendor_id, controller->native_product_id, controller->name);
    bb_controller_capabilities_t caps = bb_controller_capabilities(kind, controller->name);
    char out[2048];
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "{\"controller_index\":%u,\"controller_id\":%lu,\"controller_name\":\"%s\",\"kind\":%u,\"kind_name\":\"%s\",\"native_vid\":%u,\"native_pid\":%u,\"active\":%u,\"capabilities\":{\"buttons\":%lu,\"dpad\":%s,\"left_stick\":%s,\"right_stick\":%s,\"left_trigger\":%s,\"right_trigger\":%s},\"labels\":[", controller_index, (unsigned long)controller->id, controller->name, (unsigned)kind, bb_controller_kind_name(kind), controller->native_vendor_id, controller->native_product_id, controller->active_profile, (unsigned long)caps.buttons, caps.dpad ? "true" : "false", caps.left_stick ? "true" : "false", caps.right_stick ? "true" : "false", caps.left_trigger ? "true" : "false", caps.right_trigger ? "true" : "false");
    for (uint8_t i = 0; i < BB_BUTTON_COUNT; ++i) pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s\"%s\"", i ? "," : "", bb_controller_button_label(kind, controller->name, i));
    pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "],\"profiles\":[");
    for (uint8_t i = 0; i < controller->profile_count && pos + 150 < sizeof(out); ++i) {
        const bb_profile_t* p = &controller->profiles[i];
        int shared = shared_with_index(controller, i);
        pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s{\"index\":%u,\"id\":%lu,\"name\":\"%s\",\"mister_identity\":%u,\"mapping_mode\":\"%s\",\"shared_with\":%d}", i ? "," : "", i, (unsigned long)p->id, p->name, p->mister_identity, shared >= 0 ? "shared" : "separate", shared);
    }
    snprintf(out + pos, sizeof(out) - pos, "]}");
    send_ok(id, out);
}

static void send_profile(const char* id, uint8_t index) {
    bb_controller_t controller_value;
    bb_active_controller_get(&controller_value, NULL);
    const bb_controller_t* controller = &controller_value;
    if (!controller->profile_count || index >= controller->profile_count) {
        send_err(id, "profile");
        return;
    }
    const bb_profile_t* p = &controller->profiles[index];
    int shared = shared_with_index(controller, index);
    char out[2048];
    size_t pos = (size_t)snprintf(out, sizeof(out), "{\"index\":%u,\"id\":%lu,\"name\":\"%s\",\"mister_identity\":%u,\"mapping_mode\":\"%s\",\"shared_with\":%d,\"invert_x\":%s,\"invert_y\":%s,\"invert_rx\":%s,\"invert_ry\":%s,\"deadzone_left\":%u,\"deadzone_right\":%u,\"trigger_deadzone\":%u,\"turbo_rate_hz\":%u,\"turbo_modifier\":%u,\"turbo_enabled\":%s,\"turbo_control_mode\":%u,\"turbo_control_button\":%u,\"turbo_mask\":%u,\"map\":[", index, (unsigned long)p->id, p->name, p->mister_identity, shared >= 0 ? "shared" : "separate", shared, p->invert_x ? "true" : "false", p->invert_y ? "true" : "false", p->invert_rx ? "true" : "false", p->invert_ry ? "true" : "false", p->deadzone_left, p->deadzone_right, p->trigger_deadzone, p->turbo_rate_hz, p->turbo_modifier, p->turbo_enabled ? "true" : "false", p->turbo_control_mode, p->turbo_control_button, p->turbo_mask);
    for (uint8_t i = 0; i < BB_BUTTON_COUNT && pos + 8 < sizeof(out); ++i) pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s%u", i ? "," : "", p->button_map[i]);
    pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "],\"macros\":[");
    for (uint8_t i = 0; i < BB_MACRO_COUNT && pos + 100 < sizeof(out); ++i) {
        const bb_macro_t* m = &p->macros[i];
        pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s{\"index\":%u,\"enabled\":%s,\"name\":\"%s\",\"output_mask\":%u}", i ? "," : "", i, m->enabled ? "true" : "false", m->name, m->output_mask);
    }
    snprintf(out + pos, sizeof(out) - pos, "]}");
    send_ok(id, out);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_hex(const char* src, uint8_t* dst, size_t max, size_t* written) {
    size_t n = strlen(src);
    if ((n & 1u) || n / 2 > max) return false;
    for (size_t i = 0; i < n; i += 2) {
        int a = hex_value(src[i]);
        int b = hex_value(src[i + 1]);
        if (a < 0 || b < 0) return false;
        dst[i / 2] = (uint8_t)((a << 4) | b);
    }
    *written = n / 2;
    return true;
}

static void send_export(const char* id) {
    bb_config_get(&export_cfg);
    uint32_t checksum = bb_config_checksum(&export_cfg);
    char header[160];
    snprintf(header, sizeof(header), "BB1|%s|DATA_BEGIN|%u|%u|%lu\n", id, BB_CONFIG_SCHEMA, (unsigned)sizeof(export_cfg), (unsigned long)checksum);
    write_text(header);
    const uint8_t* bytes = (const uint8_t*)&export_cfg;
    for (size_t offset = 0; offset < sizeof(export_cfg); offset += BB_IMPORT_CHUNK_MAX) {
        size_t count = sizeof(export_cfg) - offset;
        if (count > BB_IMPORT_CHUNK_MAX) count = BB_IMPORT_CHUNK_MAX;
        char out[2 * BB_IMPORT_CHUNK_MAX + 96];
        int pos = snprintf(out, sizeof(out), "BB1|%s|DATA|%u|", id, (unsigned)offset);
        for (size_t i = 0; i < count; ++i) pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "%02X", bytes[offset + i]);
        snprintf(out + pos, sizeof(out) - (size_t)pos, "\n");
        write_text(out);
    }
    char end[64];
    snprintf(end, sizeof(end), "BB1|%s|DATA_END|\n", id);
    write_text(end);
}

static char* next_field(char** cursor) {
    if (!cursor || !*cursor) return NULL;
    char* start = *cursor;
    char* sep = strchr(start, '|');
    if (sep) {
        *sep = 0;
        *cursor = sep + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static void handle_command(char* text) {
    char* cursor = text;
    char* proto = next_field(&cursor);
    char* id = next_field(&cursor);
    char* cmd = next_field(&cursor);
    if (!proto || strcmp(proto, "BB1") || !id || !cmd) return;

    if (!strcmp(cmd, "HELLO")) {
        char out[192];
        snprintf(out, sizeof(out), "{\"protocol\":%d,\"firmware\":\"%s\",\"hardware\":\"Pico 2 W\",\"config_schema\":%d,\"firmware_id\":\"%s\"}", BLUEBRIDGE_PROTOCOL_VERSION, BLUEBRIDGE_VERSION, BB_CONFIG_SCHEMA, BLUEBRIDGE_FIRMWARE_MANIFEST);
        send_ok(id, out);
        return;
    }
    if (!strcmp(cmd, "STATUS")) {
        send_status(id);
        return;
    }
    if (!strcmp(cmd, "PAIR_START")) {
        bb_platform_start_pairing();
        send_ok(id, "pairing");
        return;
    }
    if (!strcmp(cmd, "PAIR_STOP")) {
        bb_platform_stop_pairing();
        send_ok(id, "stopped");
        return;
    }
    if (!strcmp(cmd, "FORGET_ALL")) {
        bb_platform_forget_all();
        send_ok(id, "forgotten");
        return;
    }
    if (!strcmp(cmd, "CONTROLLERS")) {
        send_controllers(id);
        return;
    }
    if (!strcmp(cmd, "CONTROLLER_FORGET")) {
        char* a = next_field(&cursor);
        if (!a || !bb_platform_forget_controller((uint8_t)atoi(a))) send_err(id, "controller"); else send_ok(id, "forgotten");
        return;
    }
    if (!strcmp(cmd, "CONTROLLER_DISCONNECT")) {
        if (!bb_platform_disconnect_controller()) send_err(id, "not_connected"); else send_ok(id, "disconnecting");
        return;
    }
    if (!strcmp(cmd, "CONTROLLER_SELECT")) {
        char* a = next_field(&cursor);
        if (!a || !bb_controller_select((uint8_t)atoi(a))) send_err(id, "controller"); else send_ok(id, "selected");
        return;
    }
    if (!strcmp(cmd, "CAPTURE_START")) {
        char* mode = next_field(&cursor);
        bb_capture_mode_t capture = mode && !strcmp(mode, "TESTER") ? BB_CAPTURE_TESTER : BB_CAPTURE_ONCE;
        if (!bb_capture_start(capture)) {
            send_err(id, "capture");
        } else {
            bb_gamepad_report_t neutral = {0};
            bb_usb_submit(&neutral);
            send_ok(id, capture == BB_CAPTURE_TESTER ? "tester" : "once");
        }
        return;
    }
    if (!strcmp(cmd, "CAPTURE_STOP")) {
        bb_capture_stop();
        bb_capture_clear_result();
        send_ok(id, "stopped");
        return;
    }
    if (!strcmp(cmd, "CAPTURE_STATUS")) {
        bb_runtime_state_t state;
        bb_state_get(&state);
        char out[512];
        snprintf(out, sizeof(out), "{\"mode\":%u,\"ready\":%s,\"button\":%u,\"buttons\":%lu,\"dpad\":%u,\"x\":%d,\"y\":%d,\"rx\":%d,\"ry\":%d,\"lt\":%u,\"rt\":%u,\"battery_supported\":%s,\"battery_percent\":%u,\"battery_state\":%u,\"rumble_supported\":%s}", state.capture_mode, state.capture_ready ? "true" : "false", state.captured_button, (unsigned long)state.raw_buttons, state.raw_dpad, state.raw_x, state.raw_y, state.raw_rx, state.raw_ry, state.raw_lt, state.raw_rt, state.battery_supported ? "true" : "false", state.battery_percent, state.battery_state, state.rumble_supported ? "true" : "false");
        send_ok(id, out);
        return;
    }
    if (!strcmp(cmd, "CAPTURE_CLEAR")) {
        bb_capture_clear_result();
        send_ok(id, "cleared");
        return;
    }
    if (!strcmp(cmd, "CONTROLLER_RUMBLE")) {
        char* strength = next_field(&cursor);
        char* duration = next_field(&cursor);
        if (!strength || !duration || !bb_bt_host_rumble_test((uint8_t)atoi(strength), (uint16_t)atoi(duration))) send_err(id, "rumble_unsupported"); else send_ok(id, "started");
        return;
    }
    if (!strcmp(cmd, "PROFILES")) {
        send_profiles(id);
        return;
    }
    if (!strcmp(cmd, "PROFILE_GET")) {
        char* a = next_field(&cursor);
        if (!a) send_err(id, "profile"); else send_profile(id, (uint8_t)atoi(a));
        return;
    }
    if (!strcmp(cmd, "PROFILE_DUPLICATE")) {
        char* a = next_field(&cursor);
        const char* name = cursor && cursor[0] ? cursor : "Copy";
        uint8_t index = 0;
        if (!a || !bb_profile_duplicate((uint8_t)atoi(a), name, &index)) send_err(id, "profile_limit");
        else {
            char out[48];
            snprintf(out, sizeof(out), "{\"index\":%u}", index);
            send_ok(id, out);
        }
        return;
    }
    if (!strcmp(cmd, "PROFILE_CREATE")) {
        uint8_t index = 0;
        const char* name = cursor && cursor[0] ? cursor : "Profile";
        if (!bb_profile_create(name, &index)) send_err(id, "profile_limit");
        else {
            char out[48];
            snprintf(out, sizeof(out), "{\"index\":%u}", index);
            send_ok(id, out);
        }
        return;
    }
    if (!strcmp(cmd, "PROFILE_SELECT")) {
        char* a = next_field(&cursor);
        if (!a) { send_err(id, "profile"); return; }
        uint16_t old_identity = bb_current_mister_identity();
        if (!bb_profile_select((uint8_t)atoi(a))) send_err(id, "profile");
        else {
            if (bb_output_mode() == BB_OUTPUT_MISTER && old_identity != bb_current_mister_identity()) bb_usb_request_reenumeration();
            bb_led_signal_profile_changed();
            send_ok(id, "selected");
        }
        return;
    }
    if (!strcmp(cmd, "PROFILE_DELETE")) {
        char* a = next_field(&cursor);
        if (!a || !bb_profile_delete((uint8_t)atoi(a))) send_err(id, "profile"); else send_ok(id, "deleted");
        return;
    }
    if (!strcmp(cmd, "PROFILE_RENAME")) {
        char* a = next_field(&cursor);
        const char* name = cursor;
        if (!a || !name || !bb_profile_rename((uint8_t)atoi(a), name)) send_err(id, "profile"); else send_ok(id, "renamed");
        return;
    }
    if (!strcmp(cmd, "PROFILE_MAP")) {
        char* a = next_field(&cursor);
        char* b = next_field(&cursor);
        char* c = next_field(&cursor);
        if (!a || !b || !c || !bb_profile_set_mapping((uint8_t)atoi(a), (uint8_t)atoi(b), (uint8_t)atoi(c))) send_err(id, "mapping"); else send_ok(id, "mapped");
        return;
    }
    if (!strcmp(cmd, "PROFILE_TUNE")) {
        char* pidx = next_field(&cursor);
        char* ix = next_field(&cursor);
        char* iy = next_field(&cursor);
        char* irx = next_field(&cursor);
        char* iry = next_field(&cursor);
        char* dzl = next_field(&cursor);
        char* dzr = next_field(&cursor);
        char* dzt = next_field(&cursor);
        char* rate = next_field(&cursor);
        char* mask = next_field(&cursor);
        if (!pidx || !ix || !iy || !irx || !iry || !dzl || !dzr || !dzt || !rate || !mask || !bb_profile_set_tuning((uint8_t)atoi(pidx), (uint8_t)atoi(ix), (uint8_t)atoi(iy), (uint8_t)atoi(irx), (uint8_t)atoi(iry), (uint8_t)atoi(dzl), (uint8_t)atoi(dzr), (uint8_t)atoi(dzt), (uint8_t)atoi(rate), (uint16_t)strtoul(mask, NULL, 10))) send_err(id, "tuning"); else send_ok(id, "updated");
        return;
    }
    if (!strcmp(cmd, "PROFILE_MACRO")) {
        char* pidx = next_field(&cursor);
        char* midx = next_field(&cursor);
        char* enabled = next_field(&cursor);
        char* name = next_field(&cursor);
        char* outputs = next_field(&cursor);
        if (!pidx || !midx || !enabled || !name || !outputs || !bb_profile_set_macro((uint8_t)atoi(pidx), (uint8_t)atoi(midx), (uint8_t)atoi(enabled), name, (uint16_t)strtoul(outputs, NULL, 10))) send_err(id, "macro"); else send_ok(id, "updated");
        return;
    }
    if (!strcmp(cmd, "PROFILE_TURBO_MODIFIER")) {
        char* pidx = next_field(&cursor);
        char* modifier = next_field(&cursor);
        if (!pidx || !modifier || !bb_profile_set_turbo_modifier((uint8_t)atoi(pidx), (uint8_t)atoi(modifier))) send_err(id, "turbo_modifier"); else send_ok(id, "updated");
        return;
    }
    if (!strcmp(cmd, "PROFILE_TURBO_CONTROL")) {
        char* pidx = next_field(&cursor);
        char* enabled = next_field(&cursor);
        char* mode = next_field(&cursor);
        char* button = next_field(&cursor);
        if (!pidx || !enabled || !mode || !button || !bb_profile_set_turbo_control((uint8_t)atoi(pidx), (uint8_t)atoi(enabled), (uint8_t)atoi(mode), (uint8_t)atoi(button))) send_err(id, "turbo_control"); else send_ok(id, "updated");
        return;
    }
    if (!strcmp(cmd, "PROFILE_MAPPING_SEPARATE")) {
        char* a = next_field(&cursor);
        if (!a) { send_err(id, "profile"); return; }
        uint16_t old_identity = bb_current_mister_identity();
        if (!bb_profile_set_separate_mapping((uint8_t)atoi(a))) send_err(id, "profile");
        else {
            if (bb_output_mode() == BB_OUTPUT_MISTER && old_identity != bb_current_mister_identity()) bb_usb_request_reenumeration();
            send_ok(id, "updated");
        }
        return;
    }
    if (!strcmp(cmd, "PROFILE_MAPPING_SHARE")) {
        char* a = next_field(&cursor);
        char* b = next_field(&cursor);
        if (!a || !b) { send_err(id, "profile"); return; }
        uint16_t old_identity = bb_current_mister_identity();
        if (!bb_profile_share_mapping((uint8_t)atoi(a), (uint8_t)atoi(b))) send_err(id, "profile");
        else {
            if (bb_output_mode() == BB_OUTPUT_MISTER && old_identity != bb_current_mister_identity()) bb_usb_request_reenumeration();
            send_ok(id, "updated");
        }
        return;
    }
    if (!strcmp(cmd, "PROFILE_UNIQUE")) {
        char* a = next_field(&cursor);
        char* b = next_field(&cursor);
        if (!a || !b) { send_err(id, "profile"); return; }
        uint16_t old_identity = bb_current_mister_identity();
        bool ok = atoi(b) != 0 ? bb_profile_set_separate_mapping((uint8_t)atoi(a)) : bb_profile_share_mapping((uint8_t)atoi(a), 0);
        if (!ok) send_err(id, "profile");
        else {
            if (bb_output_mode() == BB_OUTPUT_MISTER && old_identity != bb_current_mister_identity()) bb_usb_request_reenumeration();
            send_ok(id, "updated");
        }
        return;
    }
    if (!strcmp(cmd, "OUTPUT_GET")) {
        char out[96];
        bb_output_mode_t mode = bb_output_mode();
        snprintf(out, sizeof(out), "{\"mode\":%u,\"name\":\"%s\"}", (unsigned)mode, bb_output_mode_name(mode));
        send_ok(id, out);
        return;
    }
    if (!strcmp(cmd, "OUTPUT_SET")) {
        char* a = next_field(&cursor);
        bb_output_mode_t old_mode = bb_output_mode();
        if (!a || !bb_output_mode_set((bb_output_mode_t)atoi(a))) send_err(id, "output_mode");
        else {
            if (old_mode != bb_output_mode()) bb_usb_request_reenumeration();
            send_ok(id, "updated");
        }
        return;
    }
    if (!strcmp(cmd, "CONFIG_EXPORT")) {
        send_export(id);
        return;
    }
    if (!strcmp(cmd, "CONFIG_IMPORT_BEGIN")) {
        char* schema = next_field(&cursor);
        char* size = next_field(&cursor);
        char* checksum = next_field(&cursor);
        int schema_value = schema ? atoi(schema) : 0;
        if (!schema || !size || !checksum || (schema_value != BB_CONFIG_SCHEMA && schema_value != 8) || (size_t)strtoul(size, NULL, 10) != sizeof(bb_config_t)) {
            import_active = false;
            send_err(id, "config_header");
            return;
        }
        memset(&import_cfg, 0, sizeof(import_cfg));
        import_received = 0;
        import_checksum = (uint32_t)strtoul(checksum, NULL, 10);
        import_active = true;
        send_ok(id, "ready");
        return;
    }
    if (!strcmp(cmd, "CONFIG_IMPORT_CHUNK")) {
        char* off = next_field(&cursor);
        const char* hex = cursor;
        if (!import_active || !off || !hex) { send_err(id, "config_chunk"); return; }
        size_t offset = (size_t)strtoul(off, NULL, 10);
        uint8_t chunk[BB_IMPORT_CHUNK_MAX];
        size_t count = 0;
        if (offset != import_received || !decode_hex(hex, chunk, sizeof(chunk), &count) || offset + count > sizeof(import_cfg)) {
            import_active = false;
            send_err(id, "config_chunk");
            return;
        }
        memcpy(import_bytes + offset, chunk, count);
        import_received += count;
        send_ok(id, "chunk");
        return;
    }
    if (!strcmp(cmd, "CONFIG_IMPORT_COMMIT")) {
        if (!import_active || import_received != sizeof(import_cfg) || !bb_config_import(&import_cfg, import_checksum)) {
            import_active = false;
            send_err(id, "config_import");
            return;
        }
        import_active = false;
        bb_usb_request_reenumeration();
        send_ok(id, "imported");
        return;
    }
    if (!strcmp(cmd, "CONFIG_RESET")) {
        if (!bb_config_reset()) send_err(id, "config_reset");
        else {
            bb_usb_request_reenumeration();
            send_ok(id, "reset");
        }
        return;
    }
    if (!strcmp(cmd, "REBOOT_BOOTSEL")) {
        send_ok(id, "rebooting");
        tud_cdc_write_flush();
        reset_usb_boot(0, 0);
        return;
    }
    send_err(id, "unknown_command");
}

void bb_cdc_poll(void) {
    while (tud_cdc_available()) {
        char c;
        if (tud_cdc_read(&c, 1) != 1) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[used] = 0;
            if (used) handle_command(line);
            used = 0;
        } else if (used + 1 < sizeof(line)) {
            line[used++] = c;
        } else {
            used = 0;
        }
    }
}
