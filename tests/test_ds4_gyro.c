#include "ds4_gyro.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t report[78]={0x11};
static bb_ds4_gyro_t gyro;
static bb_input_state_t input;
static bb_pointer_sample_t pointer;
static uint16_t stamp;
static uint32_t now;
static void put16(unsigned at,int32_t v){report[at]=(uint8_t)v;report[at+1]=(uint8_t)(v>>8);}
static void step(bb_ds4_aim_t aim,int pitch,int yaw,bool touch) {
    put16(12,++stamp);put16(15,pitch);put16(17,yaw);
    pointer=(bb_pointer_sample_t){0};now+=10;
    bb_ds4_gyro_apply(&gyro,report,now,aim,touch,&input,&pointer);
}
static void reset(void){memset(&gyro,0,sizeof(gyro));memset(&input,0,sizeof(input));now=0;stamp=0;}
int main(void) {
    bb_ds4_aim_t a=bb_ds4_aim_defaults();assert(!a.enabled&&a.activation==1&&a.pause_button==10&&a.flags==BB_AIM_INVERT_X);
    step(a,1640,1640,true);step(a,1640,1640,true);assert(!pointer.x&&!pointer.y);
    a.enabled=1;a.activation=0;a.pause_button=255;a.flags=0;reset();
    step(a,1640,1640,false);assert(!pointer.x&&!pointer.y);
    step(a,1640,1640,false);assert(pointer.x==10&&pointer.y==-10);
    pointer=(bb_pointer_sample_t){0};bb_ds4_gyro_apply(&gyro,report,now+10,a,false,&input,&pointer);
    assert(!pointer.x&&!pointer.y); /* Duplicate sensor timestamp. */
    a.flags=BB_AIM_INVERT_X|BB_AIM_INVERT_Y;step(a,1640,1640,false);assert(pointer.x==-10&&pointer.y==10);
    a.flags=0;a.sensitivity=50;step(a,1640,1640,false);assert(pointer.x==5&&pointer.y==-5);
    a.sensitivity=200;step(a,1640,1640,false);assert(pointer.x==20&&pointer.y==-20);
    now+=100;step(a,1640,1640,false);assert(!pointer.x&&!pointer.y); /* No gap catch-up. */
    a.sensitivity=100;a.activation=1;
    step(a,1640,1640,false);assert(!pointer.x&&!pointer.y);
    step(a,1640,1640,true);assert(!pointer.x&&!pointer.y);
    step(a,1640,1640,true);assert(pointer.x==10);
    a.activation=2;step(a,1640,1640,true);assert(!pointer.x);
    input.lt=128;step(a,1640,1640,false);assert(!pointer.x);
    step(a,1640,1640,false);assert(pointer.x==10);
    a.pause_button=10;input.buttons=1u<<10;step(a,1640,1640,false);
    assert(!pointer.x&&!(input.buttons&(1u<<10)));
    a.flags=BB_AIM_TRIGGER_FIRE;input.buttons=1u<<7;input.rt=220;
    step(a,0,0,false);assert(pointer.buttons==1&&!input.rt&&!(input.buttons&(1u<<7)));
    input.buttons=0;input.rt=0;step(a,0,0,false);assert(!pointer.buttons);
    a.pause_button=7;input.buttons=1u<<7;input.rt=220;
    step(a,0,0,false);assert(!pointer.buttons&&!input.rt); /* Pause must not fire. */
    /* Hold pause still: estimate sensor bias, then remove it from aiming. */
    reset();a=bb_ds4_aim_defaults();a.enabled=1;a.activation=0;a.flags=0;
    for(unsigned i=0;i<60;i++){input.buttons=1u<<10;step(a,40,-30,false);assert(!pointer.x&&!pointer.y);}
    assert(gyro.bias_x==-30&&gyro.bias_y==40);
    input.buttons=0;step(a,40,-30,false);step(a,40,-30,false);assert(!pointer.x&&!pointer.y);
    step(a,1680,1610,false);assert(pointer.x==10&&pointer.y==-10);
    /* Touchpad movement and clicks survive gyro contribution. */
    put16(12,++stamp);now+=10;pointer=(bb_pointer_sample_t){3,4,1};
    bb_ds4_gyro_apply(&gyro,report,now,a,false,&input,&pointer);
    assert(pointer.x==13&&pointer.y==-6&&pointer.buttons==1);
    reset();a.activation=0;a.pause_button=255;
    step(a,0,16,false);int total=0;
    for(unsigned i=0;i<103;i++){step(a,0,16,false);total+=pointer.x;}
    assert(total==10); /* Retain slow fractional motion. */
    step(a,8,-8,false);assert(!pointer.x&&!pointer.y);
    now=0xfffffff0u;step(a,1640,1640,false);step(a,1640,1640,false);assert(pointer.x==10);
    a.pause_button=14;assert(!bb_ds4_aim_valid(a));
    puts("DS4 gyro: gating, axes, sensitivity, timestamps, gaps/wrap, bias zeroing, fractional motion, buttons and touchpad mixing passed");
}
