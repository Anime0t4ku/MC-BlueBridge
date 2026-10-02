#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLUEBRIDGE_VERSION "1.0.0-beta-13"
#define BLUEBRIDGE_FIRMWARE_MANIFEST "MCBLUEBRIDGE|" BLUEBRIDGE_VERSION "|PICO2W|SCHEMA=9"
#define BLUEBRIDGE_PROTOCOL_VERSION 2
#define BB_CONFIG_SCHEMA 9
#define BB_MAX_NAME 32
#define BB_MAX_PROFILE_NAME 24
#define BB_MAX_CONTROLLERS 8
#define BB_MAX_PROFILES 8
#define BB_BUTTON_COUNT 26
#define BB_OUTPUT_BUTTON_COUNT 16
#define BB_MACRO_COUNT 4
#define BB_MAX_MACRO_NAME 16
#define BB_MAP_MACRO1 16
#define BB_MAP_MACRO2 17
#define BB_MAP_MACRO3 18
#define BB_MAP_MACRO4 19
#define BB_MAP_TURBO_TOGGLE 20
#define BB_MAP_TURBO_HOLD 21
#define BB_MAP_DISABLED 22
#define BB_MAP_TARGET_COUNT 23

typedef enum {
    BB_OUTPUT_MISTER = 1,
    BB_OUTPUT_XINPUT = 2,
    BB_OUTPUT_GENERIC_HID = 3,
    BB_OUTPUT_SWITCH = 4
} bb_output_mode_t;

typedef struct __attribute__((packed)) {
    int8_t x;
    int8_t y;
    int8_t rx;
    int8_t ry;
    uint8_t lt;
    uint8_t rt;
    uint8_t hat;
    uint16_t buttons;
} bb_gamepad_report_t;

typedef struct __attribute__((packed)) {
    uint8_t enabled;
    uint8_t reserved;
    uint16_t output_mask;
    char name[BB_MAX_MACRO_NAME];
} bb_macro_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    char name[BB_MAX_PROFILE_NAME];
    uint8_t button_map[BB_BUTTON_COUNT];
    uint8_t invert_x;
    uint8_t invert_y;
    uint8_t invert_rx;
    uint8_t invert_ry;
    uint8_t deadzone_left;
    uint8_t deadzone_right;
    uint8_t trigger_deadzone;
    uint8_t turbo_rate_hz;
    uint8_t turbo_modifier;
    uint8_t turbo_enabled;
    uint8_t turbo_control_mode;
    uint8_t turbo_control_button;
    uint16_t turbo_mask;
    bb_macro_t macros[BB_MACRO_COUNT];
    uint8_t mister_mapping_mode;
    uint16_t mister_identity;
} bb_profile_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint8_t bluetooth_address[6];
    char name[BB_MAX_NAME];
    uint16_t native_vendor_id;
    uint16_t native_product_id;
    uint8_t profile_count;
    uint8_t active_profile;
    uint16_t mister_identity;
    bb_profile_t profiles[BB_MAX_PROFILES];
} bb_controller_t;

typedef struct __attribute__((packed)) {
    uint32_t schema;
    uint8_t output_mode;
    uint8_t controller_count;
    uint8_t active_controller;
    uint32_t next_controller_id;
    uint32_t next_profile_id;
    uint16_t next_mister_identity;
    bb_controller_t controllers[BB_MAX_CONTROLLERS];
} bb_config_t;

typedef enum {
    BB_CAPTURE_NONE = 0,
    BB_CAPTURE_ONCE = 1,
    BB_CAPTURE_TESTER = 2
} bb_capture_mode_t;

typedef struct {
    bool bt_ready;
    bool bt_scanning;
    bool pairing;
    bool controller_connected;
    bool usb_ready;
    int controller_index;
    char controller_name[BB_MAX_NAME];
    int8_t controller_rssi;
    uint32_t bt_reports;
    uint32_t usb_reports;
    uint32_t dropped_usb_reports;
    uint32_t raw_buttons;
    uint32_t button_capabilities;
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_rx;
    int16_t raw_ry;
    uint16_t raw_lt;
    uint16_t raw_rt;
    uint8_t raw_dpad;
    uint8_t capture_mode;
    bool capture_ready;
    uint8_t captured_button;
    bool battery_supported;
    uint8_t battery_percent;
    uint8_t battery_state;
    bool rumble_supported;
    bb_gamepad_report_t report;
} bb_runtime_state_t;

void bb_state_init(void);
void bb_state_set_controller(bool connected, int idx, const char* name);
void bb_state_set_scanning(bool scanning);
void bb_state_set_pairing(bool pairing);
void bb_state_set_bt_ready(bool ready);
void bb_state_set_usb_ready(bool ready);
void bb_state_update_gamepad(const bb_gamepad_report_t* report);
void bb_state_update_raw_buttons(uint32_t buttons);
void bb_state_update_button_capabilities(uint32_t buttons);
void bb_state_update_live_input(int16_t x, int16_t y, int16_t rx, int16_t ry, uint16_t lt, uint16_t rt, uint8_t dpad, uint32_t buttons);
void bb_state_update_battery(bool supported, uint8_t percent, uint8_t state);
void bb_state_set_rumble_supported(bool supported);
bool bb_capture_start(bb_capture_mode_t mode);
void bb_capture_stop(void);
bool bb_capture_process(uint32_t buttons);
void bb_capture_clear_result(void);
void bb_state_get(bb_runtime_state_t* out);
void bb_config_get(bb_config_t* out);
void bb_active_profile_get(bb_profile_t* out);
void bb_active_controller_get(bb_controller_t* out, uint8_t* active_index);
uint8_t bb_active_profile_index(void);
uint8_t bb_output_mode_value(void);
void bb_config_set(const bb_config_t* cfg, bool persist);
void bb_config_flush_pending(void);
void bb_state_note_usb_report(bool sent);
bb_controller_t* bb_config_active_controller(bb_config_t* cfg);
bb_profile_t* bb_config_active_profile(bb_config_t* cfg);
const bb_controller_t* bb_config_active_controller_const(const bb_config_t* cfg);
const bb_profile_t* bb_config_active_profile_const(const bb_config_t* cfg);
bool bb_profile_create(const char* name, uint8_t* out_index);
bool bb_profile_duplicate(uint8_t source_index, const char* name, uint8_t* out_index);
bool bb_profile_select(uint8_t index);
bool bb_profile_delete(uint8_t index);
bool bb_profile_rename(uint8_t index, const char* name);
bool bb_profile_set_mapping(uint8_t profile_index, uint8_t input, uint8_t output);
bool bb_profile_set_tuning(uint8_t profile_index, uint8_t invert_x, uint8_t invert_y, uint8_t invert_rx, uint8_t invert_ry, uint8_t deadzone_left, uint8_t deadzone_right, uint8_t trigger_deadzone, uint8_t turbo_rate_hz, uint16_t turbo_mask);
bool bb_profile_set_macro(uint8_t profile_index, uint8_t macro_index, uint8_t enabled, const char* name, uint16_t output_mask);
bool bb_profile_set_turbo_modifier(uint8_t profile_index, uint8_t modifier);
bool bb_profile_set_turbo_control(uint8_t profile_index, uint8_t enabled, uint8_t mode, uint8_t button);
bool bb_profile_set_separate_mapping(uint8_t profile_index);
bool bb_profile_share_mapping(uint8_t profile_index, uint8_t target_index);
bool bb_controller_forget(uint8_t index, uint8_t address_out[6]);
bool bb_controller_activate_or_create(const uint8_t address[6], uint16_t vendor_id, uint16_t product_id, const char* name, uint8_t* out_index);
bool bb_controller_update_identity(uint8_t index, uint16_t vendor_id, uint16_t product_id, const char* name);
bool bb_controller_select(uint8_t index);
uint8_t bb_selected_controller_index(void);
bool bb_output_mode_set(bb_output_mode_t mode);
uint16_t bb_current_mister_identity(void);
bb_output_mode_t bb_output_mode(void);
const char* bb_output_mode_name(bb_output_mode_t mode);
uint32_t bb_config_checksum(const bb_config_t* cfg);
bool bb_config_import(const bb_config_t* cfg, uint32_t checksum);
bool bb_config_reset(void);
