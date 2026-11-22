/*
 * Deterministic PIO & Pin Engine Arbitrator (engine_owner FSM) for Pico Universal.
 * Guarantees strict mutual exclusion across SWD, JTAG, SWIO, and RECOVERY engines,
 * atomic pin/PIO teardown on release/timeout/USB-reset, and sticky fault latching.
 */

#ifndef PU_FIRMWARE_ENGINE_OWNER_H
#define PU_FIRMWARE_ENGINE_OWNER_H

#include <stdbool.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("config/board_pinout.h")
#include "config/board_pinout.h"
#else
#include "../config/board_pinout.h"
#endif
#else
#include "config/board_pinout.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PU_OK = 0,
    PU_ERR_INVALID_ARG = -1,
    PU_ERR_ENGINE_BUSY = -2,
    PU_ERR_NOT_OWNER = -3,
    PU_ERR_FAULT_LATCHED = -4,
    PU_ERR_TIMEOUT = -5
} pu_status_t;

typedef enum {
    PU_ENGINE_IDLE = 0,
    PU_ENGINE_SWD = 1,
    PU_ENGINE_JTAG = 2,
    PU_ENGINE_SWIO = 3,
    PU_ENGINE_RECOVERY = 4,
    PU_ENGINE_FAULT = 5
} pu_engine_state_t;

#define PU_FAULT_OVERCURRENT  (1u << 0)
#define PU_FAULT_WATCHDOG     (1u << 1)
#define PU_FAULT_WIRE_STALL   (1u << 2)
#define PU_FAULT_UNSPECIFIED  (1u << 31)

typedef struct {
    void (*drain_pio)(void);
    void (*tristate_debug_pins)(void);
    void (*set_swio_pullup)(bool enabled);
    void (*set_vtarg_power)(bool enabled); /* optional / nullable */
} pu_pin_hal_t;

void pu_engine_init(const pu_pin_hal_t *hal);
pu_status_t pu_engine_acquire(pu_engine_state_t target, uint64_t now_us, uint32_t timeout_us);
pu_status_t pu_engine_renew_deadline(pu_engine_state_t expected_owner,
                                     uint64_t now_us,
                                     uint32_t timeout_us);
pu_status_t pu_engine_release(pu_engine_state_t expected_owner);
void pu_engine_tick(uint64_t now_us);
void pu_engine_on_usb_reset(void);
void pu_engine_latch_fault(uint32_t fault_mask);
uint32_t pu_engine_fault_mask(void);
pu_status_t pu_engine_clear_fault(void);
pu_engine_state_t pu_engine_current(void);

#ifdef __cplusplus
}
#endif

#endif /* PU_FIRMWARE_ENGINE_OWNER_H */
