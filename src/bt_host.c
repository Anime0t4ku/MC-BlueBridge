#include "bt_host.h"
#include "bluebridge_state.h"
#include "controller_driver.h"
#include "controller_input.h"
#include "usb_device.h"
#include "btstack.h"
#include "ble/gatt-service/hids_host.h"
#include "ble/le_device_db.h"
#include "ble/le_device_db_tlv.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "btstack_tlv.h"
#include "classic/device_id_server.h"
#include "classic/sdp_client.h"
#include "classic/sdp_server.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BB_HID_DESCRIPTOR_STORAGE 2048
#define BB_CLASSIC_HID_STORAGE 2048
#define BB_CLASSIC_NAME_CANDIDATES 8

static btstack_packet_callback_registration_t hci_callback;
static btstack_packet_callback_registration_t sm_callback;
static uint8_t hids_descriptor_storage[BB_HID_DESCRIPTOR_STORAGE];
static uint8_t classic_hid_storage[BB_CLASSIC_HID_STORAGE];
static uint8_t host_device_id_record[128];
static uint32_t classic_attempt_started;
static bool classic_attempt_active;
static hci_con_handle_t pending_classic_handle = HCI_CON_HANDLE_INVALID;
static uint32_t classic_generation;
static uint32_t identity_query_generation;
static bool identity_query_pending;
static bool identity_query_active;
static uint16_t identity_query_cid;
static bd_addr_t identity_query_addr;
static uint16_t identity_query_record;
static uint16_t identity_query_vid;
static uint16_t identity_query_pid;
static uint16_t identity_query_source;
static uint8_t identity_attribute[3];
static bool connected_name_request;
static bd_addr_t connected_name_addr;
static uint16_t classic_hid_cid;
static hci_con_handle_t classic_handle = HCI_CON_HANDLE_INVALID;
static uint16_t hids_cid;
static hci_con_handle_t le_handle = HCI_CON_HANDLE_INVALID;
static bool stack_ready;
static bool scanning;
static bool pairing;
static bool classic_connecting;
static bool ble_connecting;
static bool le_custom_nintendo;
static bool le_gamecube;
static bd_addr_t pending_addr;
static bd_addr_type_t pending_addr_type;
static char pending_name[32];
static uint16_t pending_vid;
static uint16_t pending_pid;
static bb_controller_identity_t current_identity;
static int current_controller_index = -1;
static bb_config_t remembered_cfg;
static gatt_client_notification_t nintendo_listener;
static gatt_client_characteristic_t nintendo_characteristic;
static btstack_timer_source_t usb_watch_timer;
static bool usb_was_ready;
static uint32_t usb_lost_since;
static bd_addr_t reconnect_block_addr;
static uint32_t reconnect_block_until;
static volatile bool rumble_request_pending;
static volatile uint8_t rumble_request_strength;
static volatile uint16_t rumble_request_duration_ms;
static bool rumble_active;
static uint8_t dualsense_output_seq;
static uint8_t switch_output_seq;
static bool classic_switch_setup_sent;
static bool classic_switch_protocol;
static uint8_t switch_setup_state;
static uint8_t switch_setup_retries;
static uint32_t switch_setup_at;
static bool manual_disconnect_pending;
static uint32_t manual_disconnect_at;
static uint8_t wiiu_setup_step;
static uint32_t wiiu_setup_at;
static uint32_t rumble_stop_at;
static uint32_t classic_reconnect_at;
static uint32_t rumble_refresh_at;
static uint8_t rumble_active_strength;
static uint8_t switch2_rumble_seq;
static uint32_t last_controller_report_at;
typedef struct {
    bd_addr_t addr;
    uint8_t page_scan_repetition_mode;
    uint16_t clock_offset;
} bb_classic_name_candidate_t;
static bb_classic_name_candidate_t classic_name_candidates[BB_CLASSIC_NAME_CANDIDATES];
static uint8_t classic_name_candidate_count;
static uint8_t classic_name_candidate_index;
static bool classic_name_request_active;

static const uint8_t nintendo_service_uuid[16] = {0xab,0x7d,0xe9,0xbe,0x89,0xfe,0x49,0xad,0x82,0x8f,0x11,0x8f,0x09,0xdf,0x7f,0xd0};
static const uint8_t nintendo_pro2_uuid[16] = {0x74,0x92,0x86,0x6c,0xec,0x3e,0x46,0x19,0x82,0x58,0x32,0x75,0x5f,0xfc,0xc0,0xf8};
static const uint8_t nintendo_gc_uuid[16] = {0x82,0x61,0xcb,0xa1,0x94,0x35,0x42,0x0c,0x84,0xd6,0xf0,0xc7,0x5a,0x2c,0x8e,0x4d};

static void start_scans(void);
static void stop_scans(void);
static bool name_has(const char* name, const char* part);
static void schedule_classic_reconnect(uint32_t delay_ms);
static void reset_classic_name_candidates(void);
static void request_next_classic_name(void);

static bool connection_in_progress(void) {
    return classic_connecting || ble_connecting || classic_name_request_active;
}

static uint32_t seeded_crc(uint8_t seed, const uint8_t* report, uint16_t length) {
    uint32_t crc = 0xffffffffu;
    crc ^= seed;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    for (uint16_t i = 0; i < length; ++i) {
        crc ^= report[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static uint32_t dualsense_output_crc(const uint8_t* report, uint16_t length) {
    return seeded_crc(0xa2, report, length);
}

static bool send_dualsense_power_off(void) {
    if (!classic_hid_cid || (current_identity.kind != BB_CONTROLLER_DUALSENSE && current_identity.kind != BB_CONTROLLER_DUALSENSE_EDGE)) return false;
    uint8_t report[47];
    memset(report, 0, sizeof(report));
    report[0] = 0x08;
    report[1] = 0x02;
    uint32_t crc = seeded_crc(0x53, report, 43);
    report[43] = (uint8_t)crc;
    report[44] = (uint8_t)(crc >> 8);
    report[45] = (uint8_t)(crc >> 16);
    report[46] = (uint8_t)(crc >> 24);
    return hid_host_send_set_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x08, &report[1], 46) == ERROR_CODE_SUCCESS;
}

static bool send_dualsense_rumble(uint8_t strength) {
    if (!classic_hid_cid || (current_identity.kind != BB_CONTROLLER_DUALSENSE && current_identity.kind != BB_CONTROLLER_DUALSENSE_EDGE)) return false;
    uint8_t report[78];
    memset(report, 0, sizeof(report));
    report[0] = 0x31;
    report[1] = (uint8_t)((dualsense_output_seq & 0x0fu) << 4);
    dualsense_output_seq = (uint8_t)((dualsense_output_seq + 1u) & 0x0fu);
    report[2] = 0x10;
    report[3] = 0x03;
    report[5] = strength;
    report[6] = strength;
    report[41] = 0x04;
    uint32_t crc = dualsense_output_crc(report, 74);
    report[74] = (uint8_t)crc;
    report[75] = (uint8_t)(crc >> 8);
    report[76] = (uint8_t)(crc >> 16);
    report[77] = (uint8_t)(crc >> 24);
    return hid_host_send_report(classic_hid_cid, 0x31, &report[1], 77) == ERROR_CODE_SUCCESS;
}

static bool send_switch_report_mode(uint8_t mode) {
    if (!classic_hid_cid) return false;
    uint8_t payload[11] = {0};
    payload[0] = (uint8_t)(switch_output_seq++ & 0x0fu);
    payload[2] = 0x01;
    payload[3] = 0x40;
    payload[4] = 0x40;
    payload[6] = 0x01;
    payload[7] = 0x40;
    payload[8] = 0x40;
    payload[9] = 0x03;
    payload[10] = mode;
    return hid_host_send_report(classic_hid_cid, 0x01, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_ds4_rumble(uint8_t strength) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_DUALSHOCK4) return false;
    uint8_t report[78];
    memset(report, 0, sizeof(report));
    report[0] = 0x11;
    report[1] = 0xc0;
    report[3] = 0x07;
    report[6] = strength;
    report[7] = strength;
    report[10] = 0x40;
    uint32_t crc = dualsense_output_crc(report, 74);
    report[74] = (uint8_t)crc;
    report[75] = (uint8_t)(crc >> 8);
    report[76] = (uint8_t)(crc >> 16);
    report[77] = (uint8_t)(crc >> 24);
    return hid_host_send_report(classic_hid_cid, 0x11, &report[1], 77) == ERROR_CODE_SUCCESS;
}

static bool send_ds3_activation(void) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_DUALSHOCK3) return false;
    uint8_t payload[4] = {0x42, 0x03, 0x00, 0x00};
    return hid_host_send_set_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0xf4, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_ds3_rumble(uint8_t strength) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_DUALSHOCK3) return false;
    uint8_t report[36] = {0};
    report[0] = 0x01;
    report[2] = 0xff;
    report[3] = strength ? 0x01 : 0x00;
    report[4] = 0xff;
    report[5] = strength;
    report[10] = 0x02;
    for (uint8_t i = 0; i < 4; ++i) {
        uint8_t o = (uint8_t)(11u + i * 5u);
        report[o] = 0xff;
        report[o + 1] = 0x27;
        report[o + 2] = 0x10;
        report[o + 4] = 0x32;
    }
    return hid_host_send_report(classic_hid_cid, 0x01, &report[1], 35) == ERROR_CODE_SUCCESS;
}

static bool send_switch_subcommand(uint8_t subcommand, uint8_t value) {
    if (!classic_hid_cid) return false;
    uint8_t payload[11];
    memset(payload, 0, sizeof(payload));
    payload[0] = (uint8_t)(switch_output_seq++ & 0x0fu);
    payload[1] = 0x00;
    payload[2] = 0x01;
    payload[3] = 0x40;
    payload[4] = 0x40;
    payload[5] = 0x00;
    payload[6] = 0x01;
    payload[7] = 0x40;
    payload[8] = 0x40;
    payload[9] = subcommand;
    payload[10] = value;
    return hid_host_send_report(classic_hid_cid, 0x01, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static void switch_setup_begin(void) {
    switch_setup_state = 1;
    switch_setup_retries = 0;
    switch_setup_at = btstack_run_loop_get_time_ms();
    classic_switch_setup_sent = false;
    bb_state_set_rumble_supported(false);
}

static void service_switch_setup(uint32_t now) {
    if (!classic_hid_cid || !classic_switch_protocol || switch_setup_state == 0 || switch_setup_state == 7) return;
    if ((int32_t)(now - switch_setup_at) < 0) return;
    bool sent = false;
    if (switch_setup_state == 1) sent = send_switch_report_mode(0x30);
    else if (switch_setup_state == 3) sent = send_switch_subcommand(0x48, 0x01);
    else if (switch_setup_state == 5) sent = send_switch_subcommand(0x30, 0x01);
    else {
        if (switch_setup_retries >= 2) {
            switch_setup_state = 0;
            classic_switch_setup_sent = false;
            bb_state_set_rumble_supported(false);
            return;
        }
        switch_setup_retries++;
        switch_setup_state--;
        switch_setup_at = now;
        return;
    }
    if (sent) {
        switch_setup_state++;
        switch_setup_at = now + 600u;
    } else {
        switch_setup_at = now + 200u;
    }
}

static void switch_setup_ack(const uint8_t* report, uint16_t len) {
    if (!report || len < 15 || report[0] != 0x21 || !(report[13] & 0x80u)) return;
    uint8_t subcommand = report[14];
    uint32_t now = btstack_run_loop_get_time_ms();
    if (switch_setup_state == 2 && subcommand == 0x03) {
        switch_setup_state = 3;
        switch_setup_retries = 0;
        switch_setup_at = now;
    } else if (switch_setup_state == 4 && subcommand == 0x48) {
        switch_setup_state = 5;
        switch_setup_retries = 0;
        switch_setup_at = now;
        bb_state_set_rumble_supported(true);
    } else if (switch_setup_state == 6 && subcommand == 0x30) {
        switch_setup_state = 7;
        classic_switch_setup_sent = true;
        switch_setup_retries = 0;
    }
}

static bool send_switch_rumble(uint8_t strength) {
    if (!classic_hid_cid || !classic_switch_protocol) return false;
    uint8_t payload[9];
    memset(payload, 0, sizeof(payload));
    uint8_t amp = strength ? (uint8_t)(0x40u + ((uint16_t)strength * 0x32u) / 255u) : 0x40u;
    payload[0] = (uint8_t)(switch_output_seq++ & 0x0fu);
    payload[1] = 0x00;
    payload[2] = 0x01;
    payload[3] = 0x40;
    payload[4] = amp;
    payload[5] = 0x00;
    payload[6] = 0x01;
    payload[7] = 0x40;
    payload[8] = amp;
    return hid_host_send_report(classic_hid_cid, 0x10, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_8bitdo_dinput_rumble(uint8_t strength) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_8BITDO || classic_switch_protocol) return false;
    uint8_t payload[4] = {strength, strength, 0, 0};
    return hid_host_send_report(classic_hid_cid, 0x05, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static void switch2_rumble_frame(uint8_t strength, uint8_t out[5]) {
    uint16_t amp = (uint16_t)(((uint32_t)strength * 0x03ffu) / 255u);
    uint64_t value = 0x0e1u | ((uint64_t)amp << 10) | ((uint64_t)0x1e1u << 20) | ((uint64_t)amp << 30);
    for (uint8_t i = 0; i < 5; ++i) out[i] = (uint8_t)(value >> (i * 8u));
}

static bool send_switch2_rumble(uint8_t strength) {
    if (le_handle == HCI_CON_HANDLE_INVALID || current_identity.kind != BB_CONTROLLER_SWITCH2_PRO) return false;
    uint8_t payload[33];
    memset(payload, 0, sizeof(payload));
    payload[1] = (uint8_t)(0x50u | (switch2_rumble_seq++ & 0x0fu));
    uint8_t frame[5];
    switch2_rumble_frame(strength, frame);
    for (uint8_t block = 0; block < 3; ++block) memcpy(&payload[2 + block * 5], frame, 5);
    memcpy(&payload[17], &payload[1], 16);
    return gatt_client_write_value_of_characteristic_without_response(le_handle, 0x0012, sizeof(payload), payload) == ERROR_CODE_SUCCESS;
}

static bool send_wiiu_write(uint32_t address, uint8_t value) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_WII_U_PRO) return false;
    uint8_t payload[21] = {0};
    payload[0] = 0x04;
    payload[1] = (uint8_t)(address >> 16);
    payload[2] = (uint8_t)(address >> 8);
    payload[3] = (uint8_t)address;
    payload[4] = 1;
    payload[5] = value;
    return hid_host_send_report(classic_hid_cid, 0x16, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_wiiu_report_mode(void) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_WII_U_PRO) return false;
    uint8_t payload[2] = {0x04, 0x34};
    return hid_host_send_report(classic_hid_cid, 0x12, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_wiiu_rumble(uint8_t strength) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_WII_U_PRO) return false;
    uint8_t payload = strength ? 0x01 : 0x00;
    return hid_host_send_report(classic_hid_cid, 0x10, &payload, 1) == ERROR_CODE_SUCCESS;
}

static bool send_current_rumble(uint8_t strength) {
    switch (current_identity.kind) {
        case BB_CONTROLLER_DUALSENSE:
        case BB_CONTROLLER_DUALSENSE_EDGE:
            return send_dualsense_rumble(strength);
        case BB_CONTROLLER_DUALSHOCK4:
            return send_ds4_rumble(strength);
        case BB_CONTROLLER_DUALSHOCK3:
            return send_ds3_rumble(strength);
        case BB_CONTROLLER_SWITCH_PRO:
        case BB_CONTROLLER_JOYCON_LEFT:
        case BB_CONTROLLER_JOYCON_RIGHT:
            return send_switch_rumble(strength);
        case BB_CONTROLLER_SWITCH2_PRO:
            return send_switch2_rumble(strength);
        case BB_CONTROLLER_8BITDO:
            return classic_switch_protocol ? send_switch_rumble(strength) : send_8bitdo_dinput_rumble(strength);
        case BB_CONTROLLER_WII_U_PRO:
            return send_wiiu_rumble(strength);
        default:
            return false;
    }
}

static void service_rumble(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    if (rumble_request_pending) {
        uint8_t strength = rumble_request_strength;
        uint16_t duration = rumble_request_duration_ms;
        rumble_request_pending = false;
        if (send_current_rumble(strength)) {
            rumble_active = strength != 0;
            rumble_active_strength = strength;
            rumble_stop_at = now + duration;
            rumble_refresh_at = now + 80u;
        }
    }
    if (rumble_active && (int32_t)(now - rumble_stop_at) < 0 && (int32_t)(now - rumble_refresh_at) >= 0) {
        bool refresh = current_identity.kind == BB_CONTROLLER_SWITCH2_PRO || (current_identity.kind == BB_CONTROLLER_8BITDO && classic_switch_protocol);
        if (refresh) send_current_rumble(rumble_active_strength);
        rumble_refresh_at = now + 80u;
    }
    if (rumble_active && (int32_t)(now - rumble_stop_at) >= 0) {
        send_current_rumble(0);
        rumble_active = false;
        rumble_active_strength = 0;
        rumble_refresh_at = 0;
    }
}

static void copy_name(char* dst, size_t size, const char* src) {
    if (!size) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool same_address(const uint8_t a[6], const uint8_t b[6]) {
    return memcmp(a, b, 6) == 0;
}

static bool reconnect_blocked(const uint8_t addr[6]) {
    if (!reconnect_block_until) return false;
    uint32_t now = btstack_run_loop_get_time_ms();
    if ((int32_t)(reconnect_block_until - now) <= 0) {
        reconnect_block_until = 0;
        memset(reconnect_block_addr, 0, sizeof(reconnect_block_addr));
        return false;
    }
    return same_address(reconnect_block_addr, addr);
}

static bool remembered_address(const uint8_t addr[6]) {
    bb_config_get(&remembered_cfg);
    for (uint8_t i = 0; i < remembered_cfg.controller_count; ++i) {
        if (same_address(remembered_cfg.controllers[i].bluetooth_address, addr)) return true;
    }
    return false;
}

static int classic_connection_filter(bd_addr_t addr, hci_link_type_t link_type) {
    if (link_type != HCI_LINK_TYPE_ACL) return 1;
    return pairing || remembered_address(addr);
}

static bool name_has(const char* name, const char* part);

static const char* remembered_name(const uint8_t addr[6]) {
    bb_config_get(&remembered_cfg);
    for (uint8_t i = 0; i < remembered_cfg.controller_count; ++i) {
        if (same_address(remembered_cfg.controllers[i].bluetooth_address, addr)) return remembered_cfg.controllers[i].name;
    }
    return NULL;
}

static bool classic_reconnect_target(bd_addr_t addr, char* name, size_t name_size) {
    bb_config_get(&remembered_cfg);
    if (!remembered_cfg.controller_count || remembered_cfg.active_controller >= remembered_cfg.controller_count) return false;
    const bb_controller_t* controller = &remembered_cfg.controllers[remembered_cfg.active_controller];
    link_key_t key;
    link_key_type_t type;
    memcpy(addr, controller->bluetooth_address, 6);
    if (!gap_get_link_key_for_bd_addr(addr, key, &type)) return false;
    copy_name(name, name_size, controller->name);
    if (controller->native_vendor_id == 0x054c || name_has(name, "dualsense") || name_has(name, "wireless controller") || name_has(name, "dualshock")) return false;
    return true;
}

static void schedule_classic_reconnect(uint32_t delay_ms) {
    classic_reconnect_at = btstack_run_loop_get_time_ms() + delay_ms;
}

static bool name_has(const char* name, const char* part) {
    if (!name || !part) return false;
    size_t n = strlen(part);
    for (; *name; ++name) {
        size_t i = 0;
        while (i < n && name[i] && ((name[i] | 0x20) == (part[i] | 0x20))) ++i;
        if (i == n) return true;
    }
    return false;
}

static bool likely_controller_name(const char* name) {
    if (!name || !name[0]) return false;
    static const char* words[] = {
        "controller", "gamepad", "joy-con", "8bitdo", "pro 3", "xbox", "dualsense", "wireless controller",
        "stadia", "steam", "nimbus", "ouya", "wiimote", "wii remote", "atari", "icade", "famicom",
        "nintendo", "genesis", "mega drive", "snes", "n64"
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) if (name_has(name, words[i])) return true;
    return false;
}

static void reset_classic_name_candidates(void) {
    memset(classic_name_candidates, 0, sizeof(classic_name_candidates));
    classic_name_candidate_count = 0;
    classic_name_candidate_index = 0;
    classic_name_request_active = false;
}

static void queue_classic_name_candidate(const uint8_t addr[6], uint8_t page_scan_repetition_mode, uint16_t clock_offset) {
    for (uint8_t i = 0; i < classic_name_candidate_count; ++i) {
        if (same_address(classic_name_candidates[i].addr, addr)) return;
    }
    if (classic_name_candidate_count >= BB_CLASSIC_NAME_CANDIDATES) return;
    bb_classic_name_candidate_t* candidate = &classic_name_candidates[classic_name_candidate_count++];
    memcpy(candidate->addr, addr, 6);
    candidate->page_scan_repetition_mode = page_scan_repetition_mode;
    candidate->clock_offset = clock_offset;
}

static void request_next_classic_name(void) {
    while (pairing && classic_name_candidate_index < classic_name_candidate_count) {
        bb_classic_name_candidate_t* candidate = &classic_name_candidates[classic_name_candidate_index];
        int status = gap_remote_name_request(candidate->addr, candidate->page_scan_repetition_mode, (uint16_t)(candidate->clock_offset | 0x8000u));
        if (status == ERROR_CODE_SUCCESS) {
            classic_name_request_active = true;
            return;
        }
        classic_name_candidate_index++;
    }
    reset_classic_name_candidates();
    if (pairing && scanning && !classic_connecting && !ble_connecting) gap_inquiry_start(5);
}

static void adv_name(const uint8_t* data, uint8_t length, char* out, size_t out_size) {
    if (!out_size) return;
    out[0] = 0;
    ad_context_t context;
    for (ad_iterator_init(&context, length, data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
        uint8_t type = ad_iterator_get_data_type(&context);
        if (type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME && type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME) continue;
        uint8_t n = ad_iterator_get_data_len(&context);
        if (n >= out_size) n = (uint8_t)(out_size - 1);
        memcpy(out, ad_iterator_get_data(&context), n);
        out[n] = 0;
        return;
    }
}

static bool adv_nintendo_identity(const uint8_t* data, uint8_t length, uint16_t* vid, uint16_t* pid) {
    ad_context_t context;
    for (ad_iterator_init(&context, length, data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
        if (ad_iterator_get_data_type(&context) != BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) continue;
        uint8_t n = ad_iterator_get_data_len(&context);
        const uint8_t* p = ad_iterator_get_data(&context);
        if (n < 9) continue;
        if (p[0] != 0x53 || p[1] != 0x05) continue;
        if (p[5] != 0x7e || p[6] != 0x05) continue;
        if (vid) *vid = 0x057e;
        if (pid) *pid = (uint16_t)p[7] | ((uint16_t)p[8] << 8);
        return true;
    }
    return false;
}

static const char* nintendo_ble_name(uint16_t pid) {
    if (pid == 0x2069) return "Nintendo Switch 2 Pro Controller";
    if (pid == 0x2073) return "Nintendo GameCube Controller";
    return "Nintendo Controller";
}

static bool accept_candidate(const uint8_t addr[6], const char* name, bool protocol_hint) {
    if (!bb_usb_ready()) return false;
    if (reconnect_blocked(addr)) return false;
    if (remembered_address(addr)) return true;
    if (!pairing) return false;
    return protocol_hint || likely_controller_name(name);
}

static void refresh_classic_identity(const char* name, uint16_t vid, uint16_t pid) {
    if (!classic_hid_cid || current_controller_index < 0) return;
    bb_controller_identity_t updated = current_identity;
    if (vid) updated.vendor_id = vid;
    if (pid) updated.product_id = pid;
    const char* resolved = name && name[0] ? name : current_identity.name;
    bb_controller_kind_t kind = bb_controller_identify(updated.vendor_id, updated.product_id, resolved);
    if (kind != BB_CONTROLLER_GENERIC) updated.kind = kind;
    bb_controller_display_name(updated.kind, resolved, updated.name, sizeof(updated.name));
    if (memcmp(&updated, &current_identity, sizeof(updated)) == 0) return;
    current_identity = updated;
    bb_driver_set_identity(&current_identity);
    bb_controller_update_identity((uint8_t)current_controller_index, updated.vendor_id, updated.product_id, updated.name);
    bb_state_set_controller(true, current_controller_index, updated.name);
    if (updated.kind == BB_CONTROLLER_8BITDO && name_has(updated.name, "pro 3")) hid_host_send_get_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x06);
}

static void classic_identity_result(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || !identity_query_active) return;
    uint8_t event = hci_event_packet_get_type(packet);
    if (event == SDP_EVENT_QUERY_ATTRIBUTE_VALUE) {
        uint16_t record = sdp_event_query_attribute_byte_get_record_id(packet);
        if (record != identity_query_record) {
            identity_query_record = record;
            identity_query_vid = 0;
            identity_query_pid = 0;
            identity_query_source = 0;
        }
        uint16_t length = sdp_event_query_attribute_byte_get_attribute_length(packet);
        uint16_t offset = sdp_event_query_attribute_byte_get_data_offset(packet);
        if (length != sizeof(identity_attribute) || offset >= length) return;
        identity_attribute[offset] = sdp_event_query_attribute_byte_get_data(packet);
        if (offset + 1 != length || identity_attribute[0] != 0x09) return;
        uint16_t value = ((uint16_t)identity_attribute[1] << 8) | identity_attribute[2];
        uint16_t attribute = sdp_event_query_attribute_byte_get_attribute_id(packet);
        if (attribute == BLUETOOTH_ATTRIBUTE_VENDOR_ID) identity_query_vid = value;
        if (attribute == BLUETOOTH_ATTRIBUTE_PRODUCT_ID) identity_query_pid = value;
        if (attribute == BLUETOOTH_ATTRIBUTE_VENDOR_ID_SOURCE) identity_query_source = value;
    } else if (event == SDP_EVENT_QUERY_COMPLETE) {
        identity_query_active = false;
        if (sdp_event_query_complete_get_status(packet) == ERROR_CODE_SUCCESS &&
            identity_query_generation == classic_generation && identity_query_cid == classic_hid_cid && same_address(identity_query_addr, current_identity.address) &&
            identity_query_source == DEVICE_ID_VENDOR_ID_SOURCE_USB && identity_query_vid && identity_query_pid) {
            refresh_classic_identity(NULL, identity_query_vid, identity_query_pid);
        }
    }
}

static void service_classic_identity(void) {
    if (!identity_query_pending || identity_query_active || !sdp_client_ready()) return;
    identity_query_pending = false;
    if (!classic_hid_cid || current_controller_index < 0) return;
    identity_query_generation = classic_generation;
    identity_query_cid = classic_hid_cid;
    memcpy(identity_query_addr, current_identity.address, sizeof(identity_query_addr));
    identity_query_record = 0xffff;
    identity_query_vid = 0;
    identity_query_pid = 0;
    identity_query_source = 0;
    identity_query_active = true;
    if (sdp_client_query_uuid16(classic_identity_result, identity_query_addr, BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION) != ERROR_CODE_SUCCESS) {
        identity_query_active = false;
        identity_query_pending = true;
    }
}

static void activate_controller(const uint8_t addr[6], uint8_t address_type, uint16_t vid, uint16_t pid, const char* name) {
    uint16_t old_identity = bb_current_mister_identity();
    memset(&current_identity, 0, sizeof(current_identity));
    memcpy(current_identity.address, addr, 6);
    current_identity.address_type = address_type;
    current_identity.vendor_id = vid;
    current_identity.product_id = pid;
    const char* stored_name = remembered_name(addr);
    const char* identity_name = name && name[0] ? name : stored_name;
    if ((!identity_name || !identity_name[0] || name_has(identity_name, "bluetooth controller")) && stored_name && stored_name[0]) identity_name = stored_name;
    current_identity.kind = bb_controller_identify(vid, pid, identity_name);
    if (stored_name && name_has(stored_name, "8bitdo") && (!name || !name[0] || name_has(name, "pro controller"))) {
        current_identity.kind = BB_CONTROLLER_8BITDO;
        identity_name = stored_name;
    }
    bb_controller_display_name(current_identity.kind, identity_name, current_identity.name, sizeof(current_identity.name));
    uint8_t controller_index = 0;
    if (!bb_controller_activate_or_create(addr, vid, pid, current_identity.name, &controller_index)) return;
    current_controller_index = controller_index;
    bb_driver_set_identity(&current_identity);
    if (current_identity.kind == BB_CONTROLLER_DUALSHOCK3) { send_ds3_activation(); send_ds3_rumble(0); }
    if (current_identity.kind == BB_CONTROLLER_DUALSHOCK4) send_ds4_rumble(0);
    if (current_identity.kind == BB_CONTROLLER_WII_U_PRO) {
        wiiu_setup_step = 1;
        wiiu_setup_at = btstack_run_loop_get_time_ms();
    }
    bb_state_set_controller(true, controller_index, current_identity.name);
    bb_state_set_scanning(false);
    bb_state_set_pairing(false);
    pairing = false;
    classic_connecting = false;
    classic_attempt_active = false;
    pending_classic_handle = HCI_CON_HANDLE_INVALID;
    ble_connecting = false;
    classic_reconnect_at = 0;

    gap_discoverable_control(1);
    scanning = false;
    last_controller_report_at = btstack_run_loop_get_time_ms();
    if (bb_output_mode() == BB_OUTPUT_MISTER && old_identity != bb_current_mister_identity()) bb_usb_request_reenumeration();
}

static void controller_disconnected(void) {
    classic_generation++;
    identity_query_pending = false;
    connected_name_request = false;
    reset_classic_name_candidates();
    reconnect_block_until = 0;
    memset(reconnect_block_addr, 0, sizeof(reconnect_block_addr));
    bb_input_state_t neutral;
    bb_input_clear(&neutral);
    bb_input_submit(&neutral);
    bb_driver_reset();
    memset(&current_identity, 0, sizeof(current_identity));
    current_controller_index = -1;
    bb_state_set_controller(false, -1, NULL);
    classic_connecting = false;
    classic_attempt_active = false;
    pending_classic_handle = HCI_CON_HANDLE_INVALID;
    ble_connecting = false;
    le_custom_nintendo = false;
    le_gamecube = false;
    le_handle = HCI_CON_HANDLE_INVALID;
    classic_hid_cid = 0;
    classic_handle = HCI_CON_HANDLE_INVALID;
    hids_cid = 0;
    dualsense_output_seq = 0;
    switch_output_seq = 0;
    classic_switch_setup_sent = false;
    classic_switch_protocol = false;
    switch_setup_state = 0;
    switch_setup_retries = 0;
    switch_setup_at = 0;
    manual_disconnect_pending = false;
    manual_disconnect_at = 0;
    wiiu_setup_step = 0;
    wiiu_setup_at = 0;
    rumble_request_pending = false;
    rumble_active = false;
    rumble_stop_at = 0;
    rumble_refresh_at = 0;
    rumble_active_strength = 0;
    switch2_rumble_seq = 0;
    last_controller_report_at = 0;
    if (stack_ready) {
        start_scans();
        schedule_classic_reconnect(3000u);
    }
}

static void start_scans(void) {
    if (!stack_ready || scanning || connection_in_progress()) return;
    scanning = true;
    bb_state_set_scanning(true);
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();

    if (pairing) gap_inquiry_start(5);
}

static void stop_scans(void) {
    if (!scanning) return;
    gap_stop_scan();
    gap_inquiry_stop();
    scanning = false;
    bb_state_set_scanning(false);
}

static void connect_classic(const uint8_t addr[6], const char* name) {
    if (classic_connecting || ble_connecting) return;
    reset_classic_name_candidates();
    classic_reconnect_at = 0;
    classic_connecting = true;
    classic_attempt_active = true;
    classic_attempt_started = btstack_run_loop_get_time_ms();
    stop_scans();
    memcpy(pending_addr, addr, 6);
    pending_addr_type = 0;
    copy_name(pending_name, sizeof(pending_name), name);
    pending_vid = 0;
    pending_pid = 0;
    uint8_t status = hid_host_connect(pending_addr, HID_PROTOCOL_MODE_REPORT, &classic_hid_cid);
    if (status != ERROR_CODE_SUCCESS) {
        classic_connecting = false;
        classic_attempt_active = false;
        start_scans();
        if (!pairing) schedule_classic_reconnect(20000u);
    }
}

static void connect_ble(const uint8_t addr[6], bd_addr_type_t addr_type, const char* name, bool custom_nintendo, bool gamecube, uint16_t vid, uint16_t pid) {
    if (connection_in_progress()) return;
    ble_connecting = true;
    stop_scans();
    memcpy(pending_addr, addr, 6);
    pending_addr_type = addr_type;
    copy_name(pending_name, sizeof(pending_name), name);
    pending_vid = vid;
    pending_pid = pid;
    le_custom_nintendo = custom_nintendo;
    le_gamecube = gamecube;
    uint8_t status = gap_connect(pending_addr, pending_addr_type);
    if (status != ERROR_CODE_SUCCESS) {
        ble_connecting = false;
        le_custom_nintendo = false;
        start_scans();
    }
}

static void nintendo_gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t type = hci_event_packet_get_type(packet);
    if (type == GATT_EVENT_NOTIFICATION) {
        uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
        if (value_handle != 0x000e) return;
        last_controller_report_at = btstack_run_loop_get_time_ms();
        bb_driver_handle_nintendo_ble_report(gatt_event_notification_get_value(packet), gatt_event_notification_get_value_length(packet), le_gamecube, pending_pid == 0x2069);
        return;
    }
    if (type == GATT_EVENT_QUERY_COMPLETE) {
        if (gatt_event_query_complete_get_att_status(packet) == ATT_ERROR_SUCCESS) {
            activate_controller(pending_addr, pending_addr_type, pending_vid, pending_pid, pending_name);
        } else {
            gap_disconnect(le_handle);
        }
    }
}

static void enable_nintendo_notifications(void) {
    memset(&nintendo_listener, 0, sizeof(nintendo_listener));
    memset(&nintendo_characteristic, 0, sizeof(nintendo_characteristic));
    nintendo_characteristic.start_handle = 0x000d;
    nintendo_characteristic.value_handle = 0x000e;
    nintendo_characteristic.end_handle = 0x0011;
    nintendo_characteristic.properties = ATT_PROPERTY_NOTIFY;
    memcpy(nintendo_characteristic.uuid128, le_gamecube ? nintendo_gc_uuid : nintendo_pro2_uuid, 16);
    gatt_client_listen_for_characteristic_value_updates(&nintendo_listener, nintendo_gatt_handler, le_handle, &nintendo_characteristic);
    gatt_client_write_client_characteristic_configuration(nintendo_gatt_handler, le_handle, &nintendo_characteristic, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
}

static void hids_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            if (gattservice_subevent_hid_service_connected_get_status(packet) != ERROR_CODE_SUCCESS) {
                gap_disconnect(le_handle);
                return;
            }
            activate_controller(pending_addr, pending_addr_type, pending_vid, pending_pid, pending_name);
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            uint8_t service = gattservice_subevent_hid_report_get_service_index(packet);
            bb_driver_set_hid_descriptor(hids_host_descriptor_storage_get_descriptor_data(hids_cid, service), hids_host_descriptor_storage_get_descriptor_len(hids_cid, service));
            last_controller_report_at = btstack_run_loop_get_time_ms();
            bb_driver_handle_hid_report(gattservice_subevent_hid_report_get_report(packet), gattservice_subevent_hid_report_get_report_len(packet));
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            controller_disconnected();
            break;
        default:
            break;
    }
}

static void sm_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    bool ready = false;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            ready = sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS;
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            ready = sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS;
            break;
        default:
            break;
    }
    if (!ready) return;
    if (le_custom_nintendo) {
        enable_nintendo_notifications();
    } else {
        hids_host_connect(le_handle, hids_handler, HID_PROTOCOL_MODE_REPORT, &hids_cid);
    }
}

static void classic_hid_event(uint8_t* packet) {
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION: {
            if (hid_subevent_incoming_connection_get_status(packet) != ERROR_CODE_SUCCESS) break;
            bd_addr_t addr;
            uint16_t incoming_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            hid_subevent_incoming_connection_get_address(packet, addr);
            if (reconnect_blocked(addr) || (!pairing && !remembered_address(addr))) {
                hid_host_decline_connection(incoming_cid);
                break;
            }
            bb_runtime_state_t state;
            bb_state_get(&state);
            if (state.controller_connected) {
                hid_host_decline_connection(incoming_cid);
                break;
            }
            if (ble_connecting) {
                hid_host_decline_connection(incoming_cid);
                break;
            }
            if (classic_connecting && memcmp(pending_addr, addr, sizeof(pending_addr)) != 0) {
                hid_host_decline_connection(incoming_cid);
                break;
            }
            uint16_t previous_cid = classic_hid_cid;
            classic_reconnect_at = 0;
            classic_hid_cid = incoming_cid;
            classic_connecting = true;
            classic_attempt_active = true;
            classic_attempt_started = btstack_run_loop_get_time_ms();
            stop_scans();
            memcpy(pending_addr, addr, sizeof(pending_addr));
            const char* known_name = remembered_name(addr);
            if (known_name && known_name[0]) copy_name(pending_name, sizeof(pending_name), known_name);
            else pending_name[0] = 0;
            if (previous_cid && previous_cid != incoming_cid) hid_host_disconnect(previous_cid);
            hid_host_accept_connection(incoming_cid, HID_PROTOCOL_MODE_REPORT);
            break;
        }
        case HID_SUBEVENT_CONNECTION_OPENED: {
            uint16_t event_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            if (classic_hid_cid && event_cid != classic_hid_cid) {
                if (hid_subevent_connection_opened_get_status(packet) == ERROR_CODE_SUCCESS) hid_host_disconnect(event_cid);
                break;
            }
            if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
                classic_connecting = false;
                classic_attempt_active = false;
                pending_classic_handle = HCI_CON_HANDLE_INVALID;
                classic_hid_cid = 0;
                classic_handle = HCI_CON_HANDLE_INVALID;
                bb_driver_reset();
                dualsense_output_seq = 0;
                switch_output_seq = 0;
                classic_switch_setup_sent = false;
                classic_switch_protocol = false;
                wiiu_setup_step = 0;
                wiiu_setup_at = 0;
                rumble_request_pending = false;
                rumble_active = false;
                rumble_stop_at = 0;
                rumble_refresh_at = 0;
                rumble_active_strength = 0;
                start_scans();
                if (!pairing) schedule_classic_reconnect(20000u);
                break;
            }
            if (!classic_attempt_active) {
                hid_host_disconnect(event_cid);
                break;
            }
            classic_hid_cid = event_cid;
            classic_handle = hid_subevent_connection_opened_get_con_handle(packet);
            dualsense_output_seq = 0;
            switch_output_seq = 0;
            classic_switch_setup_sent = false;
            classic_switch_protocol = false;
            wiiu_setup_step = 0;
            wiiu_setup_at = 0;
            rumble_request_pending = false;
            rumble_active = false;
            rumble_stop_at = 0;
            rumble_refresh_at = 0;
            rumble_active_strength = 0;
            bd_addr_t connected_addr;
            hid_subevent_connection_opened_get_bd_addr(packet, connected_addr);
            memcpy(pending_addr, connected_addr, 6);

            if (!bb_usb_ready() || (!pairing && !remembered_address(connected_addr))) {
                hid_host_disconnect(classic_hid_cid);
                break;
            }
            const char* known_name = remembered_name(connected_addr);
            const char* resolved_name = pending_name[0] && !name_has(pending_name, "bluetooth controller") ? pending_name : known_name;
            activate_controller(connected_addr, 0, 0, 0, resolved_name);
            if (current_identity.kind == BB_CONTROLLER_GENERIC || name_has(current_identity.name, "bluetooth controller")) {
                memcpy(connected_name_addr, connected_addr, sizeof(connected_name_addr));
                connected_name_request = gap_remote_name_request(connected_name_addr, 0, 0) == ERROR_CODE_SUCCESS;
            }
            break;
        }
        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            if (hid_subevent_descriptor_available_get_hid_cid(packet) != classic_hid_cid) break;
            if (hid_subevent_descriptor_available_get_status(packet) == ERROR_CODE_SUCCESS) {
                const uint8_t* descriptor = hid_descriptor_storage_get_descriptor_data(classic_hid_cid);
                uint16_t descriptor_len = hid_descriptor_storage_get_descriptor_len(classic_hid_cid);
                bb_driver_set_hid_descriptor(descriptor, descriptor_len);
                if (current_identity.kind == BB_CONTROLLER_GENERIC) identity_query_pending = true;
                bb_controller_identity_t refined = current_identity;
                if (bb_driver_refine_identity_from_hid_descriptor(descriptor, descriptor_len, &refined)) {
                    current_identity = refined;
                    bb_driver_set_identity(&current_identity);
                    if (current_controller_index >= 0) {
                        bb_controller_update_identity((uint8_t)current_controller_index, current_identity.vendor_id, current_identity.product_id, current_identity.name);
                        bb_state_set_controller(true, current_controller_index, current_identity.name);
                    }
                    if (current_identity.kind == BB_CONTROLLER_DUALSHOCK4) send_ds4_rumble(0);
                }
                if (current_identity.kind == BB_CONTROLLER_DUALSHOCK3) send_ds3_activation();
                if (current_identity.kind == BB_CONTROLLER_DUALSENSE || current_identity.kind == BB_CONTROLLER_DUALSENSE_EDGE) hid_host_send_get_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x05);
                if (current_identity.kind == BB_CONTROLLER_8BITDO && name_has(current_identity.name, "pro 3")) hid_host_send_get_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x06);
            }
            break;
        case HID_SUBEVENT_REPORT: {
            const uint8_t* report = hid_subevent_report_get_report(packet);
            uint16_t len = hid_subevent_report_get_report_len(packet);
            if (len && report[0] == 0xa1) {
                ++report;
                --len;
            }
            if (len && report[0] == 0x21) switch_setup_ack(report, len);
            if (len && (report[0] == 0x30 || report[0] == 0x3f)) {
                bool switch_kind = current_identity.kind == BB_CONTROLLER_SWITCH_PRO || current_identity.kind == BB_CONTROLLER_JOYCON_LEFT || current_identity.kind == BB_CONTROLLER_JOYCON_RIGHT || current_identity.kind == BB_CONTROLLER_8BITDO;
                if (switch_kind) {
                    classic_switch_protocol = true;
                    if (switch_setup_state == 0 && !classic_switch_setup_sent) switch_setup_begin();
                }
            }
            last_controller_report_at = btstack_run_loop_get_time_ms();
            bb_driver_handle_hid_report(report, len);
            break;
        }
        case HID_SUBEVENT_CONNECTION_CLOSED: {
            uint16_t closed_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            if (!classic_hid_cid || closed_cid == classic_hid_cid) controller_disconnected();
            break;
        }
        default:
            break;
    }
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);
    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                stack_ready = true;
                bb_state_set_bt_ready(true);
                start_scans();
                schedule_classic_reconnect(3000u);
            }
            break;
        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const char* name = remembered_name(addr);
            bool sony = name_has(name, "dualsense") || name_has(name, "dualshock");
            for (uint8_t i = 0; i < remembered_cfg.controller_count; ++i) {
                if (same_address(addr, remembered_cfg.controllers[i].bluetooth_address) && remembered_cfg.controllers[i].native_vendor_id == 0x054c) sony = true;
            }
            if (hci_event_connection_request_get_link_type(packet) == HCI_LINK_TYPE_ACL && sony &&
                !classic_connecting && !ble_connecting && current_controller_index < 0 && !reconnect_blocked(addr)) {
                reset_classic_name_candidates();
                memcpy(pending_addr, addr, sizeof(pending_addr));
                copy_name(pending_name, sizeof(pending_name), name);
                classic_connecting = true;
                classic_attempt_active = true;
                classic_attempt_started = btstack_run_loop_get_time_ms();
                classic_reconnect_at = 0;
                stop_scans();
            }
            break;
        }
        case HCI_EVENT_CONNECTION_COMPLETE: {
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            if (classic_attempt_active && same_address(addr, pending_addr)) {
                if (hci_event_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                    pending_classic_handle = hci_event_connection_complete_get_connection_handle(packet);
                } else {
                    controller_disconnected();
                }
            }
            break;
        }
        case GAP_EVENT_INQUIRY_RESULT: {
            if (classic_connecting || ble_connecting || classic_name_request_active) break;
            bd_addr_t addr;
            char name[32] = {0};
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            if (gap_event_inquiry_result_get_name_available(packet)) {
                uint8_t n = gap_event_inquiry_result_get_name_len(packet);
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, gap_event_inquiry_result_get_name(packet), n);
                name[n] = 0;
            }
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
            bool peripheral = (cod & 0x1f00u) == 0x0500u;
            uint32_t minor = cod & 0x3cu;
            bool keyboard_or_pointing = peripheral && (cod & 0xc0u);
            bool controller_peripheral = peripheral && !keyboard_or_pointing && (minor == 0x00u || minor == 0x04u || minor == 0x08u);
            bool remembered = remembered_address(addr);
            if (pairing && (remembered || controller_peripheral || likely_controller_name(name))) {
                if (accept_candidate(addr, name, controller_peripheral || remembered)) connect_classic(addr, name);
            } else if (pairing && !name[0]) {
                queue_classic_name_candidate(addr, gap_event_inquiry_result_get_page_scan_repetition_mode(packet), gap_event_inquiry_result_get_clock_offset(packet));
            }
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (pairing && scanning && !classic_connecting && !ble_connecting) {
                if (classic_name_candidate_count) request_next_classic_name();
                else gap_inquiry_start(5);
            }
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            if (connected_name_request) {
                bd_addr_t addr;
                reverse_bd_addr(&packet[3], addr);
                if (same_address(addr, connected_name_addr)) {
                    connected_name_request = false;
                    if (packet[2] == ERROR_CODE_SUCCESS && classic_hid_cid && same_address(addr, current_identity.address)) {
                        char resolved[BB_MAX_NAME];
                        copy_name(resolved, sizeof(resolved), (const char*)&packet[9]);
                        refresh_classic_identity(resolved, 0, 0);
                    }
                    break;
                }
            }
            if (!classic_name_request_active) break;
            bd_addr_t addr;
            reverse_bd_addr(&packet[3], addr);
            char name[32] = {0};
            if (packet[2] == ERROR_CODE_SUCCESS) {
                size_t n = 0;
                while (n < 248 && packet[9 + n]) n++;
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, &packet[9], n);
                name[n] = 0;
            }
            classic_name_request_active = false;
            if (packet[2] == ERROR_CODE_SUCCESS && likely_controller_name(name) && accept_candidate(addr, name, false)) {
                connect_classic(addr, name);
                break;
            }
            classic_name_candidate_index++;
            request_next_classic_name();
            break;
        }
        case GAP_EVENT_ADVERTISING_REPORT: {
            if (connection_in_progress()) break;
            bd_addr_t addr;
            char name[32];
            const uint8_t* data = gap_event_advertising_report_get_data(packet);
            uint8_t data_len = gap_event_advertising_report_get_data_length(packet);
            gap_event_advertising_report_get_address(packet, addr);
            adv_name(data, data_len, name, sizeof(name));
            bool hogp = ad_data_contains_uuid16(data_len, data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
            bool nso_new = ad_data_contains_uuid128(data_len, data, (uint8_t*)nintendo_service_uuid);
            uint16_t adv_vid = 0;
            uint16_t adv_pid = 0;
            bool nintendo_new = adv_nintendo_identity(data, data_len, &adv_vid, &adv_pid);
            bool gamecube = adv_pid == 0x2073 || name_has(name, "gamecube");
            bool switch2_pro = adv_pid == 0x2069 || name_has(name, "switch 2 pro") || name_has(name, "pro controller 2");
            bool custom_nintendo = nso_new || nintendo_new || (!hogp && (switch2_pro || gamecube));
            if (!hogp && !custom_nintendo) break;
            if (custom_nintendo) {
                if (!remembered_address(addr) && !pairing) break;
                if (!name[0]) copy_name(name, sizeof(name), nintendo_ble_name(adv_pid));
            } else if (!accept_candidate(addr, name, hogp)) {
                break;
            }
            connect_ble(addr, gap_event_advertising_report_get_address_type(packet), name, custom_nintendo, gamecube, adv_vid, adv_pid);
            break;
        }
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                if (gap_subevent_le_connection_complete_get_status(packet) != ERROR_CODE_SUCCESS) {
                    ble_connecting = false;
                    start_scans();
                    break;
                }
                le_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                if (le_custom_nintendo) enable_nintendo_notifications();
                else sm_request_pairing(le_handle);
            }
            break;
        case HCI_EVENT_HID_META:
            classic_hid_event(packet);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            if (pairing || remembered_address(addr)) gap_ssp_confirmation_response(addr);
            else gap_ssp_confirmation_negative(addr);
            break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            if (pairing || remembered_address(addr)) gap_pin_code_response(addr, "0000");
            else gap_pin_code_negative(addr);
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            if ((le_handle != HCI_CON_HANDLE_INVALID && handle == le_handle) ||
                (classic_handle != HCI_CON_HANDLE_INVALID && handle == classic_handle)) controller_disconnected();
            break;
        }
        default:
            break;
    }
}

static void usb_watch_handler(btstack_timer_source_t* timer) {
    bool ready = bb_usb_ready();
    uint32_t now = btstack_run_loop_get_time_ms();
    if (ready) {
        usb_was_ready = true;
        usb_lost_since = 0;
    } else if (usb_was_ready) {
        if (!usb_lost_since) usb_lost_since = now;
        if ((uint32_t)(now - usb_lost_since) >= 1500u) {
            bb_runtime_state_t state;
            bb_state_get(&state);
            if (state.controller_connected) bb_bt_host_disconnect_current();
            usb_was_ready = false;
            usb_lost_since = 0;
        }
    }
    if (wiiu_setup_step && classic_hid_cid && current_identity.kind == BB_CONTROLLER_WII_U_PRO && (int32_t)(now - wiiu_setup_at) >= 0) {
        bool sent = false;
        if (wiiu_setup_step == 1) sent = send_wiiu_write(0xa400f0u, 0x55);
        else if (wiiu_setup_step == 2) sent = send_wiiu_write(0xa400fbu, 0x00);
        else if (wiiu_setup_step == 3) sent = send_wiiu_report_mode();
        if (sent) {
            wiiu_setup_step++;
            wiiu_setup_at = now + 150u;
            if (wiiu_setup_step > 3) wiiu_setup_step = 0;
        }
    }
    if (classic_attempt_active && (uint32_t)(now - classic_attempt_started) >= 15000u) {
        uint16_t cid = classic_hid_cid;
        hci_con_handle_t handle = pending_classic_handle;
        if (cid) hid_host_disconnect(cid);
        if (handle != HCI_CON_HANDLE_INVALID) gap_disconnect(handle);
        controller_disconnected();
    }
    service_classic_identity();
    service_switch_setup(now);
    bb_runtime_state_t live_state;
    bb_state_get(&live_state);
    bool nintendo_stream = current_identity.kind == BB_CONTROLLER_SWITCH_PRO || current_identity.kind == BB_CONTROLLER_JOYCON_LEFT || current_identity.kind == BB_CONTROLLER_JOYCON_RIGHT || current_identity.kind == BB_CONTROLLER_SWITCH2_PRO || current_identity.kind == BB_CONTROLLER_NSO_GAMECUBE;
    if (live_state.controller_connected && nintendo_stream && last_controller_report_at && (uint32_t)(now - last_controller_report_at) >= 3000u) {
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        else if (classic_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(classic_handle);
        if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
        controller_disconnected();
    }
    if (classic_reconnect_at && (int32_t)(now - classic_reconnect_at) >= 0) {
        classic_reconnect_at = 0;
        bb_runtime_state_t state;
        bb_state_get(&state);
        if (!pairing && !connection_in_progress() && !state.controller_connected) {
            if (!bb_usb_ready()) {
                schedule_classic_reconnect(3000u);
            } else {
                bd_addr_t addr;
                char name[BB_MAX_NAME];
                if (classic_reconnect_target(addr, name, sizeof(name))) connect_classic(addr, name);
            }
        }
    }
    if (manual_disconnect_pending && (int32_t)(now - manual_disconnect_at) >= 0) {
        manual_disconnect_pending = false;
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        else if (classic_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(classic_handle);
    }
    service_rumble();
    btstack_run_loop_set_timer(timer, 100);
    btstack_run_loop_add_timer(timer);
}

void bb_bt_host_init(void) {
    reset_classic_name_candidates();
    bb_driver_reset();
    const btstack_tlv_t* tlv_impl = NULL;
    void* tlv_context = NULL;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl) {
        hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, tlv_context));
        le_device_db_tlv_configure(tlv_impl, tlv_context);
    }
    l2cap_init();
    sdp_init();
    device_id_create_sdp_record(host_device_id_record, 0x10001, DEVICE_ID_VENDOR_ID_SOURCE_USB, 0x2e8a, 0x10b1, 0x0100);
    sdp_register_service(host_device_id_record);
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
    gatt_client_init();
    hids_host_init(hids_descriptor_storage, sizeof(hids_descriptor_storage));
    hid_host_init(classic_hid_storage, sizeof(classic_hid_storage));
    hid_host_register_packet_handler(hci_handler);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_register_classic_connection_filter(classic_connection_filter);
    gap_connectable_control(1);

    gap_discoverable_control(1);
    hci_callback.callback = hci_handler;
    hci_add_event_handler(&hci_callback);
    sm_callback.callback = sm_handler;
    sm_add_event_handler(&sm_callback);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_set_timer_handler(&usb_watch_timer, usb_watch_handler);
    btstack_run_loop_set_timer(&usb_watch_timer, 100);
    btstack_run_loop_add_timer(&usb_watch_timer);
}

void bb_bt_host_start_pairing(void) {
    reset_classic_name_candidates();
    classic_reconnect_at = 0;
    pairing = true;
    bb_state_set_pairing(true);
    gap_discoverable_control(1);
    stop_scans();
    start_scans();
}

void bb_bt_host_stop_pairing(void) {
    reset_classic_name_candidates();
    pairing = false;
    bb_state_set_pairing(false);

    gap_discoverable_control(1);
    bb_runtime_state_t state;
    bb_state_get(&state);
    if (!state.controller_connected) {
        stop_scans();
        start_scans();
        schedule_classic_reconnect(3000u);
    }
}

void bb_bt_host_forget_all(void) {
    classic_reconnect_at = 0;
    gap_delete_all_link_keys();
    for (int i = 0; i < NVM_NUM_DEVICE_DB_ENTRIES; ++i) le_device_db_remove(i);
    pairing = true;
    bb_state_set_pairing(true);
    gap_discoverable_control(1);
    bb_state_set_controller(false, -1, NULL);
    start_scans();
}

bool bb_bt_host_forget_device(const uint8_t address[6]) {
    if (!address) return false;
    classic_reconnect_at = 0;
    bd_addr_t addr;
    memcpy(addr, address, 6);
    gap_drop_link_key_for_bd_addr(addr);
    int count = le_device_db_count();
    for (int i = count - 1; i >= 0; --i) {
        int addr_type = 0;
        bd_addr_t db_addr;
        le_device_db_info(i, &addr_type, db_addr, NULL);
        if (memcmp(db_addr, addr, 6) == 0) le_device_db_remove(i);
    }
    if (memcmp(current_identity.address, addr, 6) == 0) {
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
        controller_disconnected();
    }
    start_scans();
    return true;
}

bool bb_bt_host_disconnect_current(void) {

    uint32_t now = btstack_run_loop_get_time_ms();
    if (current_controller_index >= 0) {
        memcpy(reconnect_block_addr, current_identity.address, sizeof(reconnect_block_addr));
        reconnect_block_until = now + 1500u;
    } else {
        reconnect_block_until = 0;
        memset(reconnect_block_addr, 0, sizeof(reconnect_block_addr));
    }
    if (classic_hid_cid || classic_handle != HCI_CON_HANDLE_INVALID) {
        bool powered_off = false;
        if (current_identity.kind == BB_CONTROLLER_DUALSENSE || current_identity.kind == BB_CONTROLLER_DUALSENSE_EDGE) powered_off = send_dualsense_power_off();
        else if (classic_switch_protocol) powered_off = send_switch_subcommand(0x06, 0x00);
        if (powered_off) {
            manual_disconnect_pending = true;
            manual_disconnect_at = now + 900u;
        } else if (classic_hid_cid) {
            hid_host_disconnect(classic_hid_cid);
        } else {
            gap_disconnect(classic_handle);
        }
        return true;
    }
    if (le_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(le_handle);
        return true;
    }
    reconnect_block_until = 0;
    memset(reconnect_block_addr, 0, sizeof(reconnect_block_addr));
    return false;
}

bool bb_bt_host_rumble_test(uint8_t strength, uint16_t duration_ms) {
    if (!classic_hid_cid) return false;
    bb_runtime_state_t state;
    bb_state_get(&state);
    if (!state.rumble_supported) return false;
    if (duration_ms < 50) duration_ms = 50;
    if (duration_ms > 3000) duration_ms = 3000;
    rumble_request_strength = strength;
    rumble_request_duration_ms = duration_ms;
    rumble_request_pending = true;
    return true;
}

void bb_bt_host_set_output_rumble(uint8_t left, uint8_t right) {
    uint8_t strength = left > right ? left : right;
    rumble_request_strength = strength;
    rumble_request_duration_ms = strength ? 1200u : 50u;
    rumble_request_pending = true;
}
