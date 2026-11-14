#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Global, boot-applied DS4 gyro mouse settings. */
typedef struct __attribute__((packed)) {
    uint8_t enabled, sensitivity, activation, flags, pause_button;
} bb_ds4_aim_t;
#define BB_AIM_INVERT_X 1u
#define BB_AIM_INVERT_Y 2u
#define BB_AIM_TRIGGER_FIRE 4u
static inline bb_ds4_aim_t bb_ds4_aim_defaults(void) {
    return (bb_ds4_aim_t){0,100,1,BB_AIM_INVERT_X,10};
}
static inline bool bb_ds4_aim_valid(bb_ds4_aim_t a) {
    return a.enabled<=1 && a.sensitivity>=1 && a.sensitivity<=200 && a.activation<=2 &&
        !(a.flags&~7u) && (a.pause_button==255 || a.pause_button<=13);
}
