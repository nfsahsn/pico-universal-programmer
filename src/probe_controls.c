#include "include/probe_controls.h"
#include "include/led_indicator.h"
#include "pico/time.h"
#include <stddef.h>

static repeating_timer_t controls_timer;

static bool controls_tick(repeating_timer_t *timer) {
    (void)timer;
    led_tick();
    /* Selection ends at handover. Only a reset may reopen it; button noise or
     * a press during an upload must never reboot the running programmer. */
    return true;
}

bool probe_controls_init(probe_mode_t mode) {
    if ((unsigned)mode >= MODE_COUNT) return false;
    led_init();
    led_set_running(mode);
    return add_repeating_timer_ms(-5, controls_tick, NULL, &controls_timer);
}
