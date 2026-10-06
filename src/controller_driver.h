#pragma once
#include "controller_input.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void bb_driver_reset(void);
void bb_driver_set_identity(const bb_controller_identity_t* identity);
void bb_driver_set_hid_descriptor(const uint8_t* descriptor, uint16_t length);
bool bb_driver_handle_xbox_ble_report(const uint8_t* report, uint16_t length);
bool bb_driver_handle_hid_report(const uint8_t* report, uint16_t length);
bool bb_driver_handle_nintendo_ble_report(const uint8_t* report, uint16_t length, bool gamecube, bool switch2_pro);
const bb_controller_identity_t* bb_driver_identity(void);
bool bb_driver_refine_identity_from_hid_descriptor(const uint8_t* descriptor, uint16_t length, bb_controller_identity_t* value);

bool bb_driver_set_n64_calibration(const uint8_t* data, uint16_t length);

int16_t bb_driver_battery_report_id(void);
bool bb_driver_handle_battery_report(const uint8_t* report, uint16_t length);
uint8_t bb_driver_battery_report_type(void);
bool bb_driver_hid_diagnostics(uint16_t offset, char* out, size_t size);
void bb_driver_record_battery_response(uint8_t status, const uint8_t* report, uint16_t length);
void bb_driver_record_battery_request(uint8_t status);
