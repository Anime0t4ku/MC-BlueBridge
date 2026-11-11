#include "usb_descriptors.h"
#include "bluebridge_state.h"
#include "xbox360_auth.h"
#include "dualsense_descriptors.h"
#include "pico/unique_id.h"
#include <string.h>

static tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x2E8A,
    .idProduct = 0x10B1,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const* tud_descriptor_device_cb(void) {
    bb_output_mode_t mode = bb_output_mode();
    device_desc.bDeviceClass = TUSB_CLASS_MISC;
    device_desc.bDeviceSubClass = MISC_SUBCLASS_COMMON;
    device_desc.bDeviceProtocol = MISC_PROTOCOL_IAD;
    device_desc.idVendor = 0x2E8A;
    device_desc.idProduct = 0x10B1;
    device_desc.bcdDevice = 0x0100;
    if (bb_multi_mode()) {
        device_desc.idProduct = 0x10B3;
    } else if (mode == BB_OUTPUT_MISTER) {
        device_desc.idProduct = (uint16_t)(0xB000u | (bb_current_mister_identity() & 0x0fffu));
    } else if (mode == BB_OUTPUT_XINPUT) {
        device_desc.idVendor = 0x2E8A;
        device_desc.idProduct = 0x10B2;
        device_desc.bcdDevice = 0x0101;
    } else if (mode == BB_OUTPUT_XBOX360) {
        device_desc.bDeviceClass = 0xff;
        device_desc.bDeviceSubClass = 0xff;
        device_desc.bDeviceProtocol = 0xff;
        device_desc.idVendor = 0x045e;
        device_desc.idProduct = 0x028e;
        device_desc.bcdDevice = 0x0114;
    } else if (mode == BB_OUTPUT_DUALSENSE) {
        // Composite device (DualSense HID + CDC management).
        device_desc.idVendor = BB_DS5_VID;
        device_desc.idProduct = BB_DS5_PID;
        device_desc.bcdDevice = BB_DS5_BCD;
    } else if (mode == BB_OUTPUT_SWITCH) {
        device_desc.idVendor = 0x0F0D;
        device_desc.idProduct = 0x0092;
        device_desc.bcdDevice = 0x0210;
    }
    return (uint8_t const*)&device_desc;
}

static uint8_t const gamepad_report_desc[] = {
    TUD_HID_REPORT_DESC_GAMEPAD(HID_REPORT_ID(REPORT_ID_GAMEPAD))
};

static uint8_t const generic_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, REPORT_ID_GAMEPAD,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xC0
};

static uint8_t const pid_report_desc[] = {
#include "pid_report_descriptor.h"
};

static uint8_t mister_report_desc[sizeof(gamepad_report_desc) + sizeof(pid_report_desc)];

static uint8_t const switch_report_desc[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,
    0x05,0x09,0x19,0x01,0x29,0x10,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x10,0x81,0x02,
    0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x07,0x35,0x00,0x46,0x3b,0x01,0x65,0x14,
    0x75,0x04,0x95,0x01,0x81,0x42,0x65,0x00,0x75,0x04,0x95,0x01,0x81,0x03,
    0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,0x15,0x00,0x26,0xff,0x00,
    0x35,0x00,0x45,0x00,0x75,0x08,0x95,0x04,0x81,0x02,
    0x06,0x00,0xff,0x09,0x20,0x75,0x08,0x95,0x01,0x81,0x02,0xc0
};

#define COMPOSITE_ITF_CDC 0
#define COMPOSITE_ITF_CDC_DATA 1
#define COMPOSITE_ITF_GAMEPAD 2
#define COMPOSITE_ITF_TOTAL 3
#define COMPOSITE_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define SWITCH_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)
#define XINPUT_ITF_LEN 39
#define XINPUT_CONFIG_LEN (TUD_CONFIG_DESC_LEN + XINPUT_ITF_LEN + TUD_CDC_DESC_LEN)
#define XBOX360_CONFIG_LEN 153

static const uint8_t hid_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, COMPOSITE_ITF_TOTAL, 0, COMPOSITE_CONFIG_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(COMPOSITE_ITF_CDC, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_HID_DESCRIPTOR(COMPOSITE_ITF_GAMEPAD, 5, HID_ITF_PROTOCOL_NONE, sizeof(generic_report_desc), 0x83, 16, 1)
};

static const uint8_t mister_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, COMPOSITE_ITF_TOTAL, 0, COMPOSITE_CONFIG_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(COMPOSITE_ITF_CDC, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_HID_DESCRIPTOR(COMPOSITE_ITF_GAMEPAD, 5, HID_ITF_PROTOCOL_NONE, sizeof(mister_report_desc), 0x83, 16, 1)
};

#define MULTI_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + BB_MAX_SLOTS * TUD_HID_DESC_LEN)
static const uint8_t multi_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2 + BB_MAX_SLOTS, 0, MULTI_CONFIG_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_HID_DESCRIPTOR(2, 5, HID_ITF_PROTOCOL_NONE, sizeof(mister_report_desc), 0x83, 64, 1),
    TUD_HID_DESCRIPTOR(3, 6, HID_ITF_PROTOCOL_NONE, sizeof(mister_report_desc), 0x84, 64, 1)
};

static const uint8_t switch_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, SWITCH_CONFIG_LEN, 0x20, 500),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(switch_report_desc), 0x01, 0x81, 64, 4),
    TUD_CDC_DESCRIPTOR(1, 4, 0x83, 8, 0x04, 0x84, 64)
};

static const uint8_t xinput_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, XINPUT_CONFIG_LEN, 0, 500),
    0x09,0x04,0x00,0x00,0x02,0xff,0x5d,0x01,0x00,
    0x10,0x21,0x10,0x01,0x01,0x24,0x81,0x14,0x03,0x00,0x03,0x13,0x02,0x00,0x03,0x00,
    0x07,0x05,0x81,0x03,0x20,0x00,0x04,
    0x07,0x05,0x02,0x03,0x20,0x00,0x08,
    TUD_CDC_DESCRIPTOR(1, 4, 0x83, 8, 0x04, 0x84, 64)
};

static const uint8_t xbox360_config_desc[XBOX360_CONFIG_LEN] = {
    0x09,0x02,0x99,0x00,0x04,0x01,0x00,0xa0,0xfa,
    0x09,0x04,0x00,0x00,0x02,0xff,0x5d,0x01,0x00,
    0x11,0x21,0x00,0x01,0x01,0x25,0x81,0x14,0x00,0x00,0x00,0x00,0x13,0x02,0x08,0x00,0x00,
    0x07,0x05,0x81,0x03,0x20,0x00,0x04,
    0x07,0x05,0x02,0x03,0x20,0x00,0x08,
    0x09,0x04,0x01,0x00,0x04,0xff,0x5d,0x03,0x00,
    0x1b,0x21,0x00,0x01,0x01,0x01,0x83,0x40,0x01,0x04,0x20,0x16,0x85,0x00,0x00,0x00,0x00,0x00,0x00,0x16,0x06,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x05,0x83,0x03,0x20,0x00,0x02,
    0x07,0x05,0x04,0x03,0x20,0x00,0x04,
    0x07,0x05,0x85,0x03,0x20,0x00,0x40,
    0x07,0x05,0x06,0x03,0x20,0x00,0x10,
    0x09,0x04,0x02,0x00,0x01,0xff,0x5d,0x02,0x00,
    0x09,0x21,0x00,0x01,0x01,0x22,0x86,0x03,0x00,
    0x07,0x05,0x86,0x03,0x20,0x00,0x10,
    0x09,0x04,0x03,0x00,0x00,0xff,0xfd,0x13,0x04,
    0x06,0x41,0x00,0x01,0x01,0x03
};

// DualSense layout: the HID gamepad interface (interrupt IN and OUT, 64 bytes,
// 1 ms) on interface 0, followed by the CDC management interface.
#define DUALSENSE_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t dualsense_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, DUALSENSE_CONFIG_LEN, 0, 500),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(bb_ds5_report_descriptor), 0x02, 0x81, 64, 1),
    TUD_CDC_DESCRIPTOR(1, 4, 0x83, 8, 0x04, 0x84, 64)
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    if (bb_multi_mode()) return multi_config_desc;
    (void)index;
    bb_output_mode_t mode = bb_output_mode();
    if (mode == BB_OUTPUT_MISTER) return mister_config_desc;
    if (mode == BB_OUTPUT_XINPUT) return xinput_config_desc;
    if (mode == BB_OUTPUT_XBOX360) return xbox360_config_desc;
    if (mode == BB_OUTPUT_DUALSENSE) return dualsense_config_desc;
    if (mode == BB_OUTPUT_SWITCH) return switch_config_desc;
    return hid_config_desc;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    if (bb_output_mode() == BB_OUTPUT_SWITCH) return switch_report_desc;
    if (bb_output_mode() == BB_OUTPUT_DUALSENSE) return bb_ds5_report_descriptor;
    if (bb_output_mode() == BB_OUTPUT_MISTER) {
        memcpy(mister_report_desc, gamepad_report_desc, sizeof(gamepad_report_desc) - 1);
        memcpy(mister_report_desc + sizeof(gamepad_report_desc) - 1, pid_report_desc, sizeof(pid_report_desc));
        mister_report_desc[sizeof(mister_report_desc) - 1] = 0xc0;
        return mister_report_desc;
    }
    return generic_report_desc;
}

static uint16_t str_desc[128];
static char adapter_product_name[BB_MAX_NAME];
static char serial_string[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static const char* string_for_index(uint8_t index) {
    bb_output_mode_t mode = bb_output_mode();
    switch (index) {
        case 1:
            if (mode == BB_OUTPUT_DUALSENSE) return "Sony Interactive Entertainment";
            return mode == BB_OUTPUT_XBOX360 ? "\xC2\xA9Microsoft Corporation" : "MC BlueBridge";
        case 2:
            if (mode == BB_OUTPUT_XBOX360) return "Xbox 360 Controller";
            if (mode == BB_OUTPUT_DUALSENSE) return "DualSense Wireless Controller";
            bb_adapter_name_get(adapter_product_name, sizeof(adapter_product_name));
            return adapter_product_name;
        case 3:
            if (mode == BB_OUTPUT_XBOX360) return bb_xbox360_security_serial();
            if (!serial_string[0]) pico_get_unique_board_id_string(serial_string, sizeof(serial_string));
            return serial_string;
        case 4: return mode == BB_OUTPUT_XBOX360 ? "Xbox Security Method 3, Version 1.00, \xC2\xA9 2005 Microsoft Corporation. All rights reserved." : "BlueBridge CDC";
        case 5: return bb_multi_mode() ? "BlueBridge Player 1" : "BlueBridge Gamepad";
        case 6: return "BlueBridge Player 2";
        case 7: return "BlueBridge Player 3";
        case 8: return "BlueBridge Player 4";
        default: return NULL;
    }
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    if (index == 0xee && bb_output_mode() == BB_OUTPUT_XINPUT) {
        static const uint16_t os_string[] = {0x0312, 'M', 'S', 'F', 'T', '1', '0', '0', 0x0020};
        return os_string;
    }
    if (index == 0) {
        str_desc[0] = (TUSB_DESC_STRING << 8) | 4;
        str_desc[1] = 0x0409;
        return str_desc;
    }
    const char* text = string_for_index(index);
    if (!text) return NULL;
    size_t n = strlen(text);
    if (n > 127) n = 127;
    size_t count = 0;
    for (size_t i = 0; i < n && count < 127;) {
        uint32_t code = (uint8_t)text[i++];
        unsigned extra = code >= 0xc2 && code <= 0xdf ? 1 : code >= 0xe0 && code <= 0xef ? 2 : code >= 0xf0 && code <= 0xf4 ? 3 : 0;
        if (extra) {
            uint32_t value = code & ((1u << (6u - extra)) - 1u);
            bool valid = i + extra <= n;
            for (unsigned j = 0; valid && j < extra; ++j) {
                uint8_t byte = (uint8_t)text[i + j];
                if ((byte & 0xc0u) != 0x80u) valid = false;
                else value = (value << 6) | (byte & 0x3fu);
            }
            uint32_t minimum = extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u;
            if (valid && value >= minimum && value <= 0x10ffffu && !(value >= 0xd800u && value <= 0xdfffu)) {
                code = value;
                i += extra;
            } else code = 0xfffdu;
        } else if (code >= 0x80u) code = 0xfffdu;
        if (code > 0xffffu) {
            if (count + 2 > 127) break;
            code -= 0x10000u;
            str_desc[++count] = (uint16_t)(0xd800u | (code >> 10));
            str_desc[++count] = (uint16_t)(0xdc00u | (code & 0x3ffu));
        } else str_desc[++count] = (uint16_t)code;
    }
    str_desc[0] = (TUSB_DESC_STRING << 8) | (2 * count + 2);
    return str_desc;
}
