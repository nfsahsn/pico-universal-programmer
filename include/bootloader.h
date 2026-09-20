/**
 * @file bootloader.h
 * @brief Bootloader supervisor and safe CPU handover engine.
 */

#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "config/memory_map.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolves the flash memory address for a given operational mode.
 * @param mode The selected mode.
 * @return uint32_t Absolute flash address.
 */
uint32_t bootloader_get_slot_address(probe_mode_t mode);

/**
 * @brief Resolves the flash slot size for a given operational mode.
 * @param mode The selected mode.
 * @return uint32_t Size in bytes.
 */
uint32_t bootloader_get_slot_size(probe_mode_t mode);

/**
 * @brief Performs clean CPU handover and jumps to the target mode firmware.
 *
 * Sequence:
 * 1. Disables all interrupts.
 * 2. Resets peripherals to default hardware state.
 * 3. Relocates Vector Table Offset Register (SCB->VTOR).
 * 4. Sets Main Stack Pointer (MSP).
 * 5. Jumps to the target Reset_Handler.
 *
 * @param mode The target operational mode to execute.
 * @return Does not return if successful.
 */
void bootloader_jump_to_mode(probe_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_H */
