#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "tusb.h"

void bb_xbox360_auth_init(void);
void bb_xbox360_auth_task(void);
bool bb_xbox360_auth_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request);
bool bb_xbox360_authenticated(void);
const char* bb_xbox360_security_serial(void);
