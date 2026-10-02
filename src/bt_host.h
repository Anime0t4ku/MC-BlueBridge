#pragma once
#include <stdbool.h>
#include <stdint.h>
void bb_bt_host_init(void);
void bb_bt_host_start_pairing(void);
void bb_bt_host_stop_pairing(void);
void bb_bt_host_forget_all(void);
bool bb_bt_host_forget_device(const uint8_t address[6]);
bool bb_bt_host_disconnect_current(void);
bool bb_bt_host_rumble_test(uint8_t strength, uint16_t duration_ms);

void bb_bt_host_set_output_rumble(uint8_t left, uint8_t right);
