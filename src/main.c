/** Startup selection and recovery UI. The mode stays locked after handover. */
#include "config/app_config.h"
#include "include/bootloader.h"
#include "include/button.h"
#include "include/led_indicator.h"
#include "include/storage.h"
#include "include/probe_controls.h"
#include "pico/stdlib.h"

int main(void) {
    button_init();
    led_init();
    storage_init();
    probe_mode_t current_mode = storage_get_active_mode();
    led_set_mode(current_mode);
    bool probe_fault = probe_controls_take_fault_request();
    probe_mode_t save_mode;
    bool runtime_save = probe_controls_take_save_request(&save_mode);
    bool save_failed = false;
    if (runtime_save) {
        current_mode = save_mode;
        storage_set_active_mode(current_mode);
        led_set_mode(current_mode);
        if (bootloader_mode_is_valid(current_mode) && storage_save_default_mode(current_mode)) {
            led_flash_save_confirmation();
        } else {
            led_set_error();
            save_failed = true;
        }
    }
    bool boot_attempted = bootloader_recovery_required() || save_failed || probe_fault;
    if (boot_attempted) {
        led_set_error();
    }
    uint32_t last_activity_time = to_ms_since_boot(get_absolute_time());

    while (true) {
        led_tick();
        bool was_busy = button_is_busy();
        button_event_t event = button_poll();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        /* Include the entire debounce/release interval in activity. Otherwise
         * release just after the deadline can boot before a short-press event. */
        if (was_busy || button_is_busy()) {
            last_activity_time = now;
        }
        /* A runtime long press reboots while still held. Consume its release
         * without treating it as another save or mode change. */
        if (runtime_save) {
            if (!button_is_busy()) runtime_save = false;
            event = BUTTON_EVENT_NONE;
        }
        if (event == BUTTON_EVENT_SHORT_PRESS) {
            current_mode = (probe_mode_t)((current_mode + 1) % MODE_COUNT);
            storage_set_active_mode(current_mode);
            led_set_mode(current_mode);
            last_activity_time = now;
            boot_attempted = false;
        } else if (event == BUTTON_EVENT_LONG_PRESS) {
            if (bootloader_mode_is_valid(current_mode) && storage_save_default_mode(current_mode)) {
                led_flash_save_confirmation();
                boot_attempted = false;
            } else {
                led_set_error();
                boot_attempted = true;
            }
            last_activity_time = to_ms_since_boot(get_absolute_time());
        }
        now = to_ms_since_boot(get_absolute_time());
        if (!boot_attempted && !button_is_busy() &&
            (now - last_activity_time) >= BOOT_HANDOVER_DELAY_MS) {
            /* On invalid/missing images retain button control and show error.
             * A successful jump request never returns. */
            boot_attempted = true;
            if (!bootloader_jump_to_mode(current_mode)) {
                led_set_error();
            }
        }
        sleep_ms(1);
    }
}
