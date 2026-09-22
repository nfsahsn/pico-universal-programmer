/** Boot selection and early, post-reset CPU handover. */
#include "include/bootloader.h"
#include "include/image_validator.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/runtime_init.h"

/* Scratch 4..7 belong to the SDK/ROM reboot protocol. Scratch 0 stores mode;
 * 1 and 2 carry a checked one-shot handover request across a normal flash boot. */
#define JUMP_MAGIC 0xB007DA00u
#define JUMP_MASK  0xFFFFFF00u

static bool s_recovery_required;

extern void bootloader_enter_image(uint32_t vtor, uint32_t msp, uint32_t entry)
    __attribute__((noreturn));

uint32_t bootloader_get_slot_address(probe_mode_t mode) {
    switch (mode) {
        case MODE_CMSIS_DAP: return SLOT_1_CMSIS_DAP_ADDR;
        case MODE_BLACKMAGIC: return SLOT_2_BLACKMAGIC_ADDR;
        case MODE_PICORVD: return SLOT_3_PICORVD_ADDR;
        default: return 0;
    }
}

uint32_t bootloader_get_slot_size(probe_mode_t mode) {
    switch (mode) {
        case MODE_CMSIS_DAP: return SLOT_1_CMSIS_DAP_SIZE;
        case MODE_BLACKMAGIC: return SLOT_2_BLACKMAGIC_SIZE;
        case MODE_PICORVD: return SLOT_3_PICORVD_SIZE;
        default: return 0;
    }
}

bool bootloader_mode_is_valid(probe_mode_t mode) {
    uint32_t address = bootloader_get_slot_address(mode);
    uint32_t size = bootloader_get_slot_size(mode);
    return address != 0 && image_validate_slot(address, size);
}

bool bootloader_recovery_required(void) {
    return s_recovery_required;
}

/* crt0 has copied data and cleared BSS, but no SDK peripheral, IRQ or timer
 * initializers have run. Normal ROM flash boot has already configured XIP.
 * Run before PICO_RUNTIME_INIT_EARLIEST (00001) in the pinned SDK. */
static void bootloader_early_handover(void) {
    uint32_t request = watchdog_hw->scratch[1];
    uint32_t check = watchdog_hw->scratch[2];
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = 0;
    if ((request & JUMP_MASK) != JUMP_MAGIC) {
        return;
    }
    probe_mode_t mode = (probe_mode_t)(request & 0xffu);
    if (!watchdog_caused_reboot() || check != ~request || !bootloader_mode_is_valid(mode)) {
        s_recovery_required = true;
        return;
    }
    uint32_t address = bootloader_get_slot_address(mode);
    bootloader_enter_image(address + 0x100u, image_get_initial_msp(address),
                           image_get_entry_point(address));
}
PICO_RUNTIME_INIT_FUNC(bootloader_early_handover, "00000");

bool bootloader_jump_to_mode(probe_mode_t mode) {
    if (!bootloader_mode_is_valid(mode)) {
        return false;
    }
    uint32_t request = JUMP_MAGIC | (uint32_t)mode;
    watchdog_hw->scratch[1] = request;
    watchdog_hw->scratch[2] = ~request;
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}
