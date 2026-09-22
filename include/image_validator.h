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
 * 1. Slot geometry and versioned descriptor identify the requested probe mode.
 * 2. Descriptor and complete supplied image pass CRC-32 integrity checks.
 * 3. MSP is 8-byte aligned in (0x20000000, 0x20042000].
 * 4. Reset vector is Thumb code inside the verified image, not erased flash.
 * CRC protects against accidental corruption, not malicious replacement.
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
