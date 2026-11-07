#include "bt_host.h"
#include <stdio.h>
#include "bluebridge_state.h"
#include "controller_driver.h"
#include "controller_input.h"
#include "usb_device.h"
#include "btstack.h"
#include "btstack_version.h"
#include "ble/gatt-service/hids_host.h"
#include "ble/gatt-service/device_information_service_client.h"
#include "ble/gatt-service/battery_service_client.h"
#include "ble/att_db_util.h"
#include "ble/att_server.h"
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

#if defined(ENABLE_LE_SECURE_CONNECTIONS) && (BTSTACK_VERSION_MAJOR > 1 || (BTSTACK_VERSION_MAJOR == 1 && (BTSTACK_VERSION_MINOR > 8 || (BTSTACK_VERSION_MINOR == 8 && BTSTACK_VERSION_PATCH >= 2))))
#define BB_SM_DEFAULT_SC_ONLY true
#else
#define BB_SM_DEFAULT_SC_ONLY false
#endif

#define BB_HID_DESCRIPTOR_STORAGE 8192
#define BB_CLASSIC_HID_STORAGE 8192
#define BB_CLASSIC_NAME_CANDIDATES 8

static btstack_packet_callback_registration_t hci_callback;
static btstack_packet_callback_registration_t sm_callback;
static uint8_t hids_descriptor_storage[BB_HID_DESCRIPTOR_STORAGE];
static uint8_t classic_hid_storage[BB_CLASSIC_HID_STORAGE];
static uint8_t host_device_id_record[128];
static uint16_t host_service_changed_ccc_handle;
static uint16_t host_service_changed_ccc;
static uint8_t host_att_name[] = "MC BlueBridge";
static uint8_t host_att_appearance[] = {0x80, 0x00};
static uint8_t host_service_changed_range[] = {0x01, 0x00, 0xff, 0xff};
static bool stack_ready;
static bool scanning;
static bool scan_busy;
static bool pairing;
static bool classic_inquiry_active;
static uint32_t classic_inquiry_retry_at;
static bb_config_t remembered_cfg;
static btstack_timer_source_t usb_watch_timer;
static bool usb_was_ready;
static uint32_t usb_lost_since;
static uint32_t discovery_started_at;
static bool discovery_window_active;
static bool connection_settle_active;
static uint32_t connection_settle_started;
static bool ble_address_lookup_pending;
static bd_addr_t ble_address_lookup_addr;
static uint8_t ble_address_lookup_type;
static uint32_t ble_address_lookup_at;
typedef struct {
    uint32_t classic_attempt_started;
    bool classic_attempt_active;
    hci_con_handle_t pending_classic_handle;
    uint32_t classic_generation;
    bool disconnect_cleanup_active;
    hci_con_handle_t closing_handle;
    uint8_t classic_report_storage[2][128];
    uint8_t classic_report_bank;
    bool classic_setup_pending;
    uint32_t classic_setup_started;
    bool dualsense_activation_pending;
    uint8_t dualsense_activation_attempts;
    uint32_t dualsense_activation_at;
    uint32_t identity_query_generation;
    bool identity_query_pending;
    bool identity_query_active;
    uint16_t identity_query_cid;
    bd_addr_t identity_query_addr;
    uint16_t identity_query_record;
    uint16_t identity_query_vid;
    uint16_t identity_query_pid;
    uint16_t identity_query_source;
    uint8_t identity_attribute[3];
    uint32_t arcade_battery_poll_at;
    bool connected_name_request;
    bd_addr_t connected_name_addr;
    uint16_t classic_hid_cid;
    hci_con_handle_t classic_handle;
    uint16_t hids_cid;
    uint16_t xbox_battery_cid;
    hci_con_handle_t le_handle;
    bool classic_connecting;
    bool ble_connecting;
    uint32_t ble_attempt_started;
    bool ble_setup_started;
    bool ble_bond_retry;
    bool ble_security_retry;
    bool ble_secure_connections_only;
    bool ble_security_retry_pending;
    uint32_t ble_security_retry_at;
    uint8_t ultimate2c_setup_step;
    uint32_t ultimate2c_setup_at;
    uint8_t ultimate2c_protocol_step;
    uint32_t ultimate2c_protocol_at;
    uint16_t ultimate2c_protocol_handle;
    uint16_t ultimate2c_protocol_properties;
    bool ultimate2c_protocol_boot;
    const char* ble_stage;
    const char* ble_failed_stage;
    int16_t ble_setup_status;
    int16_t ble_pairing_reason;
    uint8_t ble_key_minimum;
    uint8_t ble_authentication;
    char ble_peer_name[32];
    uint32_t ble_attempt_count;
    bool le_custom_nintendo;
    bool le_gamecube;
    bd_addr_t pending_addr;
    bd_addr_type_t pending_addr_type;
    char pending_name[32];
    uint16_t pending_vid;
    uint16_t pending_pid;
    bb_controller_identity_t current_identity;
    int current_controller_index;
    gatt_client_notification_t nintendo_listener;
    gatt_client_characteristic_t nintendo_characteristic;
    bd_addr_t reconnect_block_addr;
    uint32_t reconnect_block_until;
    volatile bool queued_pending;
    volatile uint8_t queued_strength;
    volatile uint16_t queued_duration;
    bool rumble_active;
    uint8_t xbox_rumble_payload[8];
    bool xbox_rumble_write_pending;
    bool xbox_rumble_retry;
    uint8_t eightbitdo64_rumble_payload[4];
    bool eightbitdo64_rumble_write_pending;
    bool eightbitdo64_rumble_retry;
    bool eightbitdo_ble_rumble;
    uint8_t dualsense_output_seq;
    uint8_t ds4_output_report[78];
    bool ds4_descriptor_ready;
    bool ds4_activation_pending;
    uint32_t ds4_activation_at;
    uint8_t ds4_activation_attempts;
    uint8_t ds4_activation_step;
    uint8_t switch_output_seq;
    bool classic_switch_setup_sent;
    bool classic_switch_protocol;
    uint8_t switch_setup_state;
    uint8_t switch_setup_retries;
    uint32_t switch_setup_at;
    bool manual_disconnect_pending;
    uint32_t manual_disconnect_at;
    uint8_t wiiu_setup_step;
    uint32_t wiiu_setup_at;
    uint32_t rumble_stop_at;
    uint32_t rumble_refresh_at;
    uint8_t rumble_active_strength;
    uint8_t switch2_rumble_seq;
    uint32_t last_controller_report_at;
} bb_host_context_t;
static bb_host_context_t host_slots[BB_MAX_SLOTS] = {
{.closing_handle = HCI_CON_HANDLE_INVALID,.pending_classic_handle = HCI_CON_HANDLE_INVALID,.classic_handle = HCI_CON_HANDLE_INVALID,.le_handle = HCI_CON_HANDLE_INVALID,.ble_secure_connections_only = BB_SM_DEFAULT_SC_ONLY,.ble_stage = "idle",.ble_failed_stage = "none",.ble_setup_status = -1,.ble_pairing_reason = -1,.ble_key_minimum = 7,.ble_authentication = SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION,.current_controller_index = -1},
{.closing_handle = HCI_CON_HANDLE_INVALID,.pending_classic_handle = HCI_CON_HANDLE_INVALID,.classic_handle = HCI_CON_HANDLE_INVALID,.le_handle = HCI_CON_HANDLE_INVALID,.ble_secure_connections_only = BB_SM_DEFAULT_SC_ONLY,.ble_stage = "idle",.ble_failed_stage = "none",.ble_setup_status = -1,.ble_pairing_reason = -1,.ble_key_minimum = 7,.ble_authentication = SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION,.current_controller_index = -1}
};
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

static int slot_for_handle(uint16_t handle) {
    if (handle == HCI_CON_HANDLE_INVALID) return -1;
    for (int i = 0; i < BB_MAX_SLOTS; ++i) {
        if (host_slots[i].le_handle == handle || host_slots[i].classic_handle == handle || host_slots[i].pending_classic_handle == handle || host_slots[i].closing_handle == handle) return i;
    }
    return -1;
}
static int slot_for_cid(uint16_t cid, unsigned kind) {
    if (!cid) return -1;
    for (int i = 0; i < BB_MAX_SLOTS; ++i) {
        uint16_t known = kind == 0 ? host_slots[i].classic_hid_cid : kind == 1 ? host_slots[i].hids_cid : host_slots[i].xbox_battery_cid;
        if (known == cid) return i;
    }
    return -1;
}
static int slot_for_address_except(const uint8_t addr[6], int excluded) {
    for (int i = 0; i < BB_MAX_SLOTS; ++i) {
        if (i == excluded) continue;
        bb_host_context_t* c = &host_slots[i];
        if (((c->classic_hid_cid || c->classic_handle != HCI_CON_HANDLE_INVALID || c->le_handle != HCI_CON_HANDLE_INVALID || c->closing_handle != HCI_CON_HANDLE_INVALID) && (!memcmp(c->current_identity.address, addr, 6) || !memcmp(c->pending_addr, addr, 6))) ||
            ((c->classic_connecting || c->ble_connecting) && !memcmp(c->pending_addr, addr, 6))) return i;
    }
    return -1;
}
static int slot_for_address(const uint8_t addr[6]) {
    return slot_for_address_except(addr, -1);
}
static bool any_setup(void) {
    for (int i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) {
        if (host_slots[i].classic_connecting || host_slots[i].ble_connecting || host_slots[i].classic_setup_pending) return true;
    }
    return false;
}
static bool ble_setup_elsewhere(uint8_t owner) {
    for (uint8_t i = 0; i < BB_MAX_SLOTS; ++i) {
        if (i != owner && host_slots[i].ble_connecting) return true;
    }
    return false;
}
static int free_slot(void) {
    for (int i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) {
        bb_host_context_t* c = &host_slots[i];
        if (c->current_controller_index < 0 && !c->classic_connecting && !c->ble_connecting && !c->classic_hid_cid && c->classic_handle == HCI_CON_HANDLE_INVALID && c->pending_classic_handle == HCI_CON_HANDLE_INVALID && c->le_handle == HCI_CON_HANDLE_INVALID && c->closing_handle == HCI_CON_HANDLE_INVALID) return i;
    }
    return -1;
}
static bool connected_controller_present(void) {
    for (int i = 0; i < BB_MAX_SLOTS; ++i) {
        if (host_slots[i].current_controller_index >= 0) return true;
    }
    return false;
}
static bool multi_discovery_paused(uint32_t now) {
    if (!bb_multi_mode() || pairing || !connection_settle_active) return false;
    if (!connected_controller_present() || (uint32_t)(now - connection_settle_started) >= 1500u) {
        connection_settle_active = false;
        return false;
    }
    return true;
}
static int route_hci(const uint8_t* packet, uint16_t size) {
    if (!bb_multi_mode()) return 0;
    if (size < 2) return -1;
    uint8_t event = packet[0];
    bd_addr_t addr;
    switch (event) {
        case HCI_EVENT_HID_META:
            if (size < 5) return -1;
            if (packet[2] == HID_SUBEVENT_INCOMING_CONNECTION) {
                hid_subevent_incoming_connection_get_address(packet, addr);
                int known = slot_for_address(addr);
                if (known >= 0) return known;
                int empty = free_slot();
                if (empty < 0) hid_host_decline_connection(hid_subevent_incoming_connection_get_hid_cid(packet));
                return empty;
            }
            return slot_for_cid(little_endian_read_16(packet, 3), 0);
        case HCI_EVENT_CONNECTION_REQUEST:
            hci_event_connection_request_get_bd_addr(packet, addr);
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            hci_event_connection_complete_get_bd_addr(packet, addr);
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            if (size < 9) return -1;
            reverse_bd_addr(packet + 3, addr);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            return slot_for_handle(hci_event_disconnection_complete_get_connection_handle(packet));
        case HCI_EVENT_META_GAP:
            if (size < 4 || packet[2] != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) return -1;
            for (int i = 0; i < BB_MAX_SLOTS; ++i) if (host_slots[i].ble_connecting) return i;
            return -1;
        case GAP_EVENT_INQUIRY_RESULT:
        case GAP_EVENT_ADVERTISING_REPORT:
        case GAP_EVENT_INQUIRY_COMPLETE:
            return any_setup() ? -1 : free_slot();
        default: return 0;
    }
    int known = slot_for_address(addr);
    return known >= 0 ? known : free_slot();
}

static bool same_address(const uint8_t a[6], const uint8_t b[6]);
static bool remembered_address(const uint8_t addr[6]);
static int classic_connection_filter(bd_addr_t addr, hci_link_type_t link_type) {
    if (link_type != HCI_LINK_TYPE_ACL) return 1;
    return free_slot() >= 0 && (pairing || remembered_address(addr));
}

bool bb_bt_host_diagnostics(char* out, size_t size) {
    return bb_bt_host_diagnostics_slot(bb_slot_current(), out, size);
}

bool bb_bt_host_diagnostics_slot(uint8_t slot, char* out, size_t size) {
    if (slot >= (bb_multi_mode() ? BB_MAX_SLOTS : 1)) return false;
    const bb_host_context_t* context = &host_slots[slot];
    if (!out || size < 512) return false;
    char name_hex[65];
    static const char digits[] = "0123456789abcdef";
    const char* peer_name = context->ble_peer_name[0] ? context->ble_peer_name : context->pending_name;
    size_t name_length = strlen(peer_name);
    for (size_t i = 0; i < name_length; ++i) {
        uint8_t value = (uint8_t)peer_name[i];
        name_hex[i * 2] = digits[value >> 4];
        name_hex[i * 2 + 1] = digits[value & 15];
    }
    name_hex[name_length * 2] = 0;
    char identity_hex[13];
    char transport_hex[13];
    for (size_t i = 0; i < 6; ++i) {
        identity_hex[i * 2] = digits[context->current_identity.address[i] >> 4];
        identity_hex[i * 2 + 1] = digits[context->current_identity.address[i] & 15];
        transport_hex[i * 2] = digits[context->pending_addr[i] >> 4];
        transport_hex[i * 2 + 1] = digits[context->pending_addr[i] & 15];
    }
    identity_hex[12] = 0;
    transport_hex[12] = 0;
    int written = snprintf(out, size, "{\"peer_name_hex\":\"%s\",\"identity_address\":\"%s\",\"transport_address\":\"%s\",\"slot\":%u,\"key_minimum\":%u,\"authentication\":%u,\"pairing_reason\":%d,\"ble_stage\":\"%s\",\"failed_stage\":\"%s\",\"status\":%d,\"attempts\":%lu,\"ble_connecting\":%s,\"le_handle\":%u,\"hid_cid\":%u,\"controller_index\":%d,\"classic_connecting\":%s,\"classic_attempt_active\":%s,\"security_retry\":%s,\"secure_connections_only\":%s}",
        name_hex, identity_hex, transport_hex, slot, context->ble_key_minimum, context->ble_authentication, context->ble_pairing_reason, context->ble_stage, context->ble_failed_stage, context->ble_setup_status, (unsigned long)context->ble_attempt_count,
        context->ble_connecting ? "true" : "false", context->le_handle, context->hids_cid, context->current_controller_index,
        context->classic_connecting ? "true" : "false", context->classic_attempt_active ? "true" : "false", context->ble_security_retry ? "true" : "false", context->ble_secure_connections_only ? "true" : "false");
    return written >= 0 && (size_t)written < size;
}

#define classic_attempt_started (host_slots[bb_slot_current()].classic_attempt_started)
#define classic_attempt_active (host_slots[bb_slot_current()].classic_attempt_active)
#define pending_classic_handle (host_slots[bb_slot_current()].pending_classic_handle)
#define classic_report_storage (host_slots[bb_slot_current()].classic_report_storage)
#define classic_report_bank (host_slots[bb_slot_current()].classic_report_bank)
#define classic_setup_pending (host_slots[bb_slot_current()].classic_setup_pending)
#define classic_setup_started (host_slots[bb_slot_current()].classic_setup_started)
#define dualsense_activation_pending (host_slots[bb_slot_current()].dualsense_activation_pending)
#define dualsense_activation_attempts (host_slots[bb_slot_current()].dualsense_activation_attempts)
#define dualsense_activation_at (host_slots[bb_slot_current()].dualsense_activation_at)
#define closing_handle (host_slots[bb_slot_current()].closing_handle)
#define disconnect_cleanup_active (host_slots[bb_slot_current()].disconnect_cleanup_active)
#define classic_generation (host_slots[bb_slot_current()].classic_generation)
#define identity_query_generation (host_slots[bb_slot_current()].identity_query_generation)
#define identity_query_pending (host_slots[bb_slot_current()].identity_query_pending)
#define identity_query_active (host_slots[bb_slot_current()].identity_query_active)
#define identity_query_cid (host_slots[bb_slot_current()].identity_query_cid)
#define identity_query_addr (host_slots[bb_slot_current()].identity_query_addr)
#define identity_query_record (host_slots[bb_slot_current()].identity_query_record)
#define identity_query_vid (host_slots[bb_slot_current()].identity_query_vid)
#define identity_query_pid (host_slots[bb_slot_current()].identity_query_pid)
#define identity_query_source (host_slots[bb_slot_current()].identity_query_source)
#define identity_attribute (host_slots[bb_slot_current()].identity_attribute)
#define arcade_battery_poll_at (host_slots[bb_slot_current()].arcade_battery_poll_at)
#define connected_name_request (host_slots[bb_slot_current()].connected_name_request)
#define connected_name_addr (host_slots[bb_slot_current()].connected_name_addr)
#define classic_hid_cid (host_slots[bb_slot_current()].classic_hid_cid)
#define classic_handle (host_slots[bb_slot_current()].classic_handle)
#define hids_cid (host_slots[bb_slot_current()].hids_cid)
#define xbox_battery_cid (host_slots[bb_slot_current()].xbox_battery_cid)
#define le_handle (host_slots[bb_slot_current()].le_handle)
#define classic_connecting (host_slots[bb_slot_current()].classic_connecting)
#define ble_connecting (host_slots[bb_slot_current()].ble_connecting)
#define ble_attempt_started (host_slots[bb_slot_current()].ble_attempt_started)
#define ble_setup_started (host_slots[bb_slot_current()].ble_setup_started)
#define ble_bond_retry (host_slots[bb_slot_current()].ble_bond_retry)
#define ble_security_retry (host_slots[bb_slot_current()].ble_security_retry)
#define ble_secure_connections_only (host_slots[bb_slot_current()].ble_secure_connections_only)
#define ble_security_retry_pending (host_slots[bb_slot_current()].ble_security_retry_pending)
#define ble_security_retry_at (host_slots[bb_slot_current()].ble_security_retry_at)
#define ultimate2c_setup_step (host_slots[bb_slot_current()].ultimate2c_setup_step)
#define ultimate2c_setup_at (host_slots[bb_slot_current()].ultimate2c_setup_at)
#define ultimate2c_protocol_step (host_slots[bb_slot_current()].ultimate2c_protocol_step)
#define ultimate2c_protocol_at (host_slots[bb_slot_current()].ultimate2c_protocol_at)
#define ultimate2c_protocol_handle (host_slots[bb_slot_current()].ultimate2c_protocol_handle)
#define ultimate2c_protocol_properties (host_slots[bb_slot_current()].ultimate2c_protocol_properties)
#define ultimate2c_protocol_boot (host_slots[bb_slot_current()].ultimate2c_protocol_boot)
#define ble_stage (host_slots[bb_slot_current()].ble_stage)
#define ble_failed_stage (host_slots[bb_slot_current()].ble_failed_stage)
#define ble_setup_status (host_slots[bb_slot_current()].ble_setup_status)
#define ble_pairing_reason (host_slots[bb_slot_current()].ble_pairing_reason)
#define ble_key_minimum (host_slots[bb_slot_current()].ble_key_minimum)
#define ble_authentication (host_slots[bb_slot_current()].ble_authentication)
#define ble_peer_name (host_slots[bb_slot_current()].ble_peer_name)
#define ble_attempt_count (host_slots[bb_slot_current()].ble_attempt_count)
#define le_custom_nintendo (host_slots[bb_slot_current()].le_custom_nintendo)
#define le_gamecube (host_slots[bb_slot_current()].le_gamecube)
#define pending_addr (host_slots[bb_slot_current()].pending_addr)
#define pending_addr_type (host_slots[bb_slot_current()].pending_addr_type)
#define pending_name (host_slots[bb_slot_current()].pending_name)
#define pending_vid (host_slots[bb_slot_current()].pending_vid)
#define pending_pid (host_slots[bb_slot_current()].pending_pid)
#define current_identity (host_slots[bb_slot_current()].current_identity)
#define current_controller_index (host_slots[bb_slot_current()].current_controller_index)
#define nintendo_listener (host_slots[bb_slot_current()].nintendo_listener)
#define nintendo_characteristic (host_slots[bb_slot_current()].nintendo_characteristic)
#define reconnect_block_addr (host_slots[bb_slot_current()].reconnect_block_addr)
#define reconnect_block_until (host_slots[bb_slot_current()].reconnect_block_until)
#define rumble_request_pending (host_slots[bb_slot_current()].queued_pending)
#define rumble_request_strength (host_slots[bb_slot_current()].queued_strength)
#define rumble_request_duration_ms (host_slots[bb_slot_current()].queued_duration)
#define rumble_active (host_slots[bb_slot_current()].rumble_active)
#define xbox_rumble_payload (host_slots[bb_slot_current()].xbox_rumble_payload)
#define xbox_rumble_write_pending (host_slots[bb_slot_current()].xbox_rumble_write_pending)
#define xbox_rumble_retry (host_slots[bb_slot_current()].xbox_rumble_retry)
#define eightbitdo64_rumble_payload (host_slots[bb_slot_current()].eightbitdo64_rumble_payload)
#define eightbitdo64_rumble_write_pending (host_slots[bb_slot_current()].eightbitdo64_rumble_write_pending)
#define eightbitdo64_rumble_retry (host_slots[bb_slot_current()].eightbitdo64_rumble_retry)
#define eightbitdo_ble_rumble (host_slots[bb_slot_current()].eightbitdo_ble_rumble)
#define dualsense_output_seq (host_slots[bb_slot_current()].dualsense_output_seq)
#define ds4_output_report (host_slots[bb_slot_current()].ds4_output_report)
#define ds4_descriptor_ready (host_slots[bb_slot_current()].ds4_descriptor_ready)
#define ds4_activation_pending (host_slots[bb_slot_current()].ds4_activation_pending)
#define ds4_activation_at (host_slots[bb_slot_current()].ds4_activation_at)
#define ds4_activation_attempts (host_slots[bb_slot_current()].ds4_activation_attempts)
#define ds4_activation_step (host_slots[bb_slot_current()].ds4_activation_step)
#define switch_output_seq (host_slots[bb_slot_current()].switch_output_seq)
#define classic_switch_setup_sent (host_slots[bb_slot_current()].classic_switch_setup_sent)
#define classic_switch_protocol (host_slots[bb_slot_current()].classic_switch_protocol)
#define switch_setup_state (host_slots[bb_slot_current()].switch_setup_state)
#define switch_setup_retries (host_slots[bb_slot_current()].switch_setup_retries)
#define switch_setup_at (host_slots[bb_slot_current()].switch_setup_at)
#define manual_disconnect_pending (host_slots[bb_slot_current()].manual_disconnect_pending)
#define manual_disconnect_at (host_slots[bb_slot_current()].manual_disconnect_at)
#define wiiu_setup_step (host_slots[bb_slot_current()].wiiu_setup_step)
#define wiiu_setup_at (host_slots[bb_slot_current()].wiiu_setup_at)
#define rumble_stop_at (host_slots[bb_slot_current()].rumble_stop_at)
#define rumble_refresh_at (host_slots[bb_slot_current()].rumble_refresh_at)
#define rumble_active_strength (host_slots[bb_slot_current()].rumble_active_strength)
#define switch2_rumble_seq (host_slots[bb_slot_current()].switch2_rumble_seq)
#define last_controller_report_at (host_slots[bb_slot_current()].last_controller_report_at)

static int sdp_owner;
static void start_scans(void);
static void start_classic_inquiry(void);
static void stop_scans(void);
static bool name_has(const char* name, const char* part);
static void reset_classic_name_candidates(void);
static void request_next_classic_name(void);

static bool connection_in_progress(void) {
    return any_setup() || classic_name_request_active;
}

static uint8_t send_classic_report(uint16_t cid, uint16_t report_id, const uint8_t* report, uint16_t length) {
    if (!cid || cid != classic_hid_cid || !report || length > sizeof(classic_report_storage[0])) return ERROR_CODE_COMMAND_DISALLOWED;
    uint8_t bank = classic_report_bank ^ 1u;
    memcpy(classic_report_storage[bank], report, length);
    uint8_t status = hid_host_send_report(cid, report_id, classic_report_storage[bank], length);
    if (status == ERROR_CODE_SUCCESS) classic_report_bank = bank;
    return status;
}
static uint8_t send_classic_set_report(uint16_t cid, hid_report_type_t type, uint16_t report_id, const uint8_t* report, uint8_t length) {
    if (!cid || cid != classic_hid_cid || !report || length > sizeof(classic_report_storage[0])) return ERROR_CODE_COMMAND_DISALLOWED;
    uint8_t bank = classic_report_bank ^ 1u;
    memcpy(classic_report_storage[bank], report, length);
    uint8_t status = hid_host_send_set_report(cid, type, report_id, classic_report_storage[bank], length);
    if (status == ERROR_CODE_SUCCESS) classic_report_bank = bank;
    return status;
}
static void finish_classic_setup(void) {
    if (!classic_setup_pending) return;
    classic_setup_pending = false;
    if (bb_multi_mode()) {
        connection_settle_started = btstack_run_loop_get_time_ms();
        connection_settle_active = true;
    }
}
static void controller_disconnected(bool link_closed);
static void service_classic_setup(uint32_t now) {
    if (classic_setup_pending && (uint32_t)(now - classic_setup_started) >= 15000u) controller_disconnected(false);
}
static void service_dualsense_activation(uint32_t now) {
    if (!dualsense_activation_pending) return;
    if (!classic_hid_cid || closing_handle != HCI_CON_HANDLE_INVALID || manual_disconnect_pending ||
        (current_identity.kind != BB_CONTROLLER_DUALSENSE && current_identity.kind != BB_CONTROLLER_DUALSENSE_EDGE)) {
        dualsense_activation_pending = false;
        return;
    }
    if ((int32_t)(now - dualsense_activation_at) < 0) return;
    uint8_t status = hid_host_send_get_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x05);
    ++dualsense_activation_attempts;
    dualsense_activation_pending = status == ERROR_CODE_COMMAND_DISALLOWED && dualsense_activation_attempts < 10;
    dualsense_activation_at = now + 100u;
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
    return send_classic_set_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x08, &report[1], 46) == ERROR_CODE_SUCCESS;
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
    return send_classic_report(classic_hid_cid, 0x31, &report[1], 77) == ERROR_CODE_SUCCESS;
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
    return send_classic_report(classic_hid_cid, 0x01, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_ds4_rumble(uint8_t strength) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_DUALSHOCK4 || ds4_activation_step == 2) return false;
    uint8_t* report = ds4_output_report;
    memset(report, 0, sizeof(ds4_output_report));
    report[0] = 0x11;
    report[1] = 0xc4;
    report[3] = 0x07;
    report[6] = strength;
    report[7] = strength;
    report[10] = 0x40;
    uint32_t crc = dualsense_output_crc(report, 74);
    report[74] = (uint8_t)crc;
    report[75] = (uint8_t)(crc >> 8);
    report[76] = (uint8_t)(crc >> 16);
    report[77] = (uint8_t)(crc >> 24);
    return send_classic_report(classic_hid_cid, 0x11, &report[1], 77) == ERROR_CODE_SUCCESS;
}

static bool send_ds3_activation(void) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_DUALSHOCK3) return false;
    uint8_t payload[4] = {0x42, 0x03, 0x00, 0x00};
    return send_classic_set_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0xf4, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
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
    return send_classic_report(classic_hid_cid, 0x01, &report[1], 35) == ERROR_CODE_SUCCESS;
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
    return send_classic_report(classic_hid_cid, 0x01, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_n64_calibration_read(bool user) {
    uint8_t payload[15] = {0};
    payload[0] = (uint8_t)(switch_output_seq++ & 0x0fu);
    payload[2] = payload[6] = 0x01;
    payload[3] = payload[4] = payload[7] = payload[8] = 0x40;
    payload[9] = 0x10;
    payload[10] = user ? 0x10 : 0x3d;
    payload[11] = user ? 0x80 : 0x60;
    payload[14] = user ? 11 : 9;
    return send_classic_report(classic_hid_cid, 0x01, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static void switch_setup_begin(void) {
    switch_setup_state = 1;
    switch_setup_retries = 0;
    switch_setup_at = btstack_run_loop_get_time_ms();
    classic_switch_setup_sent = false;
    bb_state_set_rumble_supported(false);
}

static void service_switch_setup(uint32_t now) {
    // Keep SNES in its default HID report mode, matching the generic-host approach.
    if (current_identity.kind == BB_CONTROLLER_NSO_SNES) return;
    if (!classic_hid_cid || !classic_switch_protocol || switch_setup_state == 0 || (switch_setup_state == 7 && current_identity.kind != BB_CONTROLLER_NSO_N64) || switch_setup_state == 11) return;
    if ((int32_t)(now - switch_setup_at) < 0) return;
    bool sent = false;
    if (switch_setup_state == 1) sent = send_switch_report_mode(0x30);
    else if (switch_setup_state == 3) sent = send_switch_subcommand(0x48, 0x01);
    else if (switch_setup_state == 5) sent = send_switch_subcommand(0x30, 0x01);
    else if (switch_setup_state == 7) sent = send_n64_calibration_read(true);
    else if (switch_setup_state == 9) sent = send_n64_calibration_read(false);
    else {
        if (switch_setup_retries >= 2) {
            if (switch_setup_state == 8 || switch_setup_state == 10) {
                switch_setup_state = switch_setup_state == 8 ? 9 : 11;
                switch_setup_retries = 0;
                switch_setup_at = now;
                return;
            }
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
        bool has_rumble = current_identity.kind != BB_CONTROLLER_NSO_NES && current_identity.kind != BB_CONTROLLER_NSO_SNES && current_identity.kind != BB_CONTROLLER_NSO_GENESIS;
        bb_state_set_rumble_supported(has_rumble);
    } else if (switch_setup_state == 6 && subcommand == 0x30) {
        switch_setup_state = 7;
        classic_switch_setup_sent = true;
        switch_setup_retries = 0;
        switch_setup_at = now;
    } else if (subcommand == 0x10 && current_identity.kind == BB_CONTROLLER_NSO_N64 && len >= 20) {
        bool user = switch_setup_state == 8;
        bool factory = switch_setup_state == 10;
        uint32_t address = (uint32_t)report[15] | ((uint32_t)report[16] << 8) | ((uint32_t)report[17] << 16) | ((uint32_t)report[18] << 24);
        uint8_t count = report[19];
        if (len < 20u + count) return;
        bool valid = false;
        if (user && address == 0x8010u && count == 11) {
            valid = report[20] == 0xb2 && report[21] == 0xa1 && bb_driver_set_n64_calibration(report + 22, 9);
        } else if (factory && address == 0x603du && count == 9) {
            valid = bb_driver_set_n64_calibration(report + 20, 9);
        } else return;
        switch_setup_state = valid || factory ? 11 : 9;
        switch_setup_retries = 0;
        switch_setup_at = now;
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
    return send_classic_report(classic_hid_cid, 0x10, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

// 8BitDo controllers over BLE that take rumble as output report 5 (four 0-100
// magnitudes, PID page Magnitude): the 8BitDo 64, and any other 8BitDo whose HID
// descriptor declares that report (e.g. the Ultimate 2 Wireless, which reports
// no VID/PID over BLE).
static bool eightbitdo64_ble_connected(void) {
    return le_handle != HCI_CON_HANDLE_INVALID && hids_cid && current_identity.kind == BB_CONTROLLER_8BITDO &&
        ((current_identity.vendor_id == 0x2dc8 && current_identity.product_id == 0x3019) || eightbitdo_ble_rumble);
}

static bool send_8bitdo64_ble_rumble(uint8_t strength) {
    eightbitdo64_rumble_retry = false;
    if (!eightbitdo64_ble_connected()) return false;
    if (eightbitdo64_rumble_write_pending) {
        eightbitdo64_rumble_retry = true;
        return false;
    }
    uint8_t magnitude = (uint8_t)(((uint16_t)strength * 100u) / 255u);
    eightbitdo64_rumble_payload[0] = magnitude;
    eightbitdo64_rumble_payload[1] = magnitude;
    eightbitdo64_rumble_payload[2] = 0;
    eightbitdo64_rumble_payload[3] = 0;
    uint8_t status = hids_host_send_write_report(hids_cid, 5, HID_REPORT_TYPE_OUTPUT, eightbitdo64_rumble_payload, sizeof(eightbitdo64_rumble_payload));
    if (status == ERROR_CODE_SUCCESS) {
        eightbitdo64_rumble_write_pending = true;
        return true;
    }
    eightbitdo64_rumble_retry = status == ERROR_CODE_COMMAND_DISALLOWED || status == GATT_CLIENT_IN_WRONG_STATE;
    if (status == ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE) bb_state_set_rumble_supported(false);
    return false;
}

static bool send_8bitdo_dinput_rumble(uint8_t strength) {
    if (eightbitdo64_ble_connected()) return send_8bitdo64_ble_rumble(strength);
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_8BITDO || classic_switch_protocol) return false;
    uint8_t payload[4] = {strength, strength, 0, 0};
    return send_classic_report(classic_hid_cid, 0x05, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
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
    return send_classic_report(classic_hid_cid, 0x16, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_wiiu_report_mode(void) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_WII_U_PRO) return false;
    uint8_t payload[2] = {0x04, 0x34};
    return send_classic_report(classic_hid_cid, 0x12, payload, sizeof(payload)) == ERROR_CODE_SUCCESS;
}

static bool send_wiiu_rumble(uint8_t strength) {
    if (!classic_hid_cid || current_identity.kind != BB_CONTROLLER_WII_U_PRO) return false;
    uint8_t payload = strength ? 0x01 : 0x00;
    return send_classic_report(classic_hid_cid, 0x10, &payload, 1) == ERROR_CODE_SUCCESS;
}

static bool xbox_ble_connected(void) {
    return le_handle != HCI_CON_HANDLE_INVALID && hids_cid && (current_identity.kind == BB_CONTROLLER_XBOX_WIRELESS || current_identity.kind == BB_CONTROLLER_XBOX_SERIES || current_identity.kind == BB_CONTROLLER_XBOX_ONE);
}

static bool send_xbox_ble_rumble(uint8_t strength) {
    xbox_rumble_retry = false;
    if (!xbox_ble_connected()) return false;
    if (xbox_rumble_write_pending) {
        xbox_rumble_retry = true;
        return false;
    }
    memset(xbox_rumble_payload, 0, sizeof(xbox_rumble_payload));
    xbox_rumble_payload[0] = 3;
    uint8_t magnitude = (uint8_t)(((uint16_t)strength * 100u) / 255u);
    xbox_rumble_payload[3] = magnitude;
    xbox_rumble_payload[4] = magnitude;
    xbox_rumble_payload[5] = strength ? 255 : 0;
    uint8_t status = hids_host_send_write_report(hids_cid, 3, HID_REPORT_TYPE_OUTPUT, xbox_rumble_payload, sizeof(xbox_rumble_payload));
    if (status == ERROR_CODE_SUCCESS) {
        xbox_rumble_write_pending = true;
        return true;
    }
    xbox_rumble_retry = status == ERROR_CODE_COMMAND_DISALLOWED || status == GATT_CLIENT_IN_WRONG_STATE;
    if (status == ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE) bb_state_set_rumble_supported(false);
    return false;
}

static bool send_current_rumble(uint8_t strength) {
    switch (current_identity.kind) {
        case BB_CONTROLLER_XBOX_SERIES:
        case BB_CONTROLLER_XBOX_WIRELESS:
        case BB_CONTROLLER_XBOX_ONE:
            return send_xbox_ble_rumble(strength);
        case BB_CONTROLLER_DUALSENSE:
        case BB_CONTROLLER_DUALSENSE_EDGE:
            return send_dualsense_rumble(strength);
        case BB_CONTROLLER_DUALSHOCK4:
            return send_ds4_rumble(strength);
        case BB_CONTROLLER_DUALSHOCK3:
            return send_ds3_rumble(strength);
        case BB_CONTROLLER_NSO_N64:
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
        } else if ((xbox_ble_connected() && xbox_rumble_retry) || (eightbitdo64_ble_connected() && eightbitdo64_rumble_retry)) {
            rumble_request_pending = true;
        }
    }
    if (rumble_active && (int32_t)(now - rumble_stop_at) < 0 && (int32_t)(now - rumble_refresh_at) >= 0) {
        bool refresh = current_identity.kind == BB_CONTROLLER_SWITCH2_PRO || (current_identity.kind == BB_CONTROLLER_8BITDO && classic_switch_protocol);
        if (refresh) send_current_rumble(rumble_active_strength);
        rumble_refresh_at = now + 80u;
    }
    if (rumble_active && (int32_t)(now - rumble_stop_at) >= 0) {
        bool stopped = send_current_rumble(0);
        if (!stopped && ((xbox_ble_connected() && xbox_rumble_retry) || (eightbitdo64_ble_connected() && eightbitdo64_rumble_retry))) return;
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

static bool name_has(const char* name, const char* part);

static const char* remembered_name(const uint8_t addr[6]) {
    bb_config_get(&remembered_cfg);
    for (uint8_t i = 0; i < remembered_cfg.controller_count; ++i) {
        if (same_address(remembered_cfg.controllers[i].bluetooth_address, addr)) return remembered_cfg.controllers[i].name;
    }
    return NULL;
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
        "nintendo", "genesis", "mega drive", "snes", "n64", "md/gen control pad"
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
    if (pairing && scanning) start_classic_inquiry();
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

static bool accept_ble_advertisement(const uint8_t addr[6], bd_addr_type_t type, bool hogp, bool custom_nintendo) {
    if (bb_blocked_device_contains(addr)) return false;
    if (hogp || custom_nintendo || remembered_address(addr)) return true;
    return !pairing && type == BD_ADDR_TYPE_LE_RANDOM && (addr[0] & 0xc0u) == 0x40u;
}

static bool controller_transport_ready(void) {
    return bb_usb_ready() || bb_output_mode() == BB_OUTPUT_XBOX360;
}

static bool accept_candidate(const uint8_t addr[6], const char* name, bool protocol_hint) {
    if (!controller_transport_ready()) return false;
    if (bb_blocked_device_contains(addr)) return false;
    if (reconnect_blocked(addr)) return false;
    if (bb_multi_mode() && slot_for_address(addr) >= 0) return false;
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
    if (current_identity.kind == BB_CONTROLLER_8BITDO && name_has(current_identity.name, "sn30 pro") && name_has(resolved, "8bitdo controller")) resolved = current_identity.name;
    if (current_identity.kind == BB_CONTROLLER_8BITDO && name_has(current_identity.name, "nes30") && name_has(resolved, "8bitdo controller")) resolved = current_identity.name;
    if (updated.vendor_id == 0x2dc8 && updated.product_id == 0x9020) resolved = "8BitDo Micro";
    if (current_identity.kind == BB_CONTROLLER_8BITDO && name_has(current_identity.name, "arcade stick") && name_has(resolved, "pro controller")) resolved = current_identity.name;
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

static void classic_identity_result_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void classic_identity_result(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    int slot = bb_multi_mode() ? (sdp_owner) : 0;
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    classic_identity_result_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void classic_identity_result_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
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
    sdp_owner = bb_slot_current();
    if (sdp_client_query_uuid16(classic_identity_result, identity_query_addr, BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION) != ERROR_CODE_SUCCESS) {
        identity_query_active = false;
        identity_query_pending = true;
    }
}

static void activate_controller(const uint8_t addr[6], uint8_t address_type, uint16_t vid, uint16_t pid, const char* name) {
    bd_addr_t identity_addr;
    memcpy(identity_addr, addr, 6);
    if (le_handle != HCI_CON_HANDLE_INVALID) {
        int index = sm_le_device_index(le_handle);
        if (index >= 0) {
            int identity_type;
            bd_addr_t bonded_addr;
            le_device_db_info(index, &identity_type, bonded_addr, NULL);
            if (identity_type == BD_ADDR_TYPE_LE_PUBLIC || identity_type == BD_ADDR_TYPE_LE_RANDOM) {
                bb_controller_resolve_identity(addr, bonded_addr);
                memcpy(identity_addr, bonded_addr, 6);
                address_type = (uint8_t)identity_type;
            }
        }
    }
    if (bb_blocked_device_contains(identity_addr) || bb_blocked_device_contains(addr)) {
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
        bb_bt_host_forget_device(identity_addr);
        return;
    }
    uint16_t old_identity = bb_current_mister_identity();
    memset(&current_identity, 0, sizeof(current_identity));
    memcpy(current_identity.address, identity_addr, 6);
    current_identity.address_type = address_type;
    current_identity.vendor_id = vid;
    current_identity.product_id = pid;
    const char* stored_name = remembered_name(identity_addr);
    const char* identity_name = name && name[0] ? name : stored_name;
    if ((!identity_name || !identity_name[0] || name_has(identity_name, "bluetooth controller")) && stored_name && stored_name[0]) identity_name = stored_name;
    current_identity.kind = bb_controller_identify(vid, pid, identity_name);
    if (stored_name && name_has(stored_name, "8bitdo") && (!name || !name[0] || name_has(name, "pro controller"))) {
        current_identity.kind = BB_CONTROLLER_8BITDO;
        identity_name = stored_name;
    }
    if (vid == 0x2dc8 && pid == 0x301b) identity_name = "8BitDo Ultimate 2C Wireless";
    if (vid == 0x2dc8 && pid == 0x3019) identity_name = "8BitDo 64";
    bb_controller_display_name(current_identity.kind, identity_name, current_identity.name, sizeof(current_identity.name));
    uint8_t controller_index = 0;
    if (bb_multi_mode()) {
        int slot = slot_for_address_except(identity_addr, bb_slot_current());
        if (slot >= 0) {
            if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
            if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
            return;
        }
    }
    if (!bb_controller_activate_or_create(identity_addr, vid, pid, current_identity.name, &controller_index)) {
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
        return;
    }
    current_controller_index = controller_index;
    bb_driver_set_identity(&current_identity);
    if (current_identity.kind == BB_CONTROLLER_NSO_N64 && classic_hid_cid && !classic_switch_protocol) {
        classic_switch_protocol = true;
        switch_setup_begin();
    }
    if (current_identity.kind == BB_CONTROLLER_DUALSHOCK3) { send_ds3_activation(); send_ds3_rumble(0); }
    ds4_descriptor_ready = false;
    if (current_identity.kind == BB_CONTROLLER_DUALSHOCK4) {
        ds4_activation_pending = true;
        ds4_activation_attempts = 0;
        ds4_activation_step = 0;
        ds4_activation_at = btstack_run_loop_get_time_ms() + 100u;
    }
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
    if (le_handle != HCI_CON_HANDLE_INVALID) ble_stage = "active";

    gap_discoverable_control(1);
    scanning = false;
    last_controller_report_at = btstack_run_loop_get_time_ms();
    if (!bb_multi_mode() && bb_output_mode() == BB_OUTPUT_MISTER && old_identity != bb_current_mister_identity()) bb_usb_request_reenumeration();
    if (bb_multi_mode()) {
        connection_settle_started = btstack_run_loop_get_time_ms();
        connection_settle_active = true;
        start_scans();
    }
}

static void retire_controller_link(bool link_closed) {
    hci_con_handle_t handle = le_handle != HCI_CON_HANDLE_INVALID ? le_handle :
        classic_handle != HCI_CON_HANDLE_INVALID ? classic_handle : pending_classic_handle;
    if (!link_closed && handle != HCI_CON_HANDLE_INVALID && gap_get_connection_type(handle) != GAP_CONNECTION_INVALID) {
        closing_handle = handle;
        gap_disconnect(handle);
    }
}
static bool release_closing_handle(hci_con_handle_t handle) {
    if (handle == HCI_CON_HANDLE_INVALID || handle != closing_handle) return false;
    closing_handle = HCI_CON_HANDLE_INVALID;
    return true;
}

static void controller_disconnected(bool link_closed) {
    if (disconnect_cleanup_active) return;
    if (le_handle == HCI_CON_HANDLE_INVALID && classic_handle == HCI_CON_HANDLE_INVALID &&
        pending_classic_handle == HCI_CON_HANDLE_INVALID && !hids_cid && !classic_hid_cid &&
        !xbox_battery_cid && !ble_connecting && !classic_connecting && !classic_attempt_active &&
        current_controller_index < 0) return;
    disconnect_cleanup_active = true;
    bool owned_classic_setup = classic_connecting || classic_attempt_active;
    bool other_ble_setup = ble_setup_elsewhere(bb_slot_current());
    retire_controller_link(link_closed);
    uint16_t battery_cid = xbox_battery_cid;
    xbox_battery_cid = 0;
    classic_generation++;
    classic_setup_pending = false;
    dualsense_activation_pending = false;
    dualsense_activation_attempts = 0;
    ds4_descriptor_ready = false;
    ds4_activation_pending = false;
    ds4_activation_step = 0;
    arcade_battery_poll_at = 0;
    identity_query_pending = false;
    connected_name_request = false;
    if (owned_classic_setup) reset_classic_name_candidates();
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
    ultimate2c_setup_step = 0;
    ultimate2c_protocol_step = 0;
    ultimate2c_protocol_handle = 0;
    ble_security_retry_pending = ble_security_retry_pending && pairing && !manual_disconnect_pending;
    if (le_custom_nintendo) gatt_client_stop_listening_for_characteristic_value_updates(&nintendo_listener);
    host_service_changed_ccc = 0;
    if (!other_ble_setup) {
        sm_set_encryption_key_size_range(7, 16);
        sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
        if (ble_security_retry) sm_set_secure_connections_only_mode(BB_SM_DEFAULT_SC_ONLY);
    }
    ble_setup_started = false;
    if (!strcmp(ble_failed_stage, "none")) ble_stage = "disconnected";
    le_custom_nintendo = false;
    le_gamecube = false;
    le_handle = HCI_CON_HANDLE_INVALID;
    classic_hid_cid = 0;
    classic_handle = HCI_CON_HANDLE_INVALID;
    hids_cid = 0;
    if (battery_cid) battery_service_client_disconnect(battery_cid);
    xbox_rumble_write_pending = false;
    xbox_rumble_retry = false;
    eightbitdo64_rumble_write_pending = false;
    eightbitdo64_rumble_retry = false;
    eightbitdo_ble_rumble = false;
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
    disconnect_cleanup_active = false;
    if (stack_ready) {
        if (!ble_security_retry_pending) start_scans();
    }
}

static void start_classic_inquiry(void) {
    if (!stack_ready || !pairing || !scanning || classic_inquiry_active || connection_in_progress() || free_slot() < 0) return;
    uint32_t now = btstack_run_loop_get_time_ms();
    if (classic_inquiry_retry_at && (int32_t)(now - classic_inquiry_retry_at) < 0) return;
    if (gap_inquiry_start(5) == ERROR_CODE_SUCCESS) {
        classic_inquiry_active = true;
        classic_inquiry_retry_at = 0;
    } else classic_inquiry_retry_at = now + 250u;
}

static void start_scans(void) {
    if (multi_discovery_paused(btstack_run_loop_get_time_ms())) return;
    if (!stack_ready || connection_in_progress() || free_slot() < 0) return;
    bool busy = bb_multi_mode() && connected_controller_present();
    if (scanning && scan_busy != busy) stop_scans();
    if (!scanning) {
        scanning = true;
        if (bb_multi_mode()) {
            discovery_started_at = btstack_run_loop_get_time_ms();
            discovery_window_active = true;
        }
        bb_state_set_scanning(true);
        scan_busy = busy;
        gap_set_scan_parameters(1, busy ? 96 : 48, busy ? 24 : 48);
        gap_start_scan();
    }
    start_classic_inquiry();
}

static void stop_scans(void) {
    bool was_scanning = scanning;
    scanning = false;
    discovery_window_active = false;
    classic_inquiry_active = false;
    classic_inquiry_retry_at = 0;
    bb_state_set_scanning(false);
    if (!was_scanning) return;
    gap_stop_scan();
    gap_inquiry_stop();
}

static void connect_classic(const uint8_t addr[6], const char* name) {
    if (multi_discovery_paused(btstack_run_loop_get_time_ms())) return;
    if (classic_connecting || ble_connecting || free_slot() < 0) return;
    reset_classic_name_candidates();
    classic_connecting = true;
    classic_attempt_active = true;
    classic_attempt_started = btstack_run_loop_get_time_ms();
    stop_scans();
    memcpy(pending_addr, addr, 6);
    pending_addr_type = 0;
    copy_name(pending_name, sizeof(pending_name), name);
    pending_vid = 0;
    pending_pid = 0;
    gap_set_page_timeout(bb_multi_mode() && !pairing ? 4800u : 0x6000u);
    uint8_t status = hid_host_connect(pending_addr, HID_PROTOCOL_MODE_REPORT, &classic_hid_cid);
    if (status != ERROR_CODE_SUCCESS) {
        classic_connecting = false;
        classic_attempt_active = false;
        start_scans();
    }
}

static void configure_ble_security(void) {
    ble_key_minimum = 7;
    ble_authentication = SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION;
    ble_secure_connections_only = BB_SM_DEFAULT_SC_ONLY;
    sm_set_secure_connections_only_mode(BB_SM_DEFAULT_SC_ONLY);
    sm_set_authentication_requirements(ble_authentication);
    sm_set_encryption_key_size_range(ble_key_minimum, 16);
}

static void connect_ble(const uint8_t addr[6], bd_addr_type_t addr_type, const char* name, bool custom_nintendo, bool gamecube, uint16_t vid, uint16_t pid) {
    const char* resolved_name = name && name[0] ? name : remembered_name(addr);
    if (multi_discovery_paused(btstack_run_loop_get_time_ms())) return;
    if (connection_in_progress() || slot_for_address(addr) >= 0 || free_slot() < 0) return;
    ble_connecting = true;
    ble_attempt_started = btstack_run_loop_get_time_ms();
    ble_setup_started = false;
    ble_bond_retry = false;
    ble_security_retry = false;
    ble_security_retry_pending = false;
    ble_stage = "connecting";
    ble_failed_stage = "none";
    ble_setup_status = -1;
    ble_pairing_reason = -1;
    copy_name(ble_peer_name, sizeof(ble_peer_name), resolved_name);
    configure_ble_security();
    ++ble_attempt_count;
    stop_scans();
    memcpy(pending_addr, addr, 6);
    pending_addr_type = addr_type;
    copy_name(pending_name, sizeof(pending_name), resolved_name);
    pending_vid = vid;
    pending_pid = pid;
    le_custom_nintendo = custom_nintendo;
    le_gamecube = gamecube;
    uint8_t status = gap_connect(pending_addr, pending_addr_type);
    if (status != ERROR_CODE_SUCCESS) {
        sm_set_encryption_key_size_range(7, 16);
        sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
        if (ble_security_retry) sm_set_secure_connections_only_mode(BB_SM_DEFAULT_SC_ONLY);
        ble_connecting = false;
        le_custom_nintendo = false;
        start_scans();
    }
}

static void nintendo_gatt_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void nintendo_gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    int slot = bb_multi_mode() ? (size >= 4 ? slot_for_handle(little_endian_read_16(packet, 2)) : -1) : 0;
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    nintendo_gatt_handler_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void nintendo_gatt_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
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

static void xbox_battery_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void xbox_battery_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    if ((packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) || size < 5 || packet[0] != HCI_EVENT_GATTSERVICE_META) return;
    int slot = slot_for_cid(little_endian_read_16(packet, 3), 2);
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    xbox_battery_handler_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void xbox_battery_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    if ((packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) || size < 8 || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META || !xbox_ble_connected() || !xbox_battery_cid) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_CONNECTED:
            if (gattservice_subevent_battery_service_connected_get_bas_cid(packet) != xbox_battery_cid) return;
            if (gattservice_subevent_battery_service_connected_get_status(packet) != ERROR_CODE_SUCCESS) {
                xbox_battery_cid = 0;
                return;
            }
            if (gattservice_subevent_battery_service_connected_get_num_instances(packet)) battery_service_client_read_battery_level(xbox_battery_cid, 0);
            break;
        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_LEVEL: {
            if (gattservice_subevent_battery_service_level_get_bas_cid(packet) != xbox_battery_cid || gattservice_subevent_battery_service_level_get_service_index(packet) != 0 || gattservice_subevent_battery_service_level_get_att_status(packet) != ATT_ERROR_SUCCESS) return;
            uint8_t level = gattservice_subevent_battery_service_level_get_level(packet);
            if (level <= 100) bb_state_update_battery(true, level, 0);
            break;
        }
        default:
            break;
    }
}

static bool ble_is_ultimate2c(void) {
    return (pending_vid == 0x2dc8 && pending_pid == 0x301b) || name_has(pending_name, "ultimate 2c wireless");
}

static bool ble_ultimate_security_retry_candidate(void) {
    static const char* name = "8BitDo Ultimate";
    return ble_is_ultimate2c() || (strlen(pending_name) == strlen(name) && name_has(pending_name, name));
}

static void ble_identity_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void ble_setup_fail(uint8_t status);

static void hids_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void hids_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    if ((packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) || size < 5 || packet[0] != HCI_EVENT_GATTSERVICE_META) return;
    int slot = slot_for_cid(little_endian_read_16(packet, 3), 1);
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    hids_handler_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void hids_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if ((packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            if (size < 8 || le_handle == HCI_CON_HANDLE_INVALID) return;
            if (gattservice_subevent_hid_service_connected_get_status(packet) != ERROR_CODE_SUCCESS) {
                ble_setup_fail(gattservice_subevent_hid_service_connected_get_status(packet));
                return;
            }
            if (ble_is_ultimate2c()) {
                ultimate2c_setup_step = 1;
                ultimate2c_setup_at = btstack_run_loop_get_time_ms();
                ble_stage = "report_mode";
                break;
            }
            activate_controller(pending_addr, pending_addr_type, pending_vid, pending_pid, pending_name);
            xbox_rumble_write_pending = false;
            xbox_rumble_retry = false;
            if (xbox_ble_connected()) {
                bb_state_set_rumble_supported(true);
                if (!xbox_battery_cid && battery_service_client_connect(le_handle, xbox_battery_handler, 60000, &xbox_battery_cid) != ERROR_CODE_SUCCESS) xbox_battery_cid = 0;
            }
            break;
        case GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION:
            if (size < 6) return;
            if (ultimate2c_setup_step == 3 &&
                gattservice_subevent_hid_service_reports_notification_get_hids_cid(packet) == hids_cid &&
                gattservice_subevent_hid_service_reports_notification_get_configuration(packet) == 1) {
                ultimate2c_setup_step = 4;
                ultimate2c_setup_at = btstack_run_loop_get_time_ms();
                ble_stage = "device_information";
            }
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            if (size < 9 || le_handle == HCI_CON_HANDLE_INVALID) return;
            if (gattservice_subevent_hid_report_get_report_len(packet) > size - 9u) return;
            uint8_t service = gattservice_subevent_hid_report_get_service_index(packet);
            const uint8_t* descriptor = hids_host_descriptor_storage_get_descriptor_data(hids_cid, service);
            uint16_t descriptor_len = hids_host_descriptor_storage_get_descriptor_len(hids_cid, service);
            bb_driver_set_hid_descriptor(descriptor, descriptor_len);
            if (!eightbitdo_ble_rumble && current_identity.kind == BB_CONTROLLER_8BITDO && descriptor && descriptor_len &&
                btstack_hid_get_report_size_for_id(5, HID_REPORT_TYPE_OUTPUT, descriptor, descriptor_len) == 4) {
                eightbitdo_ble_rumble = true;
                bb_state_set_rumble_supported(true);
            }
            last_controller_report_at = btstack_run_loop_get_time_ms();
            const uint8_t* report = gattservice_subevent_hid_report_get_report(packet);
            uint16_t length = gattservice_subevent_hid_report_get_report_len(packet);
            if (!bb_driver_handle_xbox_ble_report(report, length)) bb_driver_handle_hid_report(report, length);
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_REPORT_WRITTEN:
            if (size < 7) return;
            if (gattservice_subevent_hid_report_written_get_hids_cid(packet) == hids_cid && gattservice_subevent_hid_report_written_get_report_id(packet) == 5) eightbitdo64_rumble_write_pending = false;
            if (gattservice_subevent_hid_report_written_get_hids_cid(packet) == hids_cid && gattservice_subevent_hid_report_written_get_report_id(packet) == 3) xbox_rumble_write_pending = false;
            break;
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            controller_disconnected(true);
            break;
        default:
            break;
    }
}

static void ble_setup_fail(uint8_t status) {
    ble_setup_status = status;
    ble_failed_stage = ble_stage;
    ble_stage = "disconnecting";
    if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
    else {
        gap_connect_cancel();
        controller_disconnected(false);
    }
}

static void ble_start_hid(void) {
    ble_stage = "hid_setup";
    uint8_t status = hids_host_connect(le_handle, hids_handler, HID_PROTOCOL_MODE_REPORT, &hids_cid);
    if (status != ERROR_CODE_SUCCESS) ble_setup_fail(status);
}

static void ultimate2c_protocol_finish(void) {
    ultimate2c_protocol_step = 0;
    ble_start_hid();
}

static void ultimate2c_protocol_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void ultimate2c_protocol_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    int slot = bb_multi_mode() ? (size >= 4 ? slot_for_handle(little_endian_read_16(packet, 2)) : -1) : 0;
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    ultimate2c_protocol_handler_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void ultimate2c_protocol_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 2 || !ultimate2c_protocol_step || !ble_connecting || le_handle == HCI_CON_HANDLE_INVALID) return;
    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            if (size < 32 || ultimate2c_protocol_step != 2 || gatt_event_characteristic_query_result_get_handle(packet) != le_handle) return;
            gatt_client_characteristic_t characteristic;
            gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
            if (!ultimate2c_protocol_handle) {
                ultimate2c_protocol_handle = characteristic.value_handle;
                ultimate2c_protocol_properties = characteristic.properties;
            }
            break;
        }
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
            if (size < 13 || ultimate2c_protocol_step != 4 || gatt_event_characteristic_value_query_result_get_handle(packet) != le_handle ||
                gatt_event_characteristic_value_query_result_get_value_handle(packet) != ultimate2c_protocol_handle) return;
            if (gatt_event_characteristic_value_query_result_get_value_length(packet) == 1) {
                ultimate2c_protocol_boot = gatt_event_characteristic_value_query_result_get_value(packet)[0] == HID_PROTOCOL_MODE_BOOT;
            }
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            if (size < 9 || gatt_event_query_complete_get_handle(packet) != le_handle) return;
            if (ultimate2c_protocol_step == 2) {
                if (gatt_event_query_complete_get_att_status(packet) == ATT_ERROR_SUCCESS && ultimate2c_protocol_handle &&
                    (ultimate2c_protocol_properties & ATT_PROPERTY_READ)) ultimate2c_protocol_step = 3;
                else ultimate2c_protocol_finish();
            } else if (ultimate2c_protocol_step == 4) {
                if (gatt_event_query_complete_get_att_status(packet) == ATT_ERROR_SUCCESS && ultimate2c_protocol_boot &&
                    (ultimate2c_protocol_properties & ATT_PROPERTY_WRITE_WITHOUT_RESPONSE)) ultimate2c_protocol_step = 5;
                else ultimate2c_protocol_finish();
            }
            break;
        default:
            break;
    }
}

static void ble_identity_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void ble_identity_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    int slot = bb_multi_mode() ? (size >= 5 ? slot_for_handle(little_endian_read_16(packet, 3)) : -1) : 0;
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    ble_identity_handler_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void ble_identity_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    if ((packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) || size < 6 || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_PNP_ID:
            if (size < 13 || gattservice_subevent_device_information_pnp_id_get_con_handle(packet) != le_handle || gattservice_subevent_device_information_pnp_id_get_att_status(packet) != ERROR_CODE_SUCCESS) return;
            if (gattservice_subevent_device_information_pnp_id_get_vendor_source_id(packet) == 2) {
                pending_vid = gattservice_subevent_device_information_pnp_id_get_vendor_id(packet);
                pending_pid = gattservice_subevent_device_information_pnp_id_get_product_id(packet);
            }
            break;
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_DONE:
            if (gattservice_subevent_device_information_done_get_con_handle(packet) == le_handle && le_handle != HCI_CON_HANDLE_INVALID) {
                if (ble_is_ultimate2c() && hids_cid) {
                    if (ultimate2c_setup_step == 5) {
                        ultimate2c_setup_step = 0;
                        activate_controller(pending_addr, pending_addr_type, pending_vid, pending_pid, pending_name);
                    }
                } else ble_start_hid();
            }
            break;
        default:
            break;
    }
}

static void sm_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void sm_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    if (packet_type == HCI_EVENT_PACKET && size >= 11 && ble_address_lookup_pending &&
        (packet[0] == SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED || packet[0] == SM_EVENT_IDENTITY_RESOLVING_FAILED)) {
        bd_addr_t address;
        sm_event_identity_resolving_failed_get_address(packet, address);
        if (same_address(address, ble_address_lookup_addr)) {
            ble_address_lookup_pending = false;
            if (packet[0] == SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED && size >= 20 && !connection_in_progress() && controller_transport_ready()) {
                bd_addr_t identity;
                sm_event_identity_resolving_succeeded_get_identity_address(packet, identity);
                int slot = free_slot();
                if (slot >= 0 && !bb_blocked_device_contains(identity) && remembered_address(identity) && slot_for_address(identity) < 0) {
                    bb_slot_enter((uint8_t)slot);
                    if (!reconnect_blocked(identity)) connect_ble(address, ble_address_lookup_type, remembered_name(identity), false, false, 0, 0);
                }
            }
            bb_slot_enter(previous);
            return;
        }
    }
    int slot = bb_multi_mode() ? (size >= 5 ? slot_for_handle(little_endian_read_16(packet, 2)) : -1) : 0;
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    sm_handler_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void sm_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 5) return;
    hci_con_handle_t handle;
    uint8_t status;
    bool reencrypt = false;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            handle = sm_event_just_works_request_get_handle(packet);
            if (handle == le_handle) sm_just_works_confirm(handle);
            return;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            handle = sm_event_numeric_comparison_request_get_handle(packet);
            if (handle == le_handle) sm_numeric_comparison_confirm(handle);
            return;
        case SM_EVENT_PAIRING_COMPLETE:
            if (size < 13) return;
            handle = sm_event_pairing_complete_get_handle(packet);
            status = sm_event_pairing_complete_get_status(packet);
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            if (size < 12) return;
            handle = sm_event_reencryption_complete_get_handle(packet);
            status = sm_event_reencryption_complete_get_status(packet);
            reencrypt = true;
            break;
        default:
            return;
    }
    if (handle != le_handle || le_handle == HCI_CON_HANDLE_INVALID || !ble_connecting) return;
    ble_setup_status = status;
    ble_pairing_reason = reencrypt ? -1 : sm_event_pairing_complete_get_reason(packet);
    if (status != ERROR_CODE_SUCCESS) {
        if (!reencrypt && pairing && status == ERROR_CODE_AUTHENTICATION_FAILURE &&
            ble_pairing_reason == SM_REASON_AUTHENTHICATION_REQUIREMENTS && !ble_security_retry &&
            !ble_setup_started && ble_ultimate_security_retry_candidate()) {
            ble_security_retry = true;
            ble_security_retry_pending = true;
            ble_security_retry_at = btstack_run_loop_get_time_ms();
            ble_stage = "security_retry_pending";
        } else if (reencrypt && pairing && !ble_bond_retry && !ble_setup_started) {
            ble_bond_retry = true;
            ble_stage = "bond_repair";
            gap_delete_bonding(pending_addr_type, pending_addr);
            sm_request_pairing(le_handle);
        } else ble_setup_fail(status);
        return;
    }
    if (ble_setup_started) return;
    ble_setup_started = true;
    if (le_custom_nintendo) {
        enable_nintendo_notifications();
    } else if (ble_is_ultimate2c()) {
        ultimate2c_protocol_step = 1;
        ultimate2c_protocol_at = btstack_run_loop_get_time_ms();
        ultimate2c_protocol_handle = 0;
        ultimate2c_protocol_properties = 0;
        ultimate2c_protocol_boot = false;
        ble_stage = "protocol_probe";
    } else {
        ble_stage = "device_information";
        if (device_information_service_client_query(le_handle, ble_identity_handler) != ERROR_CODE_SUCCESS) ble_start_hid();
    }
}

static void classic_hid_event(uint8_t* packet) {
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION: {
            if (hid_subevent_incoming_connection_get_status(packet) != ERROR_CODE_SUCCESS) break;
            bd_addr_t addr;
            uint16_t incoming_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            hid_subevent_incoming_connection_get_address(packet, addr);
            if (closing_handle != HCI_CON_HANDLE_INVALID || reconnect_blocked(addr) || (!pairing && !remembered_address(addr))) {
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
                controller_disconnected(false);
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

            if (!controller_transport_ready() || (!pairing && !remembered_address(connected_addr))) {
                hid_host_disconnect(classic_hid_cid);
                break;
            }
            const char* known_name = remembered_name(connected_addr);
            const char* resolved_name = pending_name[0] && !name_has(pending_name, "bluetooth controller") ? pending_name : known_name;
            classic_setup_pending = true;
            classic_setup_started = btstack_run_loop_get_time_ms();
            activate_controller(connected_addr, 0, 0, 0, resolved_name);
            if (current_identity.kind == BB_CONTROLLER_GENERIC || current_identity.kind == BB_CONTROLLER_SWITCH_PRO || name_has(current_identity.name, "bluetooth controller")) {
                memcpy(connected_name_addr, connected_addr, sizeof(connected_name_addr));
                connected_name_request = gap_remote_name_request(connected_name_addr, 0, 0) == ERROR_CODE_SUCCESS;
            }
            break;
        }
        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            if (hid_subevent_descriptor_available_get_hid_cid(packet) != classic_hid_cid) break;
            finish_classic_setup();
            if (hid_subevent_descriptor_available_get_status(packet) == ERROR_CODE_SUCCESS) {
                const uint8_t* descriptor = hid_descriptor_storage_get_descriptor_data(classic_hid_cid);
                uint16_t descriptor_len = hid_descriptor_storage_get_descriptor_len(classic_hid_cid);
                bb_driver_set_hid_descriptor(descriptor, descriptor_len);
                ds4_descriptor_ready = true;
                if (current_identity.kind == BB_CONTROLLER_GENERIC || current_identity.kind == BB_CONTROLLER_SWITCH_PRO ||
                    (current_identity.kind == BB_CONTROLLER_8BITDO && name_has(current_identity.name, "8bitdo controller"))) identity_query_pending = true;
                bb_controller_identity_t refined = current_identity;
                if (bb_driver_refine_identity_from_hid_descriptor(descriptor, descriptor_len, &refined)) {
                    current_identity = refined;
                    bb_driver_set_identity(&current_identity);
                    if (current_controller_index >= 0) {
                        bb_controller_update_identity((uint8_t)current_controller_index, current_identity.vendor_id, current_identity.product_id, current_identity.name);
                        bb_state_set_controller(true, current_controller_index, current_identity.name);
                    }
                    if (current_identity.kind == BB_CONTROLLER_DUALSHOCK4) {
                        ds4_activation_pending = true;
                        ds4_activation_attempts = 0;
                        ds4_activation_step = 0;
                        ds4_activation_at = btstack_run_loop_get_time_ms() + 100u;
                    }
                }
                if (current_identity.kind == BB_CONTROLLER_DUALSHOCK3) send_ds3_activation();
                if (current_identity.kind == BB_CONTROLLER_DUALSENSE || current_identity.kind == BB_CONTROLLER_DUALSENSE_EDGE) {
                    dualsense_activation_pending = true;
                    dualsense_activation_attempts = 0;
                    dualsense_activation_at = btstack_run_loop_get_time_ms();
                    service_dualsense_activation(dualsense_activation_at);
                }
                if (current_identity.kind == BB_CONTROLLER_8BITDO && name_has(current_identity.name, "pro 3")) hid_host_send_get_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x06);
            }
            break;
        case HID_SUBEVENT_GET_REPORT_RESPONSE: {
            if (hid_subevent_get_report_response_get_hid_cid(packet) == classic_hid_cid &&
                current_identity.kind == BB_CONTROLLER_DUALSHOCK4 && ds4_activation_step == 2) {
                ds4_activation_step = 3;
                ds4_activation_pending = false;
                break;
            }
            if (hid_subevent_get_report_response_get_hid_cid(packet) != classic_hid_cid ||
                current_identity.kind != BB_CONTROLLER_8BITDO || !name_has(current_identity.name, "arcade stick")) break;
            uint8_t status = hid_subevent_get_report_response_get_handshake_status(packet);
            bb_driver_record_battery_response(status, hid_subevent_get_report_response_get_report(packet), hid_subevent_get_report_response_get_report_len(packet));
            if (status != 0) break;
            bb_driver_handle_battery_report(hid_subevent_get_report_response_get_report(packet), hid_subevent_get_report_response_get_report_len(packet));
            break;
        }
        case HID_SUBEVENT_REPORT: {
            if (!classic_hid_cid || hid_subevent_report_get_hid_cid(packet) != classic_hid_cid) break;
            const uint8_t* report = hid_subevent_report_get_report(packet);
            uint16_t len = hid_subevent_report_get_report_len(packet);
            if (len && report[0] == 0xa1) {
                ++report;
                --len;
            }
            if (len && current_identity.kind == BB_CONTROLLER_8BITDO && name_has(current_identity.name, "arcade stick")) {
                if (len >= 12 && (report[0] == 0x30 || report[0] == 0x3f)) {
                    if (!name_has(current_identity.name, "s mode")) refresh_classic_identity("8BitDo Arcade Stick - S Mode", 0, 0);
                } else if (report[0] != 0x21 && report[0] != 0x30 && report[0] != 0x3f && !name_has(current_identity.name, "x mode")) {
                    refresh_classic_identity("8BitDo Arcade Stick - X Mode", 0, 0);
                }
            }
            if (len && report[0] == 0x21) switch_setup_ack(report, len);
            if (len && (report[0] == 0x30 || report[0] == 0x3f)) {
                bool switch_kind = current_identity.kind == BB_CONTROLLER_NSO_NES || current_identity.kind == BB_CONTROLLER_NSO_SNES || current_identity.kind == BB_CONTROLLER_NSO_GENESIS || current_identity.kind == BB_CONTROLLER_NSO_N64 || current_identity.kind == BB_CONTROLLER_SWITCH_PRO || current_identity.kind == BB_CONTROLLER_JOYCON_LEFT || current_identity.kind == BB_CONTROLLER_JOYCON_RIGHT || current_identity.kind == BB_CONTROLLER_8BITDO;
                // Leave SNES report mode unchanged; skip Switch mode, rumble and LED setup.
                if (switch_kind && current_identity.kind != BB_CONTROLLER_NSO_SNES) {
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
            if (classic_hid_cid && closed_cid == classic_hid_cid) controller_disconnected(false);
            break;
        }
        default:
            break;
    }
}

static void hci_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t previous = bb_slot_current();
    if (packet_type == HCI_EVENT_PACKET && size >= 2 && packet[0] == GAP_EVENT_INQUIRY_COMPLETE) classic_inquiry_active = false;
    int slot = bb_multi_mode() ? (route_hci(packet, size)) : 0;
    if (slot < 0) return;
    bb_slot_enter((uint8_t)slot);
    if (bb_multi_mode()) { bb_runtime_state_t live; bb_state_get(&live); if (live.controller_connected) current_controller_index = live.controller_index; }
    hci_handler_impl(packet_type, channel, packet, size);
    bb_slot_enter(previous);
}
static void hci_handler_impl(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
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
            }
            break;
        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const char* name = remembered_name(addr);
            if (hci_event_connection_request_get_link_type(packet) == HCI_LINK_TYPE_ACL && (pairing || remembered_address(addr)) &&
                !classic_connecting && !ble_connecting && current_controller_index < 0 && !reconnect_blocked(addr)) {
                reset_classic_name_candidates();
                memcpy(pending_addr, addr, sizeof(pending_addr));
                copy_name(pending_name, sizeof(pending_name), name);
                classic_connecting = true;
                classic_attempt_active = true;
                classic_attempt_started = btstack_run_loop_get_time_ms();
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
                    controller_disconnected(false);
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
                else start_classic_inquiry();
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
            if (bb_multi_mode() && slot_for_address(addr) >= 0) break;
            adv_name(data, data_len, name, sizeof(name));
            bool hogp = ad_data_contains_uuid16(data_len, data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
            bool nso_new = ad_data_contains_uuid128(data_len, data, (uint8_t*)nintendo_service_uuid);
            uint16_t adv_vid = 0;
            uint16_t adv_pid = 0;
            bool nintendo_new = adv_nintendo_identity(data, data_len, &adv_vid, &adv_pid);
            bool gamecube = adv_pid == 0x2073 || name_has(name, "gamecube");
            bool switch2_pro = adv_pid == 0x2069 || name_has(name, "switch 2 pro") || name_has(name, "pro controller 2");
            bool custom_nintendo = nso_new || nintendo_new || (!hogp && (switch2_pro || gamecube));
            uint8_t address_type = gap_event_advertising_report_get_address_type(packet);
            if (!accept_ble_advertisement(addr, address_type, hogp, custom_nintendo)) break;
            if (custom_nintendo) {
                if (!remembered_address(addr) && !pairing) break;
                if (!name[0]) copy_name(name, sizeof(name), nintendo_ble_name(adv_pid));
            } else if (!accept_candidate(addr, name, hogp)) {
                uint8_t type = gap_event_advertising_report_get_address_type(packet);
                if (!pairing && !ble_address_lookup_pending && !reconnect_blocked(addr) && type == BD_ADDR_TYPE_LE_RANDOM && (addr[0] & 0xc0u) == 0x40u) {
                    memcpy(ble_address_lookup_addr, addr, 6);
                    ble_address_lookup_type = type;
                    ble_address_lookup_at = btstack_run_loop_get_time_ms();
                    ble_address_lookup_pending = true;
                    if (sm_address_resolution_lookup(type, addr)) ble_address_lookup_pending = false;
                }
                break;
            }
            connect_ble(addr, gap_event_advertising_report_get_address_type(packet), name, custom_nintendo, gamecube, adv_vid, adv_pid);
            break;
        }
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                if (gap_subevent_le_connection_complete_get_status(packet) != ERROR_CODE_SUCCESS) {
                    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
                    if (ble_security_retry) sm_set_secure_connections_only_mode(BB_SM_DEFAULT_SC_ONLY);
                    sm_set_encryption_key_size_range(7, 16);
                    ble_setup_status = gap_subevent_le_connection_complete_get_status(packet);
                    ble_failed_stage = "connecting";
                    ble_stage = "failed";
                    ble_connecting = false;
                    start_scans();
                    break;
                }
                le_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                ble_stage = le_custom_nintendo ? "notifications" : "security";
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
            if (release_closing_handle(handle)) {
                start_scans();
                break;
            }
            if ((le_handle != HCI_CON_HANDLE_INVALID && handle == le_handle) ||
                (classic_handle != HCI_CON_HANDLE_INVALID && handle == classic_handle) ||
                (pending_classic_handle != HCI_CON_HANDLE_INVALID && handle == pending_classic_handle)) controller_disconnected(true);
            break;
        }
        default:
            break;
    }
}

static volatile bool multi_disconnect_requests[BB_MAX_SLOTS];
static bool disconnect_current_impl(void);

static volatile uint8_t pairing_command;
static volatile bool forget_all_command;
static volatile bool forget_commands[BB_MAX_CONTROLLERS];
static uint8_t forget_addresses[BB_MAX_CONTROLLERS][6];
static void start_pairing_impl(void);
static void stop_pairing_impl(void);
static void forget_all_impl(void);
static bool forget_device_impl(const uint8_t address[6]);

static void service_link_recovery(uint32_t now) {
    if (closing_handle != HCI_CON_HANDLE_INVALID && gap_get_connection_type(closing_handle) == GAP_CONNECTION_INVALID) {
        closing_handle = HCI_CON_HANDLE_INVALID;
        start_scans();
    }
    if ((le_handle != HCI_CON_HANDLE_INVALID && gap_get_connection_type(le_handle) == GAP_CONNECTION_INVALID) ||
        (classic_handle != HCI_CON_HANDLE_INVALID && gap_get_connection_type(classic_handle) == GAP_CONNECTION_INVALID)) controller_disconnected(true);
    if (ble_connecting && le_handle == HCI_CON_HANDLE_INVALID && !strcmp(ble_stage, "connecting") &&
        (uint32_t)(now - ble_attempt_started) >= 4000u) ble_setup_fail(ERROR_CODE_CONNECTION_TIMEOUT);
}

static void service_slot(void) {
    if (bb_multi_mode()) {
        bb_runtime_state_t live;
        bb_state_get(&live);
        if (live.controller_connected) current_controller_index = live.controller_index;
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    service_link_recovery(now);
    service_classic_setup(now);
    service_dualsense_activation(now);
    if (multi_disconnect_requests[bb_slot_current()]) {
        multi_disconnect_requests[bb_slot_current()] = false;
        disconnect_current_impl();
    }
    if (ble_security_retry_pending && (uint32_t)(now - ble_security_retry_at) >= 100u) {
        ble_security_retry_pending = false;
        if (pairing && !ble_setup_started) {
            bool reconnect = le_handle == HCI_CON_HANDLE_INVALID;
            if (reconnect && !connection_in_progress()) {
                bd_addr_t addr;
                char name[sizeof(pending_name)];
                memcpy(addr, pending_addr, sizeof(addr));
                memcpy(name, pending_name, sizeof(name));
                connect_ble(addr, pending_addr_type, name, false, false, pending_vid, pending_pid);
                ble_security_retry = true;
            }
            if (ble_connecting) {
                ble_authentication = SM_AUTHREQ_BONDING;
                ble_secure_connections_only = false;
                sm_set_secure_connections_only_mode(false);
                sm_set_authentication_requirements(ble_authentication);
                sm_set_encryption_key_size_range(ble_key_minimum, 16);
                ble_attempt_started = now;
                ble_stage = "security_retry";
                if (!reconnect) sm_request_pairing(le_handle);
            }
        }
    }
    if (ultimate2c_protocol_step && le_handle != HCI_CON_HANDLE_INVALID && ble_connecting) {
        if ((uint32_t)(now - ultimate2c_protocol_at) >= 6000u) {
            ultimate2c_protocol_step = 0;
            ble_setup_fail(ERROR_CODE_CONNECTION_TIMEOUT);
        } else if (ultimate2c_protocol_step == 1) {
            ultimate2c_protocol_step = 2;
            if (gatt_client_discover_characteristics_for_handle_range_by_uuid16(ultimate2c_protocol_handler, le_handle, 1, 0xffff,
                    ORG_BLUETOOTH_CHARACTERISTIC_PROTOCOL_MODE) != ERROR_CODE_SUCCESS) ultimate2c_protocol_step = 1;
        } else if (ultimate2c_protocol_step == 3) {
            ultimate2c_protocol_step = 4;
            if (gatt_client_read_value_of_characteristic_using_value_handle(ultimate2c_protocol_handler, le_handle,
                    ultimate2c_protocol_handle) != ERROR_CODE_SUCCESS) ultimate2c_protocol_step = 3;
        } else if (ultimate2c_protocol_step == 5) {
            uint8_t mode = HID_PROTOCOL_MODE_REPORT;
            if (gatt_client_write_value_of_characteristic_without_response(le_handle, ultimate2c_protocol_handle, 1, &mode) == ERROR_CODE_SUCCESS) {
                ultimate2c_protocol_finish();
            }
        }
    }
    if (ultimate2c_setup_step && le_handle != HCI_CON_HANDLE_INVALID && ble_connecting) {
        if ((uint32_t)(now - ultimate2c_setup_at) >= 6000u) {
            bool hid_ready = ultimate2c_setup_step >= 4;
            ultimate2c_setup_step = 0;
            if (hid_ready) activate_controller(pending_addr, pending_addr_type, pending_vid, pending_pid, pending_name);
            else ble_setup_fail(ERROR_CODE_CONNECTION_TIMEOUT);
        } else if (ultimate2c_setup_step == 1) {
            uint8_t status = hids_host_send_set_protocol_mode(hids_cid, 0, HID_PROTOCOL_MODE_REPORT);
            if (status == ERROR_CODE_SUCCESS || status == ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE) {
                ultimate2c_setup_step = 2;
                ultimate2c_setup_at = now;
            }
        } else if (ultimate2c_setup_step == 2 && (uint32_t)(now - ultimate2c_setup_at) >= 300u) {
            ultimate2c_setup_step = 3;
            if (hids_host_enable_notifications(hids_cid) != ERROR_CODE_SUCCESS) ultimate2c_setup_step = 2;
            else ble_stage = "notifications";
        } else if (ultimate2c_setup_step == 4) {
            ultimate2c_setup_step = 5;
            if (device_information_service_client_query(le_handle, ble_identity_handler) != ERROR_CODE_SUCCESS) ultimate2c_setup_step = 4;
        }
    }
    if (ds4_activation_pending && ds4_descriptor_ready && classic_hid_cid &&
        current_identity.kind == BB_CONTROLLER_DUALSHOCK4 && !manual_disconnect_pending &&
        (int32_t)(now - ds4_activation_at) >= 0) {
        if (ds4_activation_attempts < 10) {
            ++ds4_activation_attempts;
            if (ds4_activation_step == 0) {
                if (send_ds4_rumble(0)) {
                    ds4_activation_step = 1;
                    ds4_activation_attempts = 0;
                }
            } else if (ds4_activation_step == 1) {
                ds4_activation_step = 2;
                if (hid_host_send_get_report(classic_hid_cid, HID_REPORT_TYPE_FEATURE, 0x05) == ERROR_CODE_SUCCESS) {
                    ds4_activation_pending = false;
                } else ds4_activation_step = 1;
            }
            ds4_activation_at = now + 100u;
        } else ds4_activation_pending = false;
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
    if (ble_connecting && strcmp(ble_stage, "disconnecting") &&
        (uint32_t)(now - ble_attempt_started) >= 30000u) ble_setup_fail(ERROR_CODE_CONNECTION_TIMEOUT);
    if (classic_attempt_active && (uint32_t)(now - classic_attempt_started) >= 15000u) {
        uint16_t cid = classic_hid_cid;
        hci_con_handle_t handle = pending_classic_handle;
        if (cid) hid_host_disconnect(cid);
        if (handle != HCI_CON_HANDLE_INVALID) gap_disconnect(handle);
        controller_disconnected(false);
    }
    if (classic_hid_cid && current_controller_index >= 0 && current_identity.kind == BB_CONTROLLER_8BITDO &&
        name_has(current_identity.name, "arcade stick") && (int32_t)(now - arcade_battery_poll_at) >= 0) {
        int16_t id = bb_driver_battery_report_id();
        if (id >= 0) {
            uint8_t status = hid_host_send_get_report(classic_hid_cid, (hid_report_type_t)bb_driver_battery_report_type(), (uint16_t)id);
            bb_driver_record_battery_request(status);
            arcade_battery_poll_at = now + (status == ERROR_CODE_SUCCESS ? 60000u : 1000u);
        } else arcade_battery_poll_at = now + 1000u;
    }
    service_classic_identity();
    service_switch_setup(now);
    bb_runtime_state_t live_state;
    bb_state_get(&live_state);
    bool nintendo_stream = current_identity.kind == BB_CONTROLLER_NSO_N64 || current_identity.kind == BB_CONTROLLER_SWITCH_PRO || current_identity.kind == BB_CONTROLLER_JOYCON_LEFT || current_identity.kind == BB_CONTROLLER_JOYCON_RIGHT || current_identity.kind == BB_CONTROLLER_SWITCH2_PRO || current_identity.kind == BB_CONTROLLER_NSO_GAMECUBE;
    if (live_state.controller_connected && nintendo_stream && last_controller_report_at && (uint32_t)(now - last_controller_report_at) >= 3000u) {
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        else if (classic_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(classic_handle);
        if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
        controller_disconnected(false);
    }
    if (manual_disconnect_pending && (int32_t)(now - manual_disconnect_at) >= 0) {
        manual_disconnect_pending = false;
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        else if (classic_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(classic_handle);
    }
    service_rumble();
}
static void usb_watch_handler(btstack_timer_source_t* timer) {
    uint8_t previous = bb_slot_current();
    if (pairing_command) {
        uint8_t command = pairing_command;
        pairing_command = 0;
        if (command == 1) start_pairing_impl(); else stop_pairing_impl();
    }
    if (forget_all_command) { forget_all_command = false; forget_all_impl(); }
    for (unsigned request = 0; request < BB_MAX_CONTROLLERS; ++request) {
        if (!forget_commands[request]) continue;
        uint8_t address[6];
        memcpy(address, forget_addresses[request], 6);
        forget_commands[request] = false;
        int slot = slot_for_address(address);
        if (slot >= 0) bb_slot_enter((uint8_t)slot);
        forget_device_impl(address);
    }
    bool ready = controller_transport_ready();
    uint32_t now = btstack_run_loop_get_time_ms();
    if (ble_address_lookup_pending && (uint32_t)(now - ble_address_lookup_at) >= 3000u) ble_address_lookup_pending = false;
    if (ready) { usb_was_ready = true; usb_lost_since = 0; }
    else if (usb_was_ready) {
        if (!usb_lost_since) usb_lost_since = now;
        if ((uint32_t)(now - usb_lost_since) >= 1500u) {
            for (uint8_t i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) {
                bb_slot_enter(i);
                disconnect_current_impl();
            }
            usb_was_ready = false;
            usb_lost_since = 0;
        }
    }
    for (uint8_t i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) {
        bb_slot_enter(i);
        service_slot();
    }
    if ((pairing || bb_multi_mode()) && !connection_in_progress() && free_slot() >= 0 && controller_transport_ready()) start_scans();
    bb_slot_enter(previous);
    btstack_run_loop_set_timer(timer, 100);
    btstack_run_loop_add_timer(timer);
}

static uint16_t host_att_read(hci_con_handle_t handle, uint16_t attribute, uint16_t offset, uint8_t* buffer, uint16_t size) {
    (void)handle;
    if (attribute != host_service_changed_ccc_handle) return 0;
    uint8_t value[2] = {(uint8_t)host_service_changed_ccc, (uint8_t)(host_service_changed_ccc >> 8)};
    return att_read_callback_handle_blob(value, sizeof(value), offset, buffer, size);
}

static int host_att_write(hci_con_handle_t handle, uint16_t attribute, uint16_t transaction, uint16_t offset, uint8_t* buffer, uint16_t size) {
    (void)handle;
    if (attribute != host_service_changed_ccc_handle) return ATT_ERROR_WRITE_NOT_PERMITTED;
    if (transaction != ATT_TRANSACTION_MODE_NONE) return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    if (offset) return ATT_ERROR_INVALID_OFFSET;
    if (size != 2) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
    uint16_t value = little_endian_read_16(buffer, 0);
    if (value != 0 && value != 2) return ATT_ERROR_VALUE_NOT_ALLOWED;
    host_service_changed_ccc = value;
    return ATT_ERROR_SUCCESS;
}

static void setup_host_att_server(void) {
    att_db_util_init();
    att_db_util_add_service_uuid16(0x1800);
    att_db_util_add_characteristic_uuid16(0x2a00, ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE, host_att_name, sizeof(host_att_name) - 1);
    att_db_util_add_characteristic_uuid16(0x2a01, ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE, host_att_appearance, sizeof(host_att_appearance));
    att_db_util_add_service_uuid16(0x1801);
    uint16_t value_handle = att_db_util_add_characteristic_uuid16(0x2a05, ATT_PROPERTY_INDICATE, ATT_SECURITY_NONE, ATT_SECURITY_NONE, host_service_changed_range, sizeof(host_service_changed_range));
    host_service_changed_ccc_handle = value_handle + 1;
    host_service_changed_ccc = 0;
    att_server_init(att_db_util_get_address(), host_att_read, host_att_write);
}

void bb_bt_host_init(void) {
    reset_classic_name_candidates();
    bb_driver_reset();
    bb_config_get(&remembered_cfg);
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
    sm_set_encryption_key_size_range(7, 16);
    gatt_client_init();
    setup_host_att_server();
    device_information_service_client_init();
    battery_service_client_init();
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

bool bb_bt_host_pairing_available(void) { return free_slot() >= 0; }
void bb_bt_host_start_pairing(void) { pairing_command = 1; }
void bb_bt_host_stop_pairing(void) { pairing_command = 2; }
void bb_bt_host_forget_all(void) { forget_all_command = true; }
bool bb_bt_host_forget_device(const uint8_t address[6]) {
    if (!address) return false;
    for (unsigned i = 0; i < BB_MAX_CONTROLLERS; ++i) {
        if (forget_commands[i]) continue;
        memcpy(forget_addresses[i], address, 6);
        forget_commands[i] = true;
        return true;
    }
    return false;
}

static void start_pairing_impl(void) {
    if (free_slot() < 0) return;
    reset_classic_name_candidates();
    pairing = true;
    bb_state_set_pairing(true);
    gap_discoverable_control(1);
    stop_scans();
    start_scans();
}

static void stop_pairing_impl(void) {
    reset_classic_name_candidates();
    pairing = false;
    bb_state_set_pairing(false);

    gap_discoverable_control(1);
    stop_scans();
    start_scans();
}

static void forget_all_impl(void) {
    uint8_t previous = bb_slot_current();
    for (uint8_t i = 0; i < (bb_multi_mode() ? BB_MAX_SLOTS : 1); ++i) {
        bb_slot_enter(i);
        if (classic_hid_cid) hid_host_disconnect(classic_hid_cid);
        if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
        controller_disconnected(false);
    }
    bb_slot_enter(previous);
    gap_delete_all_link_keys();
    for (int i = 0; i < NVM_NUM_DEVICE_DB_ENTRIES; ++i) le_device_db_remove(i);
    pairing = true;
    bb_state_set_pairing(true);
    gap_discoverable_control(1);
    bb_state_set_controller(false, -1, NULL);
    start_scans();
}

static bool forget_device_impl(const uint8_t address[6]) {
    if (!address) return false;
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
        controller_disconnected(false);
    }
    start_scans();
    return true;
}

bool bb_bt_host_disconnect_current(void) {
    int slot = bb_controller_slot(bb_selected_controller_index());
    if (slot < 0) return false;
    multi_disconnect_requests[slot] = true;
    return true;
}

static bool disconnect_current_impl(void) {

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
        } else if (current_identity.kind == BB_CONTROLLER_DUALSHOCK4 && classic_handle != HCI_CON_HANDLE_INVALID) {
            if (gap_disconnect(classic_handle) != ERROR_CODE_SUCCESS && classic_hid_cid) hid_host_disconnect(classic_hid_cid);
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
    if (!classic_hid_cid && !xbox_ble_connected() && !eightbitdo64_ble_connected()) return false;
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

void bb_bt_host_set_slot_rumble(uint8_t slot, uint8_t left, uint8_t right) {
    if (slot >= BB_MAX_SLOTS) return;
    uint8_t strength = left > right ? left : right;
    bb_host_context_t* context = &host_slots[slot];
    context->queued_strength = strength;
    context->queued_duration = strength ? 1200u : 50u;
    context->queued_pending = true;
}

