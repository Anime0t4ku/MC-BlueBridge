#pragma once
#include <stdbool.h>
#include <stdint.h>
void bb_feedback_reset(void);
void bb_feedback_poll(uint32_t now);
void bb_feedback_set(uint8_t report_id, bool feature, const uint8_t* data, uint16_t length);
uint16_t bb_feedback_get(uint8_t report_id, bool feature, uint8_t* data, uint16_t length);
void bb_feedback_stop(void);
