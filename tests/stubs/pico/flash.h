#pragma once
#include "pico/stdlib.h"
extern int test_flash_error;
static inline int flash_safe_execute(void (*fn)(void*), void* context, uint32_t timeout) {
    (void)timeout; if(test_flash_error)return test_flash_error; fn(context); return PICO_OK;
}
