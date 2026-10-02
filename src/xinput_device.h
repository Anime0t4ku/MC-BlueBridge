#pragma once
#include <stdbool.h>
#include <stdint.h>
bool bb_xinput_ready(void);
bool bb_xinput_send(const uint8_t report[20]);
