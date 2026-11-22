/*
 * Host unit & invariant test suite for Task 3:
 *   Deterministic PIO & Pin Engine Arbitrator (firmware/engine_owner.h)
 */

#ifdef NDEBUG
#error "Assertions must remain enabled in host tests"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config/board_pinout.h"
#include "firmware/engine_owner.h"

typedef enum {
    HAL_EVENT_NONE = 0,
    HAL_EVENT_DRAIN_PIO = 1,
    HAL_EVENT_TRISTATE = 2,
    HAL_EVENT_SWIO_PU_ON = 3,
    HAL_EVENT_SWIO_PU_OFF = 4,
    HAL_EVENT_VTARG_ON = 5,
    HAL_EVENT_VTARG_OFF = 6
} hal_event_t;

#define MAX_HAL_EVENTS 64u

static int g_drain_calls = 0;
static int g_tristate_calls = 0;
static int g_swio_pu_calls = 0;
static int g_vtarg_calls = 0;
static int g_hal_depth = 0;
static int g_max_hal_depth = 0;
static bool g_swio_pu_enabled = false;
static bool g_vtarg_enabled = false;
static hal_event_t g_events[MAX_HAL_EVENTS];
static size_t g_event_count = 0u;

static void enter_hal_cb(void) {
    ++g_hal_depth;
    if (g_hal_depth > g_max_hal_depth) {
        g_max_hal_depth = g_hal_depth;
    }
}

static void leave_hal_cb(void) {
    assert(g_hal_depth > 0);
    --g_hal_depth;
}

static void record_event(hal_event_t ev) {
    assert(g_event_count < MAX_HAL_EVENTS);
    g_events[g_event_count++] = ev;
}

static void reset_hal_spy(bool initial_vtarg) {
    g_drain_calls = 0;
    g_tristate_calls = 0;
    g_swio_pu_calls = 0;
    g_vtarg_calls = 0;
    g_hal_depth = 0;
    g_max_hal_depth = 0;
    g_swio_pu_enabled = false;
    g_vtarg_enabled = initial_vtarg;
    memset(g_events, 0, sizeof(g_events));
    g_event_count = 0u;
}

static void fake_drain_pio(void) {
    enter_hal_cb();
    ++g_drain_calls;
    record_event(HAL_EVENT_DRAIN_PIO);
    leave_hal_cb();
}

static void fake_tristate_debug_pins(void) {
    enter_hal_cb();
    ++g_tristate_calls;
    record_event(HAL_EVENT_TRISTATE);
    leave_hal_cb();
}

static void fake_set_swio_pullup(bool enabled) {
    enter_hal_cb();
    ++g_swio_pu_calls;
    g_swio_pu_enabled = enabled;
    record_event(enabled ? HAL_EVENT_SWIO_PU_ON : HAL_EVENT_SWIO_PU_OFF);
    leave_hal_cb();
}

static void fake_set_vtarg_power(bool enabled) {
    enter_hal_cb();
    ++g_vtarg_calls;
    g_vtarg_enabled = enabled;
    record_event(enabled ? HAL_EVENT_VTARG_ON : HAL_EVENT_VTARG_OFF);
    leave_hal_cb();
}

static const pu_pin_hal_t kFullHal = {
    .drain_pio = fake_drain_pio,
    .tristate_debug_pins = fake_tristate_debug_pins,
    .set_swio_pullup = fake_set_swio_pullup,
    .set_vtarg_power = fake_set_vtarg_power,
};

/* Review Focus #1: Simultaneous cross-interface contention must be rejected without touching pins. */
static void test_engine_owner_rejects_cross_interface_contention(void) {
    reset_hal_spy(true);
    pu_engine_init(&kFullHal);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_engine_fault_mask() == 0u);
    assert(g_event_count == 0u);

    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 500000u) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_SWIO);
    assert(g_swio_pu_enabled == true);
    assert(g_event_count == 1u && g_events[0] == HAL_EVENT_SWIO_PU_ON);

    /* Peer and same-engine acquisitions must return BUSY without touching any HAL callback. */
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1200u, 500000u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_acquire(PU_ENGINE_JTAG, 1250u, 500000u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_acquire(PU_ENGINE_RECOVERY, 1300u, 500000u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1350u, 500000u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_current() == PU_ENGINE_SWIO);
    assert(g_swio_pu_enabled == true);
    assert(g_event_count == 1u);

    /* Release executes ordered atomic teardown without disturbing VTARG power. */
    assert(pu_engine_release(PU_ENGINE_SWIO) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == true);
    assert(g_vtarg_calls == 0);
    assert(g_event_count == 4u);
    assert(g_events[1] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[2] == HAL_EVENT_TRISTATE);
    assert(g_events[3] == HAL_EVENT_SWIO_PU_OFF);

    /* Subsequent acquisition of SWD, JTAG, and RECOVERY succeeds cleanly once IDLE. */
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1500u, 500000u) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_SWD);
    assert(g_swio_pu_enabled == false);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1600u, 500000u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);

    assert(pu_engine_acquire(PU_ENGINE_JTAG, 2000u, 500000u) == PU_OK);
    assert(pu_engine_acquire(PU_ENGINE_RECOVERY, 2100u, 500000u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_release(PU_ENGINE_JTAG) == PU_OK);

    assert(pu_engine_acquire(PU_ENGINE_RECOVERY, 3000u, 500000u) == PU_OK);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 3100u, 500000u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_release(PU_ENGINE_RECOVERY) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

/* Review Focus #3: Mid-transfer USB reset and wire-deadline expiry tear down cleanly to IDLE. */
static void test_engine_owner_teardown_on_usb_reset_and_deadline(void) {
    reset_hal_spy(true);
    pu_engine_init(&kFullHal);

    assert(pu_engine_acquire(PU_ENGINE_JTAG, 1000u, 50000u) == PU_OK);
    pu_engine_tick(1000u);
    pu_engine_tick(50999u);
    assert(pu_engine_current() == PU_ENGINE_JTAG);
    assert(g_event_count == 0u);

    /* Exact deadline (1000 + 50000 == 51000) triggers atomic teardown. */
    pu_engine_tick(51000u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_drain_calls == 1 && g_tristate_calls == 1 && g_swio_pu_calls == 1);
    assert(g_vtarg_calls == 0);
    assert(g_events[0] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[1] == HAL_EVENT_TRISTATE);
    assert(g_events[2] == HAL_EVENT_SWIO_PU_OFF);

    /* Further ticks while IDLE must not re-fire HAL teardown callbacks. */
    pu_engine_tick(51001u);
    pu_engine_tick(200000u);
    assert(g_event_count == 3u);

    /* USB reset tears down active SWIO session and disables 1 kOhm pull-up. */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 60000u, 500000u) == PU_OK);
    assert(g_swio_pu_enabled == true);
    pu_engine_on_usb_reset();
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == true);
    assert(g_event_count == 4u);
    assert(g_events[1] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[2] == HAL_EVENT_TRISTATE);
    assert(g_events[3] == HAL_EVENT_SWIO_PU_OFF);

    /* Repeated USB reset while already IDLE is a quiet no-op. */
    pu_engine_on_usb_reset();
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_event_count == 4u);

    /* Zero timeout_us expires immediately on tick at the same timestamp. */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_RECOVERY, 75000u, 0u) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_RECOVERY);
    pu_engine_tick(75000u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_drain_calls == 1 && g_tristate_calls == 1 && g_swio_pu_calls == 1);
}

/* Review Focus #5: Latched hardware fault cuts VTARG, blocks acquires, and survives USB reset/tick. */
static void test_engine_owner_fault_latch_survives_usb_reset_and_tick_until_cleared(void) {
    reset_hal_spy(true);
    pu_engine_init(&kFullHal);

    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 500000u) == PU_OK);
    assert(g_swio_pu_enabled == true);
    assert(g_vtarg_enabled == true);

    /* Zero fault mask must fail-safe to PU_FAULT_UNSPECIFIED and cut VTARG power. */
    pu_engine_latch_fault(0u);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() == PU_FAULT_UNSPECIFIED);
    assert(pu_engine_fault_mask() != 0u);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == false);
    assert(g_event_count == 5u);
    assert(g_events[1] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[2] == HAL_EVENT_TRISTATE);
    assert(g_events[3] == HAL_EVENT_SWIO_PU_OFF);
    assert(g_events[4] == HAL_EVENT_VTARG_OFF);
    assert(pu_engine_clear_fault() == PU_OK);

    /* Non-zero fault mask executes immediate ordered shutdown including VTARG power-off. */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1500u, 500000u) == PU_OK);
    reset_hal_spy(true);
    g_swio_pu_enabled = true;
    pu_engine_latch_fault(PU_FAULT_OVERCURRENT);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() == PU_FAULT_OVERCURRENT);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == false);
    assert(g_event_count == 4u);
    assert(g_events[0] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[1] == HAL_EVENT_TRISTATE);
    assert(g_events[2] == HAL_EVENT_SWIO_PU_OFF);
    assert(g_events[3] == HAL_EVENT_VTARG_OFF);

    /* Subsequent fault bits accumulate via bitwise OR. */
    pu_engine_latch_fault(PU_FAULT_WATCHDOG | PU_FAULT_WIRE_STALL);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() ==
           (PU_FAULT_OVERCURRENT | PU_FAULT_WATCHDOG | PU_FAULT_WIRE_STALL));

    /* All valid engine acquisitions must fail with PU_ERR_FAULT_LATCHED without touching HAL. */
    size_t events_before = g_event_count;
    assert(pu_engine_acquire(PU_ENGINE_SWD, 2000u, 10000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_acquire(PU_ENGINE_JTAG, 2000u, 10000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 2000u, 10000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_acquire(PU_ENGINE_RECOVERY, 2000u, 10000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_renew_deadline(PU_ENGINE_SWIO, 2000u, 10000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_release(PU_ENGINE_SWIO) == PU_ERR_NOT_OWNER);
    assert(g_event_count == events_before);

    /* Neither USB reset nor deadline tick may clear a latched fault. */
    pu_engine_on_usb_reset();
    pu_engine_tick(9999999u);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() ==
           (PU_FAULT_OVERCURRENT | PU_FAULT_WATCHDOG | PU_FAULT_WIRE_STALL));
    assert(g_event_count == events_before);

    /* Explicit clear_fault transitions FAULT -> IDLE and clears fault_mask. */
    assert(pu_engine_clear_fault() == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_engine_fault_mask() == 0u);

    /* Idempotent clear_fault while IDLE succeeds; clear_fault while busy is rejected. */
    assert(pu_engine_clear_fault() == PU_OK);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 3000u, 50000u) == PU_OK);
    assert(pu_engine_clear_fault() == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_current() == PU_ENGINE_SWD);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);
}

static void test_engine_owner_rejects_wrong_owner_release_and_invalid_targets(void) {
    reset_hal_spy(true);
    pu_engine_init(&kFullHal);

    /* Invalid targets rejected when IDLE without side effects. */
    assert(pu_engine_acquire(PU_ENGINE_IDLE, 100u, 1000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_acquire(PU_ENGINE_FAULT, 100u, 1000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_acquire((pu_engine_state_t)99, 100u, 1000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_release(PU_ENGINE_IDLE) == PU_ERR_INVALID_ARG);
    assert(pu_engine_release(PU_ENGINE_FAULT) == PU_ERR_INVALID_ARG);
    assert(pu_engine_release((pu_engine_state_t)99) == PU_ERR_INVALID_ARG);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_ERR_NOT_OWNER);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_event_count == 0u);

    /* Wrong-owner release and invalid targets must not disturb active SWD owner. */
    assert(pu_engine_acquire(PU_ENGINE_SWD, 500u, 10000u) == PU_OK);
    assert(pu_engine_release(PU_ENGINE_JTAG) == PU_ERR_NOT_OWNER);
    assert(pu_engine_release(PU_ENGINE_SWIO) == PU_ERR_NOT_OWNER);
    assert(pu_engine_release(PU_ENGINE_RECOVERY) == PU_ERR_NOT_OWNER);
    assert(pu_engine_release(PU_ENGINE_IDLE) == PU_ERR_INVALID_ARG);
    assert(pu_engine_release(PU_ENGINE_FAULT) == PU_ERR_INVALID_ARG);
    assert(pu_engine_acquire(PU_ENGINE_IDLE, 600u, 1000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_acquire(PU_ENGINE_FAULT, 600u, 1000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_current() == PU_ENGINE_SWD);
    assert(g_event_count == 0u);

    /* Releasing FAULT pseudo-owner while faulted must never unlatch PU_ENGINE_FAULT. */
    pu_engine_latch_fault(0x02u);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_release(PU_ENGINE_FAULT) == PU_ERR_INVALID_ARG);
    assert(pu_engine_acquire(PU_ENGINE_IDLE, 700u, 1000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_acquire(PU_ENGINE_FAULT, 700u, 1000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_clear_fault() == PU_OK);
}

static void test_engine_owner_renew_deadline_and_u64_overflow_safety(void) {
    reset_hal_spy(true);
    pu_engine_init(&kFullHal);

    assert(pu_engine_renew_deadline(PU_ENGINE_SWD, 1000u, 5000u) == PU_ERR_NOT_OWNER);
    assert(pu_engine_renew_deadline(PU_ENGINE_IDLE, 1000u, 5000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_renew_deadline(PU_ENGINE_FAULT, 1000u, 5000u) == PU_ERR_INVALID_ARG);

    assert(pu_engine_acquire(PU_ENGINE_SWD, 1000u, 5000u) == PU_OK);
    assert(pu_engine_renew_deadline(PU_ENGINE_JTAG, 4000u, 50000u) == PU_ERR_NOT_OWNER);
    assert(pu_engine_renew_deadline(PU_ENGINE_IDLE, 4000u, 50000u) == PU_ERR_INVALID_ARG);

    /* Valid renewal extends deadline from 6000 -> 14000. */
    assert(pu_engine_renew_deadline(PU_ENGINE_SWD, 4000u, 10000u) == PU_OK);
    pu_engine_tick(6000u);
    pu_engine_tick(13999u);
    assert(pu_engine_current() == PU_ENGINE_SWD);
    pu_engine_tick(14000u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* Wrap-around across UINT64_MAX -> 0 on both acquire and renew_deadline. */
    const uint64_t near_max = UINT64_MAX - 25u; /* deadline wraps to 74u */
    assert(pu_engine_acquire(PU_ENGINE_JTAG, near_max, 100u) == PU_OK);
    pu_engine_tick(near_max);
    pu_engine_tick(UINT64_MAX);
    pu_engine_tick(0u);
    pu_engine_tick(73u);
    assert(pu_engine_current() == PU_ENGINE_JTAG);

    /* Renew near UINT64_MAX with max uint32_t timeout_us (UINT32_MAX). */
    assert(pu_engine_renew_deadline(PU_ENGINE_JTAG, UINT64_MAX - 5u, UINT32_MAX) == PU_OK);
    const uint64_t wrapped_max_deadline = (UINT64_MAX - 5u) + (uint64_t)UINT32_MAX;
    pu_engine_tick(UINT64_MAX);
    pu_engine_tick(0u);
    pu_engine_tick(wrapped_max_deadline - 1u);
    assert(pu_engine_current() == PU_ENGINE_JTAG);
    pu_engine_tick(wrapped_max_deadline);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* Boundary: acquire at exact UINT64_MAX with timeout_us == 0u expires at UINT64_MAX. */
    assert(pu_engine_acquire(PU_ENGINE_SWD, UINT64_MAX, 0u) == PU_OK);
    pu_engine_tick(UINT64_MAX);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

static void init_from_transient_stack_hal(void) {
    pu_pin_hal_t transient_hal = {
        .drain_pio = NULL,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = NULL,
        .set_vtarg_power = NULL,
    };
    pu_engine_init(&transient_hal);
    memset(&transient_hal, 0xA5, sizeof(transient_hal));
}

static void test_engine_owner_null_hal_and_partial_hal_callbacks_safe(void) {
    reset_hal_spy(false);

    /* Completely NULL HAL pointer must operate safely through all transitions. */
    pu_engine_init(NULL);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 100u, 50u) == PU_OK);
    pu_engine_tick(150u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 200u, 50u) == PU_OK);
    pu_engine_on_usb_reset();
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    pu_engine_latch_fault(0x10u);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() == 0x10u);
    assert(pu_engine_clear_fault() == PU_OK);
    assert(g_event_count == 0u);

    /* PU_BOARD_PICO profile HAL without set_vtarg_power (NULL) must fault safely. */
    const pu_pin_hal_t pico_hal = {
        .drain_pio = fake_drain_pio,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = fake_set_swio_pullup,
        .set_vtarg_power = NULL,
    };
    pu_engine_init(&pico_hal);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 500u, 1000u) == PU_OK);
    pu_engine_latch_fault(0x08u);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(g_drain_calls == 1 && g_tristate_calls == 1 && g_swio_pu_calls == 1);
    assert(g_vtarg_calls == 0);
    assert(pu_engine_clear_fault() == PU_OK);

    /* Partial HAL passed on stack must be copied by value inside pu_engine_init. */
    reset_hal_spy(false);
    init_from_transient_stack_hal();
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 1000u) == PU_OK);
    assert(pu_engine_release(PU_ENGINE_SWIO) == PU_OK);
    assert(g_tristate_calls == 1);
    assert(g_drain_calls == 0 && g_swio_pu_calls == 0 && g_vtarg_calls == 0);
}

static pu_status_t g_nested_acquire_status = PU_OK;
static pu_status_t g_nested_clear_status = PU_OK;

static void drain_with_concurrent_acquire_attempt(void) {
    enter_hal_cb();
    ++g_drain_calls;
    /* Simulates Core 0 attempting acquire/clear_fault while Core 1 is mid-teardown. */
    g_nested_acquire_status = pu_engine_acquire(PU_ENGINE_JTAG, 5000u, 1000u);
    g_nested_clear_status = pu_engine_clear_fault();
    leave_hal_cb();
}

static void swio_pu_with_concurrent_overcurrent_irq(bool enabled) {
    enter_hal_cb();
    ++g_swio_pu_calls;
    if (enabled) {
        /* Simulates VTARG_FAULT_N IRQ firing on Core 0 before Core 1 finishes GPIO setup. */
        pu_engine_latch_fault(0x20u);
        g_swio_pu_enabled = true;
        record_event(HAL_EVENT_SWIO_PU_ON);
    } else {
        g_swio_pu_enabled = false;
        record_event(HAL_EVENT_SWIO_PU_OFF);
    }
    leave_hal_cb();
}

static void test_engine_owner_inflight_teardown_and_irq_preemption_safety(void) {
    reset_hal_spy(true);
    const pu_pin_hal_t race_release_hal = {
        .drain_pio = drain_with_concurrent_acquire_attempt,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = fake_set_swio_pullup,
        .set_vtarg_power = fake_set_vtarg_power,
    };
    pu_engine_init(&race_release_hal);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1000u, 10000u) == PU_OK);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);
    assert(g_nested_acquire_status == PU_ERR_ENGINE_BUSY);
    assert(g_nested_clear_status == PU_ERR_ENGINE_BUSY);
    assert(g_max_hal_depth == 1);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_engine_acquire(PU_ENGINE_JTAG, 6000u, 10000u) == PU_OK);
    assert(pu_engine_release(PU_ENGINE_JTAG) == PU_OK);

    reset_hal_spy(true);
    const pu_pin_hal_t race_fault_hal = {
        .drain_pio = fake_drain_pio,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = swio_pu_with_concurrent_overcurrent_irq,
        .set_vtarg_power = fake_set_vtarg_power,
    };
    pu_engine_init(&race_fault_hal);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 2000u, 10000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() == 0x20u);
    assert(g_max_hal_depth == 1);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == false);
    assert(g_event_count == 5u);
    assert(g_events[0] == HAL_EVENT_SWIO_PU_ON);
    assert(g_events[1] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[2] == HAL_EVENT_TRISTATE);
    assert(g_events[3] == HAL_EVENT_SWIO_PU_OFF);
    assert(g_events[4] == HAL_EVENT_VTARG_OFF);
    assert(pu_engine_clear_fault() == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

static void swio_pu_with_concurrent_usb_reset(bool enabled) {
    enter_hal_cb();
    ++g_swio_pu_calls;
    record_event(enabled ? HAL_EVENT_SWIO_PU_ON : HAL_EVENT_SWIO_PU_OFF);
    if (enabled) {
        g_swio_pu_enabled = true;
        pu_engine_on_usb_reset();
    } else {
        g_swio_pu_enabled = false;
    }
    leave_hal_cb();
}

static void swio_pu_with_concurrent_deadline_tick(bool enabled) {
    enter_hal_cb();
    ++g_swio_pu_calls;
    record_event(enabled ? HAL_EVENT_SWIO_PU_ON : HAL_EVENT_SWIO_PU_OFF);
    if (enabled) {
        g_swio_pu_enabled = true;
        pu_engine_tick(6000u);
    } else {
        g_swio_pu_enabled = false;
    }
    leave_hal_cb();
}

static void swio_pu_with_reset_then_zero_fault(bool enabled) {
    enter_hal_cb();
    ++g_swio_pu_calls;
    record_event(enabled ? HAL_EVENT_SWIO_PU_ON : HAL_EVENT_SWIO_PU_OFF);
    if (enabled) {
        g_swio_pu_enabled = true;
        pu_engine_on_usb_reset();
        pu_engine_latch_fault(0u);
    } else {
        g_swio_pu_enabled = false;
    }
    leave_hal_cb();
}

static void test_engine_owner_swio_acquire_preempted_by_reset_or_tick_never_returns_ok(void) {
    /* USB reset arriving during set_swio_pullup(true) must abort acquire and tear down cleanly. */
    reset_hal_spy(true);
    const pu_pin_hal_t reset_race_hal = {
        .drain_pio = fake_drain_pio,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = swio_pu_with_concurrent_usb_reset,
        .set_vtarg_power = fake_set_vtarg_power,
    };
    pu_engine_init(&reset_race_hal);
    pu_status_t rc_reset = pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 500000u);
    assert(rc_reset != PU_OK);
    assert(rc_reset == PU_ERR_ENGINE_BUSY || rc_reset == PU_ERR_TIMEOUT);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_max_hal_depth == 1);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == true);
    assert(g_event_count == 4u);
    assert(g_events[0] == HAL_EVENT_SWIO_PU_ON);
    assert(g_events[1] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[2] == HAL_EVENT_TRISTATE);
    assert(g_events[3] == HAL_EVENT_SWIO_PU_OFF);

    /* Wire deadline tick arriving during set_swio_pullup(true) must abort acquire and tear down. */
    reset_hal_spy(true);
    const pu_pin_hal_t tick_race_hal = {
        .drain_pio = fake_drain_pio,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = swio_pu_with_concurrent_deadline_tick,
        .set_vtarg_power = fake_set_vtarg_power,
    };
    pu_engine_init(&tick_race_hal);
    pu_status_t rc_tick = pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 5000u);
    assert(rc_tick != PU_OK);
    assert(rc_tick == PU_ERR_ENGINE_BUSY || rc_tick == PU_ERR_TIMEOUT);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_max_hal_depth == 1);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == true);
    assert(g_event_count == 4u);
    assert(g_events[0] == HAL_EVENT_SWIO_PU_ON);
    assert(g_events[1] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[2] == HAL_EVENT_TRISTATE);
    assert(g_events[3] == HAL_EVENT_SWIO_PU_OFF);

    /* USB reset + fault(0) during set_swio_pullup(true) coalesce into one fault teardown. */
    reset_hal_spy(true);
    const pu_pin_hal_t reset_fault_hal = {
        .drain_pio = fake_drain_pio,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = swio_pu_with_reset_then_zero_fault,
        .set_vtarg_power = fake_set_vtarg_power,
    };
    pu_engine_init(&reset_fault_hal);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 500000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() == PU_FAULT_UNSPECIFIED);
    assert(g_max_hal_depth == 1);
    assert(g_drain_calls == 1);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == false);
    assert(pu_engine_clear_fault() == PU_OK);
}

static pu_engine_state_t g_state_during_drain = PU_ENGINE_IDLE;
static pu_status_t g_acquire_during_drain = PU_OK;
static pu_status_t g_renew_during_drain = PU_OK;
static pu_status_t g_release_during_drain = PU_OK;

static void drain_observing_mid_teardown_state(void) {
    enter_hal_cb();
    ++g_drain_calls;
    record_event(HAL_EVENT_DRAIN_PIO);
    g_state_during_drain = pu_engine_current();
    g_acquire_during_drain = pu_engine_acquire(PU_ENGINE_RECOVERY, 9000u, 1000u);
    g_renew_during_drain = pu_engine_renew_deadline(g_state_during_drain, 9000u, 50000u);
    g_release_during_drain = pu_engine_release(g_state_during_drain);
    /* Concurrent USB reset and tick mid-teardown must be idempotent no-ops. */
    pu_engine_on_usb_reset();
    pu_engine_tick(999999u);
    leave_hal_cb();
}

static void test_engine_owner_does_not_publish_idle_before_teardown_completes(void) {
    const pu_pin_hal_t observe_hal = {
        .drain_pio = drain_observing_mid_teardown_state,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = fake_set_swio_pullup,
        .set_vtarg_power = fake_set_vtarg_power,
    };
    pu_engine_init(&observe_hal);

    /* 1. pu_engine_release() must keep active owner visible until teardown finishes. */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1000u, 50000u) == PU_OK);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);
    assert(g_state_during_drain == PU_ENGINE_SWD);
    assert(g_state_during_drain != PU_ENGINE_IDLE);
    assert(g_acquire_during_drain == PU_ERR_ENGINE_BUSY);
    assert(g_renew_during_drain == PU_ERR_NOT_OWNER);
    assert(g_release_during_drain == PU_ERR_NOT_OWNER);
    assert(g_max_hal_depth == 1);
    assert(g_drain_calls == 1);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* 2. pu_engine_tick() must keep active owner visible until teardown finishes. */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_JTAG, 2000u, 10000u) == PU_OK);
    pu_engine_tick(12000u);
    assert(g_state_during_drain == PU_ENGINE_JTAG);
    assert(g_state_during_drain != PU_ENGINE_IDLE);
    assert(g_acquire_during_drain == PU_ERR_ENGINE_BUSY);
    assert(g_renew_during_drain == PU_ERR_NOT_OWNER);
    assert(g_release_during_drain == PU_ERR_NOT_OWNER);
    assert(g_max_hal_depth == 1);
    assert(g_drain_calls == 1);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* 3. pu_engine_on_usb_reset() must keep active owner visible until teardown finishes. */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 3000u, 10000u) == PU_OK);
    pu_engine_on_usb_reset();
    assert(g_state_during_drain == PU_ENGINE_SWIO);
    assert(g_state_during_drain != PU_ENGINE_IDLE);
    assert(g_acquire_during_drain == PU_ERR_ENGINE_BUSY);
    assert(g_renew_during_drain == PU_ERR_NOT_OWNER);
    assert(g_release_during_drain == PU_ERR_NOT_OWNER);
    assert(g_max_hal_depth == 1);
    assert(g_drain_calls == 1);
    assert(g_swio_pu_enabled == false);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

static void drain_latching_wire_stall_fault(void) {
    enter_hal_cb();
    ++g_drain_calls;
    /* Guard against unbounded recursion so an unfixed bug fails deterministically. */
    assert(g_drain_calls <= 4);
    record_event(HAL_EVENT_DRAIN_PIO);
    pu_engine_latch_fault(PU_FAULT_WIRE_STALL);
    leave_hal_cb();
}

static void test_engine_owner_fault_inside_drain_pio_coalesces_without_recursion(void) {
    const pu_pin_hal_t fault_in_drain_hal = {
        .drain_pio = drain_latching_wire_stall_fault,
        .tristate_debug_pins = fake_tristate_debug_pins,
        .set_swio_pullup = fake_set_swio_pullup,
        .set_vtarg_power = fake_set_vtarg_power,
    };
    pu_engine_init(&fault_in_drain_hal);

    /*
     * (a1) Direct pu_engine_latch_fault() where drain_pio() also calls pu_engine_latch_fault()
     * must not recurse and must execute exactly one clean fault teardown cutting VTARG.
     */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 50000u) == PU_OK);
    assert(g_swio_pu_enabled == true);
    pu_engine_latch_fault(PU_FAULT_OVERCURRENT);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() == (PU_FAULT_OVERCURRENT | PU_FAULT_WIRE_STALL));
    assert(g_max_hal_depth == 1);
    assert(g_drain_calls == 1);
    assert(g_tristate_calls == 1);
    assert(g_swio_pu_calls == 2); /* 1 ON during acquire + 1 OFF during fault teardown */
    assert(g_vtarg_calls == 1);
    assert(g_swio_pu_enabled == false);
    assert(g_vtarg_enabled == false);
    assert(g_event_count == 5u);
    assert(g_events[1] == HAL_EVENT_DRAIN_PIO);
    assert(g_events[2] == HAL_EVENT_TRISTATE);
    assert(g_events[3] == HAL_EVENT_SWIO_PU_OFF);
    assert(g_events[4] == HAL_EVENT_VTARG_OFF);
    assert(pu_engine_clear_fault() == PU_OK);

    /*
     * (a2) Normal release teardown whose drain_pio() latches a fault must not recurse,
     * must run a coalesced fault teardown that cuts VTARG, and must stay in PU_ENGINE_FAULT.
     */
    reset_hal_spy(true);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 2000u, 50000u) == PU_OK);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_fault_mask() == PU_FAULT_WIRE_STALL);
    assert(g_max_hal_depth == 1);
    assert(g_vtarg_calls == 1);
    assert(g_vtarg_enabled == false);
    assert(g_swio_pu_enabled == false);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 3000u, 50000u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_clear_fault() == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

int main(void) {
    test_engine_owner_rejects_cross_interface_contention();
    test_engine_owner_teardown_on_usb_reset_and_deadline();
    test_engine_owner_fault_latch_survives_usb_reset_and_tick_until_cleared();
    test_engine_owner_rejects_wrong_owner_release_and_invalid_targets();
    test_engine_owner_renew_deadline_and_u64_overflow_safety();
    test_engine_owner_null_hal_and_partial_hal_callbacks_safe();
    test_engine_owner_inflight_teardown_and_irq_preemption_safety();
    test_engine_owner_swio_acquire_preempted_by_reset_or_tick_never_returns_ok();
    test_engine_owner_does_not_publish_idle_before_teardown_completes();
    test_engine_owner_fault_inside_drain_pio_coalesces_without_recursion();
    printf("test_engine_owner: all 10 test suites passed\n");
    return 0;
}
