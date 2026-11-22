/*
 * Hardened WCH CH32V003 1-Wire SWIO Engine for Pico Universal:
 *   - PIO1 open-drain asynchronous PWM physical layer (singlewire.pio)
 *   - Strict QingKe V2 Table 3-5 DMSTATUS disambiguation (0xFFFFFFFF / 0x00000000 safe)
 *   - Non-destructive PUID1 pre-flight check + verified SHDWCFGR.OUTEN == 0 reset
 *   - Hardened GDB RSP stub + cross-page 4-byte software breakpoints (SoftBreak)
 *   - Deterministic engine_owner arbitration (PU_ENGINE_SWIO / PU_ENGINE_RECOVERY)
 */

#ifndef PU_FIRMWARE_SWIO_ENGINE_H
#define PU_FIRMWARE_SWIO_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("firmware/engine_owner.h")
#include "firmware/engine_owner.h"
#else
#include "engine_owner.h"
#endif
#else
#include "firmware/engine_owner.h"
#endif

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PU_CH32_OK = 0,
    PU_CH32_ERR_BUSY,
    PU_CH32_ERR_FAULT,
    PU_CH32_ERR_NO_REPLY,
    PU_CH32_ERR_ALREADY_HALTED,
    PU_CH32_ERR_DETACH_FAILED,
    PU_CH32_ERR_UNEXPECTED_STATE,
    PU_CH32_ERR_FLASH_FAIL
} pu_ch32_status_t;

/* 5-cause silent-bus remediation bitmask */
#define PU_CH32_DIAG_POWER           (1u << 0)
#define PU_CH32_DIAG_GROUND          (1u << 1)
#define PU_CH32_DIAG_PULLUP          (1u << 2)
#define PU_CH32_DIAG_PD1_REUSE       (1u << 3)
#define PU_CH32_DIAG_READ_PROTECT    (1u << 4)
#define PU_CH32_DIAG_ALL_FIVE_CAUSES \
    (PU_CH32_DIAG_POWER | PU_CH32_DIAG_GROUND | PU_CH32_DIAG_PULLUP | \
     PU_CH32_DIAG_PD1_REUSE | PU_CH32_DIAG_READ_PROTECT)

/* CH32V003 flash geometry & timing bounds */
#define PU_CH32_FLASH_BASE           0x08000000u
#define PU_CH32_FLASH_SIZE           0x00004000u
#define PU_CH32_FLASH_PAGE_SIZE      64u
#define PU_CH32_FLASH_PAGE_COUNT     (PU_CH32_FLASH_SIZE / PU_CH32_FLASH_PAGE_SIZE)
#define PU_CH32_SRAM_BASE            0x20000000u
#define PU_CH32_SRAM_SIZE            0x00000800u

#define PU_SWIO_WIRE_TIMEOUT_US      25000u
#define PU_SWIO_CHECK_TIMEOUT_US     500000u
#define PU_SWIO_SESSION_TIMEOUT_US   5000000u
#define PU_SWIO_RECOVERY_TIMEOUT_US  1000000u

typedef struct {
    pu_ch32_status_t status;
    uint32_t chip_id;
    uint32_t flash_kb;
    uint32_t flash_writes;
    uint32_t diagnosis_mask;
    bool resumed_running;
    bool outen_disabled_verified;
} pu_ch32_check_result_t;

typedef struct {
    uint32_t sys_clock_hz;
    void (*bus_open)(void);
    void (*bus_close)(void);
    void (*bus_reset)(void);
    bool (*read_reg)(uint8_t reg_addr, uint32_t *out_val, uint32_t timeout_us);
    bool (*write_reg)(uint8_t reg_addr, uint32_t val, uint32_t timeout_us);
    void (*set_vtarg_power)(bool enabled);
    void (*delay_us)(uint32_t delay_us);
    uint64_t (*now_us)(void);
} pu_swio_wire_hal_t;

void pu_swio_init(const pu_swio_wire_hal_t *hal);
pu_ch32_check_result_t pu_ch32_non_destructive_check(uint64_t now_us);
size_t pu_ch32_gdb_feed(const uint8_t *in,
                        size_t len,
                        uint8_t *out,
                        size_t out_cap,
                        uint64_t now_us);
size_t pu_ch32_gdb_poll(uint8_t *out,
                        size_t out_cap,
                        uint64_t now_us);
pu_status_t pu_ch32_power_cycle_unbrick(uint64_t now_us);
void pu_ch32_on_disconnect(void);

#ifdef PU_HOST_TEST
typedef enum {
    PU_WIRE_HEALTHY_CH32V003 = 0,
    PU_WIRE_SILENT_HIGH_FF,
    PU_WIRE_SILENT_LOW_00,
    PU_WIRE_ALREADY_HALTED,
    PU_WIRE_OUTEN_RESET_FAIL,
    PU_WIRE_RESUME_FAIL
} pu_fake_wire_mode_t;

void pu_swio_set_fake_wire_mode(pu_fake_wire_mode_t mode);
pu_status_t pu_ch32_gdb_send_checked(const char *packet_without_csum,
                                     uint8_t *out,
                                     size_t out_cap,
                                     uint64_t now_us);
bool pu_swio_fake_flash_matches_original(void);
void pu_swio_inject_fake_flash_write_fail(int fail_on_write_index);
const uint16_t *pu_swio_get_pio_instructions(size_t *out_count);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PU_FIRMWARE_SWIO_ENGINE_H */
