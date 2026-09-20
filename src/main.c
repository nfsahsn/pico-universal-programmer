/**
 * @file main.c
 * @brief Entry point and mode supervisor for the Pico Universal Programmer.
 */

#include "config/app_config.h"
#include "config/memory_map.h"
#include "include/bootloader.h"
#include "include/button.h"
#include "include/led_indicator.h"
#include "include/storage.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "include/image_validator.h"

int main(void) {
    /* INTERCEPT: Hardware reboot from bootloader_jump_to_mode. */
    uint32_t jump_req = watchdog_hw->scratch[7];
    if ((jump_req & 0xFFFFFF00) == 0xDEAD0000) {
        probe_mode_t mode = (probe_mode_t)(jump_req & 0xFF);
        watchdog_hw->scratch[7] = 0;

        uint32_t slot_addr;
        if (mode == MODE_CMSIS_DAP) slot_addr = SLOT_1_CMSIS_DAP_ADDR;
        else if (mode == MODE_BLACKMAGIC) slot_addr = SLOT_2_BLACKMAGIC_ADDR;
        else if (mode == MODE_PICORVD) slot_addr = SLOT_3_PICORVD_ADDR;
        else slot_addr = SLOT_1_CMSIS_DAP_ADDR;

        uint32_t msp = image_get_initial_msp(slot_addr);
        uint32_t entry = image_get_entry_point(slot_addr);
        
        *((volatile uint32_t *)0xE000ED08) = slot_addr + 0x100;
        
        __asm volatile (
            "msr msp, %0\n"
            "isb\n"
            "bx %1\n"
            : : "r"(msp), "r"(entry) : "memory"
        );
        while(1);
    }

    button_init();
    led_init();
    storage_init();

    probe_mode_t current_mode = storage_get_active_mode();
    led_set_mode(current_mode);

    uint32_t last_activity_time = to_ms_since_boot(get_absolute_time());

    while (true) {
        led_tick();
        button_event_t event = button_poll();

        if (event == BUTTON_EVENT_SHORT_PRESS) {
            current_mode = (probe_mode_t)((current_mode + 1) % MODE_COUNT);
            storage_set_active_mode(current_mode);
            led_set_mode(current_mode);
            last_activity_time = to_ms_since_boot(get_absolute_time());
        } else if (event == BUTTON_EVENT_LONG_PRESS) {
            storage_save_default_mode(current_mode);
            led_flash_save_confirmation();
            last_activity_time = to_ms_since_boot(get_absolute_time());
        }

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (!button_is_pressed() && ((now - last_activity_time) >= 5000)) {
            sleep_ms(50);
            bootloader_jump_to_mode(current_mode);
        }
    }

    return 0;
}
