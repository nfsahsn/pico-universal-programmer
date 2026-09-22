/**
 * @file image_validator.c
 * @brief Validates ARM Cortex-M vector tables before executing CPU handover.
 */

#include "include/image_validator.h"
#include "include/image_format.h"
#include "include/crc32.h"
#include <stddef.h>

#define RP2040_SRAM_BASE        (0x20000000U)
#define RP2040_SRAM_TOP         (0x20042000U) /* 264 KB SRAM limit */

uint32_t image_get_initial_msp(uint32_t slot_addr) {
    const uint32_t *vector_table = (const uint32_t *)(uintptr_t)(slot_addr + 0x100);
    return vector_table[0];
}

uint32_t image_get_entry_point(uint32_t slot_addr) {
    const uint32_t *vector_table = (const uint32_t *)(uintptr_t)(slot_addr + 0x100);
    return vector_table[1];
}

bool image_validate_slot(uint32_t slot_addr, uint32_t slot_size) {
    /* Check geometry before dereferencing flash, including overflow. */
    if ((slot_addr & 0xfffu) != 0 || slot_size < 0x108u ||
        slot_addr < FLASH_BASE_ADDR || slot_size > FLASH_TOTAL_SIZE ||
        slot_addr - FLASH_BASE_ADDR > FLASH_TOTAL_SIZE - slot_size) {
        return false;
    }

    uint32_t mode;
    if (slot_addr == SLOT_1_CMSIS_DAP_ADDR && slot_size == SLOT_1_CMSIS_DAP_SIZE) mode = MODE_CMSIS_DAP;
    else if (slot_addr == SLOT_2_BLACKMAGIC_ADDR && slot_size == SLOT_2_BLACKMAGIC_SIZE) mode = MODE_BLACKMAGIC;
    else if (slot_addr == SLOT_3_PICORVD_ADDR && slot_size == SLOT_3_PICORVD_SIZE) mode = MODE_PICORVD;
    else return false;

    const image_descriptor_t *descriptor = (const image_descriptor_t *)(uintptr_t)
        (slot_addr + slot_size - IMAGE_DESCRIPTOR_PAGE_SIZE);
    if (descriptor->magic != IMAGE_DESCRIPTOR_MAGIC || descriptor->version != IMAGE_DESCRIPTOR_VERSION ||
        descriptor->base != slot_addr || descriptor->mode != mode || descriptor->flags != 0 ||
        descriptor->length < 512u || descriptor->length % 256u != 0 ||
        descriptor->length > slot_size - IMAGE_DESCRIPTOR_PAGE_SIZE ||
        descriptor->header_crc32 != programmer_crc32(descriptor, offsetof(image_descriptor_t, header_crc32))) {
        return false;
    }
    if (descriptor->image_crc32 != programmer_crc32((const void *)(uintptr_t)slot_addr, descriptor->length)) {
        return false;
    }
    uint32_t msp = image_get_initial_msp(slot_addr);
    uint32_t entry = image_get_entry_point(slot_addr);

    /* 1. Validate Initial Main Stack Pointer (MSP) */
    if (msp <= RP2040_SRAM_BASE || msp > RP2040_SRAM_TOP || (msp & 0x7) != 0) {
        return false;
    }

    /* 2. Validate Reset Handler Address */
    /* Must reside within target slot range */
    if ((entry & ~1u) < slot_addr + 0x100u || entry >= (slot_addr + descriptor->length)) {
        return false;
    }

    /* Cortex-M0+ requires Thumb mode (bit 0 must be 1) */
    if ((entry & 1) == 0) {
        return false;
    }

    if (*(const uint16_t *)(uintptr_t)(entry & ~1u) == 0xffffu) {
        return false;
    }

    return true;
}
