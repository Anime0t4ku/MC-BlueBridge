#pragma once
#include <stdbool.h>
#include <stdint.h>
void bb_platform_init(void);
void bb_platform_start_pairing(void);
void bb_platform_stop_pairing(void);
void bb_platform_forget_all(void);
bool bb_platform_forget_controller(uint8_t index);
bool bb_platform_disconnect_controller(void);
