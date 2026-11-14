#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
void bb_bt_host_init(void);
bool bb_bt_host_pairing_available(void);
void bb_bt_host_start_pairing(void);
void bb_bt_host_stop_pairing(void);
void bb_bt_host_forget_all(void);
bool bb_bt_host_forget_device(const uint8_t address[6]);
bool bb_bt_host_disconnect_current(void);
bool bb_bt_host_rumble_test(uint8_t strength, uint16_t duration_ms);

void bb_bt_host_set_output_rumble(uint8_t left, uint8_t right);

bool bb_bt_host_diagnostics(char* out, size_t size);
bool bb_bt_host_diagnostics_slot(uint8_t slot, char* out, size_t size);

void bb_bt_host_set_slot_rumble(uint8_t slot, uint8_t left, uint8_t right);
void bb_bt_host_set_slot_rumble_motors(uint8_t slot, uint8_t strength, uint8_t big, uint8_t small);
void bb_bt_host_set_ds3_big_gain(uint16_t percent);
uint16_t bb_bt_host_ds3_big_gain(void);
void bb_bt_host_local_address(char* out, size_t size);
