/**
 * @file storage.h
 * @brief Manages fast RAM-retained mode state and persistent Flash configuration.
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "config/memory_map.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the storage subsystem.
 */
void storage_init(void);

/**
 * @brief Retrieves the currently selected operational mode.
 * Checks retained RAM scratch register first; falls back to Flash default if invalid.
 * @return probe_mode_t Currently active mode.
 */
probe_mode_t storage_get_active_mode(void);

/**
 * @brief Sets the active mode for the next soft reboot (stored in Watchdog Scratch RAM).
 * Does NOT write to Flash, preventing flash wear during rapid button cycling.
 * @param mode The mode to set.
 */
void storage_set_active_mode(probe_mode_t mode);

/**
 * @brief Permanently commits the specified mode as the default power-on mode in Flash.
 * @param mode The mode to save as default.
 * @return bool True if successfully written and verified.
 */
bool storage_save_default_mode(probe_mode_t mode);

/**
 * @brief Reads the persistent default mode directly from Flash memory.
 * @return probe_mode_t Default mode stored in flash, or MODE_CMSIS_DAP if unprogrammed.
 */
probe_mode_t storage_get_default_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
