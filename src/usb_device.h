#pragma once
#include <stdbool.h>
#include "touch_mouse.h"
#include "bluebridge_state.h"

bool bb_usb_touch_mouse_enabled(void);
void bb_usb_touch_mouse_submit(bb_pointer_sample_t sample,bool event_driven);
void bb_usb_touch_mouse_release(void);
void bb_usb_core1(void);
void bb_usb_submit(const bb_gamepad_report_t* report);
void bb_usb_set_press_latch(bool enabled);
bool bb_usb_ready(void);
void bb_usb_request_reenumeration(void);
void bb_usb_service_pending(void);
