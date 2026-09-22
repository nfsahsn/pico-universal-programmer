#include "include/probe_controls.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"

bool probe_controls_take_save_request(probe_mode_t *mode) {
    uint32_t request = watchdog_hw->scratch[3];
    watchdog_hw->scratch[3] = 0;
    uint32_t value = request & 0xffu;
    if (!watchdog_caused_reboot() || (request & 0xffff0000u) != 0xc0de0000u ||
        ((request >> 8) & 0xffu) != (value ^ 0xffu) || value >= MODE_COUNT) {
        return false;
    }
    *mode = (probe_mode_t)value;
    return true;
}

/* A probe arms this marker before enabling a service-loop watchdog. A stalled
 * engine returns to interactive recovery; normal button presses cannot reboot it. */
bool probe_controls_take_fault_request(void) {
    uint32_t request = watchdog_hw->scratch[3];
    if ((request & 0xffff0000u) != 0xfa170000u) return false;
    watchdog_hw->scratch[3] = 0;
    uint32_t mode = request & 0xffu;
    return watchdog_caused_reboot() && mode < MODE_COUNT &&
           ((request >> 8) & 0xffu) == (mode ^ 0xffu);
}
