#include "include/probe_controls.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

// Generated upstream code includes reviewed protocol fixes and loop heartbeat.
#define main picorvd_engine_main
#include "PicoRVD/src/main.cpp"
#undef main

int main() {
    if (!probe_controls_init(MODE_PICORVD)) panic("Mode control timer unavailable");
    watchdog_hw->scratch[3] = PROBE_WATCHDOG_TOKEN(MODE_PICORVD);
    watchdog_enable(8000, true);
    return picorvd_engine_main();
}
