#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool touching, have_timestamp;
    uint8_t contact, timestamp;
    int16_t x,y, remainder_x,remainder_y;
    uint32_t at;
} bb_ds4_pointer_t;
typedef struct { int16_t x,y; uint8_t buttons; } bb_pointer_sample_t;
/* Original implementation from the documented DS4 Bluetooth report layout.
 * Full 78-byte report 0x11 only; CRC and touch-record bounds are validated. */
bool bb_ds4_pointer_decode(bb_ds4_pointer_t* state, const uint8_t* p, uint16_t len,
                           uint32_t now, bb_pointer_sample_t* out);
typedef struct { int32_t x,y; uint32_t sequence; uint8_t buttons; } bb_mouse_event_t;
typedef struct {
    bb_mouse_event_t events[8];
    uint32_t sequence, overflows;
    uint8_t head,count,last_buttons;
} bb_mouse_queue_t;
/* Caller serializes queue operations. Relative movement is consumed only on ack. */
void bb_mouse_queue_push(bb_mouse_queue_t* q, bb_pointer_sample_t sample);
bool bb_mouse_queue_peek(const bb_mouse_queue_t* q, bb_pointer_sample_t* out, uint32_t* token);
void bb_mouse_queue_ack(bb_mouse_queue_t* q, uint32_t token, bb_pointer_sample_t sent);
void bb_mouse_queue_release(bb_mouse_queue_t* q);

void bb_mouse_queue_expire_motion(bb_mouse_queue_t* q);
