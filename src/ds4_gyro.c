#include "ds4_gyro.h"
#include <string.h>

static int16_t s16(const uint8_t* p) {
    uint16_t v=(uint16_t)(p[0]|((uint16_t)p[1]<<8));
    return (int16_t)(v<=32767u?(int32_t)v:(int32_t)v-65536);
}
static int32_t abs32(int32_t v){return v<0?-v:v;}
static int16_t bounded(int32_t v){return (int16_t)(v>32767?32767:v< -32767?-32767:v);}

void bb_ds4_gyro_apply(bb_ds4_gyro_t* s,const uint8_t* p,uint32_t now,
    bb_ds4_aim_t aim,bool touching,bb_input_state_t* input,bb_pointer_sample_t* pointer) {
    if(!aim.enabled||!bb_ds4_aim_valid(aim)){memset(s,0,sizeof(*s));return;}
    bool paused=aim.pause_button<32 && (input->buttons&(1u<<aim.pause_button));
    if(aim.pause_button<32)input->buttons&=~(1u<<aim.pause_button);
    if(paused && aim.pause_button==13)pointer->buttons&=~1u;
    bool active=!paused && (aim.activation==0 || (aim.activation==1&&touching) ||
        (aim.activation==2&&(input->lt>=128 || (input->buttons&(1u<<6)))));
    if(aim.flags&BB_AIM_TRIGGER_FIRE) {
        if(!paused && (input->rt>=128 || (input->buttons&(1u<<7))))pointer->buttons|=1;
        input->rt=0;input->buttons&=~(1u<<7);
    }
    uint16_t timestamp=(uint16_t)(p[12]|((uint16_t)p[13]<<8));
    uint32_t dt=now-s->at;
    if(!active){s->active=false;s->remainder_x=s->remainder_y=0;}
    bool gap=!s->have_timestamp || dt>50u;
    if(s->have_timestamp && !gap && (int16_t)(timestamp-s->timestamp)<=0)return;
    s->have_timestamp=true;s->timestamp=timestamp;s->at=now;
    int32_t pitch=s16(p+15),yaw=s16(p+17);
    /* Hold the selected pause button with the controller still for >=0.5s
     * and >=32 fresh samples to estimate resting bias. Movement/gaps restart
     * collection. This deliberately never adapts bias during active aiming. */
    if(paused) {
        if(gap || !s->collecting || abs32(pitch-s->first_y)>16 || abs32(yaw-s->first_x)>16) {
            s->collecting=true;s->zero_at=now;s->zero_samples=0;
            s->zero_x=s->zero_y=0;s->first_x=(int16_t)yaw;s->first_y=(int16_t)pitch;
        }
        if(s->zero_samples<1024) {
            s->zero_x+=yaw;s->zero_y+=pitch;++s->zero_samples;
            if(s->zero_samples>=32 && (uint32_t)(now-s->zero_at)>=500u) {
                s->bias_x=(int16_t)(s->zero_x/s->zero_samples);
                s->bias_y=(int16_t)(s->zero_y/s->zero_samples);
            }
        }
    } else s->collecting=false;
    if(active && s->active && !gap && dt) {
        yaw-=s->bias_x;pitch-=s->bias_y;
        if(abs32(yaw)<=8)yaw=0;
        if(abs32(pitch)<=8)pitch=0;
        if(aim.flags&BB_AIM_INVERT_X)yaw=-yaw;
        if(aim.flags&BB_AIM_INVERT_Y)pitch=-pitch;
        /* Nominal DS4 scale: 16.4 raw units/degree/s. At 100% use
         * 10 mouse counts/degree, retaining sub-count motion. Use int64
         * because bias subtraction can span the full signed sensor range. */
        int64_t x=(int64_t)yaw*dt*aim.sensitivity+s->remainder_x;
        int64_t y=-(int64_t)pitch*dt*aim.sensitivity+s->remainder_y;
        pointer->x=bounded((int32_t)pointer->x+(int32_t)(x/164000));
        pointer->y=bounded((int32_t)pointer->y+(int32_t)(y/164000));
        s->remainder_x=(int32_t)(x%164000);s->remainder_y=(int32_t)(y%164000);
    } else s->remainder_x=s->remainder_y=0;
    s->active=active;
}
