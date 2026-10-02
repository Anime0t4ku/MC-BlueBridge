#include "led_status.h"
#include "bluebridge_state.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "btstack_run_loop.h"

static btstack_timer_source_t timer;
static uint32_t tick;
static uint32_t profile_signal_until;
static int error_state;
static int led_on = -1;

static void apply_led(int on) {
    if (on == led_on) return;
    led_on = on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
}

static void timer_handler(btstack_timer_source_t* ts) {
    (void)ts;
    tick++;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    int on = 0;
    if (error_state) {
        on = (tick & 1u) == 0;
    } else if ((int32_t)(profile_signal_until - now) > 0) {
        uint32_t phase = tick % 10u;
        on = phase == 0 || phase == 1 || phase == 4 || phase == 5;
    } else {
        bb_runtime_state_t state;
        bb_state_get(&state);
        if (state.controller_connected) on = 1;
        else if (state.pairing) on = ((tick / 2u) & 1u) == 0;
        else on = ((tick / 8u) & 1u) == 0;
    }
    apply_led(on);
    btstack_run_loop_set_timer(&timer, 100);
    btstack_run_loop_add_timer(&timer);
}

void bb_led_init(void) {
    tick = 0;
    profile_signal_until = 0;
    error_state = 0;
    led_on = -1;
    btstack_run_loop_set_timer_handler(&timer, timer_handler);
    btstack_run_loop_set_timer(&timer, 100);
    btstack_run_loop_add_timer(&timer);
}

void bb_led_signal_profile_changed(void) {
    profile_signal_until = to_ms_since_boot(get_absolute_time()) + 1000;
}

void bb_led_set_error(int enabled) {
    error_state = enabled ? 1 : 0;
}
