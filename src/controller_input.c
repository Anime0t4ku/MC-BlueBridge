#include "controller_input.h"
#include "bluebridge_state.h"
#include "usb_device.h"
#include <ctype.h>
#include <string.h>
#include "pico/stdlib.h"

#define BB_DPAD_UP 1
#define BB_DPAD_DOWN 2
#define BB_DPAD_LEFT 4
#define BB_DPAD_RIGHT 8

static bool contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (; *haystack; ++haystack) {
        size_t i = 0;
        while (i < n && haystack[i] && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

static int8_t axis_to_i8(int16_t value) {
    if (value < -32767) value = -32767;
    return (int8_t)(value / 256);
}

static int16_t apply_axis_deadzone(int16_t value, uint8_t percent) {
    int32_t v = value;
    int32_t sign = v < 0 ? -1 : 1;
    int32_t mag = v < 0 ? -v : v;
    int32_t threshold = (32767 * percent) / 100;
    if (mag <= threshold) return 0;
    int32_t scaled = ((mag - threshold) * 32767) / (32767 - threshold);
    if (scaled > 32767) scaled = 32767;
    return (int16_t)(scaled * sign);
}

static uint16_t apply_trigger_deadzone(uint16_t value, uint8_t percent) {
    if (value > 255u) value = 255u;
    if (percent >= 100u) return 0;
    uint32_t threshold = (255u * percent) / 100u;
    if (value <= threshold) return 0;
    return (uint16_t)(((uint32_t)value - threshold) * 255u / (255u - threshold));
}
void bb_input_clear(bb_input_state_t* state) {
    memset(state, 0, sizeof(*state));
}

uint8_t bb_hat_from_dpad(uint8_t dpad) {
    bool u = (dpad & BB_DPAD_UP) != 0;
    bool d = (dpad & BB_DPAD_DOWN) != 0;
    bool l = (dpad & BB_DPAD_LEFT) != 0;
    bool r = (dpad & BB_DPAD_RIGHT) != 0;
    if (u && r) return 2;
    if (r && d) return 4;
    if (d && l) return 6;
    if (l && u) return 8;
    if (u) return 1;
    if (r) return 3;
    if (d) return 5;
    if (l) return 7;
    return 0;
}

void bb_input_submit(const bb_input_state_t* state) {
    static uint32_t last_profile_id;
    static uint32_t previous_inputs;
    static uint16_t runtime_turbo_mask;
    static bool turbo_toggle;
    static uint8_t last_turbo_enabled;
    static uint8_t last_turbo_mode;
    static uint8_t last_turbo_button;
    static uint8_t last_turbo_modifier;
    bb_state_update_live_input(state->x, state->y, state->rx, state->ry, state->lt, state->rt, state->dpad, state->buttons);
    if (bb_capture_process(state->buttons)) {
        previous_inputs = state->buttons;
        bb_gamepad_report_t neutral = {0};
        bb_state_update_gamepad(&neutral);
        bb_usb_submit(&neutral);
        return;
    }
    bb_profile_t profile;
    bb_active_profile_get(&profile);
    if (profile.id != last_profile_id || profile.turbo_enabled != last_turbo_enabled || profile.turbo_control_mode != last_turbo_mode || profile.turbo_control_button != last_turbo_button || profile.turbo_modifier != last_turbo_modifier) {
        last_profile_id = profile.id;
        last_turbo_enabled = profile.turbo_enabled;
        last_turbo_mode = profile.turbo_control_mode;
        last_turbo_button = profile.turbo_control_button;
        last_turbo_modifier = profile.turbo_modifier;
        previous_inputs = 0;
        runtime_turbo_mask = 0;
        turbo_toggle = false;
    }
    int16_t x = apply_axis_deadzone(state->x, profile.deadzone_left);
    int16_t y = apply_axis_deadzone(state->y, profile.deadzone_left);
    int16_t rx = apply_axis_deadzone(state->rx, profile.deadzone_right);
    int16_t ry = apply_axis_deadzone(state->ry, profile.deadzone_right);
    if (profile.invert_x) x = (int16_t)-x;
    if (profile.invert_y) y = (int16_t)-y;
    if (profile.invert_rx) rx = (int16_t)-rx;
    if (profile.invert_ry) ry = (int16_t)-ry;
    bb_gamepad_report_t report = {0};
    report.x = axis_to_i8(x);
    report.y = axis_to_i8(y);
    report.rx = axis_to_i8(rx);
    report.ry = axis_to_i8(ry);
    uint16_t lt = apply_trigger_deadzone(state->lt, profile.trigger_deadzone);
    uint16_t rt = apply_trigger_deadzone(state->rt, profile.trigger_deadzone);
    report.lt = (uint8_t)lt;
    report.rt = (uint8_t)rt;
    report.hat = bb_hat_from_dpad(state->dpad);
    uint16_t mapped = 0;
    uint32_t pressed_edges = state->buttons & ~previous_inputs;
    bool shortcut_layer = profile.turbo_enabled && profile.turbo_control_mode == 0 && profile.turbo_modifier < BB_BUTTON_COUNT && (state->buttons & (1u << profile.turbo_modifier));
    bool dedicated_hold = false;
    if (shortcut_layer) {
        for (uint8_t i = 0; i < BB_BUTTON_COUNT; ++i) {
            if (i == profile.turbo_modifier || !(pressed_edges & (1u << i))) continue;
            uint8_t target = profile.button_map[i];
            if (target < BB_OUTPUT_BUTTON_COUNT) runtime_turbo_mask ^= (uint16_t)(1u << target);
        }
    }
    if (profile.turbo_enabled && profile.turbo_control_mode == 1 && profile.turbo_control_button < BB_BUTTON_COUNT && (pressed_edges & (1u << profile.turbo_control_button))) turbo_toggle = !turbo_toggle;
    if (profile.turbo_enabled && profile.turbo_control_mode == 2 && profile.turbo_control_button < BB_BUTTON_COUNT && (state->buttons & (1u << profile.turbo_control_button))) dedicated_hold = true;
    for (uint8_t i = 0; i < BB_BUTTON_COUNT; ++i) {
        if (!(state->buttons & (1u << i))) continue;
        if (shortcut_layer) continue;
        if (profile.turbo_enabled && profile.turbo_control_mode > 0 && i == profile.turbo_control_button) continue;
        uint8_t target = profile.button_map[i];
        if (target < BB_OUTPUT_BUTTON_COUNT) {
            mapped |= (uint16_t)(1u << target);
        } else if (target >= BB_MAP_MACRO1 && target <= BB_MAP_MACRO4) {
            uint8_t macro_index = (uint8_t)(target - BB_MAP_MACRO1);
            if (profile.macros[macro_index].enabled && profile.macros[macro_index].name[0]) mapped |= profile.macros[macro_index].output_mask;
        }
    }
    if (profile.turbo_enabled && profile.turbo_rate_hz) {
        uint16_t active_turbo = 0;
        if (profile.turbo_control_mode == 0) active_turbo = runtime_turbo_mask;
        else if (turbo_toggle || dedicated_hold) active_turbo = profile.turbo_mask;
        active_turbo &= mapped;
        if (active_turbo) {
            uint32_t period_ms = 1000u / profile.turbo_rate_hz;
            if (period_ms < 2) period_ms = 2;
            uint32_t phase = to_ms_since_boot(get_absolute_time()) % period_ms;
            if (phase >= period_ms / 2u) mapped &= (uint16_t)~active_turbo;
        }
    }
    previous_inputs = state->buttons;
    report.buttons = mapped;
    bb_state_update_gamepad(&report);
    bb_usb_submit(&report);
}

bb_controller_kind_t bb_controller_identify(uint16_t vid, uint16_t pid, const char* name) {
    if (vid == 0x054c) {
        if (pid == 0x0ce6) return BB_CONTROLLER_DUALSENSE;
        if (pid == 0x0df2) return BB_CONTROLLER_DUALSENSE_EDGE;
        if (pid == 0x05c4 || pid == 0x09cc) return BB_CONTROLLER_DUALSHOCK4;
        if (pid == 0x0268) return BB_CONTROLLER_DUALSHOCK3;
        if (pid == 0x03d5 || pid == 0x0c5e) return BB_CONTROLLER_PS_MOVE;
    }
    if (vid == 0x057e) {
        if (pid == 0x2009) return BB_CONTROLLER_SWITCH_PRO;
        if (pid == 0x2006) return BB_CONTROLLER_JOYCON_LEFT;
        if (pid == 0x2007) return BB_CONTROLLER_JOYCON_RIGHT;
        if (pid == 0x0330) return BB_CONTROLLER_WII_U_PRO;
        if (pid == 0x0306) return BB_CONTROLLER_WII_REMOTE;
        if (pid == 0x2017) return BB_CONTROLLER_NSO_SNES;
        if (pid == 0x2019) return BB_CONTROLLER_NSO_N64;
        if (pid == 0x2069) return BB_CONTROLLER_SWITCH2_PRO;
        if (pid == 0x2073) return BB_CONTROLLER_NSO_GAMECUBE;
    }
    if (vid == 0x045e) {
        if (pid == 0x02e0 || pid == 0x02fd || pid == 0x02ea) return BB_CONTROLLER_XBOX_ONE;
        if (pid == 0x0b13) return BB_CONTROLLER_XBOX_SERIES;
        if (pid == 0x0b0a) return BB_CONTROLLER_XBOX_ADAPTIVE;
        if (pid == 0x0b00) return BB_CONTROLLER_XBOX_ELITE;
    }
    if (vid == 0x28de) {
        if (pid == 0x1302 || pid == 0x1304) return BB_CONTROLLER_STEAM2;
        return BB_CONTROLLER_STEAM;
    }
    if (vid == 0x18d1) return BB_CONTROLLER_STADIA;
    if (vid == 0x2dc8) return BB_CONTROLLER_8BITDO;
    if (contains_ci(name, "dualsense edge")) return BB_CONTROLLER_DUALSENSE_EDGE;
    if (contains_ci(name, "playstation(r)3") || contains_ci(name, "playstation 3") || contains_ci(name, "dualshock 3")) return BB_CONTROLLER_DUALSHOCK3;
    if (contains_ci(name, "dualshock 4")) return BB_CONTROLLER_DUALSHOCK4;
    if (contains_ci(name, "dualsense")) return BB_CONTROLLER_DUALSENSE;
    if (contains_ci(name, "rvl-cnt-01-uc")) return BB_CONTROLLER_WII_U_PRO;
    if (contains_ci(name, "rvl-cnt-01")) return BB_CONTROLLER_WII_REMOTE;
    if (contains_ci(name, "stadia")) return BB_CONTROLLER_STADIA;
    if (contains_ci(name, "steam controller 2") || contains_ci(name, "triton")) return BB_CONTROLLER_STEAM2;
    if (contains_ci(name, "xbox") && contains_ci(name, "elite")) return BB_CONTROLLER_XBOX_ELITE;
    if (contains_ci(name, "switch 2 pro") || contains_ci(name, "pro controller 2")) return BB_CONTROLLER_SWITCH2_PRO;
    if (contains_ci(name, "gamecube") && contains_ci(name, "nintendo")) return BB_CONTROLLER_NSO_GAMECUBE;
    if (contains_ci(name, "n64") || contains_ci(name, "nintendo 64")) return BB_CONTROLLER_NSO_N64;
    if (contains_ci(name, "snes") || contains_ci(name, "super famicom")) return BB_CONTROLLER_NSO_SNES;
    if (contains_ci(name, "genesis") || contains_ci(name, "mega drive")) return BB_CONTROLLER_NSO_GENESIS;
    if (contains_ci(name, "nes controller") || contains_ci(name, "famicom")) return BB_CONTROLLER_NSO_NES;
    if (contains_ci(name, "pro controller")) return BB_CONTROLLER_SWITCH_PRO;
    if (contains_ci(name, "joy-con (l)")) return BB_CONTROLLER_JOYCON_LEFT;
    if (contains_ci(name, "joy-con (r)")) return BB_CONTROLLER_JOYCON_RIGHT;
    if (contains_ci(name, "xbox")) return BB_CONTROLLER_XBOX_SERIES;
    if (contains_ci(name, "8bitdo") || contains_ci(name, "pro 3")) return BB_CONTROLLER_8BITDO;
    if (contains_ci(name, "nimbus")) return BB_CONTROLLER_NIMBUS;
    if (contains_ci(name, "ouya")) return BB_CONTROLLER_OUYA;
    if (contains_ci(name, "atari vcs")) return BB_CONTROLLER_ATARI_VCS;
    if (contains_ci(name, "icade")) return BB_CONTROLLER_ICADE;
    return BB_CONTROLLER_GENERIC;
}

const char* bb_controller_button_label(bb_controller_kind_t kind, const char* name, uint8_t index) {
    static const char* generic[BB_BUTTON_COUNT] = {"South","East","West","North","L1","R1","L2","R2","Select","Start","L3","R3","Home","Capture","Extra 1","Extra 2","L4","R4","L5","R5","P1","P2","P3","P4","Aux 1","Aux 2"};
    static const char* dualsense[BB_BUTTON_COUNT] = {"Cross","Circle","Square","Triangle","L1","R1","L2","R2","Create","Options","L3","R3","PS","Touchpad Click","Mute","Extra","Right Back","Left Back","Fn Right","Fn Left","P1","P2","P3","P4","Aux 1","Aux 2"};
    static const char* ds4[BB_BUTTON_COUNT] = {"Cross","Circle","Square","Triangle","L1","R1","L2","R2","Share","Options","L3","R3","PS","Touchpad Click","Extra 1","Extra 2","L4","R4","L5","R5","P1","P2","P3","P4","Aux 1","Aux 2"};
    static const char* ds3[BB_BUTTON_COUNT] = {"Cross","Circle","Square","Triangle","L1","R1","L2","R2","Select","Start","L3","R3","PS","Extra 1","Extra 2","Extra 3","L4","R4","L5","R5","P1","P2","P3","P4","Aux 1","Aux 2"};
    static const char* xbox[BB_BUTTON_COUNT] = {"A","B","X","Y","LB","RB","LT","RT","View","Menu","LS","RS","Xbox","Share","Extra 1","Extra 2","P1","P3","P2","P4","Aux 1","Aux 2","Aux 3","Aux 4","Aux 5","Aux 6"};
    static const char* nintendo[BB_BUTTON_COUNT] = {"B","A","Y","X","L","R","ZL","ZR","Minus","Plus","L Stick","R Stick","Home","Capture","GL","GR","L4","R4","L5","R5","P1","P2","P3","P4","Aux 1","Aux 2"};
    static const char* switch2[BB_BUTTON_COUNT] = {"B","A","Y","X","L","R","ZL","ZR","Minus","Plus","L Stick","R Stick","Home","Capture","GL","GR","C","L4","R4","L5","P1","P2","P3","P4","Aux 1","Aux 2"};
    static const char* stadia[BB_BUTTON_COUNT] = {"A","B","X","Y","L1","R1","L2","R2","Options","Menu","L3","R3","Stadia","Capture","Assistant","Extra","L4","R4","L5","R5","P1","P2","P3","P4","Aux 1","Aux 2"};
    static const char* n64[BB_BUTTON_COUNT] = {"A","B","C Left","C Right","L","R","Z","C Down","Unused","Start","ZR","C Up","Home","Capture","L4","R4","L5","R5","P1","P2","P3","P4","A3","A4","Aux 1","Aux 2"};
    static const char* snes[BB_BUTTON_COUNT] = {"B","A","Y","X","L","R","ZL","ZR","Select","Start","L Stick","R Stick","Home","Capture","L4","R4","L5","R5","P1","P2","P3","P4","A3","A4","Aux 1","Aux 2"};
    static const char* nes[BB_BUTTON_COUNT] = {"B","A","Y","X","L","R","ZL","ZR","Select","Start","L Stick","R Stick","Home","Capture","L4","R4","L5","R5","P1","P2","P3","P4","A3","A4","Aux 1","Aux 2"};
    static const char* genesis[BB_BUTTON_COUNT] = {"B","C","A","X","Y","Z","L","R","Mode","Start","L Stick","R Stick","Home","Capture","L4","R4","L5","R5","P1","P2","P3","P4","A3","A4","Aux 1","Aux 2"};
    static const char* eightbitdo[BB_BUTTON_COUNT] = {"B","A","Y","X","L1","R1","L2","R2","Select","Start","L3","R3","Home","Star","Extra 1","Extra 2","L4","R4","P1","P2","P3","P4","Aux 1","Aux 2","Aux 3","Aux 4"};
    static const char* steam2[BB_BUTTON_COUNT] = {"A","B","X","Y","L1","R1","L2","R2","View","Menu","L3","R3","Steam","QAM","Left Pad Click","Right Pad Click","L4","R4","L5","R5","P1","P2","P3","P4","Aux 1","Aux 2"};
    const char** labels = generic;
    if (kind == BB_CONTROLLER_DUALSENSE || kind == BB_CONTROLLER_DUALSENSE_EDGE) labels = dualsense;
    else if (kind == BB_CONTROLLER_DUALSHOCK4) labels = ds4;
    else if (kind == BB_CONTROLLER_DUALSHOCK3) labels = ds3;
    else if (kind == BB_CONTROLLER_XBOX_ONE || kind == BB_CONTROLLER_XBOX_SERIES || kind == BB_CONTROLLER_XBOX_ADAPTIVE || kind == BB_CONTROLLER_XBOX_ELITE) labels = xbox;
    else if (kind == BB_CONTROLLER_STADIA) labels = stadia;
    else if (kind == BB_CONTROLLER_NSO_N64) labels = n64;
    else if (kind == BB_CONTROLLER_NSO_SNES) labels = snes;
    else if (kind == BB_CONTROLLER_NSO_NES) labels = nes;
    else if (kind == BB_CONTROLLER_NSO_GENESIS) labels = genesis;
    else if (kind == BB_CONTROLLER_8BITDO) labels = eightbitdo;
    else if (kind == BB_CONTROLLER_STEAM2) labels = steam2;
    else if (kind == BB_CONTROLLER_SWITCH2_PRO) labels = switch2;
    else if (kind == BB_CONTROLLER_SWITCH_PRO || kind == BB_CONTROLLER_JOYCON_LEFT || kind == BB_CONTROLLER_JOYCON_RIGHT || kind == BB_CONTROLLER_NSO_GAMECUBE || kind == BB_CONTROLLER_WII_U_PRO) labels = nintendo;
    if (kind == BB_CONTROLLER_8BITDO && index == 18 && contains_ci(name, "pro 3")) return "PL";
    if (kind == BB_CONTROLLER_8BITDO && index == 19 && contains_ci(name, "pro 3")) return "PR";
    return index < BB_BUTTON_COUNT ? labels[index] : "Unknown";
}

bb_controller_capabilities_t bb_controller_capabilities(bb_controller_kind_t kind, const char* name) {
    bb_controller_capabilities_t c = {0x03ffffffu, 1, 1, 1, 1, 1};
    switch (kind) {
        case BB_CONTROLLER_DUALSENSE:
            c.buttons = 0x00007fffu;
            break;
        case BB_CONTROLLER_DUALSENSE_EDGE:
            c.buttons = 0x00007fffu;
            break;
        case BB_CONTROLLER_DUALSHOCK4:
            c.buttons = 0x00003fffu;
            break;
        case BB_CONTROLLER_DUALSHOCK3:
            c.buttons = 0x00001fffu;
            break;
        case BB_CONTROLLER_SWITCH_PRO:
            c.buttons = 0x00003fffu;
            break;
        case BB_CONTROLLER_SWITCH2_PRO:
            c.buttons = 0x0001ffffu;
            break;
        case BB_CONTROLLER_JOYCON_LEFT:
            c.buttons = 0x00002560u;
            c.right_stick = 0;
            c.right_trigger = 0;
            break;
        case BB_CONTROLLER_JOYCON_RIGHT:
            c.buttons = 0x00001aafu;
            c.dpad = 0;
            c.left_stick = 0;
            c.left_trigger = 0;
            break;
        case BB_CONTROLLER_NSO_NES:
            c.buttons = 0x00000333u;
            c.left_stick = 0;
            c.right_stick = 0;
            c.left_trigger = 0;
            c.right_trigger = 0;
            break;
        case BB_CONTROLLER_NSO_SNES:
            c.buttons = 0x000003ffu;
            c.left_stick = 0;
            c.right_stick = 0;
            c.left_trigger = 0;
            c.right_trigger = 0;
            break;
        case BB_CONTROLLER_NSO_N64:
            c.buttons = 0x00003effu;
            c.right_stick = 0;
            c.right_trigger = 0;
            break;
        case BB_CONTROLLER_NSO_GENESIS:
            c.buttons = 0x0000033fu;
            c.left_stick = 0;
            c.right_stick = 0;
            c.left_trigger = 0;
            c.right_trigger = 0;
            break;
        case BB_CONTROLLER_NSO_GAMECUBE:
            c.buttons = 0x00005ff3u;
            break;
        case BB_CONTROLLER_8BITDO:
            c.buttons = 0x00003fffu;
            if (contains_ci(name, "pro 3")) c.buttons = 0x000f1fffu;
            else if (contains_ci(name, "ultimate")) c.buttons = 0x000c3fffu;
            break;
        case BB_CONTROLLER_XBOX_ELITE:
            c.buttons = 0x000f1fffu;
            break;
        case BB_CONTROLLER_STEAM2:
            c.buttons = 0x000f3fffu;
            break;
        case BB_CONTROLLER_WII_U_PRO:
            c.buttons = 0x00001fffu;
            break;
        case BB_CONTROLLER_WII_REMOTE:
            c.buttons = 0x00001333u;
            c.left_stick = 0;
            c.right_stick = 0;
            c.left_trigger = 0;
            c.right_trigger = 0;
            break;
        case BB_CONTROLLER_XBOX_ONE:
        case BB_CONTROLLER_XBOX_ADAPTIVE:
            c.buttons = 0x00001fffu;
            break;
        case BB_CONTROLLER_XBOX_SERIES:
            c.buttons = 0x00003fffu;
            break;
        case BB_CONTROLLER_STADIA:
            c.buttons = 0x00007fffu;
            break;
        default:
            break;
    }
    return c;
}

const char* bb_controller_kind_name(bb_controller_kind_t kind) {
    switch (kind) {
        case BB_CONTROLLER_DUALSENSE: return "DualSense";
        case BB_CONTROLLER_DUALSENSE_EDGE: return "DualSense Edge";
        case BB_CONTROLLER_DUALSHOCK4: return "DualShock 4";
        case BB_CONTROLLER_DUALSHOCK3: return "DualShock 3";
        case BB_CONTROLLER_PS_MOVE: return "PlayStation Move";
        case BB_CONTROLLER_SWITCH_PRO: return "Switch Pro Controller";
        case BB_CONTROLLER_JOYCON_LEFT: return "Joy-Con (L)";
        case BB_CONTROLLER_JOYCON_RIGHT: return "Joy-Con (R)";
        case BB_CONTROLLER_WII_U_PRO: return "Wii U Pro Controller";
        case BB_CONTROLLER_WII_REMOTE: return "Wii Remote";
        case BB_CONTROLLER_WII_CLASSIC: return "Wii Classic Controller";
        case BB_CONTROLLER_WII_BALANCE_BOARD: return "Wii Balance Board";
        case BB_CONTROLLER_XBOX_ONE: return "Xbox One Controller";
        case BB_CONTROLLER_XBOX_SERIES: return "Xbox Series Controller";
        case BB_CONTROLLER_XBOX_ADAPTIVE: return "Xbox Adaptive Controller";
        case BB_CONTROLLER_XBOX_ELITE: return "Xbox Elite Controller";
        case BB_CONTROLLER_STEAM: return "Steam Controller";
        case BB_CONTROLLER_STEAM2: return "Steam Controller 2";
        case BB_CONTROLLER_STADIA: return "Stadia Controller";
        case BB_CONTROLLER_8BITDO: return "8BitDo Controller";
        case BB_CONTROLLER_NIMBUS: return "SteelSeries Nimbus";
        case BB_CONTROLLER_OUYA: return "OUYA Controller";
        case BB_CONTROLLER_ATARI_VCS: return "Atari VCS Controller";
        case BB_CONTROLLER_ICADE: return "iCade Controller";
        case BB_CONTROLLER_NSO_NES: return "NSO NES Controller";
        case BB_CONTROLLER_NSO_SNES: return "NSO SNES Controller";
        case BB_CONTROLLER_NSO_N64: return "NSO N64 Controller";
        case BB_CONTROLLER_NSO_GENESIS: return "NSO Genesis / Mega Drive Controller";
        case BB_CONTROLLER_NSO_GAMECUBE: return "NSO GameCube Controller";
        case BB_CONTROLLER_SWITCH2_PRO: return "Switch 2 Pro Controller";
        case BB_CONTROLLER_GENERIC: return "Bluetooth Controller";
        default: return "Unknown Controller";
    }
}

void bb_controller_display_name(bb_controller_kind_t kind, const char* raw_name, char* out, size_t out_size) {
    if (!out || !out_size) return;
    const char* value = bb_controller_kind_name(kind);
    if (kind == BB_CONTROLLER_8BITDO) {
        if (contains_ci(raw_name, "pro 3")) value = "8BitDo Pro 3";
        else if (contains_ci(raw_name, "pro 2")) value = "8BitDo Pro 2";
        else if (contains_ci(raw_name, "ultimate")) value = "8BitDo Ultimate Controller";
        else if (contains_ci(raw_name, "sn30")) value = "8BitDo SN30 Controller";
        else if (contains_ci(raw_name, "m30")) value = "8BitDo M30 Controller";
    } else if (kind == BB_CONTROLLER_GENERIC && raw_name && raw_name[0]) {
        value = raw_name;
    }
    size_t n = strlen(value);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, value, n);
    out[n] = 0;
}
