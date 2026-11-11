#include "bootsel_button.h"
#include "bluebridge_platform.h"
#include "bluebridge_state.h"
#include "usb_device.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "pico/flash.h"
#include "pico/platform.h"

static bool initialized;
static bool observed;
static bool stable;
static uint32_t changed_at;
static uint32_t pressed_at;
static bool pairing_action;
static bool recovery_action;

static void __no_inline_not_in_flash_func(sample_bootsel)(void* context) {
    uint32_t saved = ioqspi_hw->io[1].ctrl;
    ioqspi_hw->io[1].ctrl = (saved & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS) |
        (GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB);
    for (volatile unsigned delay = 0; delay < 1000; ++delay) {}
    bool pressed = (sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS) == 0;
    ioqspi_hw->io[1].ctrl = saved;
    *(bool*)context = pressed;
}

void bb_bootsel_poll(uint32_t now) {
    bool pressed = false;
    if (flash_safe_execute(sample_bootsel, &pressed, 1) != PICO_OK) return;
    if (!initialized) {
        initialized = true;
        observed = pressed;
        stable = pressed;
        changed_at = now;
        pressed_at = pressed ? now : 0;
        return;
    }
    if (pressed != observed) {
        observed = pressed;
        changed_at = now;
        return;
    }
    if (pressed != stable && (uint32_t)(now - changed_at) >= 30u) {
        stable = pressed;
        if (stable) {
            pressed_at = now;
            pairing_action = false;
            recovery_action = false;
        }
        return;
    }
    if (!stable) return;
    uint32_t held = (uint32_t)(now - pressed_at);
    if (!pairing_action && held >= 1000u) {
        pairing_action = true;
        bb_runtime_state_t state;
        bb_state_get(&state);
        if (state.pairing) bb_platform_stop_pairing();
        else bb_platform_start_pairing();
    }
    if (!recovery_action && held >= 5000u &&
        (bb_output_mode() == BB_OUTPUT_XBOX360 || bb_output_mode() == BB_OUTPUT_DUALSENSE)) {
        recovery_action = true;
        bb_platform_stop_pairing();
        if (bb_output_mode_set(BB_OUTPUT_MISTER)) bb_usb_request_reenumeration();
    }
}
