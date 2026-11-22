/*
 * IF 5 Vendor Control, Telemetry & Hardware Fault Protection for Pico Universal.
 *
 * Binary framing (little-endian):
 *   Request:  [cmd_id, payload...]
 *   Response: [cmd_id, pu_ctrl_status_t, payload...]
 */

#ifndef PU_FIRMWARE_CONTROL_TELEMETRY_H
#define PU_FIRMWARE_CONTROL_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "firmware/engine_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PU_FW_VERSION_MAJOR        1u
#define PU_FW_VERSION_MINOR        0u
#define PU_FW_VERSION_PATCH        0u

#define PU_CTRL_BOOTSEL_MAGIC      0xB0075E10u

/* Capability bitmask returned by PU_CTRL_CMD_GET_INFO */
#define PU_CAP_LEVEL_SHIFTER       (1u << 0)
#define PU_CAP_VTARG_SWITCH        (1u << 1)
#define PU_CAP_ADC_TELEMETRY       (1u << 2)

/* Self-test bitmask returned by PU_CTRL_CMD_SELF_TEST */
#define PU_SELFTEST_ENGINE_OK      (1u << 0)
#define PU_SELFTEST_JOURNAL_OK     (1u << 1)
#define PU_SELFTEST_ADC_RAIL_OK    (1u << 2)
#define PU_SELFTEST_NO_FAULT_OK    (1u << 3)

typedef enum {
    PU_CTRL_CMD_GET_INFO      = 0x01u,
    PU_CTRL_CMD_GET_TELEMETRY = 0x02u,
    PU_CTRL_CMD_SET_VTARG     = 0x03u,
    PU_CTRL_CMD_CLEAR_FAULT   = 0x04u,
    PU_CTRL_CMD_CH32_UNBRICK  = 0x05u,
    PU_CTRL_CMD_ENTER_BOOTSEL = 0x06u,
    PU_CTRL_CMD_SELF_TEST     = 0x07u
} pu_ctrl_cmd_t;

typedef enum {
    PU_CTRL_STATUS_OK            = 0x00u,
    PU_CTRL_STATUS_INVALID_ARG   = 0x01u,
    PU_CTRL_STATUS_UNSUPPORTED   = 0x02u,
    PU_CTRL_STATUS_BUSY          = 0x03u,
    PU_CTRL_STATUS_FAULT_LATCHED = 0x04u,
    PU_CTRL_STATUS_ERROR         = 0x05u
} pu_ctrl_status_t;

typedef struct {
    bool board_is_pro_pcb;
    bool has_level_shifter;
    bool has_vtarg_switch;
    bool has_adc_telemetry;
    uint8_t active_slot;
    void (*set_vtarg_power)(bool enabled);
    bool (*is_overcurrent_active)(void);
    uint16_t (*read_adc_vtref_raw)(void);
    uint16_t (*read_adc_itarg_raw)(void);
    pu_status_t (*request_ch32_unbrick)(uint64_t now_us);
    void (*enter_bootsel)(void);
    uint64_t (*now_us)(void);
} pu_ctrl_hal_t;

void pu_ctrl_init(const pu_ctrl_hal_t *hal);
#ifdef PU_HOST_TEST
void pu_ctrl_init_for_test(void);
#endif

size_t pu_ctrl_handle_request(const uint8_t *req,
                              size_t len,
                              uint8_t *resp,
                              size_t cap);

/* Non-consuming check + consuming trigger so caller flushes USB reply BEFORE ROM jump */
bool pu_ctrl_is_bootsel_pending(void);
bool pu_ctrl_poll_deferred_bootsel(void);

/* Invoked by VTARG_FAULT_N falling-edge IRQ on PU_BOARD_PRO_PCB (Review Focus #5) */
void pu_telemetry_on_overcurrent_irq(void);

/* ADC conversion helpers (12-bit 0..4095, 3.3V ADC VREF) */
uint16_t pu_telemetry_adc_to_vtref_mv(uint16_t raw_adc);
uint16_t pu_telemetry_adc_to_itarg_ma(uint16_t raw_adc);

#if !defined(PU_HOST_TEST)
void pu_ctrl_rp2040_init(uint8_t active_slot,
                         pu_status_t (*request_ch32_unbrick_cb)(uint64_t now_us));
#endif

#ifdef __cplusplus
}
#endif

#endif /* PU_FIRMWARE_CONTROL_TELEMETRY_H */
