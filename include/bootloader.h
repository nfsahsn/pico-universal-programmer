#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "config/memory_map.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Invalid modes return zero, never a different mode's partition. */
uint32_t bootloader_get_slot_address(probe_mode_t mode);
uint32_t bootloader_get_slot_size(probe_mode_t mode);
bool bootloader_mode_is_valid(probe_mode_t mode);
bool bootloader_recovery_required(void);

/* Reject invalid images, otherwise request normal flash reboot and early
 * handover before SDK initialization. Returns false on rejection; a successful
 * request does not return. */
bool bootloader_jump_to_mode(probe_mode_t mode);

#ifdef __cplusplus
}
#endif
#endif
