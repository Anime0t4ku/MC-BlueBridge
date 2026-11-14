#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BB_LIGHTBAR_PLAYERS 4
#define BB_LIGHTBAR_CONTROLLERS 8
typedef struct { uint8_t r,g,b; } bb_rgb_t;
typedef struct { uint8_t player,custom_enabled;bb_rgb_t rgb; } bb_lightbar_choice_t;
typedef struct {
    bb_rgb_t players[BB_LIGHTBAR_PLAYERS];
    bb_lightbar_choice_t controllers[BB_LIGHTBAR_CONTROLLERS];
    uint8_t theme; /* 0 custom, 1 Sega, 2 Nintendo. Themes define P1/P2. */
} bb_lightbar_config_t;

static inline bool bb_lightbar_theme_colours(uint8_t theme,bb_rgb_t out[2]) {
    if(theme==1){out[0]=(bb_rgb_t){0,0,64};out[1]=(bb_rgb_t){64,32,0};return true;}
    if(theme==2){out[0]=(bb_rgb_t){64,0,0};out[1]=(bb_rgb_t){0,64,0};return true;}
    return false;
}
static inline bb_lightbar_config_t bb_lightbar_defaults(void) {
    bb_lightbar_config_t c={.players={{64,0,0},{0,64,0},{0,64,0},{32,0,32}},.theme=2};
    for(unsigned i=0;i<BB_LIGHTBAR_CONTROLLERS;i++)c.controllers[i].rgb=c.players[0];
    return c;
}
static inline bool bb_lightbar_valid(const bb_lightbar_config_t* c) {
    if(c->theme>2)return false;
    bb_rgb_t theme[2];
    if(c->theme && (!bb_lightbar_theme_colours(c->theme,theme)||memcmp(c->players,theme,sizeof(theme))))return false;
    for(unsigned i=0;i<BB_LIGHTBAR_CONTROLLERS;i++)
        if(c->controllers[i].player>4 || c->controllers[i].custom_enabled>1)return false;
    return true;
}
static inline bb_rgb_t bb_lightbar_resolve(const bb_lightbar_config_t* c,unsigned index,unsigned slot) {
    unsigned player=slot<BB_LIGHTBAR_PLAYERS?slot:0;
    if(index<BB_LIGHTBAR_CONTROLLERS) {
        const bb_lightbar_choice_t* choice=&c->controllers[index];
        if(choice->custom_enabled)return choice->rgb;
        if(choice->player>=1&&choice->player<=4)player=choice->player-1u;
    }
    return c->players[player];
}
/* Full DS4 Bluetooth report, retaining the existing 4 ms polling setup.
 * Rumble and RGB are always sent together to avoid one cancelling the other. */
/* Start below 20%, recover at 25%; suppress for charging/full/unknown data. */
static inline bool bb_ds4_low_battery_blink(bool previous,bool supported,uint8_t percent,uint8_t state) {
    return supported && state==1 && percent<(previous?25u:20u);
}
void bb_ds4_output_build(uint8_t report[78],uint8_t strength,bb_rgb_t colour,bool low_battery,bool update_blink);
