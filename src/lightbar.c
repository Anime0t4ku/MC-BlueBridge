#include "lightbar.h"
static uint32_t byte_crc(uint32_t crc,uint8_t byte) {
    crc^=byte;
    for(unsigned i=0;i<8;i++)crc=(crc>>1)^((crc&1u)?0xedb88320u:0);
    return crc;
}
void bb_ds4_output_build(uint8_t p[78],uint8_t strength,bb_rgb_t colour,bool low_battery,bool update_blink) {
    memset(p,0,78);p[0]=0x11;p[1]=0xc4;p[3]=update_blink?0x07:0x03;
    p[6]=strength;p[7]=strength;p[8]=colour.r;p[9]=colour.g;p[10]=colour.b;
    /* DS4 hardware timing is in 10 ms units: 1 s on, 1 s off. Sending
     * zero for both durations restores a solid light. Only update blink timing
     * when it changes, so repeated rumble reports cannot restart its phase. */
    p[11]=low_battery?100:0;p[12]=low_battery?100:0;
    uint32_t crc=byte_crc(0xffffffffu,0xa2);
    for(unsigned i=0;i<74;i++)crc=byte_crc(crc,p[i]);
    crc=~crc;
    for(unsigned i=0;i<4;i++)p[74+i]=(uint8_t)(crc>>(8*i));
}
