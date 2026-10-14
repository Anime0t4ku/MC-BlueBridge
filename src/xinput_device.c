#include "xinput_device.h"
#include "bt_host.h"
#include "bluebridge_state.h"
#include "xbox360_auth.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include <string.h>

static uint8_t port;
static uint8_t input_ep;
static uint8_t output_ep;
static uint8_t accessory_out_ep1;
static uint8_t accessory_out_ep2;
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t input_buffer[32];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t output_buffer[32];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t accessory_out_buffer1[32];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t accessory_out_buffer2[32];

static void reset(uint8_t rhport) {
    port = rhport;
    input_ep = 0;
    output_ep = 0;
    accessory_out_ep1 = 0;
    accessory_out_ep2 = 0;
}

static void init(void) {
    reset(0);
}

static bool deinit(void) {
    reset(0);
    return true;
}

static bool open_endpoint(uint8_t rhport, const tusb_desc_endpoint_t* ep) {
    return ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT &&
           tu_edpt_packet_size(ep) == 32 &&
           usbd_edpt_open(rhport, ep);
}

static uint16_t open_xbox360_interface(uint8_t rhport, const tusb_desc_interface_t* itf, uint16_t available) {
    const uint8_t* bytes = (const uint8_t*)itf;
    uint16_t offset = itf->bLength;
    uint8_t opened = 0;

    while (offset + 2 <= available) {
        uint8_t length = bytes[offset];
        if (length < 2 || length > available - offset) return 0;
        if (bytes[offset + 1] == TUSB_DESC_INTERFACE) break;
        if (bytes[offset + 1] == TUSB_DESC_ENDPOINT) {
            if (length != sizeof(tusb_desc_endpoint_t)) return 0;
            const tusb_desc_endpoint_t* ep = (const tusb_desc_endpoint_t*)&bytes[offset];
            if (!open_endpoint(rhport, ep)) return 0;
            uint8_t address = ep->bEndpointAddress;
            if (itf->bInterfaceNumber == 0) {
                if (address & 0x80u) input_ep = address;
                else output_ep = address;
            } else if (!(address & 0x80u)) {
                if (!accessory_out_ep1) accessory_out_ep1 = address;
                else if (!accessory_out_ep2) accessory_out_ep2 = address;
            }
            ++opened;
        }
        offset += length;
    }

    if (opened != itf->bNumEndpoints) return 0;

    if (itf->bInterfaceNumber == 0) {
        if (!input_ep || !output_ep) return 0;
        port = rhport;
        if (!usbd_edpt_xfer(port, output_ep, output_buffer, sizeof(output_buffer))) return 0;
    } else {
        if (accessory_out_ep1 && !usbd_edpt_busy(rhport, accessory_out_ep1))
            usbd_edpt_xfer(rhport, accessory_out_ep1, accessory_out_buffer1, sizeof(accessory_out_buffer1));
        if (accessory_out_ep2 && !usbd_edpt_busy(rhport, accessory_out_ep2))
            usbd_edpt_xfer(rhport, accessory_out_ep2, accessory_out_buffer2, sizeof(accessory_out_buffer2));
    }

    return offset;
}

static uint16_t open_interface(uint8_t rhport, const tusb_desc_interface_t* itf, uint16_t available) {
    if (bb_output_mode() == BB_OUTPUT_XBOX360) {
        if (itf->bInterfaceClass == 0xff && itf->bInterfaceSubClass == 0xfd &&
            itf->bInterfaceProtocol == 0x13 && itf->bInterfaceNumber == 3 &&
            itf->bNumEndpoints == 0) {
            const uint8_t* bytes = (const uint8_t*)itf;
            uint16_t offset = itf->bLength;
            while (offset + 2 <= available) {
                uint8_t length = bytes[offset];
                if (length < 2 || length > available - offset) return 0;
                if (bytes[offset + 1] == TUSB_DESC_INTERFACE) break;
                offset += length;
            }
            return offset;
        }
        if (itf->bInterfaceClass == 0xff && itf->bInterfaceSubClass == 0x5d &&
            itf->bInterfaceNumber <= 2)
            return open_xbox360_interface(rhport, itf, available);
        return 0;
    }

    if (itf->bInterfaceClass != 0xff || itf->bInterfaceSubClass != 0x5d ||
        itf->bInterfaceProtocol != 1 || itf->bNumEndpoints != 2 || available < 39) return 0;
    const uint8_t* bytes = (const uint8_t*)itf;
    uint16_t offset = itf->bLength;
    uint8_t in = 0, out = 0;
    const tusb_desc_endpoint_t* endpoints[2] = {0};
    unsigned count = 0;
    while (offset + 2 <= available && count < 2) {
        uint8_t length = bytes[offset];
        if (length < 2 || length > available - offset) return 0;
        if (bytes[offset + 1] == TUSB_DESC_INTERFACE) return 0;
        if (bytes[offset + 1] == TUSB_DESC_ENDPOINT) {
            if (length != sizeof(tusb_desc_endpoint_t)) return 0;
            const tusb_desc_endpoint_t* ep = (const tusb_desc_endpoint_t*)&bytes[offset];
            if (ep->bmAttributes.xfer != TUSB_XFER_INTERRUPT || tu_edpt_packet_size(ep) != 32) return 0;
            if (ep->bEndpointAddress & 0x80) {
                if (in) return 0;
                in = ep->bEndpointAddress;
            } else {
                if (out || !ep->bEndpointAddress) return 0;
                out = ep->bEndpointAddress;
            }
            endpoints[count++] = ep;
        }
        offset += length;
    }
    if (count != 2 || !in || !out) return 0;
    for (unsigned i = 0; i < 2; ++i) if (!usbd_edpt_open(rhport, endpoints[i])) return 0;
    port = rhport;
    input_ep = in;
    output_ep = out;
    if (!usbd_edpt_xfer(port, output_ep, output_buffer, sizeof(output_buffer))) {
        reset(rhport);
        return 0;
    }
    return offset;
}

static bool control(uint8_t rhport, uint8_t stage, const tusb_control_request_t* request) {
    if (bb_output_mode() == BB_OUTPUT_XBOX360) return bb_xbox360_auth_control(rhport, stage, request);
    return false;
}

static bool transfer(uint8_t rhport, uint8_t endpoint, xfer_result_t result, uint32_t length) {
    if (endpoint == input_ep) return true;
    if (endpoint == output_ep) {
        if (result == XFER_RESULT_SUCCESS && length >= 8 && output_buffer[0] == 0 && output_buffer[1] == 8)
            bb_bt_host_set_output_rumble(output_buffer[3], output_buffer[4]);
        return usbd_edpt_xfer(rhport, output_ep, output_buffer, sizeof(output_buffer));
    }
    if (endpoint == accessory_out_ep1)
        return usbd_edpt_xfer(rhport, accessory_out_ep1, accessory_out_buffer1, sizeof(accessory_out_buffer1));
    if (endpoint == accessory_out_ep2)
        return usbd_edpt_xfer(rhport, accessory_out_ep2, accessory_out_buffer2, sizeof(accessory_out_buffer2));
    return endpoint == 0x83 || endpoint == 0x85 || endpoint == 0x86;
}

static const usbd_class_driver_t driver = {
    .name = "BlueBridge XInput",
    .init = init,
    .deinit = deinit,
    .reset = reset,
    .open = open_interface,
    .control_xfer_cb = control,
    .xfer_cb = transfer,
    .sof = NULL
};

const usbd_class_driver_t* usbd_app_driver_get_cb(uint8_t* count) {
    *count = 1;
    return &driver;
}

bool bb_xinput_ready(void) {
    return input_ep && tud_ready() && !usbd_edpt_busy(port, input_ep);
}

bool bb_xinput_send(const uint8_t report[20]) {
    if (!bb_xinput_ready() || !usbd_edpt_claim(port, input_ep)) return false;
    memcpy(input_buffer, report, 20);
    if (usbd_edpt_xfer(port, input_ep, input_buffer, 20)) return true;
    usbd_edpt_release(port, input_ep);
    return false;
}
