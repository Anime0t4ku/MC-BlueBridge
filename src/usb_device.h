#pragma once
#include <stdbool.h>
#include "bluebridge_state.h"

void bb_usb_core1(void);
void bb_usb_submit(const bb_gamepad_report_t* report);
bool bb_usb_ready(void);
void bb_usb_request_reenumeration(void);
