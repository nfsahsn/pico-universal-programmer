#include "pin_control.h"
#include "probe_config.h"
#include "hardware/gpio.h"

extern bool probe_swd_is_active(void);
extern void probe_wait_idle(void);
static bool pins_forced;

void probe_pin_write(unsigned pin, bool high) {
    if (!probe_swd_is_active()) {
        gpio_put(pin, high); // JTAG uses the SIO pin function.
        return;
    }
    // SIO writes do not drive a pin owned by PIO. Finish queued SWD clocks
    // before overriding its output; restore PIO ownership at the next operation.
    probe_wait_idle();
    gpio_set_outover(pin, high ? GPIO_OVERRIDE_HIGH : GPIO_OVERRIDE_LOW);
    gpio_set_oeover(pin, GPIO_OVERRIDE_HIGH);
    pins_forced = true;
}

void probe_pin_release(void) {
    if (!pins_forced) return;
    gpio_set_outover(PROBE_PIN_SWCLK, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(PROBE_PIN_SWDIO, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(PROBE_PIN_SWCLK, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(PROBE_PIN_SWDIO, GPIO_OVERRIDE_NORMAL);
    pins_forced = false;
}
