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

#include "config/flash_layout.h"

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

/* Legacy configuration format, retained only for read migration. New writes
 * use the versioned CRC-32 journal in storage.c and both config sectors. */
#define CONFIG_SECTOR_MAGIC             (0x50524F42U) /* 'PROB' in ASCII */

typedef struct {
    uint32_t magic;           /**< Header validation magic                       */
    uint32_t default_mode;    /**< Stored default mode (probe_mode_t)            */
    uint32_t boot_count;      /**< Legacy save counter                           */
    uint32_t crc32;           /**< Legacy XOR checksum (not an actual CRC)       */
} persistent_config_t;

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_MAP_H */
