#include "xbox360_auth.h"
#include "pico/unique_id.h"
#include "xsm3.h"
#include <string.h>

#define BB_XSM3_IDENT_LEN 0x1d
#define BB_XSM3_INIT_LEN 0x22
#define BB_XSM3_VERIFY_LEN 0x16
#define BB_XSM3_INIT_RESPONSE_LEN 0x2e
#define BB_XSM3_VERIFY_RESPONSE_LEN 0x16
#define BB_XSM3_SERIAL_LEN 11

static uint8_t identification[BB_XSM3_IDENT_LEN];
static uint8_t init_packet[BB_XSM3_INIT_LEN];
static uint8_t verify_packet[BB_XSM3_VERIFY_LEN];
static uint8_t state_reply[2] = {2, 0};
static char security_serial[BB_XSM3_SERIAL_LEN + 1];
static uint16_t response_length;
static bool initialized;
static bool authenticated;

static uint8_t checksum(const uint8_t* packet) {
    uint8_t value = 0;
    uint8_t end = (uint8_t)(packet[4] + 5u);
    for (uint8_t i = 5; i < end; ++i) value ^= packet[i];
    return value;
}

static char hex_digit(uint8_t value) {
    value &= 0x0f;
    return (char)(value < 10 ? ('0' + value) : ('A' + value - 10));
}

static void make_identity(void) {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    memcpy(identification, xsm3_id_data_ms_controller, sizeof(identification));
    for (unsigned i = 0; i < BB_XSM3_SERIAL_LEN; ++i) {
        uint8_t source = board_id.id[(PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1u) - (i / 2u)];
        security_serial[i] = hex_digit((i & 1u) ? source : (uint8_t)(source >> 4));
        identification[5u + i] = (uint8_t)security_serial[i];
    }
    security_serial[BB_XSM3_SERIAL_LEN] = 0;
    identification[BB_XSM3_IDENT_LEN - 1u] = checksum(identification);
}

void bb_xbox360_auth_init(void) {
    if (!initialized) {
        make_identity();
        initialized = true;
    }
    memset(init_packet, 0, sizeof(init_packet));
    memset(verify_packet, 0, sizeof(verify_packet));
    memset(xsm3_challenge_response, 0, 0x30);
    xsm3_initialise_state();
    xsm3_set_identification_data(identification);
    response_length = 0;
    authenticated = false;
}

void bb_xbox360_auth_task(void) {
}

static uint16_t response_size(void) {
    if (xsm3_challenge_response[0] != 0x49 || xsm3_challenge_response[1] != 0x4c) return 0;
    uint16_t length = (uint16_t)xsm3_challenge_response[4] + 6u;
    if (length > 0x30u) return 0;
    return length;
}

bool bb_xbox360_auth_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if ((request->wIndex & 0xffu) != 3u) return false;
    uint8_t type = request->bmRequestType & 0x60u;
    if (type != 0x20u && type != 0x40u) return false;

    switch (request->bRequest) {
        case 0x81:
            if (stage == CONTROL_STAGE_SETUP) {
                bb_xbox360_auth_init();
                uint16_t len = request->wLength < sizeof(identification) ? request->wLength : sizeof(identification);
                return tud_control_xfer(rhport, request, identification, len);
            }
            return true;

        case 0x82:
            if (stage == CONTROL_STAGE_SETUP) {
                if (request->wLength != sizeof(init_packet)) return false;
                return tud_control_xfer(rhport, request, init_packet, sizeof(init_packet));
            }
            if (stage == CONTROL_STAGE_DATA) {
                memset(xsm3_challenge_response, 0, 0x30);
                xsm3_do_challenge_init(init_packet);
                response_length = response_size();
                authenticated = false;
                return response_length == BB_XSM3_INIT_RESPONSE_LEN;
            }
            return true;

        case 0x83:
            if (stage != CONTROL_STAGE_SETUP) return true;
            if (!response_length) return false;
            {
                uint16_t len = request->wLength < response_length ? request->wLength : response_length;
                return tud_control_xfer(rhport, request, xsm3_challenge_response, len);
            }

        case 0x84:
            if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, NULL, 0);
            return true;

        case 0x86:
            if (stage == CONTROL_STAGE_SETUP) {
                uint16_t len = request->wLength < sizeof(state_reply) ? request->wLength : sizeof(state_reply);
                return tud_control_xfer(rhport, request, state_reply, len);
            }
            return true;

        case 0x87:
            if (stage == CONTROL_STAGE_SETUP) {
                if (request->wLength != sizeof(verify_packet)) return false;
                return tud_control_xfer(rhport, request, verify_packet, sizeof(verify_packet));
            }
            if (stage == CONTROL_STAGE_DATA) {
                memset(xsm3_challenge_response, 0, 0x30);
                xsm3_do_challenge_verify(verify_packet);
                response_length = response_size();
                authenticated = response_length == BB_XSM3_VERIFY_RESPONSE_LEN;
                return authenticated;
            }
            return true;

        default:
            return false;
    }
}

bool bb_xbox360_authenticated(void) {
    return authenticated;
}

const char* bb_xbox360_security_serial(void) {
    if (!initialized) {
        make_identity();
        initialized = true;
    }
    return security_serial;
}
