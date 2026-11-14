#include "lightbar.h"
#include <assert.h>
#include <stdio.h>

static uint32_t checksum(const uint8_t* p) {
    uint32_t crc=0xffffffffu;
    for(int i=-1;i<74;i++) {
        crc^=i<0?0xa2:p[i];
        for(unsigned j=0;j<8;j++)crc=crc&1u?(crc>>1)^0xedb88320u:crc>>1;
    }
    return ~crc;
}
static bool eq(bb_rgb_t a,bb_rgb_t b){return a.r==b.r&&a.g==b.g&&a.b==b.b;}
int main(void) {
    bb_lightbar_config_t c=bb_lightbar_defaults();
    assert(c.theme==2&&bb_lightbar_valid(&c));
    assert(eq(bb_lightbar_resolve(&c,0,0),(bb_rgb_t){64,0,0}));
    assert(eq(bb_lightbar_resolve(&c,1,1),(bb_rgb_t){0,64,0}));
    c.controllers[0].player=4;assert(eq(bb_lightbar_resolve(&c,0,0),c.players[3]));
    c.controllers[0].custom_enabled=1;c.controllers[0].rgb=(bb_rgb_t){3,77,255};
    assert(eq(bb_lightbar_resolve(&c,0,1),(bb_rgb_t){3,77,255}));
    c.controllers[0].player=5;assert(!bb_lightbar_valid(&c));c.controllers[0].player=0;
    c.controllers[0].custom_enabled=2;assert(!bb_lightbar_valid(&c));c.controllers[0].custom_enabled=0;
    bb_rgb_t theme[2];assert(bb_lightbar_theme_colours(1,theme));
    c.players[0]=theme[0];c.players[1]=theme[1];assert(!bb_lightbar_valid(&c));
    c.theme=1;assert(bb_lightbar_valid(&c));
    assert(eq(c.players[0],(bb_rgb_t){0,0,64})&&eq(c.players[1],(bb_rgb_t){64,32,0}));
    c.players[0]=(bb_rgb_t){11,22,33};assert(!bb_lightbar_valid(&c));c.theme=0;assert(bb_lightbar_valid(&c));
    uint8_t p[78];
    bb_ds4_output_build(p,127,(bb_rgb_t){7,13,19},false,true);
    assert(p[0]==0x11&&p[1]==0xc4&&p[3]==7&&p[6]==127&&p[7]==127);
    assert(p[8]==7&&p[9]==13&&p[10]==19&&!p[11]&&!p[12]);
    uint32_t crc=(uint32_t)p[74]|((uint32_t)p[75]<<8)|((uint32_t)p[76]<<16)|((uint32_t)p[77]<<24);
    assert(crc==checksum(p));
    /* Stopping rumble must retain RGB, including all-off as a valid colour. */
    bb_ds4_output_build(p,0,(bb_rgb_t){7,13,19},false,true);assert(!p[6]&&!p[7]&&p[8]==7&&p[10]==19);
    bb_ds4_output_build(p,200,(bb_rgb_t){0},false,true);assert(p[6]==200&&!p[8]&&!p[9]&&!p[10]);
    assert(!bb_ds4_low_battery_blink(false,true,20,1));
    assert(bb_ds4_low_battery_blink(false,true,19,1));
    assert(bb_ds4_low_battery_blink(true,true,20,1));
    assert(bb_ds4_low_battery_blink(true,true,24,1));
    assert(!bb_ds4_low_battery_blink(true,true,25,1));
    assert(!bb_ds4_low_battery_blink(true,true,5,2)); /* Charging. */
    assert(!bb_ds4_low_battery_blink(true,true,5,3)); /* Full. */
    assert(!bb_ds4_low_battery_blink(true,false,0,0));
    assert(!bb_ds4_low_battery_blink(false,true,255,1));
    for(unsigned strength=0;strength<=255;strength+=85) {
        bb_ds4_output_build(p,(uint8_t)strength,(bb_rgb_t){7,13,19},true,true);
        assert(p[6]==strength&&p[7]==strength&&p[8]==7&&p[9]==13&&p[10]==19);
        assert(p[11]==100&&p[12]==100&&(p[3]&4));
        crc=(uint32_t)p[74]|((uint32_t)p[75]<<8)|((uint32_t)p[76]<<16)|((uint32_t)p[77]<<24);
        assert(crc==checksum(p));
    }
    bb_ds4_output_build(p,255,(bb_rgb_t){7,13,19},true,false);
    assert(p[3]==3&&p[6]==255&&p[11]==100&&p[12]==100); /* Retain blink phase. */
    bb_ds4_output_build(p,0,(bb_rgb_t){7,13,19},false,true);assert(!p[11]&&!p[12]&&p[8]==7&&(p[3]&4));
    puts("lightbar: themes, slot/preset/override, RGB+rumble+blink encoding, CRC, battery thresholds, hysteresis and charging/unknown suppression passed");
}
