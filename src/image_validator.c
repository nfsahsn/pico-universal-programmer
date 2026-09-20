/**
 * @file image_validator.c
 * @brief Validates ARM Cortex-M vector tables before executing CPU handover.
 */

#include "include/image_validator.h"

#define RP2040_SRAM_BASE        (0x20000000U)
#define RP2040_SRAM_TOP         (0x20042000U) /* 264 KB SRAM limit */

uint32_t image_get_initial_msp(uint32_t slot_addr) {
    const uint32_t *vector_table = (const uint32_t *)(slot_addr + 0x100);
    return vector_table[0];
}

uint32_t image_get_entry_point(uint32_t slot_addr) {
    const uint32_t *vector_table = (const uint32_t *)(slot_addr + 0x100);
    return vector_table[1];
}

bool image_validate_slot(uint32_t slot_addr, uint32_t slot_size) {
    uint32_t msp = image_get_initial_msp(slot_addr);
    uint32_t entry = image_get_entry_point(slot_addr);

    /* 1. Validate Initial Main Stack Pointer (MSP) */
    if (msp < RP2040_SRAM_BASE || msp > RP2040_SRAM_TOP || (msp & 0x3) != 0) {
        return false;
    }

    /* 2. Validate Reset Handler Address */
    /* Must reside within target slot range */
    if (entry < slot_addr || entry >= (slot_addr + slot_size)) {
        return false;
    }

    /* Cortex-M0+ requires Thumb mode (bit 0 must be 1) */
    if ((entry & 1) == 0) {
        return false;
    }

    return true;
}
