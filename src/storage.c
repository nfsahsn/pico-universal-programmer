/**
 * @file storage.c
 * @brief Manages RAM scratch register state and persistent Flash storage.
 */

#include "include/storage.h"
#include "config/memory_map.h"
#include "hardware/flash.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include <string.h>

/* Pointer to the persistent config in flash memory */
static const persistent_config_t *flash_config = (const persistent_config_t *)SLOT_CONFIG_ADDR;

static uint32_t calculate_checksum(const persistent_config_t *cfg) {
    return cfg->magic ^ cfg->default_mode ^ cfg->boot_count;
}

void storage_init(void) {
    /* No special hardware initialization required for watchdog scratch or flash read */
}

probe_mode_t storage_get_active_mode(void) {
    /* 1. Check Watchdog Scratch Register 0 (survives soft reset) */
    uint32_t scratch_val = watchdog_hw->scratch[0];
    if (IS_VALID_MODE_MAGIC(scratch_val)) {
        return EXTRACT_MODE_FROM_MAGIC(scratch_val);
    }

    /* 2. Fall back to Flash persistent default */
    return storage_get_default_mode();
}

void storage_set_active_mode(probe_mode_t mode) {
    if (mode >= MODE_COUNT) {
        mode = MODE_CMSIS_DAP;
    }
    /* Write to scratch register 0 - retained in RAM across soft reset */
    watchdog_hw->scratch[0] = MAKE_MODE_MAGIC(mode);
}

probe_mode_t storage_get_default_mode(void) {
    /* Validate flash sector */
    if (flash_config->magic == CONFIG_SECTOR_MAGIC) {
        if (flash_config->crc32 == calculate_checksum(flash_config)) {
            if (flash_config->default_mode < MODE_COUNT) {
                return (probe_mode_t)flash_config->default_mode;
            }
        }
    }
    /* Default fallback if unprogrammed */
    return MODE_CMSIS_DAP;
}

bool storage_save_default_mode(probe_mode_t mode) {
    if (mode >= MODE_COUNT) {
        return false;
    }

    /* Prepare buffer for one flash page (256 bytes) */
    uint8_t buffer[FLASH_PAGE_SIZE];
    memset(buffer, 0xFF, sizeof(buffer));

    persistent_config_t *cfg = (persistent_config_t *)buffer;
    cfg->magic = CONFIG_SECTOR_MAGIC;
    cfg->default_mode = (uint32_t)mode;
    cfg->boot_count = (flash_config->magic == CONFIG_SECTOR_MAGIC) ? (flash_config->boot_count + 1) : 1;
    cfg->crc32 = calculate_checksum(cfg);

    /* Erase sector (4096 bytes) and write page (256 bytes) with interrupts disabled */
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SLOT_CONFIG_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SLOT_CONFIG_OFFSET, buffer, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    /* Update current active mode scratch register to match */
    storage_set_active_mode(mode);

    /* Verify write */
    return (flash_config->magic == CONFIG_SECTOR_MAGIC &&
            flash_config->default_mode == (uint32_t)mode);
}
