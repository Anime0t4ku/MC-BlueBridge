#include "bluebridge_state.h"
#include "config_store.h"
#include "pico/critical_section.h"
#include "pico/platform.h"
#include <stdio.h>
#include <string.h>

static critical_section_t lock;
static bb_runtime_state_t runtime_slots[BB_MAX_SLOTS];
static uint8_t io_slot;
static bool boot_multi;
static volatile bool mode_restart;
#define runtime_state runtime_slots[bb_slot_current()]
static bb_config_t config_state;
static volatile bool config_save_pending;
static bb_config_t save_copy;
static uint32_t capture_previous[BB_MAX_SLOTS];
#define capture_previous_buttons capture_previous[bb_slot_current()]
static uint32_t capture_masks[BB_MAX_SLOTS];
#define capture_button_mask capture_masks[bb_slot_current()]
static int management_controller_index = -1;

bool bb_multi_mode(void) { return boot_multi; }
bool bb_mode_restart_pending(void) { return mode_restart; }
void bb_slot_enter(uint8_t slot) { if (slot < BB_MAX_SLOTS) io_slot = slot; }
uint8_t bb_slot_current(void) {
    if (!boot_multi) return 0;
    if (!get_core_num()) return io_slot;
    for (uint8_t i = 0; i < BB_MAX_SLOTS; ++i) {
        if (runtime_slots[i].controller_connected && runtime_slots[i].controller_index == management_controller_index) return i;
    }
    return 0;
}
void bb_state_get_slot(uint8_t slot, bb_runtime_state_t* out) {
    if (!out || slot >= BB_MAX_SLOTS) return;
    critical_section_enter_blocking(&lock);
    *out = runtime_slots[slot];
    critical_section_exit(&lock);
}
int bb_controller_slot(uint8_t controller) {
    int slot = -1;
    critical_section_enter_blocking(&lock);
    for (uint8_t i = 0; i < (boot_multi ? BB_MAX_SLOTS : 1); ++i) {
        if (runtime_slots[i].controller_connected && runtime_slots[i].controller_index == controller) slot = i;
    }
    critical_section_exit(&lock);
    return slot;
}
bool bb_multi_mode_set(bool enabled) {
    critical_section_enter_blocking(&lock);
    if (config_state.multi_controller != enabled) {
        if (enabled) {
            config_state.single_output_mode = config_state.output_mode;
            config_state.output_mode = BB_OUTPUT_MISTER;
        } else {
            int controller = -1;
            for (unsigned slot = 0; slot < BB_MAX_SLOTS; ++slot) {
                if (!runtime_slots[slot].controller_connected) continue;
                int index = runtime_slots[slot].controller_index;
                if (index < 0 || index >= config_state.controller_count) continue;
                if (controller < 0 || index == management_controller_index) controller = index;
                if (index == management_controller_index) break;
            }
            if (controller >= 0) config_state.active_controller = (uint8_t)controller;
            uint8_t mode = config_state.single_output_mode;
            config_state.output_mode = mode >= BB_OUTPUT_MISTER && mode <= BB_OUTPUT_LAST ? mode : BB_OUTPUT_MISTER;
        }
        config_state.multi_controller = enabled;
        config_save_pending = true;
        mode_restart = true;
    }
    critical_section_exit(&lock);
    return true;
}
static uint8_t input_controller_index_locked(void) {
    if (boot_multi && !get_core_num()) return runtime_state.controller_index >= 0 ? (uint8_t)runtime_state.controller_index : 0xff;
    if (boot_multi && get_core_num() && management_controller_index >= 0) return (uint8_t)management_controller_index;
    return config_state.active_controller;
}

static void set_name(char* dst, size_t size, const char* src, const char* fallback) {
    const char* value = src && src[0] ? src : fallback;
    size_t out = 0;
    while (*value && out + 1 < size) {
        unsigned char c = (unsigned char)*value++;
        if (c < 32 || c == '|' || c == '\"' || c == '\\') c = '_';
        dst[out++] = (char)c;
    }
    dst[out] = 0;
}

static void init_profile(bb_profile_t* profile, uint32_t id, uint16_t mister_identity, const char* name) {
    memset(profile, 0, sizeof(*profile));
    profile->id = id;
    profile->mister_identity = mister_identity;
    set_name(profile->name, sizeof(profile->name), name, "Default");
    for (int i = 0; i < BB_BUTTON_COUNT; ++i) profile->button_map[i] = i < BB_OUTPUT_BUTTON_COUNT ? (uint8_t)i : BB_MAP_DISABLED;
    profile->deadzone_left = 8;
    profile->deadzone_right = 8;
    profile->trigger_deadzone = 4;
    profile->turbo_rate_hz = 12;
    profile->turbo_modifier = 8;
    profile->turbo_enabled = 0;
    profile->turbo_control_mode = 0;
    profile->turbo_control_button = 13;
}

static uint16_t allocate_mister_identity_locked(void) {
    uint16_t identity = config_state.next_mister_identity;
    if (identity == 0 || identity > 0x0fff) identity = 1;
    config_state.next_mister_identity = (uint16_t)(identity + 1);
    if (config_state.next_mister_identity > 0x0fff) config_state.next_mister_identity = 1;
    return identity;
}

static void init_defaults(bb_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->schema = BB_CONFIG_SCHEMA;
    cfg->output_mode = BB_OUTPUT_MISTER;
    cfg->next_controller_id = 1;
    cfg->next_profile_id = 1;
    cfg->next_mister_identity = 1;
}

uint32_t bb_config_checksum(const bb_config_t* cfg) {
    const uint8_t* p = (const uint8_t*)cfg;
    size_t n = sizeof(*cfg);
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

void bb_state_init(void) {
    critical_section_init(&lock);
    memset(runtime_slots, 0, sizeof(runtime_slots));
    for (unsigned i = 0; i < BB_MAX_SLOTS; ++i) {
        runtime_slots[i].controller_index = -1;
        runtime_slots[i].captured_button = 0xff;
    }
    if (!bb_config_load(&config_state)) {
        init_defaults(&config_state);
    } else if (config_state.schema == 8 || config_state.schema == 9 || config_state.schema == 10 || config_state.schema == 11 || config_state.schema == 12) {
        config_state.schema = BB_CONFIG_SCHEMA;
        if (config_state.output_mode < BB_OUTPUT_MISTER || config_state.output_mode > BB_OUTPUT_LAST) config_state.output_mode = BB_OUTPUT_MISTER;
        config_save_pending = true;
    } else if (config_state.schema != BB_CONFIG_SCHEMA) {
        init_defaults(&config_state);
    } else if (config_state.output_mode < BB_OUTPUT_MISTER || config_state.output_mode > BB_OUTPUT_LAST) {
        config_state.output_mode = BB_OUTPUT_MISTER;
        config_save_pending = true;
    }
    boot_multi = config_state.multi_controller == 1;
    config_state.multi_controller = boot_multi;
    if (boot_multi) config_state.output_mode = BB_OUTPUT_MISTER;
}

void bb_state_set_controller(bool connected, int idx, const char* name) {
    critical_section_enter_blocking(&lock);
    int previous_index = runtime_state.controller_index;
    runtime_state.controller_connected = connected;
    runtime_state.controller_index = connected ? idx : -1;
    snprintf(runtime_state.controller_name, sizeof(runtime_state.controller_name), "%s", name ? name : (connected ? "Bluetooth Controller" : ""));
    if (!connected) {
        memset(&runtime_state.report, 0, sizeof(runtime_state.report));
        runtime_state.report.hat = 0;
        runtime_state.raw_buttons = 0;
        runtime_state.button_capabilities = 0;
        runtime_state.raw_x = 0;
        runtime_state.raw_y = 0;
        runtime_state.raw_rx = 0;
        runtime_state.raw_ry = 0;
        runtime_state.raw_lt = 0;
        runtime_state.raw_rt = 0;
        runtime_state.raw_dpad = 0;
        runtime_state.battery_supported = false;
        runtime_state.battery_percent = 0;
        runtime_state.battery_state = 0;
        runtime_state.rumble_supported = false;
        runtime_state.capture_mode = BB_CAPTURE_NONE;
        runtime_state.capture_ready = false;
        runtime_state.captured_button = 0xff;
        capture_previous_buttons = 0;
        capture_button_mask = 0;
    }
    if (!connected && boot_multi && previous_index == management_controller_index) {
        for (unsigned slot = 0; slot < BB_MAX_SLOTS; ++slot) {
            if (runtime_slots[slot].controller_connected) {
                management_controller_index = runtime_slots[slot].controller_index;
                break;
            }
        }
    }
    critical_section_exit(&lock);
}

void bb_state_set_scanning(bool scanning) {
    critical_section_enter_blocking(&lock);
    for (unsigned i = 0; i < BB_MAX_SLOTS; ++i) runtime_slots[i].bt_scanning = scanning;
    critical_section_exit(&lock);
}

void bb_state_set_pairing(bool pairing) {
    critical_section_enter_blocking(&lock);
    for (unsigned i = 0; i < BB_MAX_SLOTS; ++i) runtime_slots[i].pairing = pairing;
    critical_section_exit(&lock);
}

void bb_state_set_bt_ready(bool ready) {
    critical_section_enter_blocking(&lock);
    for (unsigned i = 0; i < BB_MAX_SLOTS; ++i) runtime_slots[i].bt_ready = ready;
    critical_section_exit(&lock);
}

void bb_state_set_usb_ready(bool ready) {
    critical_section_enter_blocking(&lock);
    for (unsigned i = 0; i < BB_MAX_SLOTS; ++i) runtime_slots[i].usb_ready = ready;
    critical_section_exit(&lock);
}

void bb_state_update_gamepad(const bb_gamepad_report_t* report) {
    critical_section_enter_blocking(&lock);
    runtime_state.report = *report;
    runtime_state.bt_reports++;
    critical_section_exit(&lock);
}

void bb_state_update_raw_buttons(uint32_t buttons) {
    critical_section_enter_blocking(&lock);
    runtime_state.raw_buttons = buttons;
    critical_section_exit(&lock);
}

void bb_state_update_button_capabilities(uint32_t buttons) {
    critical_section_enter_blocking(&lock);
    runtime_state.button_capabilities = buttons;
    critical_section_exit(&lock);
}

void bb_state_update_live_input(int16_t x, int16_t y, int16_t rx, int16_t ry, uint16_t lt, uint16_t rt, uint8_t dpad, uint32_t buttons) {
    critical_section_enter_blocking(&lock);
    runtime_state.raw_x = x;
    runtime_state.raw_y = y;
    runtime_state.raw_rx = rx;
    runtime_state.raw_ry = ry;
    runtime_state.raw_lt = lt;
    runtime_state.raw_rt = rt;
    runtime_state.raw_dpad = dpad;
    runtime_state.raw_buttons = buttons;
    critical_section_exit(&lock);
}

void bb_state_update_battery(bool supported, uint8_t percent, uint8_t state) {
    critical_section_enter_blocking(&lock);
    runtime_state.battery_supported = supported;
    runtime_state.battery_percent = percent > 100 ? 100 : percent;
    runtime_state.battery_state = state;
    critical_section_exit(&lock);
}

void bb_state_set_rumble_supported(bool supported) {
    critical_section_enter_blocking(&lock);
    runtime_state.rumble_supported = supported;
    critical_section_exit(&lock);
}

bool bb_capture_start(bb_capture_mode_t mode) {
    if (mode != BB_CAPTURE_ONCE && mode != BB_CAPTURE_TESTER) return false;
    critical_section_enter_blocking(&lock);
    runtime_state.capture_mode = (uint8_t)mode;
    runtime_state.capture_ready = false;
    runtime_state.captured_button = 0xff;
    capture_previous_buttons = runtime_state.raw_buttons;
    capture_button_mask = 0;
    critical_section_exit(&lock);
    return true;
}

void bb_capture_stop(void) {
    critical_section_enter_blocking(&lock);
    runtime_state.capture_mode = BB_CAPTURE_NONE;
    capture_previous_buttons = runtime_state.raw_buttons;
    capture_button_mask = 0;
    critical_section_exit(&lock);
}

bool bb_capture_is_tester(void) {
    critical_section_enter_blocking(&lock);
    bool tester = runtime_state.capture_mode == BB_CAPTURE_TESTER;
    critical_section_exit(&lock);
    return tester;
}

bool bb_capture_process(uint32_t buttons) {
    bool suppress = false;
    critical_section_enter_blocking(&lock);
    if (runtime_state.capture_mode == BB_CAPTURE_TESTER) {
        suppress = true;
        capture_previous_buttons = buttons;
    } else if (runtime_state.capture_mode == BB_CAPTURE_ONCE) {
        suppress = true;
        uint32_t edges = buttons & ~capture_previous_buttons;
        if (!runtime_state.capture_ready && edges) {
            for (uint8_t i = 0; i < BB_BUTTON_COUNT; ++i) {
                uint32_t mask = (uint32_t)(1u << i);
                if (edges & mask) {
                    runtime_state.capture_ready = true;
                    runtime_state.captured_button = i;
                    capture_button_mask = mask;
                    break;
                }
            }
        }
        capture_previous_buttons = buttons;
        if (runtime_state.capture_ready && capture_button_mask && !(buttons & capture_button_mask)) {
            runtime_state.capture_mode = BB_CAPTURE_NONE;
            capture_button_mask = 0;
        }
    }
    critical_section_exit(&lock);
    return suppress;
}

void bb_capture_clear_result(void) {
    critical_section_enter_blocking(&lock);
    runtime_state.capture_ready = false;
    runtime_state.captured_button = 0xff;
    critical_section_exit(&lock);
}

void bb_state_get(bb_runtime_state_t* out) {
    critical_section_enter_blocking(&lock);
    *out = runtime_state;
    if (boot_multi && get_core_num() && out->controller_index != management_controller_index) {
        bool bt = out->bt_ready, usb = out->usb_ready, pair = out->pairing, scan = out->bt_scanning;
        memset(out, 0, sizeof(*out));
        out->controller_index = -1;
        out->captured_button = 0xff;
        out->bt_ready = bt; out->usb_ready = usb; out->pairing = pair; out->bt_scanning = scan;
    }
    critical_section_exit(&lock);
}

void bb_config_get(bb_config_t* out) {
    critical_section_enter_blocking(&lock);
    *out = config_state;
    critical_section_exit(&lock);
}

void bb_active_profile_get(bb_profile_t* out) {
    critical_section_enter_blocking(&lock);
    bb_controller_t* controller = NULL;
    if (config_state.controller_count && input_controller_index_locked() < config_state.controller_count) controller = &config_state.controllers[input_controller_index_locked()];
    if (controller && controller->profile_count && controller->active_profile < controller->profile_count) *out = controller->profiles[controller->active_profile];
    else init_profile(out, 0, 1, "Default");
    critical_section_exit(&lock);
}

static uint8_t selected_controller_index_locked(void) {
    if (management_controller_index >= 0 && management_controller_index < config_state.controller_count) return (uint8_t)management_controller_index;
    if (config_state.controller_count && input_controller_index_locked() < config_state.controller_count) return config_state.active_controller;
    return 0;
}

static bb_controller_t* selected_controller_locked(void) {
    if (!config_state.controller_count) return NULL;
    uint8_t index = selected_controller_index_locked();
    if (index >= config_state.controller_count) return NULL;
    return &config_state.controllers[index];
}

uint8_t bb_selected_controller_index(void) {
    uint8_t index;
    critical_section_enter_blocking(&lock);
    index = selected_controller_index_locked();
    critical_section_exit(&lock);
    return index;
}

void bb_active_controller_get(bb_controller_t* out, uint8_t* active_index) {
    critical_section_enter_blocking(&lock);
    uint8_t input_index = boot_multi && !get_core_num() ? input_controller_index_locked() : selected_controller_index_locked();
    bb_controller_t* controller = input_index < config_state.controller_count ? &config_state.controllers[input_index] : NULL;
    if (controller) {
        uint8_t index = input_index;
        *out = *controller;
        if (active_index) *active_index = index;
    } else {
        memset(out, 0, sizeof(*out));
        if (active_index) *active_index = 0;
    }
    critical_section_exit(&lock);
}

uint8_t bb_active_profile_index(void) {
    uint8_t index = 0;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count && input_controller_index_locked() < config_state.controller_count) {
        bb_controller_t* controller = &config_state.controllers[input_controller_index_locked()];
        if (controller->profile_count && controller->active_profile < controller->profile_count) index = controller->active_profile;
    }
    critical_section_exit(&lock);
    return index;
}

uint8_t bb_output_mode_value(void) {
    uint8_t mode;
    critical_section_enter_blocking(&lock);
    mode = config_state.output_mode;
    critical_section_exit(&lock);
    return mode;
}

void bb_config_set(const bb_config_t* cfg, bool persist) {
    critical_section_enter_blocking(&lock);
    config_state = *cfg;
    if (persist) config_save_pending = true;
    critical_section_exit(&lock);
}

void bb_config_flush_pending(void) {
    bool save = false;
    critical_section_enter_blocking(&lock);
    if (config_save_pending) {
        save_copy = config_state;
        config_save_pending = false;
        save = true;
    }
    critical_section_exit(&lock);
    if (save) bb_config_save(&save_copy);
}

void bb_state_note_slot_usb_report(uint8_t slot, bool sent) {
    if (slot >= BB_MAX_SLOTS) return;
    critical_section_enter_blocking(&lock);
    if (sent) runtime_slots[slot].usb_reports++;
    else runtime_slots[slot].dropped_usb_reports++;
    critical_section_exit(&lock);
}

void bb_state_note_usb_report(bool sent) {
    critical_section_enter_blocking(&lock);
    if (sent) runtime_state.usb_reports++;
    else runtime_state.dropped_usb_reports++;
    critical_section_exit(&lock);
}

bb_controller_t* bb_config_active_controller(bb_config_t* cfg) {
    if (!cfg || cfg->controller_count == 0 || cfg->active_controller >= cfg->controller_count) return NULL;
    return &cfg->controllers[cfg->active_controller];
}

bb_profile_t* bb_config_active_profile(bb_config_t* cfg) {
    bb_controller_t* controller = bb_config_active_controller(cfg);
    if (!controller || controller->profile_count == 0 || controller->active_profile >= controller->profile_count) return NULL;
    return &controller->profiles[controller->active_profile];
}

const bb_controller_t* bb_config_active_controller_const(const bb_config_t* cfg) {
    if (!cfg || cfg->controller_count == 0 || cfg->active_controller >= cfg->controller_count) return NULL;
    return &cfg->controllers[cfg->active_controller];
}

const bb_profile_t* bb_config_active_profile_const(const bb_config_t* cfg) {
    const bb_controller_t* controller = bb_config_active_controller_const(cfg);
    if (!controller || controller->profile_count == 0 || controller->active_profile >= controller->profile_count) return NULL;
    return &controller->profiles[controller->active_profile];
}

bool bb_profile_duplicate(uint8_t source_index, const char* name, uint8_t* out_index) {
    bool ok = false;
    uint8_t index = 0;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (source_index < controller->profile_count && controller->profile_count < BB_MAX_PROFILES) {
            index = controller->profile_count++;
            controller->profiles[index] = controller->profiles[source_index];
            controller->profiles[index].id = config_state.next_profile_id++;
            controller->profiles[index].mister_identity = allocate_mister_identity_locked();
            controller->profiles[index].mister_mapping_mode = 0;
            set_name(controller->profiles[index].name, sizeof(controller->profiles[index].name), name, "Copy");
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    if (ok && out_index) *out_index = index;
    return ok;
}

bool bb_profile_create(const char* name, uint8_t* out_index) {
    bool ok = false;
    uint8_t index = 0;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (controller->profile_count < BB_MAX_PROFILES) {
            index = controller->profile_count++;
            init_profile(&controller->profiles[index], config_state.next_profile_id++, allocate_mister_identity_locked(), name);
            controller->profiles[index].mister_mapping_mode = 0;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    if (ok && out_index) *out_index = index;
    return ok;
}

bool bb_profile_select(uint8_t index) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (index < controller->profile_count) {
            controller->active_profile = index;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_delete(uint8_t index) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (index > 0 && index < controller->profile_count) {
            for (uint8_t i = index; i + 1 < controller->profile_count; ++i) controller->profiles[i] = controller->profiles[i + 1];
            controller->profile_count--;
            if (controller->active_profile >= controller->profile_count) controller->active_profile = 0;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_rename(uint8_t index, const char* name) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (name && name[0] && config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (index < controller->profile_count) {
            set_name(controller->profiles[index].name, sizeof(controller->profiles[index].name), name, "Profile");
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_set_mapping(uint8_t profile_index, uint8_t input, uint8_t output) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (input < BB_BUTTON_COUNT && output < BB_MAP_TARGET_COUNT && config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (profile_index < controller->profile_count) {
            controller->profiles[profile_index].button_map[input] = output;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_set_tuning(uint8_t profile_index, uint8_t invert_x, uint8_t invert_y, uint8_t invert_rx, uint8_t invert_ry, uint8_t deadzone_left, uint8_t deadzone_right, uint8_t trigger_deadzone, uint8_t turbo_rate_hz, uint16_t turbo_mask) {
    bool ok = false;
    if (deadzone_left > 50 || deadzone_right > 50 || trigger_deadzone > 50 || turbo_rate_hz < 1 || turbo_rate_hz > 30) return false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (profile_index < controller->profile_count) {
            bb_profile_t* profile = &controller->profiles[profile_index];
            profile->invert_x = invert_x ? 1 : 0;
            profile->invert_y = invert_y ? 1 : 0;
            profile->invert_rx = invert_rx ? 1 : 0;
            profile->invert_ry = invert_ry ? 1 : 0;
            profile->deadzone_left = deadzone_left;
            profile->deadzone_right = deadzone_right;
            profile->trigger_deadzone = trigger_deadzone;
            if (!boot_multi) {
                profile->turbo_rate_hz = turbo_rate_hz;
                profile->turbo_mask = turbo_mask;
            }
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_set_turbo_modifier(uint8_t profile_index, uint8_t modifier) {
    bool ok = false;
    if (modifier >= BB_BUTTON_COUNT) return false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (profile_index < controller->profile_count) {
            controller->profiles[profile_index].turbo_modifier = modifier;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_set_macro(uint8_t profile_index, uint8_t macro_index, uint8_t enabled, const char* name, uint16_t output_mask) {
    bool ok = false;
    if (macro_index >= BB_MACRO_COUNT) return false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (profile_index < controller->profile_count) {
            bb_profile_t* profile = &controller->profiles[profile_index];
            bb_macro_t* macro = &profile->macros[macro_index];
            macro->enabled = enabled ? 1 : 0;
            macro->reserved = 0;
            macro->output_mask = output_mask;
            set_name(macro->name, sizeof(macro->name), name, "");
            if (!macro->name[0]) {
                macro->enabled = 0;
                macro->output_mask = 0;
                for (uint8_t i = 0; i < BB_BUTTON_COUNT; ++i) {
                    if (profile->button_map[i] == (uint8_t)(BB_MAP_MACRO1 + macro_index)) profile->button_map[i] = BB_MAP_DISABLED;
                }
            }
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_set_turbo_control(uint8_t profile_index, uint8_t enabled, uint8_t mode, uint8_t button) {
    bool ok = false;
    if (mode > 2 || button >= BB_BUTTON_COUNT) return false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (profile_index < controller->profile_count) {
            bb_profile_t* profile = &controller->profiles[profile_index];
            profile->turbo_enabled = enabled ? 1 : 0;
            profile->turbo_control_mode = mode;
            profile->turbo_control_button = button;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_set_separate_mapping(uint8_t profile_index) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (profile_index < controller->profile_count) {
            bb_profile_t* profile = &controller->profiles[profile_index];
            bool shared = false;
            for (uint8_t i = 0; i < controller->profile_count; ++i) {
                if (i != profile_index && controller->profiles[i].mister_identity == profile->mister_identity) {
                    shared = true;
                    break;
                }
            }
            if (shared || profile->mister_identity == 0) profile->mister_identity = allocate_mister_identity_locked();
            profile->mister_mapping_mode = 0;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_profile_share_mapping(uint8_t profile_index, uint8_t target_index) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count) {
        bb_controller_t* controller = selected_controller_locked();
        if (profile_index < controller->profile_count && target_index < controller->profile_count && profile_index != target_index) {
            controller->profiles[profile_index].mister_identity = controller->profiles[target_index].mister_identity;
            controller->profiles[profile_index].mister_mapping_mode = 1;
            config_save_pending = true;
            ok = true;
        }
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_adapter_rename(const char* name) {
    if (!name || strlen(name) >= BB_MAX_NAME) return false;
    critical_section_enter_blocking(&lock);
    set_name(config_state.adapter_name, sizeof(config_state.adapter_name), name, "");
    config_save_pending = true;
    critical_section_exit(&lock);
    return true;
}

void bb_adapter_custom_name_get(char* out, size_t size) {
    if (!out || !size) return;
    critical_section_enter_blocking(&lock);
    set_name(out, size, config_state.adapter_name, "");
    critical_section_exit(&lock);
}

void bb_adapter_name_get(char* out, size_t size) {
    bb_adapter_custom_name_get(out, size);
    if (out && size && !out[0]) set_name(out, size, "", "MC BlueBridge adapter");
}

bool bb_controller_rename(uint8_t index, const char* name) {
    if (!name || strlen(name) >= BB_MAX_NAME) return false;
    critical_section_enter_blocking(&lock);
    bool ok = index < config_state.controller_count;
    if (ok) {
        set_name(config_state.controller_names[index], BB_MAX_NAME, name, "");
        config_save_pending = true;
    }
    critical_section_exit(&lock);
    return ok;
}

void bb_controller_name_get(uint8_t index, char* out, size_t size) {
    if (!out || !size) return;
    out[0] = 0;
    critical_section_enter_blocking(&lock);
    if (index < config_state.controller_count) {
        const char* custom = config_state.controller_names[index];
        set_name(out, size, custom[0] ? custom : config_state.controllers[index].name, "Bluetooth Controller");
    }
    critical_section_exit(&lock);
}

bool bb_controller_forget(uint8_t index, uint8_t address_out[6]) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (index < config_state.controller_count) {
        if (address_out) memcpy(address_out, config_state.controllers[index].bluetooth_address, 6);
        for (uint8_t i = index; i + 1 < config_state.controller_count; ++i) {
            config_state.controllers[i] = config_state.controllers[i + 1];
            memcpy(config_state.controller_names[i], config_state.controller_names[i + 1], BB_MAX_NAME);
        }
        memset(config_state.controller_names[config_state.controller_count - 1], 0, BB_MAX_NAME);
        config_state.controller_count--;
        if (boot_multi) {
            for (unsigned slot = 0; slot < BB_MAX_SLOTS; ++slot) {
                if (runtime_slots[slot].controller_index == index) {
                    runtime_slots[slot].controller_index = -1;
                    runtime_slots[slot].controller_connected = false;
                    runtime_slots[slot].controller_name[0] = 0;
                    memset(&runtime_slots[slot].report, 0, sizeof(runtime_slots[slot].report));
                    runtime_slots[slot].raw_buttons = 0;
                }
                else if (runtime_slots[slot].controller_index > index) runtime_slots[slot].controller_index--;
            }
        }
        if (config_state.controller_count == 0) config_state.active_controller = 0;
        else if (config_state.active_controller > index) config_state.active_controller--;
        else if (config_state.active_controller >= config_state.controller_count) config_state.active_controller = 0;
        if (management_controller_index == index) management_controller_index = config_state.controller_count ? config_state.active_controller : -1;
        else if (management_controller_index > index) management_controller_index--;
        config_save_pending = true;
        ok = true;
    }
    critical_section_exit(&lock);
    return ok;
}

static bool address_equal(const uint8_t a[6], const uint8_t b[6]) {
    return memcmp(a, b, 6) == 0;
}

static bool address_is_zero(const uint8_t address[6]) {
    static const uint8_t zero[6] = {0};
    return address_equal(address, zero);
}

bool bb_blocked_device_contains(const uint8_t address[6]) {
    if (!address || address_is_zero(address)) return false;
    bool found = false;
    critical_section_enter_blocking(&lock);
    for (uint8_t i = 0; i < config_state.blocked_device_count; ++i) {
        if (address_equal(config_state.blocked_devices[i].bluetooth_address, address)) { found = true; break; }
    }
    critical_section_exit(&lock);
    return found;
}

bool bb_blocked_device_add(const uint8_t address[6], const char* name) {
    if (!address || address_is_zero(address)) return false;
    bool ok = false;
    critical_section_enter_blocking(&lock);
    for (uint8_t i = 0; i < config_state.blocked_device_count; ++i) {
        if (address_equal(config_state.blocked_devices[i].bluetooth_address, address)) {
            if (name && name[0]) set_name(config_state.blocked_devices[i].name, sizeof(config_state.blocked_devices[i].name), name, "Bluetooth Device");
            config_save_pending = true;
            ok = true;
            break;
        }
    }
    if (!ok && config_state.blocked_device_count < BB_MAX_BLOCKED_DEVICES) {
        bb_blocked_device_t* entry = &config_state.blocked_devices[config_state.blocked_device_count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->bluetooth_address, address, 6);
        set_name(entry->name, sizeof(entry->name), name, "Bluetooth Device");
        config_save_pending = true;
        ok = true;
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_blocked_device_remove(const uint8_t address[6]) {
    if (!address) return false;
    bool ok = false;
    critical_section_enter_blocking(&lock);
    for (uint8_t i = 0; i < config_state.blocked_device_count; ++i) {
        if (!address_equal(config_state.blocked_devices[i].bluetooth_address, address)) continue;
        for (uint8_t j = i; j + 1 < config_state.blocked_device_count; ++j) config_state.blocked_devices[j] = config_state.blocked_devices[j + 1];
        memset(&config_state.blocked_devices[config_state.blocked_device_count - 1], 0, sizeof(config_state.blocked_devices[0]));
        config_state.blocked_device_count--;
        config_save_pending = true;
        ok = true;
        break;
    }
    critical_section_exit(&lock);
    return ok;
}

uint8_t bb_blocked_devices_get(bb_blocked_device_t* out, uint8_t max_count) {
    critical_section_enter_blocking(&lock);
    uint8_t count = config_state.blocked_device_count < max_count ? config_state.blocked_device_count : max_count;
    if (out && count) memcpy(out, config_state.blocked_devices, count * sizeof(*out));
    critical_section_exit(&lock);
    return count;
}

bool bb_controller_address(uint8_t index, uint8_t out[6]) {
    if (!out) return false;
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (index < config_state.controller_count) { memcpy(out, config_state.controllers[index].bluetooth_address, 6); ok = true; }
    critical_section_exit(&lock);
    return ok;
}

bool bb_controller_resolve_identity(const uint8_t address[6], const uint8_t identity[6]) {
    bool known = false;
    critical_section_enter_blocking(&lock);
    int transient = -1;
    for (uint8_t i = 0; i < config_state.controller_count; ++i) {
        if (address_equal(config_state.controllers[i].bluetooth_address, identity)) known = true;
        if (address_equal(config_state.controllers[i].bluetooth_address, address)) transient = i;
    }
    if (!known && transient >= 0) {
        memcpy(config_state.controllers[transient].bluetooth_address, identity, 6);
        config_save_pending = true;
        known = true;
    }
    critical_section_exit(&lock);
    return known;
}

bool bb_controller_activate_or_create(const uint8_t address[6], uint16_t vendor_id, uint16_t product_id, const char* name, uint8_t* out_index) {
    bool ok = false;
    uint8_t index = 0;
    critical_section_enter_blocking(&lock);
    int found = -1;
    for (uint8_t i = 0; i < config_state.controller_count; ++i) {
        if (address_equal(config_state.controllers[i].bluetooth_address, address)) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        index = (uint8_t)found;
        bb_controller_t* controller = &config_state.controllers[index];
        bool changed = false;
        if (!address_equal(controller->bluetooth_address, address)) {
            memcpy(controller->bluetooth_address, address, 6);
            changed = true;
        }
        if (vendor_id && controller->native_vendor_id != vendor_id) {
            controller->native_vendor_id = vendor_id;
            changed = true;
        }
        if (product_id && controller->native_product_id != product_id) {
            controller->native_product_id = product_id;
            changed = true;
        }
        if (name && name[0] && strcmp(controller->name, name) != 0) {
            set_name(controller->name, sizeof(controller->name), name, "Bluetooth Controller");
            changed = true;
        }
        if (!boot_multi && config_state.active_controller != index) {
            config_state.active_controller = index;
            changed = true;
        }
        if (!boot_multi || management_controller_index < 0) management_controller_index = index;
        if (changed) config_save_pending = true;
        ok = true;
    } else if (config_state.controller_count < BB_MAX_CONTROLLERS) {
        index = config_state.controller_count++;
        bb_controller_t* controller = &config_state.controllers[index];
        memset(controller, 0, sizeof(*controller));
        controller->id = config_state.next_controller_id++;
        memcpy(controller->bluetooth_address, address, 6);
        controller->native_vendor_id = vendor_id;
        controller->native_product_id = product_id;
        set_name(controller->name, sizeof(controller->name), name, "Bluetooth Controller");
        memset(config_state.controller_names[index], 0, BB_MAX_NAME);
        controller->profile_count = 1;
        controller->active_profile = 0;
        uint16_t identity = allocate_mister_identity_locked();
        controller->mister_identity = identity;
        init_profile(&controller->profiles[0], config_state.next_profile_id++, identity, "Default");
        controller->profiles[0].mister_mapping_mode = 0;
        if (!boot_multi) config_state.active_controller = index;
        if (!boot_multi || management_controller_index < 0) management_controller_index = index;
        config_save_pending = true;
        ok = true;
    }
    critical_section_exit(&lock);
    if (ok && out_index) *out_index = index;
    return ok;
}

bool bb_controller_update_identity(uint8_t index, uint16_t vendor_id, uint16_t product_id, const char* name) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (index < config_state.controller_count) {
        bb_controller_t* controller = &config_state.controllers[index];
        controller->native_vendor_id = vendor_id;
        controller->native_product_id = product_id;
        set_name(controller->name, sizeof(controller->name), name, "Bluetooth Controller");
        config_save_pending = true;
        ok = true;
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_controller_select(uint8_t index) {
    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (index < config_state.controller_count) {
        if (boot_multi && management_controller_index != index) {
            for (unsigned slot = 0; slot < BB_MAX_SLOTS; ++slot) {
                runtime_slots[slot].capture_mode = BB_CAPTURE_NONE;
                runtime_slots[slot].capture_ready = false;
                runtime_slots[slot].captured_button = 0xff;
                capture_masks[slot] = 0;
                capture_previous[slot] = runtime_slots[slot].raw_buttons;
            }
        }
        management_controller_index = index;
        ok = true;
    }
    critical_section_exit(&lock);
    return ok;
}

bool bb_output_mode_set(bb_output_mode_t mode) {
    if (mode < BB_OUTPUT_MISTER || mode > BB_OUTPUT_LAST || (boot_multi && mode != BB_OUTPUT_MISTER)) return false;
    critical_section_enter_blocking(&lock);
    config_state.output_mode = (uint8_t)mode;
    config_save_pending = true;
    critical_section_exit(&lock);
    return true;
}

uint16_t bb_current_mister_identity(void) {
    if (boot_multi) return 0;
    uint16_t identity = 1;
    critical_section_enter_blocking(&lock);
    if (config_state.controller_count && input_controller_index_locked() < config_state.controller_count) {
        bb_controller_t* controller = &config_state.controllers[input_controller_index_locked()];
        if (controller->profile_count && controller->active_profile < controller->profile_count) {
            bb_profile_t* profile = &controller->profiles[controller->active_profile];
            if (profile->mister_identity) identity = profile->mister_identity;
        }
    }
    critical_section_exit(&lock);
    return identity;
}

bb_output_mode_t bb_output_mode(void) {
    return (bb_output_mode_t)bb_output_mode_value();
}

const char* bb_output_mode_name(bb_output_mode_t mode) {
    switch (mode) {
        case BB_OUTPUT_MISTER: return "MiSTer";
        case BB_OUTPUT_XINPUT: return "X-Input";
        case BB_OUTPUT_GENERIC_HID: return "Generic HID";
        case BB_OUTPUT_SWITCH: return "Nintendo Switch";
        case BB_OUTPUT_XBOX360: return "Xbox 360 (Console)";
        case BB_OUTPUT_DUALSENSE: return "DualSense";
        default: return "MiSTer";
    }
}

bool bb_config_import(const bb_config_t* cfg, uint32_t checksum) {
    if (!cfg || (cfg->schema != BB_CONFIG_SCHEMA && cfg->schema != 8 && cfg->schema != 9 && cfg->schema != 10 && cfg->schema != 11 && cfg->schema != 12) || bb_config_checksum(cfg) != checksum) return false;
    if (cfg->controller_count > BB_MAX_CONTROLLERS) return false;
    if (cfg->controller_count && cfg->active_controller >= cfg->controller_count) return false;
    for (uint8_t i = 0; i < cfg->controller_count; ++i) {
        if (cfg->controllers[i].profile_count == 0 || cfg->controllers[i].profile_count > BB_MAX_PROFILES) return false;
        if (cfg->controllers[i].active_profile >= cfg->controllers[i].profile_count) return false;
    }
    bb_config_t value = *cfg;
    value.schema = BB_CONFIG_SCHEMA;
    if (cfg->schema < 11) memset(value.adapter_name, 0, sizeof(value.adapter_name));
    if (cfg->schema < 13) { value.blocked_device_count = 0; memset(value.blocked_devices, 0, sizeof(value.blocked_devices)); }
    value.adapter_name[BB_MAX_NAME - 1] = 0;
    char clean_adapter_name[BB_MAX_NAME] = {0};
    set_name(clean_adapter_name, sizeof(clean_adapter_name), value.adapter_name, "");
    memcpy(value.adapter_name, clean_adapter_name, sizeof(clean_adapter_name));
    for (uint8_t i = 0; i < BB_MAX_CONTROLLERS; ++i) {
        value.controller_names[i][BB_MAX_NAME - 1] = 0;
        char clean[BB_MAX_NAME] = {0};
        set_name(clean, sizeof(clean), value.controller_names[i], "");
        memcpy(value.controller_names[i], clean, sizeof(clean));
    }
    if (value.output_mode < BB_OUTPUT_MISTER || value.output_mode > BB_OUTPUT_LAST) value.output_mode = BB_OUTPUT_MISTER;
    if (value.multi_controller > 1 || value.blocked_device_count > BB_MAX_BLOCKED_DEVICES) return false;
    if (value.multi_controller) value.output_mode = BB_OUTPUT_MISTER;
    if (value.multi_controller != boot_multi) mode_restart = true;
    bb_config_set(&value, true);
    return true;
}

bool bb_config_reset(void) {
    bb_config_t fresh;
    init_defaults(&fresh);
    if (boot_multi) mode_restart = true;
    bb_config_set(&fresh, true);
    return true;
}
