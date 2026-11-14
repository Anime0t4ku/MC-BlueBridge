#include "touch_mouse.h"
#include <string.h>
static uint32_t crc_byte(uint32_t crc,uint8_t byte) {
    crc^=byte;
    for(unsigned i=0;i<8;i++)crc=(crc>>1)^((crc&1u)?0xedb88320u:0);
    return crc;
}
bool bb_ds4_pointer_decode(bb_ds4_pointer_t* s,const uint8_t* p,uint16_t len,
                           uint32_t now,bb_pointer_sample_t* out) {
    if(!p||len!=78||p[0]!=0x11||p[35]>4)return false;
    uint32_t crc=crc_byte(0xffffffffu,0xa1);
    for(unsigned i=0;i<74;i++)crc=crc_byte(crc,p[i]);
    uint32_t received=(uint32_t)p[74]|((uint32_t)p[75]<<8)|((uint32_t)p[76]<<16)|((uint32_t)p[77]<<24);
    if((~crc)!=received)return false;
    *out=(bb_pointer_sample_t){.buttons=(p[9]&2u)?1:0};
    if((uint32_t)(now-s->at)>500u)memset(s,0,sizeof(*s));
    s->at=now;
    for(unsigned i=0;i<p[35];i++) {
        const uint8_t* record=p+36+9*i;
        uint8_t timestamp=record[0];
        if(s->have_timestamp && (int8_t)(timestamp-s->timestamp)<=0)continue;
        s->have_timestamp=true;s->timestamp=timestamp;
        const uint8_t* point=record+1;
        if(point[0]&0x80u)point+=4;
        int16_t x=(int16_t)(point[1]|((point[2]&15u)<<8));
        int16_t y=(int16_t)((point[2]>>4)|(point[3]<<4));
        uint8_t id=point[0]&0x7fu;
        if((point[0]&0x80u)||x>=1920||y>=942) {
            s->touching=false;s->remainder_x=s->remainder_y=0;continue;
        }
        int dx=x-s->x,dy=y-s->y;
        if(s->touching && id==s->contact && dx>=-512&&dx<=512&&dy>=-512&&dy<=512) {
            int rx=dx+s->remainder_x,ry=dy+s->remainder_y;
            out->x+=(int16_t)(rx/4);out->y+=(int16_t)(ry/4);
            s->remainder_x=(int16_t)(rx%4);s->remainder_y=(int16_t)(ry%4);
        } else s->remainder_x=s->remainder_y=0;
        s->touching=true;s->contact=id;s->x=x;s->y=y;
    }
    return true;
}
static int32_t bounded(int32_t x){return x>32767?32767:x< -32767?-32767:x;}
void bb_mouse_queue_push(bb_mouse_queue_t* q,bb_pointer_sample_t s) {
    if(q->count) {
        bb_mouse_event_t* e=&q->events[(q->head+q->count-1u)%8];
        if(e->buttons==s.buttons){e->x=bounded(e->x+s.x);e->y=bounded(e->y+s.y);return;}
    } else if(!s.x&&!s.y&&s.buttons==q->last_buttons)return;
    if(q->count==8){q->head=(q->head+1u)%8;--q->count;++q->overflows;}
    unsigned tail=(q->head+q->count)%8;
    q->events[tail]=(bb_mouse_event_t){.x=s.x,.y=s.y,.buttons=s.buttons,.sequence=++q->sequence};
    ++q->count;
}
bool bb_mouse_queue_peek(const bb_mouse_queue_t* q,bb_pointer_sample_t* out,uint32_t* token) {
    if(!q->count)return false;
    const bb_mouse_event_t* e=&q->events[q->head];
    *out=(bb_pointer_sample_t){.x=(int16_t)(e->x>127?127:e->x< -127?-127:e->x),
        .y=(int16_t)(e->y>127?127:e->y< -127?-127:e->y),.buttons=e->buttons};
    *token=e->sequence;return true;
}
void bb_mouse_queue_ack(bb_mouse_queue_t* q,uint32_t token,bb_pointer_sample_t sent) {
    q->last_buttons=sent.buttons;
    if(!q->count||q->events[q->head].sequence!=token)return;
    bb_mouse_event_t* e=&q->events[q->head];e->x-=sent.x;e->y-=sent.y;
    if(!e->x&&!e->y){q->head=(q->head+1u)%8;--q->count;}
}
void bb_mouse_queue_release(bb_mouse_queue_t* q) {
    q->head=q->count=0;
    bb_mouse_queue_push(q,(bb_pointer_sample_t){0});
}

void bb_mouse_queue_expire_motion(bb_mouse_queue_t* q) {
    for(unsigned i=0;i<q->count;i++) {
        bb_mouse_event_t* e=&q->events[(q->head+i)%8];e->x=e->y=0;e->sequence=++q->sequence;
    }
}
