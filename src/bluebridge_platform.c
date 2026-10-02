#include "bluebridge_platform.h"
#include "bt_host.h"

void bb_platform_init(void) {
    bb_bt_host_init();
}

void bb_platform_start_pairing(void) {
    bb_bt_host_start_pairing();
}

void bb_platform_stop_pairing(void) {
    bb_bt_host_stop_pairing();
}

void bb_platform_forget_all(void) {
    bb_bt_host_forget_all();
}

bool bb_platform_forget_controller(uint8_t index) {
    uint8_t address[6];
    if (!bb_controller_forget(index, address)) return false;
    bb_bt_host_forget_device(address);
    return true;
}

bool bb_platform_disconnect_controller(void) {
    return bb_bt_host_disconnect_current();
}
