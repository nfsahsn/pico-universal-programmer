/**
 * @file bootloader.c
 * @brief Bootloader supervisor and safe CPU handover engine.
 */

#include "include/bootloader.h"
#include "include/image_validator.h"
#include "include/led_indicator.h"
#include "hardware/resets.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
#include "hardware/watchdog.h"

uint32_t bootloader_get_slot_address(probe_mode_t mode) {
    switch (mode) {
        case MODE_CMSIS_DAP:
            return SLOT_1_CMSIS_DAP_ADDR;
        case MODE_PICORVD:
            return SLOT_3_PICORVD_ADDR;
        default:
            return SLOT_1_CMSIS_DAP_ADDR;
    }
}

uint32_t bootloader_get_slot_size(probe_mode_t mode) {
    switch (mode) {
        case MODE_CMSIS_DAP:
            return SLOT_1_CMSIS_DAP_SIZE;
        case MODE_PICORVD:
            return SLOT_3_PICORVD_SIZE;
        default:
            return SLOT_1_CMSIS_DAP_SIZE;
    }
}

void bootloader_jump_to_mode(probe_mode_t mode) {
    /* We use scratch[7] to pass the desired mode across a hardware reboot. 
       Magic number 0xDEAD0000 is used to validate it. */
    watchdog_hw->scratch[7] = 0xDEAD0000 | (mode & 0xFF);
    
    /* Trigger a full hardware watchdog reset. 
       PC=0 tells the BootROM to boot normally from flash (0x10000000). */
    watchdog_reboot(0, 0, 10);
    
    while(1) tight_loop_contents();
}
