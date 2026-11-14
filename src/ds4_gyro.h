#pragma once
#include "touch_mouse.h"
#include "controller_input.h"
#include "ds4_aim.h"

typedef struct {
    bool active, have_timestamp, collecting;
    uint16_t timestamp, zero_samples;
    uint32_t at, zero_at;
    int32_t remainder_x, remainder_y;
    int32_t zero_x, zero_y;
    int16_t bias_x, bias_y, first_x, first_y;
} bb_ds4_gyro_t;

/* Caller must first validate the complete 78-byte DS4 BT report and CRC with
 * bb_ds4_pointer_decode. Never call this for partial or unverified reports. */
void bb_ds4_gyro_apply(bb_ds4_gyro_t* state, const uint8_t* report, uint32_t now,
    bb_ds4_aim_t aim, bool touching, bb_input_state_t* input,
    bb_pointer_sample_t* pointer);
