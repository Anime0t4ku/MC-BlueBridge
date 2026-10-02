#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BB_CONTROLLER_UNKNOWN = 0,
    BB_CONTROLLER_GENERIC,
    BB_CONTROLLER_DUALSENSE,
    BB_CONTROLLER_DUALSENSE_EDGE,
    BB_CONTROLLER_DUALSHOCK4,
    BB_CONTROLLER_DUALSHOCK3,
    BB_CONTROLLER_PS_MOVE,
    BB_CONTROLLER_SWITCH_PRO,
    BB_CONTROLLER_JOYCON_LEFT,
    BB_CONTROLLER_JOYCON_RIGHT,
    BB_CONTROLLER_WII_U_PRO,
    BB_CONTROLLER_WII_REMOTE,
    BB_CONTROLLER_WII_CLASSIC,
    BB_CONTROLLER_WII_BALANCE_BOARD,
    BB_CONTROLLER_XBOX_ONE,
    BB_CONTROLLER_XBOX_SERIES,
    BB_CONTROLLER_XBOX_ADAPTIVE,
    BB_CONTROLLER_XBOX_ELITE,
    BB_CONTROLLER_STEAM,
    BB_CONTROLLER_STEAM2,
    BB_CONTROLLER_STADIA,
    BB_CONTROLLER_8BITDO,
    BB_CONTROLLER_NIMBUS,
    BB_CONTROLLER_OUYA,
    BB_CONTROLLER_ATARI_VCS,
    BB_CONTROLLER_ICADE,
    BB_CONTROLLER_NSO_NES,
    BB_CONTROLLER_NSO_SNES,
    BB_CONTROLLER_NSO_N64,
    BB_CONTROLLER_NSO_GENESIS,
    BB_CONTROLLER_NSO_GAMECUBE,
    BB_CONTROLLER_SWITCH2_PRO
} bb_controller_kind_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t rx;
    int16_t ry;
    uint16_t lt;
    uint16_t rt;
    uint32_t buttons;
    uint8_t dpad;
} bb_input_state_t;

typedef struct {
    uint8_t address[6];
    uint8_t address_type;
    uint16_t vendor_id;
    uint16_t product_id;
    bb_controller_kind_t kind;
    char name[32];
} bb_controller_identity_t;

typedef struct {
    uint32_t buttons;
    uint8_t dpad;
    uint8_t left_stick;
    uint8_t right_stick;
    uint8_t left_trigger;
    uint8_t right_trigger;
} bb_controller_capabilities_t;

void bb_input_clear(bb_input_state_t* state);
void bb_input_submit(const bb_input_state_t* state);
uint8_t bb_hat_from_dpad(uint8_t dpad);
bb_controller_kind_t bb_controller_identify(uint16_t vendor_id, uint16_t product_id, const char* name);
const char* bb_controller_kind_name(bb_controller_kind_t kind);
void bb_controller_display_name(bb_controller_kind_t kind, const char* raw_name, char* out, size_t out_size);
const char* bb_controller_button_label(bb_controller_kind_t kind, const char* name, uint8_t index);
bb_controller_capabilities_t bb_controller_capabilities(bb_controller_kind_t kind, const char* name);
