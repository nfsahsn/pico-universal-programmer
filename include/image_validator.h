/**
 * @file image_validator.h
 * @brief Validates ARM Cortex-M vector tables before executing CPU handover.
 */

#ifndef IMAGE_VALIDATOR_H
#define IMAGE_VALIDATOR_H

#include "config/memory_map.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Validates if a target flash slot contains a valid executable image.
 *
 * Checks:
 * 1. Initial Stack Pointer (MSP) points inside RP2040 SRAM (0x20000000 - 0x20042000).
 * 2. Reset Vector points inside the target slot's flash memory range and has Thumb bit set (bit 0 == 1).
 *
 * @param slot_addr The absolute flash memory address of the target slot.
 * @param slot_size The size of the slot in bytes.
 * @return bool True if the slot appears to contain a valid executable image.
 */
bool image_validate_slot(uint32_t slot_addr, uint32_t slot_size);

/**
 * @brief Returns the Initial Main Stack Pointer (MSP) for the given image.
 */
uint32_t image_get_initial_msp(uint32_t slot_addr);

/**
 * @brief Returns the Reset Handler entry point address for the given image.
 */
uint32_t image_get_entry_point(uint32_t slot_addr);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_VALIDATOR_H */
