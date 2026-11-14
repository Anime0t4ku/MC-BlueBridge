#include "bluebridge_state.h"
#include "config_store.h"
#include "hardware/flash.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint8_t test_flash[PICO_FLASH_SIZE_BYTES];int test_flash_error;unsigned test_flash_erases,test_core;uint32_t test_time;
static bb_config_t cfg;
static uint32_t hash(const void* p,size_t n){const uint8_t* q=p;uint32_t h=2166136261u;while(n--){h^=*q++;h*=16777619u;}return h;}
static bool eq(bb_rgb_t a,bb_rgb_t b){return a.r==b.r&&a.g==b.g&&a.b==b.b;}
int main(void) {
    memset(test_flash,255,sizeof(test_flash));bb_state_init();
    bb_lightbar_config_t light;bb_lightbar_get(&light);assert(light.theme==2);
    assert(eq(bb_lightbar_colour(255,0),(bb_rgb_t){64,0,0}));
    const uint8_t a[6]={11,2,3,4,5,6},b[6]={12,2,3,4,5,6},c[6]={13,2,3,4,5,6};
    uint8_t ia,ib,ic;uint32_t ida,idb;bb_lightbar_choice_t choice;
    assert(bb_controller_activate_or_create(a,0x054c,0x09cc,"DualShock 4",&ia));
    assert(bb_controller_activate_or_create(b,0x054c,0x09cc,"DualShock 4",&ib));
    assert(bb_lightbar_controller_get(ia,&ida,&choice));assert(!choice.player&&!choice.custom_enabled);
    assert(bb_lightbar_controller_get(ib,&idb,&choice));
    assert(eq(bb_lightbar_colour(ia,0),light.players[0])&&eq(bb_lightbar_colour(ib,1),light.players[1]));
    choice=(bb_lightbar_choice_t){4,1,{7,8,9}};
    assert(!bb_lightbar_controller_set(ia,ida+999,choice));
    assert(bb_lightbar_controller_set(ib,idb,choice));assert(eq(bb_lightbar_colour(ib,1),choice.rgb));
    choice.player=5;assert(!bb_lightbar_controller_set(ib,idb,choice));
    bb_rgb_t palette[4];memcpy(palette,light.players,sizeof(palette));
    assert(bb_lightbar_theme_colours(1,palette));assert(bb_lightbar_palette_set(1,palette));
    assert(!bb_lightbar_palette_set(2,palette));
    assert(!bb_mode_restart_pending());
    test_time+=250;bb_config_flush_pending();bb_state_init();
    bb_lightbar_get(&light);assert(light.theme==1&&eq(light.controllers[ib].rgb,(bb_rgb_t){7,8,9}));
    assert(bb_controller_forget(ia,NULL));assert(bb_lightbar_controller_get(0,&ida,&choice));
    assert(ida==idb&&choice.custom_enabled&&eq(choice.rgb,(bb_rgb_t){7,8,9}));
    assert(bb_controller_activate_or_create(c,0x054c,0x09cc,"DualShock 4",&ic));
    assert(bb_lightbar_controller_get(ic,&ida,&choice)&&!choice.custom_enabled&&!choice.player);
    /* A genuine upstream schema-13 image has no fork-only fields. */
    bb_config_get(&cfg);cfg.schema=13;
    uint8_t original[BB_CONFIG_V13_SIZE];memcpy(original,&cfg,sizeof(original));
    uint32_t header[]={0x42424733u,BB_CONFIG_V13_SIZE,hash(&cfg,BB_CONFIG_V13_SIZE)};
    unsigned base=PICO_FLASH_SIZE_BYTES-16*FLASH_SECTOR_SIZE;
    memcpy(test_flash+base,header,sizeof(header));memcpy(test_flash+base+sizeof(header),&cfg,BB_CONFIG_V13_SIZE);
    bb_state_init();bb_lightbar_get(&light);
    assert(light.theme==2&&!bb_touch_mouse_get()&&!bb_ds4_aim_get().enabled);
    bb_config_get(&cfg);assert(cfg.controller_count==2&&cfg.schema==14);
    assert(!memcmp((uint8_t*)&cfg+4,original+4,sizeof(original)-4));
    cfg.lightbar.controllers[0].player=5;assert(!bb_config_import(&cfg,bb_config_checksum(&cfg)));
    cfg.schema=13;assert(bb_config_import(&cfg,bb_config_checksum(&cfg)));
    bb_config_get(&cfg);cfg.touch_mouse_enabled=1;cfg.multi_controller=2;
    assert(!bb_config_import(&cfg,bb_config_checksum(&cfg))&&!bb_mode_restart_pending());
    assert(bb_ds4_pointer_settings_set((bb_ds4_aim_t){1,145,2,3,10},true));
    assert(bb_mode_restart_pending());bb_config_flush_pending();bb_state_init();
    assert(bb_touch_mouse_current()&&bb_ds4_aim_current().sensitivity==145);
    bb_config_get(&cfg);cfg.ds4_aim.sensitivity=0;assert(!bb_config_import(&cfg,bb_config_checksum(&cfg)));
    assert(bb_config_reset()&&bb_mode_restart_pending());bb_config_flush_pending();bb_state_init();
    assert(!bb_touch_mouse_current()&&!bb_ds4_aim_current().enabled);
    puts("DS4 state: themes, overrides, persistence, record reuse, schema-13 migration, import validation and reset passed");
}
