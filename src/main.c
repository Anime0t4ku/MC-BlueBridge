#include "bluebridge_state.h"
#include "bluebridge_platform.h"
#include "usb_device.h"
#include "led_status.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "btstack_run_loop.h"

int main(void) {
    bb_state_init();
    flash_safe_execute_core_init();
    multicore_launch_core1(bb_usb_core1);
    if (cyw43_arch_init()) return 1;
    bb_led_init();
    bb_platform_init();
    btstack_run_loop_execute();
    return 0;
}
