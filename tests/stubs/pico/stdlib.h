#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pico/platform.h"
#define PICO_OK 0
typedef unsigned int uint;
extern uint32_t test_time;
static inline uint32_t get_absolute_time(void) { return test_time; }
static inline uint32_t to_ms_since_boot(uint32_t time) { return time; }
