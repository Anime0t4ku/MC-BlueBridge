#include "usb_descriptors.h"
#include "bluebridge_state.h"
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
    if (mode == BB_OUTPUT_MISTER) {
        device_desc.idProduct = (uint16_t)(0xB000u | (bb_current_mister_identity() & 0x0fffu));
    } else if (mode == BB_OUTPUT_XINPUT) {
        device_desc.idVendor = 0x2E8A;
        device_desc.idProduct = 0x10B2;
        device_desc.bcdDevice = 0x0101;
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

static const uint8_t hid_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, COMPOSITE_ITF_TOTAL, 0, COMPOSITE_CONFIG_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(COMPOSITE_ITF_CDC, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_HID_DESCRIPTOR(COMPOSITE_ITF_GAMEPAD, 5, HID_ITF_PROTOCOL_NONE, sizeof(gamepad_report_desc), 0x83, 16, 1)
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

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    bb_output_mode_t mode = bb_output_mode();
    if (mode == BB_OUTPUT_XINPUT) return xinput_config_desc;
    if (mode == BB_OUTPUT_SWITCH) return switch_config_desc;
    return hid_config_desc;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return bb_output_mode() == BB_OUTPUT_SWITCH ? switch_report_desc : gamepad_report_desc;
}

static uint16_t str_desc[64];
static char serial_string[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static const char* string_for_index(uint8_t index) {
    bb_output_mode_t mode = bb_output_mode();
    switch (index) {
        case 1: return mode == BB_OUTPUT_SWITCH ? "MC BlueBridge" : mode == BB_OUTPUT_XINPUT ? "MC BlueBridge" : "MC BlueBridge";
        case 2: return mode == BB_OUTPUT_SWITCH ? "MC BlueBridge Switch" : mode == BB_OUTPUT_XINPUT ? "MC BlueBridge X-Input" : mode == BB_OUTPUT_MISTER ? "MC BlueBridge MiSTer" : "MC BlueBridge Generic HID";
        case 3:
            if (!serial_string[0]) pico_get_unique_board_id_string(serial_string, sizeof(serial_string));
            return serial_string;
        case 4: return "BlueBridge CDC";
        case 5: return "BlueBridge Gamepad";
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
    if (n > 63) n = 63;
    for (size_t i = 0; i < n; ++i) str_desc[1 + i] = text[i];
    str_desc[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return str_desc;
}
