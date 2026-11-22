/*
 * Host unit tests for Task 6:
 *   - IF 5 Control/Telemetry protocol (GET_INFO, GET_TELEMETRY, SET_VTARG,
 *     CLEAR_FAULT, CH32_UNBRICK, ENTER_BOOTSEL magic guard, SELF_TEST)
 *   - Review Focus #5: VTARG overcurrent IRQ latches engine_owner FAULT,
 *     cuts VTARG immediately, blocks debug acquisition & VTARG re-enable,
 *     refuses CLEAR_FAULT while hardware fault pin remains active, and
 *     recovers cleanly once hardware fault clears.
 *   - Concurrent DMA Target UART Bridge (IF 3-4) ring buffers, drop counters,
 *     live line-coding bounds (300..3000000 baud), and zero collision with
 *     active engine_owner debug sessions.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "firmware/control_telemetry.h"
#include "firmware/engine_owner.h"
#include "firmware/uart_bridge.h"

static bool g_vtarg_enabled = false;
static bool g_hw_fault_asserted = false;
static bool g_sync_oc_on_vtarg_on = false;
static bool g_bootsel_called = false;
static bool g_unbrick_hook_called = false;
static uint16_t g_fake_adc_vtref_raw = 2482u; /* ~3.300 V via 2:1 divider on 3.3 V ADC ref */
static uint16_t g_fake_adc_itarg_raw = 620u;  /* ~250 mA via INA180A1 (20 V/V) * 0.1 Ohm */
static int g_tristate_calls = 0;

static void fake_drain_pio(void) {}
static void fake_tristate(void) { g_tristate_calls++; }
static void fake_set_swio_pu(bool en) { (void)en; }
static void fake_set_vtarg(bool en) {
    g_vtarg_enabled = en;
    if (en && g_sync_oc_on_vtarg_on) {
        g_hw_fault_asserted = true;
        pu_telemetry_on_overcurrent_irq();
    }
}

static bool fake_read_fault_active(void) { return g_hw_fault_asserted; }
static uint16_t fake_read_adc_vtref(void) { return g_fake_adc_vtref_raw; }
static uint16_t fake_read_adc_itarg(void) { return g_fake_adc_itarg_raw; }
static pu_status_t fake_request_unbrick(uint64_t now_us) {
    (void)now_us;
    g_unbrick_hook_called = true;
    return PU_OK;
}
static void fake_enter_bootsel(void) { g_bootsel_called = true; }
static uint64_t fake_now_us(void) { return 50000u; }

static const pu_pin_hal_t kPinHal = {
    .drain_pio = fake_drain_pio,
    .tristate_debug_pins = fake_tristate,
    .set_swio_pullup = fake_set_swio_pu,
    .set_vtarg_power = fake_set_vtarg,
};

static const pu_ctrl_hal_t kProCtrlHal = {
    .board_is_pro_pcb = true,
    .has_level_shifter = true,
    .has_vtarg_switch = true,
    .has_adc_telemetry = true,
    .active_slot = 0u,
    .set_vtarg_power = fake_set_vtarg,
    .is_overcurrent_active = fake_read_fault_active,
    .read_adc_vtref_raw = fake_read_adc_vtref,
    .read_adc_itarg_raw = fake_read_adc_itarg,
    .request_ch32_unbrick = fake_request_unbrick,
    .enter_bootsel = fake_enter_bootsel,
    .now_us = fake_now_us,
};

static const pu_ctrl_hal_t kPicoCtrlHal = {
    .board_is_pro_pcb = false,
    .has_level_shifter = false,
    .has_vtarg_switch = false,
    .has_adc_telemetry = false,
    .active_slot = 0u,
    .set_vtarg_power = NULL,
    .is_overcurrent_active = NULL,
    .read_adc_vtref_raw = NULL,
    .read_adc_itarg_raw = NULL,
    .request_ch32_unbrick = NULL,
    .enter_bootsel = fake_enter_bootsel,
    .now_us = fake_now_us,
};

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_telemetry_overcurrent_forces_engine_fault(void) {
    g_vtarg_enabled = false;
    g_hw_fault_asserted = false;
    g_tristate_calls = 0;

    pu_engine_init(&kPinHal);
    pu_ctrl_init(&kProCtrlHal);

    /* Enable VTARG and acquire SWD */
    uint8_t set_on[2] = { PU_CTRL_CMD_SET_VTARG, 1u };
    uint8_t resp[64] = {0};
    size_t n = pu_ctrl_handle_request(set_on, sizeof(set_on), resp, sizeof(resp));
    assert(n == 3u && resp[0] == PU_CTRL_CMD_SET_VTARG && resp[1] == PU_CTRL_STATUS_OK && resp[2] == 1u);
    assert(g_vtarg_enabled == true);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1000u, 500000u) == PU_OK);

    /* Short target 3.3V -> overcurrent IRQ fires */
    g_hw_fault_asserted = true;
    pu_telemetry_on_overcurrent_irq();

    assert(g_vtarg_enabled == false);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert((pu_engine_fault_mask() & PU_FAULT_OVERCURRENT) != 0u);
    assert(g_tristate_calls >= 1);

    /* New debug acquisition and SET_VTARG(1) must both fail while faulted */
    assert(pu_engine_acquire(PU_ENGINE_SWD, 2000u, 500000u) == PU_ERR_FAULT_LATCHED);
    n = pu_ctrl_handle_request(set_on, sizeof(set_on), resp, sizeof(resp));
    assert(n >= 2u && resp[1] == PU_CTRL_STATUS_FAULT_LATCHED);
    assert(g_vtarg_enabled == false);

    /* CLEAR_FAULT must refuse while hardware fault pin is still asserted */
    uint8_t clear_req[1] = { PU_CTRL_CMD_CLEAR_FAULT };
    n = pu_ctrl_handle_request(clear_req, sizeof(clear_req), resp, sizeof(resp));
    assert(n >= 2u && resp[1] == PU_CTRL_STATUS_FAULT_LATCHED);
    assert(pu_engine_current() == PU_ENGINE_FAULT);

    /* Once physical short is removed, CLEAR_FAULT transitions back to IDLE */
    g_hw_fault_asserted = false;
    n = pu_ctrl_handle_request(clear_req, sizeof(clear_req), resp, sizeof(resp));
    assert(n == 2u && resp[0] == PU_CTRL_CMD_CLEAR_FAULT && resp[1] == PU_CTRL_STATUS_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_engine_fault_mask() == 0u);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 3000u, 500000u) == PU_OK);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);
}

static void test_ctrl_info_telemetry_and_selftest(void) {
    pu_engine_init(&kPinHal);
    pu_uart_bridge_init(NULL);

    /* PRO_PCB profile */
    g_vtarg_enabled = false;
    g_hw_fault_asserted = false;
    g_fake_adc_vtref_raw = 2048u; /* half of 4096 -> 1.65V * 2 = 3300 mV */
    g_fake_adc_itarg_raw = 621u;  /* 621/4096 * 3300mV / (20 * 0.1 Ohm) = ~250 mA */
    pu_ctrl_init(&kProCtrlHal);

    uint8_t info_req[1] = { PU_CTRL_CMD_GET_INFO };
    uint8_t resp[64] = {0};
    size_t n = pu_ctrl_handle_request(info_req, sizeof(info_req), resp, sizeof(resp));
    assert(n == 13u && resp[0] == PU_CTRL_CMD_GET_INFO && resp[1] == PU_CTRL_STATUS_OK);
    assert(resp[2] == PU_FW_VERSION_MAJOR && resp[3] == PU_FW_VERSION_MINOR);
    assert(resp[5] == 1u); /* board_is_pro_pcb */
    assert((resp[6] & PU_CAP_ADC_TELEMETRY) != 0u);
    assert((resp[6] & PU_CAP_VTARG_SWITCH) != 0u);
    assert(read_u32_le(&resp[9]) == 0u);

    /* Verify GET_INFO preserves all 32 fault_mask bits including [23:16] */
    pu_engine_latch_fault(0x805A3C01u);
    n = pu_ctrl_handle_request(info_req, sizeof(info_req), resp, sizeof(resp));
    assert(n == 13u && read_u32_le(&resp[9]) == 0x805A3C01u);
    assert(pu_engine_clear_fault() == PU_OK);

    uint8_t tel_req[1] = { PU_CTRL_CMD_GET_TELEMETRY };
    n = pu_ctrl_handle_request(tel_req, sizeof(tel_req), resp, sizeof(resp));
    assert(n == 13u && resp[0] == PU_CTRL_CMD_GET_TELEMETRY && resp[1] == PU_CTRL_STATUS_OK);
    uint16_t vtref_mv = read_u16_le(&resp[2]);
    uint16_t itarg_ma = read_u16_le(&resp[4]);
    assert(vtref_mv >= 3290u && vtref_mv <= 3310u);
    assert(itarg_ma >= 245u && itarg_ma <= 255u);
    assert(resp[8] == 1u); /* telemetry_supported */

    uint8_t st_req[1] = { PU_CTRL_CMD_SELF_TEST };
    n = pu_ctrl_handle_request(st_req, sizeof(st_req), resp, sizeof(resp));
    assert(n == 6u && resp[1] == PU_CTRL_STATUS_OK);
    uint32_t st_bits = read_u32_le(&resp[2]);
    assert((st_bits & PU_SELFTEST_ENGINE_OK) != 0u);
    assert((st_bits & PU_SELFTEST_ADC_RAIL_OK) != 0u);

    /* Over-voltage ADC reading (> 5600 mV) must fail PU_SELFTEST_ADC_RAIL_OK */
    g_fake_adc_vtref_raw = 3900u; /* ~6284 mV */
    n = pu_ctrl_handle_request(st_req, sizeof(st_req), resp, sizeof(resp));
    assert(n == 6u && (read_u32_le(&resp[2]) & PU_SELFTEST_ADC_RAIL_OK) == 0u);
    g_fake_adc_vtref_raw = 2048u;

    /* Synchronous overcurrent firing inside set_vtarg_power(true) must NOT report OK */
    g_sync_oc_on_vtarg_on = true;
    uint8_t set_on_oc[2] = { PU_CTRL_CMD_SET_VTARG, 1u };
    n = pu_ctrl_handle_request(set_on_oc, sizeof(set_on_oc), resp, sizeof(resp));
    g_sync_oc_on_vtarg_on = false;
    assert(n >= 2u && resp[1] == PU_CTRL_STATUS_FAULT_LATCHED);
    assert(g_vtarg_enabled == false && pu_engine_current() == PU_ENGINE_FAULT);
    g_hw_fault_asserted = false;
    assert(pu_engine_clear_fault() == PU_OK);

    /* CH32_UNBRICK routes through request_ch32_unbrick hook */
    g_unbrick_hook_called = false;
    uint8_t unbrick_req[1] = { PU_CTRL_CMD_CH32_UNBRICK };
    n = pu_ctrl_handle_request(unbrick_req, sizeof(unbrick_req), resp, sizeof(resp));
    assert(n == 2u && resp[1] == PU_CTRL_STATUS_OK && g_unbrick_hook_called);

    /* PICO profile: VTARG switch unsupported, telemetry_supported == 0 */
    pu_ctrl_init(&kPicoCtrlHal);
    uint8_t set_on[2] = { PU_CTRL_CMD_SET_VTARG, 1u };
    n = pu_ctrl_handle_request(set_on, sizeof(set_on), resp, sizeof(resp));
    assert(n >= 2u && resp[1] == PU_CTRL_STATUS_UNSUPPORTED);

    n = pu_ctrl_handle_request(tel_req, sizeof(tel_req), resp, sizeof(resp));
    assert(n == 13u && resp[1] == PU_CTRL_STATUS_OK);
    assert(read_u16_le(&resp[2]) == 0u && read_u16_le(&resp[4]) == 0u && resp[8] == 0u);
}

static void test_ctrl_bootsel_requires_magic_confirmation(void) {
    g_bootsel_called = false;
    g_vtarg_enabled = true;
    pu_engine_init(&kPinHal);
    pu_ctrl_init(&kProCtrlHal);

    uint8_t bad_short[1] = { PU_CTRL_CMD_ENTER_BOOTSEL };
    uint8_t resp[32] = {0};
    size_t n = pu_ctrl_handle_request(bad_short, sizeof(bad_short), resp, sizeof(resp));
    assert(n >= 2u && resp[1] == PU_CTRL_STATUS_INVALID_ARG && !g_bootsel_called);
    assert(!pu_ctrl_poll_deferred_bootsel());

    uint8_t bad_magic[5] = { PU_CTRL_CMD_ENTER_BOOTSEL, 0x11u, 0x22u, 0x33u, 0x44u };
    n = pu_ctrl_handle_request(bad_magic, sizeof(bad_magic), resp, sizeof(resp));
    assert(n >= 2u && resp[1] == PU_CTRL_STATUS_INVALID_ARG && !g_bootsel_called);

    uint8_t good_magic[5] = { PU_CTRL_CMD_ENTER_BOOTSEL, 0x10u, 0x5Eu, 0x07u, 0xB0u };
    n = pu_ctrl_handle_request(good_magic, sizeof(good_magic), resp, sizeof(resp));
    /* Deferred until caller flushes USB OK reply */
    assert(n == 2u && resp[1] == PU_CTRL_STATUS_OK && !g_bootsel_called);
    assert(pu_ctrl_poll_deferred_bootsel() == true);
    assert(g_bootsel_called == true && g_vtarg_enabled == false);
}

static void test_uart_bridge_ring_buffer_line_coding_and_drop_counter(void) {
    pu_engine_init(&kPinHal);
    pu_uart_bridge_init(NULL);

    /* Acquire SWD to prove UART bridge runs concurrently with zero engine contention */
    assert(pu_engine_acquire(PU_ENGINE_SWD, 100u, 500000u) == PU_OK);

    /* Line coding validation */
    assert(pu_uart_bridge_apply_line_coding(115200u, 8u, 1u, PU_UART_PARITY_NONE) == true);
    assert(pu_uart_bridge_apply_line_coding(3000000u, 8u, 1u, PU_UART_PARITY_EVEN) == true);
    assert(pu_uart_bridge_apply_line_coding(0u, 8u, 1u, PU_UART_PARITY_NONE) == false);
    assert(pu_uart_bridge_apply_line_coding(3000001u, 8u, 1u, PU_UART_PARITY_NONE) == false);
    assert(pu_uart_bridge_apply_line_coding(115200u, 9u, 1u, PU_UART_PARITY_NONE) == false);

    /* Fill RX ring past capacity (2048 B -> capacity 2047 usable bytes) and verify drop count */
    uint8_t pattern[2100];
    for (size_t i = 0u; i < sizeof(pattern); ++i) {
        pattern[i] = (uint8_t)(i & 0xFFu);
    }
    size_t pushed = pu_uart_bridge_rx_push(pattern, sizeof(pattern));
    assert(pushed == PU_UART_RING_USABLE_CAP);
    assert(pu_uart_bridge_rx_drop_count() == (uint32_t)(sizeof(pattern) - PU_UART_RING_USABLE_CAP));

    /* Drain half, push more across wrap boundary, verify exact byte order */
    uint8_t out[2100];
    size_t popped1 = pu_uart_bridge_rx_pop(out, 1000u);
    assert(popped1 == 1000u);
    assert(memcmp(out, pattern, 1000u) == 0);

    uint8_t tail[512];
    for (size_t i = 0u; i < sizeof(tail); ++i) {
        tail[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
    }
    assert(pu_uart_bridge_rx_push(tail, sizeof(tail)) == sizeof(tail));

    size_t rem_first = PU_UART_RING_USABLE_CAP - 1000u;
    size_t popped2 = pu_uart_bridge_rx_pop(out, rem_first);
    assert(popped2 == rem_first);
    assert(memcmp(out, &pattern[1000], rem_first) == 0);

    size_t popped3 = pu_uart_bridge_rx_pop(out, sizeof(tail));
    assert(popped3 == sizeof(tail));
    assert(memcmp(out, tail, sizeof(tail)) == 0);

    /* TX ring round-trip */
    assert(pu_uart_bridge_tx_push(tail, 128u) == 128u);
    assert(pu_uart_bridge_tx_pop(out, 128u) == 128u);
    assert(memcmp(out, tail, 128u) == 0);

    /* engine_owner remained undisturbed */
    assert(pu_engine_current() == PU_ENGINE_SWD);
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);
}

int main(void) {
    test_telemetry_overcurrent_forces_engine_fault();
    test_ctrl_info_telemetry_and_selftest();
    test_ctrl_bootsel_requires_magic_confirmation();
    test_uart_bridge_ring_buffer_line_coding_and_drop_counter();
    printf("test_control_telemetry: ALL PASSED\n");
    return 0;
}
