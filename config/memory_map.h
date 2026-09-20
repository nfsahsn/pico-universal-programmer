/**
 * @file memory_map.h
 * @brief Memory map and partition layout for the Pico Universal Programmer.
 *
 * Defines flash memory offsets, partition sizes, and magic tokens used
 * to switch between operational modes on the RP2040.
 */

#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Total RP2040 Flash Size: 2 MB (0x10000000 - 0x10200000) */
#define FLASH_BASE_ADDR                 (0x10000000U)
#define FLASH_TOTAL_SIZE                (2 * 1024 * 1024U)

/* -------------------------------------------------------------------------
 * Flash Partition Offsets & Sizes
 * ------------------------------------------------------------------------- */

/* Slot 0: Multi-Boot Chainloader & Supervisor (60 KB) */
#define SLOT_BOOTLOADER_OFFSET          (0x00000000U)
#define SLOT_BOOTLOADER_ADDR            (FLASH_BASE_ADDR + SLOT_BOOTLOADER_OFFSET)
#define SLOT_BOOTLOADER_SIZE            (60 * 1024U)

/* Configuration Sector: Persistent Mode Storage (4 KB - 1 Flash Sector) */
#define SLOT_CONFIG_OFFSET              (0x0000F000U)
#define SLOT_CONFIG_ADDR                (FLASH_BASE_ADDR + SLOT_CONFIG_OFFSET)
#define SLOT_CONFIG_SIZE                (4 * 1024U)

/* Slot 1: Mode 1 - Raspberry Pi Debug Probe / CMSIS-DAP v2 (512 KB) */
#define SLOT_1_CMSIS_DAP_OFFSET         (0x00010000U)
#define SLOT_1_CMSIS_DAP_ADDR           (FLASH_BASE_ADDR + SLOT_1_CMSIS_DAP_OFFSET)
#define SLOT_1_CMSIS_DAP_SIZE           (512 * 1024U)

/* Slot 2: Mode 2 - Black Magic Probe GDB Server (512 KB) */
#define SLOT_2_BLACKMAGIC_OFFSET        (0x00090000U)
#define SLOT_2_BLACKMAGIC_ADDR          (FLASH_BASE_ADDR + SLOT_2_BLACKMAGIC_OFFSET)
#define SLOT_2_BLACKMAGIC_SIZE          (512 * 1024U)

/* Slot 3: Mode 3 - PicoRVD WCH CH32V003 1-Wire SWIO (512 KB) */
#define SLOT_3_PICORVD_OFFSET           (0x00110000U)
#define SLOT_3_PICORVD_ADDR             (FLASH_BASE_ADDR + SLOT_3_PICORVD_OFFSET)
#define SLOT_3_PICORVD_SIZE             (512 * 1024U)

/* -------------------------------------------------------------------------
 * Mode Identifiers & Magic Numbers
 * ------------------------------------------------------------------------- */
typedef enum {
    MODE_CMSIS_DAP   = 0,  /**< Mode 1: ARM / ESP32 / RISC-V OpenOCD & Arduino */
    MODE_BLACKMAGIC  = 1,  /**< Mode 2: Black Magic Probe GDB Server            */
    MODE_PICORVD     = 2,  /**< Mode 3: WCH CH32V003 1-Wire SWIO programmer   */
    MODE_COUNT       = 3   /**< Total number of selectable modes               */
} probe_mode_t;

/* Magic tokens used in retained RAM scratch register to ensure validity */
#define MODE_MAGIC_MASK                 (0xFFFF0000U)
#define MODE_MAGIC_PREFIX               (0xDA000000U)

#define MAKE_MODE_MAGIC(mode)           (MODE_MAGIC_PREFIX | ((uint32_t)(mode) & 0xFFFFU))
#define EXTRACT_MODE_FROM_MAGIC(magic)  ((probe_mode_t)((magic) & 0xFFFFU))
#define IS_VALID_MODE_MAGIC(magic)      (((magic) & MODE_MAGIC_MASK) == MODE_MAGIC_PREFIX && \
                                         (EXTRACT_MODE_FROM_MAGIC(magic) < MODE_COUNT))

/* Persistent Flash Config Header */
#define CONFIG_SECTOR_MAGIC             (0x50524F42U) /* 'PROB' in ASCII */

typedef struct {
    uint32_t magic;           /**< Header validation magic                       */
    uint32_t default_mode;    /**< Stored default mode (probe_mode_t)            */
    uint32_t boot_count;      /**< Lifetime boot counter                         */
    uint32_t crc32;           /**< Checksum over configuration data              */
} persistent_config_t;

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_MAP_H */
