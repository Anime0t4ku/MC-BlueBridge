#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bluebridge_state.h"

#define BB_DS5_INPUT_SIZE 63

// Builds the 63-byte body of input report 0x01 (without the report ID).
void bb_ds5_build_input(const bb_gamepad_report_t* r, uint8_t out[BB_DS5_INPUT_SIZE]);
// Host output report (rumble, lightbar, player LEDs). report_id 0 means the ID
// is the first byte of the buffer (interrupt OUT endpoint).
void bb_ds5_handle_output(uint8_t report_id, const uint8_t* buffer, uint16_t length);
// Host GET_REPORT for a feature report; returns the number of bytes written.
uint16_t bb_ds5_get_feature(uint8_t report_id, uint8_t* buffer, uint16_t length);
void bb_ds5_task(void);
void bb_ds5_reset(void);
