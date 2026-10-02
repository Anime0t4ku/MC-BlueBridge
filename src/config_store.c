#include "config_store.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <string.h>

#define BB_CFG_MAGIC 0x42424733u
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif
#define BB_CFG_OFFSET (PICO_FLASH_SIZE_BYTES - (16 * FLASH_SECTOR_SIZE))
#define BB_CFG_STORAGE_SIZE (4 * FLASH_SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    uint8_t enabled;
    uint8_t reserved;
    uint16_t output_mask;
    char name[BB_MAX_MACRO_NAME];
} legacy_macro_v7_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    char name[BB_MAX_PROFILE_NAME];
    uint8_t button_map[16];
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
    legacy_macro_v7_t macros[BB_MACRO_COUNT];
    uint8_t mister_mapping_mode;
    uint16_t mister_identity;
} legacy_profile_v7_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint8_t bluetooth_address[6];
    char name[BB_MAX_NAME];
    uint16_t native_vendor_id;
    uint16_t native_product_id;
    uint8_t profile_count;
    uint8_t active_profile;
    uint16_t mister_identity;
    legacy_profile_v7_t profiles[BB_MAX_PROFILES];
} legacy_controller_v7_t;

typedef struct __attribute__((packed)) {
    uint32_t schema;
    uint8_t output_mode;
    uint8_t controller_count;
    uint8_t active_controller;
    uint32_t next_controller_id;
    uint32_t next_profile_id;
    uint16_t next_mister_identity;
    legacy_controller_v7_t controllers[BB_MAX_CONTROLLERS];
} legacy_config_v7_t;

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t checksum;
} stored_header_t;

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t checksum;
    bb_config_t cfg;
} stored_cfg_t;

typedef struct {
    uint8_t* sector;
} flash_write_params_t;

static uint32_t checksum_bytes(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    while (size--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static void migrate_v7(const legacy_config_v7_t* old, bb_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->schema = BB_CONFIG_SCHEMA;
    cfg->output_mode = old->output_mode;
    cfg->controller_count = old->controller_count > BB_MAX_CONTROLLERS ? BB_MAX_CONTROLLERS : old->controller_count;
    cfg->active_controller = old->active_controller;
    cfg->next_controller_id = old->next_controller_id;
    cfg->next_profile_id = old->next_profile_id;
    cfg->next_mister_identity = old->next_mister_identity;
    for (uint8_t i = 0; i < cfg->controller_count; ++i) {
        const legacy_controller_v7_t* src = &old->controllers[i];
        bb_controller_t* dst = &cfg->controllers[i];
        dst->id = src->id;
        memcpy(dst->bluetooth_address, src->bluetooth_address, sizeof(dst->bluetooth_address));
        memcpy(dst->name, src->name, sizeof(dst->name));
        dst->native_vendor_id = src->native_vendor_id;
        dst->native_product_id = src->native_product_id;
        dst->profile_count = src->profile_count > BB_MAX_PROFILES ? BB_MAX_PROFILES : src->profile_count;
        dst->active_profile = src->active_profile < dst->profile_count ? src->active_profile : 0;
        dst->mister_identity = src->mister_identity;
        for (uint8_t j = 0; j < dst->profile_count; ++j) {
            const legacy_profile_v7_t* sp = &src->profiles[j];
            bb_profile_t* dp = &dst->profiles[j];
            memset(dp, 0, sizeof(*dp));
            dp->id = sp->id;
            memcpy(dp->name, sp->name, sizeof(dp->name));
            for (uint8_t k = 0; k < 16; ++k) dp->button_map[k] = sp->button_map[k];
            for (uint8_t k = 16; k < BB_BUTTON_COUNT; ++k) dp->button_map[k] = BB_MAP_DISABLED;
            dp->invert_x = sp->invert_x;
            dp->invert_y = sp->invert_y;
            dp->invert_rx = sp->invert_rx;
            dp->invert_ry = sp->invert_ry;
            dp->deadzone_left = sp->deadzone_left;
            dp->deadzone_right = sp->deadzone_right;
            dp->trigger_deadzone = sp->trigger_deadzone;
            dp->turbo_rate_hz = sp->turbo_rate_hz;
            dp->turbo_modifier = sp->turbo_modifier;
            dp->turbo_enabled = sp->turbo_enabled;
            dp->turbo_control_mode = sp->turbo_control_mode;
            dp->turbo_control_button = sp->turbo_control_button;
            dp->turbo_mask = sp->turbo_mask;
            memcpy(dp->macros, sp->macros, sizeof(dp->macros));
            dp->mister_mapping_mode = sp->mister_mapping_mode;
            dp->mister_identity = sp->mister_identity;
        }
    }
}

static void __not_in_flash_func(write_config_flash)(void* param) {
    flash_write_params_t* params = (flash_write_params_t*)param;
    flash_range_erase(BB_CFG_OFFSET, BB_CFG_STORAGE_SIZE);
    flash_range_program(BB_CFG_OFFSET, params->sector, BB_CFG_STORAGE_SIZE);
}

bool bb_config_load(bb_config_t* cfg) {
    const uint8_t* base = (const uint8_t*)(XIP_BASE + BB_CFG_OFFSET);
    const stored_header_t* header = (const stored_header_t*)base;
    if (header->magic != BB_CFG_MAGIC) return false;
    const uint8_t* payload = base + sizeof(stored_header_t);
    if (header->size == sizeof(bb_config_t)) {
        const bb_config_t* stored = (const bb_config_t*)payload;
        if (header->checksum != checksum_bytes(stored, sizeof(*stored))) return false;
        *cfg = *stored;
        return true;
    }
    if (header->size == sizeof(legacy_config_v7_t)) {
        const legacy_config_v7_t* stored = (const legacy_config_v7_t*)payload;
        if (stored->schema != 7 || header->checksum != checksum_bytes(stored, sizeof(*stored))) return false;
        migrate_v7(stored, cfg);
        return true;
    }
    return false;
}

bool bb_config_save(const bb_config_t* cfg) {
    if (sizeof(stored_cfg_t) > BB_CFG_STORAGE_SIZE) return false;
    static uint8_t sector[BB_CFG_STORAGE_SIZE];
    memset(sector, 0xff, sizeof(sector));
    stored_cfg_t* stored = (stored_cfg_t*)sector;
    stored->magic = BB_CFG_MAGIC;
    stored->size = sizeof(bb_config_t);
    stored->cfg = *cfg;
    stored->checksum = bb_config_checksum(&stored->cfg);
    flash_write_params_t params = { .sector = sector };
    return flash_safe_execute(write_config_flash, &params, 1000) == PICO_OK;
}
