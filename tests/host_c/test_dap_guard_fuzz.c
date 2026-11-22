/*
 * Host unit & 50,000-packet deterministic fuzz tests for Pico Universal's
 * hardened CMSIS-DAP v2 engine (dap_engine + dap_guard + engine_owner).
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "firmware/dap_engine.h"
#include "firmware/engine_owner.h"

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void test_dap_info_and_basic_commands(void) {
    pu_dap_init_for_test();
    uint8_t resp[64];

    /* Product string must be "Pico Universal CMSIS-DAP" */
    const uint8_t req_prod[2] = {ID_DAP_Info, DAP_ID_PRODUCT};
    size_t n = pu_dap_process_packet(req_prod, sizeof(req_prod), resp, sizeof(resp), 100u);
    const char *expected_prod = "Pico Universal CMSIS-DAP";
    assert(n == 2u + strlen(expected_prod) + 1u);
    assert(resp[0] == ID_DAP_Info);
    assert(resp[1] == strlen(expected_prod) + 1u);
    assert(memcmp(&resp[2], expected_prod, strlen(expected_prod) + 1u) == 0);

    /* Serial string must be 16 hex chars + NUL = 17 bytes */
    const uint8_t req_ser[2] = {ID_DAP_Info, DAP_ID_SER_NUM};
    n = pu_dap_process_packet(req_ser, sizeof(req_ser), resp, sizeof(resp), 200u);
    assert(n == 19u && resp[0] == ID_DAP_Info && resp[1] == 17u && resp[18] == '\0');

    /* Capabilities must include both SWD (bit 0) and JTAG (bit 1) */
    const uint8_t req_cap[2] = {ID_DAP_Info, DAP_ID_CAPABILITIES};
    n = pu_dap_process_packet(req_cap, sizeof(req_cap), resp, sizeof(resp), 300u);
    assert(n == 3u && resp[0] == ID_DAP_Info && resp[1] == 1u);
    assert((resp[2] & 0x03u) == 0x03u);

    /* Packet Size must be 64 (0x0040 LE) */
    const uint8_t req_psize[2] = {ID_DAP_Info, DAP_ID_PACKET_SIZE};
    n = pu_dap_process_packet(req_psize, sizeof(req_psize), resp, sizeof(resp), 400u);
    assert(n == 4u && resp[0] == ID_DAP_Info && resp[1] == 2u);
    assert(resp[2] == 64u && resp[3] == 0u);

    /* Packet Count >= 1 */
    const uint8_t req_pcnt[2] = {ID_DAP_Info, DAP_ID_PACKET_COUNT};
    n = pu_dap_process_packet(req_pcnt, sizeof(req_pcnt), resp, sizeof(resp), 500u);
    assert(n == 3u && resp[0] == ID_DAP_Info && resp[1] == 1u && resp[2] >= 1u);
}

/* Spy state for verifying buffer-snapshot isolation, pin handoff, and zero-touch contention */
typedef struct {
    unsigned port_setup_calls;
    unsigned port_off_calls;
    unsigned swd_transfer_calls;
    unsigned swd_seq_calls;
    unsigned jtag_seq_calls;
    unsigned swj_seq_calls;
    unsigned delay_calls;
    unsigned pio_drain_calls;
    unsigned sio_override_calls;
    unsigned pin_io_calls;
    unsigned sio_release_calls;
    bool override_active;
    bool override_active_during_delay;
    bool override_released_before_jtag_seq;
    bool override_released_before_swj_seq;
    bool override_released_before_swd_seq;
    bool override_released_before_swd_xfer;
    bool override_released_before_port_off;
    uint8_t pio_idle_pins;
    uint8_t sio_override_mask;
    uint8_t sio_override_out;
    bool nreset_driven_low;
    uint32_t last_pin_wait_us;
    uint8_t step_log[16];
    unsigned step_count;
    uint8_t *mutated_caller_buf;
    size_t mutated_caller_len;
    uint32_t dp_ctrl_stat;
} spy_wire_state_t;

static spy_wire_state_t g_spy;

static void spy_port_setup(uint8_t port) {
    g_spy.port_setup_calls++;
    g_spy.override_active = false;
    g_spy.sio_override_mask = 0u;
    g_spy.nreset_driven_low = false;
    g_spy.pio_idle_pins = (port == DAP_PORT_JTAG)
                              ? (uint8_t)(PU_DAP_PIN_SWDIO_TMS | PU_DAP_PIN_TDI)
                              : (uint8_t)PU_DAP_PIN_SWDIO_TMS;
}

static void spy_port_off(void) {
    g_spy.port_off_calls++;
    g_spy.override_released_before_port_off = !g_spy.override_active;
    g_spy.override_active = false;
    g_spy.sio_override_mask = 0u;
    g_spy.nreset_driven_low = false;
}

static uint8_t spy_swd_transfer(uint8_t req_hdr,
                                uint32_t *data,
                                uint8_t turnaround_cycles,
                                bool force_data_phase) {
    (void)turnaround_cycles;
    (void)force_data_phase;
    g_spy.swd_transfer_calls++;
    g_spy.override_released_before_swd_xfer = !g_spy.override_active;
    /* Simulate USB reset / ring reuse corrupting caller's request buffer mid-batch! */
    if (g_spy.mutated_caller_buf != NULL) {
        memset(g_spy.mutated_caller_buf, 0xFF, g_spy.mutated_caller_len);
        g_spy.mutated_caller_buf = NULL;
    }
    if ((req_hdr & DAP_TRANSFER_RnW) != 0u) {
        *data = g_spy.dp_ctrl_stat;
    } else {
        g_spy.dp_ctrl_stat = *data;
    }
    return DAP_TRANSFER_OK;
}

static void spy_swd_sequence(uint32_t bit_count,
                             bool capture,
                             const uint8_t *tx_data,
                             uint8_t *rx_data) {
    (void)tx_data;
    g_spy.swd_seq_calls++;
    g_spy.override_released_before_swd_seq = !g_spy.override_active;
    if (capture && rx_data != NULL) {
        memset(rx_data, 0, (bit_count + 7u) / 8u);
    }
}

static void spy_jtag_sequence(uint32_t bit_count,
                              bool tms,
                              const uint8_t *tdi_data,
                              uint8_t *tdo_data) {
    (void)tms;
    (void)tdi_data;
    g_spy.jtag_seq_calls++;
    g_spy.override_released_before_jtag_seq = !g_spy.override_active;
    if (tdo_data != NULL) {
        memset(tdo_data, 0, (bit_count + 7u) / 8u);
    }
}

static void spy_swj_sequence(uint32_t bit_count, const uint8_t *data) {
    (void)bit_count;
    (void)data;
    g_spy.swj_seq_calls++;
    g_spy.override_released_before_swj_seq = !g_spy.override_active;
}

static void spy_delay_us(uint32_t delay_us) {
    (void)delay_us;
    g_spy.delay_calls++;
    g_spy.override_active_during_delay = g_spy.override_active;
}

static void spy_pio_drain(void) {
    g_spy.pio_drain_calls++;
    if (g_spy.step_count < sizeof(g_spy.step_log)) {
        g_spy.step_log[g_spy.step_count++] = 1u;
    }
}

static void spy_sio_override_begin(void) {
    g_spy.sio_override_calls++;
    g_spy.override_active = true;
    if (g_spy.step_count < sizeof(g_spy.step_log)) {
        g_spy.step_log[g_spy.step_count++] = 2u;
    }
}

static uint8_t spy_sample_pins(void) {
    uint8_t pio_or_sio = (uint8_t)((g_spy.pio_idle_pins & ~g_spy.sio_override_mask) |
                                   (g_spy.sio_override_out & g_spy.sio_override_mask));
    uint8_t pins = (uint8_t)(pio_or_sio | PU_DAP_PIN_TDO | PU_DAP_PIN_nTRST);
    if (!g_spy.nreset_driven_low) {
        pins |= PU_DAP_PIN_nRESET;
    }
    return pins;
}

static uint8_t spy_swj_pins_io(uint8_t pin_output, uint8_t pin_select, uint32_t wait_us) {
    const uint8_t sio_pins = (uint8_t)(PU_DAP_PIN_SWCLK_TCK | PU_DAP_PIN_SWDIO_TMS | PU_DAP_PIN_TDI);
    uint8_t sel_sio = (uint8_t)(pin_select & sio_pins);
    g_spy.sio_override_mask |= sel_sio;
    g_spy.sio_override_out = (uint8_t)((g_spy.sio_override_out & ~sel_sio) | (pin_output & sel_sio));
    if ((pin_select & PU_DAP_PIN_nRESET) != 0u) {
        g_spy.nreset_driven_low = ((pin_output & PU_DAP_PIN_nRESET) == 0u);
    }
    g_spy.pin_io_calls++;
    g_spy.last_pin_wait_us = wait_us;
    if (g_spy.step_count < sizeof(g_spy.step_log)) {
        g_spy.step_log[g_spy.step_count++] = 3u;
    }
    return spy_sample_pins();
}

static void spy_sio_override_end(void) {
    g_spy.sio_release_calls++;
    g_spy.override_active = false;
    g_spy.sio_override_mask = 0u;
    if (g_spy.step_count < sizeof(g_spy.step_log)) {
        g_spy.step_log[g_spy.step_count++] = 4u;
    }
}

static void init_spy_dap(void) {
    memset(&g_spy, 0, sizeof(g_spy));
    pu_dap_init_for_test();
    pu_dap_wire_hal_t hal;
    memset(&hal, 0, sizeof(hal));
    hal.sys_clock_hz = 125000000u;
    hal.port_setup = spy_port_setup;
    hal.port_off = spy_port_off;
    hal.swj_sequence = spy_swj_sequence;
    hal.swd_sequence = spy_swd_sequence;
    hal.swd_transfer = spy_swd_transfer;
    hal.jtag_sequence = spy_jtag_sequence;
    hal.pio_drain = spy_pio_drain;
    hal.sio_override_begin = spy_sio_override_begin;
    hal.swj_pins_io = spy_swj_pins_io;
    hal.sio_override_end = spy_sio_override_end;
    hal.delay_us = spy_delay_us;
    pu_dap_init(&hal);
}

static void test_dap_guard_snapshot_survives_buffer_reuse(void) {
    /*
     * 4-command batch:
     *   1. DAP_Connect(SWD)              -> [0x02, 0x01] (2B)
     *   2. DAP_Transfer(write 0xA55A1234)-> [0x05, 0x01, 0x01] (3B)
     *   3. DAP_Transfer(read)            -> [0x05, 0x01, 0x01, 0x34, 0x12, 0x5A, 0xA5] (7B)
     *   4. DAP_Disconnect                -> [0x03, 0x00] (2B)
     * Total response = 2 (batch hdr) + 2 + 3 + 7 + 2 = 16 bytes.
     */
    const uint8_t kBatch[] = {
        ID_DAP_ExecuteCommands, 4u,
        ID_DAP_Connect, DAP_PORT_SWD,
        ID_DAP_Transfer, 0u, 1u, 0x04u /* DP Write */, 0x34u, 0x12u, 0x5Au, 0xA5u,
        ID_DAP_Transfer, 0u, 1u, 0x06u /* DP Read */,
        ID_DAP_Disconnect
    };

    /* Run 1: Clean separate buffers */
    init_spy_dap();
    uint8_t clean_resp[64] = {0};
    size_t clean_len = pu_dap_process_packet(kBatch, sizeof(kBatch), clean_resp, sizeof(clean_resp), 1000u);
    assert(clean_len == 16u);
    assert(clean_resp[0] == ID_DAP_ExecuteCommands && clean_resp[1] == 4u);
    assert(clean_resp[2] == ID_DAP_Connect && clean_resp[3] == DAP_PORT_SWD);
    assert(clean_resp[4] == ID_DAP_Transfer && clean_resp[5] == 1u && clean_resp[6] == DAP_TRANSFER_OK);
    assert(clean_resp[7] == ID_DAP_Transfer && clean_resp[8] == 1u && clean_resp[9] == DAP_TRANSFER_OK);
    assert(clean_resp[10] == 0x34u && clean_resp[11] == 0x12u &&
           clean_resp[12] == 0x5Au && clean_resp[13] == 0xA5u);
    assert(clean_resp[14] == ID_DAP_Disconnect && clean_resp[15] == DAP_OK);

    /* Run 2: Aliased in-place buffer (req == resp) */
    init_spy_dap();
    uint8_t shared_buf[64] = {0};
    memcpy(shared_buf, kBatch, sizeof(kBatch));
    size_t aliased_len = pu_dap_process_packet(shared_buf, sizeof(kBatch), shared_buf, sizeof(shared_buf), 2000u);
    assert(aliased_len == clean_len);
    assert(memcmp(shared_buf, clean_resp, clean_len) == 0);

    /* Run 3: Mid-batch caller buffer overwrite (simulating USB reset clearing RX ring) */
    init_spy_dap();
    uint8_t volatile_req[64];
    memcpy(volatile_req, kBatch, sizeof(kBatch));
    g_spy.mutated_caller_buf = volatile_req;
    g_spy.mutated_caller_len = sizeof(volatile_req);
    uint8_t mutated_resp[64] = {0};
    size_t mutated_len = pu_dap_process_packet(volatile_req, sizeof(kBatch), mutated_resp, sizeof(mutated_resp), 3000u);
    assert(volatile_req[0] == 0xFFu); /* Confirm callback really wiped caller's buffer */
    assert(mutated_len == clean_len);
    assert(memcmp(mutated_resp, clean_resp, clean_len) == 0);
}

static void test_dap_cross_interface_contention_and_deadline_renewal(void) {
    init_spy_dap();
    uint8_t resp[64];

    /* Simulate IF 1-2 CH32 SWIO owning engine_owner */
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1000u, 500000u) == PU_OK);

    const uint8_t conn_swd[2] = {ID_DAP_Connect, DAP_PORT_SWD};
    size_t n = pu_dap_process_packet(conn_swd, sizeof(conn_swd), resp, sizeof(resp), 1100u);
    assert(n == 2u && resp[0] == ID_DAP_Connect && resp[1] == 0x00u);
    assert(pu_engine_current() == PU_ENGINE_SWIO);
    assert(g_spy.port_setup_calls == 0u && g_spy.swd_transfer_calls == 0u);

    const uint8_t conn_jtag[2] = {ID_DAP_Connect, DAP_PORT_JTAG};
    n = pu_dap_process_packet(conn_jtag, sizeof(conn_jtag), resp, sizeof(resp), 1200u);
    assert(n == 2u && resp[0] == ID_DAP_Connect && resp[1] == 0x00u);
    assert(pu_engine_current() == PU_ENGINE_SWIO);
    assert(g_spy.port_setup_calls == 0u);

    /* DAP_Disconnect while SWIO owns bus must NOT release SWIO! */
    const uint8_t disc[1] = {ID_DAP_Disconnect};
    n = pu_dap_process_packet(disc, sizeof(disc), resp, sizeof(resp), 1300u);
    assert(n == 2u && resp[0] == ID_DAP_Disconnect);
    assert(pu_engine_current() == PU_ENGINE_SWIO);

    /* Release SWIO, latch hardware FAULT -> DAP_Connect must fail cleanly */
    assert(pu_engine_release(PU_ENGINE_SWIO) == PU_OK);
    pu_engine_latch_fault(PU_FAULT_OVERCURRENT);
    n = pu_dap_process_packet(conn_swd, sizeof(conn_swd), resp, sizeof(resp), 1400u);
    assert(n == 2u && resp[0] == ID_DAP_Connect && resp[1] == 0x00u);
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(g_spy.port_setup_calls == 0u);

    /* Clear fault -> DAP_Connect(SWD) succeeds and renews deadline on traffic */
    assert(pu_engine_clear_fault() == PU_OK);
    n = pu_dap_process_packet(conn_swd, sizeof(conn_swd), resp, sizeof(resp), 2000u);
    assert(n == 2u && resp[0] == ID_DAP_Connect && resp[1] == DAP_PORT_SWD);
    assert(pu_engine_current() == PU_ENGINE_SWD);
    assert(g_spy.port_setup_calls == 1u);

    /* Renew deadline with an active transfer near session expiry */
    const uint8_t xfer[8] = {ID_DAP_Transfer, 0u, 1u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u};
    n = pu_dap_process_packet(xfer, sizeof(xfer), resp, sizeof(resp), 2000u + PU_DAP_SESSION_TIMEOUT_US - 10u);
    assert(n == 3u && resp[2] == DAP_TRANSFER_OK);
    /* Tick past original connect deadline -> still alive because transfer renewed deadline! */
    pu_engine_tick(2000u + PU_DAP_SESSION_TIMEOUT_US + 100u);
    assert(pu_engine_current() == PU_ENGINE_SWD);

    /* Now let renewed deadline expire via pu_engine_tick and let SWIO take over */
    pu_engine_tick(2000u + 2u * PU_DAP_SESSION_TIMEOUT_US + 100u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 20000000u, 500000u) == PU_OK);

    /* Stale DAP session must detect lost ownership and refuse wire access */
    uint32_t prev_swd_calls = g_spy.swd_transfer_calls;
    n = pu_dap_process_packet(xfer, sizeof(xfer), resp, sizeof(resp), 20000100u);
    assert(n == 3u && resp[1] == 0u && resp[2] == DAP_TRANSFER_ERROR);
    assert(g_spy.swd_transfer_calls == prev_swd_calls);
    assert(pu_engine_current() == PU_ENGINE_SWIO);

    /* Once SWIO releases, DAP_Connect(SWD) cleanly re-acquires */
    assert(pu_engine_release(PU_ENGINE_SWIO) == PU_OK);
    n = pu_dap_process_packet(conn_swd, sizeof(conn_swd), resp, sizeof(resp), 20000200u);
    assert(n == 2u && resp[1] == DAP_PORT_SWD);
    assert(pu_engine_current() == PU_ENGINE_SWD);

    /* USB disconnect tears down SWD ownership cleanly */
    pu_dap_on_usb_disconnect();
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_spy.port_off_calls == 1u);
}

static void test_dap_swj_pins_handoff_and_bounded_wait(void) {
    init_spy_dap();
    uint8_t resp[64];
    const uint8_t conn_swd[2] = {ID_DAP_Connect, DAP_PORT_SWD};
    assert(pu_dap_process_packet(conn_swd, 2u, resp, sizeof(resp), 100u) == 2u);
    assert(resp[1] == DAP_PORT_SWD);

    /* Request SWCLK/SWDIO/nRESET override with huge 0xFFFFFFFF wait -> must bound wait and keep override latched */
    const uint8_t pins_req[7] = {
        ID_DAP_SWJ_Pins,
        PU_DAP_PIN_SWDIO_TMS,
        (uint8_t)(PU_DAP_PIN_SWCLK_TCK | PU_DAP_PIN_SWDIO_TMS | PU_DAP_PIN_nRESET),
        0xFFu, 0xFFu, 0xFFu, 0xFFu
    };
    size_t n = pu_dap_process_packet(pins_req, sizeof(pins_req), resp, sizeof(resp), 200u);
    assert(n == 2u && resp[0] == ID_DAP_SWJ_Pins);
    assert(g_spy.step_count == 3u);
    assert(g_spy.step_log[0] == 1u); /* 1. PIO drain */
    assert(g_spy.step_log[1] == 2u); /* 2. SIO override begin */
    assert(g_spy.step_log[2] == 3u); /* 3. Pin write/wait/read (override stays latched!) */
    assert(g_spy.sio_release_calls == 0u);
    assert(g_spy.override_active == true);
    assert(g_spy.last_pin_wait_us <= PU_DAP_MAX_PIN_WAIT_US);

    /* Next PIO transfer (DAP_Transfer) releases SIO overrides BEFORE SWD wire transfer */
    const uint8_t xfer[4] = {ID_DAP_Transfer, 0u, 1u, DAP_TRANSFER_RnW};
    n = pu_dap_process_packet(xfer, sizeof(xfer), resp, sizeof(resp), 300u);
    assert(n == 7u && resp[2] == DAP_TRANSFER_OK);
    assert(g_spy.step_count == 4u && g_spy.step_log[3] == 4u);
    assert(g_spy.sio_release_calls == 1u);
    assert(g_spy.override_released_before_swd_xfer == true);
}

static void test_dap_swj_pins_esp32_tdi_hold_across_delay_and_release(void) {
    init_spy_dap();
    uint8_t resp[64];

    const uint8_t conn_jtag[2] = {ID_DAP_Connect, DAP_PORT_JTAG};
    assert(pu_dap_process_packet(conn_jtag, sizeof(conn_jtag), resp, sizeof(resp), 100u) == 2u);
    assert(resp[1] == DAP_PORT_JTAG);

    /* PIO JTAG idles TDI high (1.8 V flash strap); ESP32_FLASH_VOLTAGE 3.3 overrides TDI=0 */
    const uint8_t hold_tdi_low[7] = {
        ID_DAP_SWJ_Pins, 0x00u, PU_DAP_PIN_TDI, 0u, 0u, 0u, 0u
    };
    assert(pu_dap_process_packet(hold_tdi_low, sizeof(hold_tdi_low), resp, sizeof(resp), 200u) == 2u);
    assert((resp[1] & PU_DAP_PIN_TDI) == 0u);
    assert(g_spy.override_active == true && g_spy.sio_release_calls == 0u);

    /* DAP_Delay + nRESET pulse + DAP_Delay must preserve TDI=0 override */
    const uint8_t delay_5ms[3] = {ID_DAP_Delay, 0x88u, 0x13u};
    assert(pu_dap_process_packet(delay_5ms, sizeof(delay_5ms), resp, sizeof(resp), 210u) == 2u);
    assert(g_spy.delay_calls == 1u && g_spy.override_active_during_delay == true);
    assert(g_spy.sio_release_calls == 0u);

    const uint8_t nrst_low[7] = {
        ID_DAP_SWJ_Pins, 0x00u, PU_DAP_PIN_nRESET, 0u, 0u, 0u, 0u
    };
    assert(pu_dap_process_packet(nrst_low, sizeof(nrst_low), resp, sizeof(resp), 220u) == 2u);
    assert((resp[1] & PU_DAP_PIN_TDI) == 0u);
    assert((resp[1] & PU_DAP_PIN_nRESET) == 0u);
    assert(g_spy.sio_release_calls == 0u);

    assert(pu_dap_process_packet(delay_5ms, sizeof(delay_5ms), resp, sizeof(resp), 230u) == 2u);
    assert(g_spy.override_active_during_delay == true && g_spy.sio_release_calls == 0u);

    const uint8_t nrst_high[7] = {
        ID_DAP_SWJ_Pins, PU_DAP_PIN_nRESET, PU_DAP_PIN_nRESET, 0u, 0u, 0u, 0u
    };
    assert(pu_dap_process_packet(nrst_high, sizeof(nrst_high), resp, sizeof(resp), 240u) == 2u);
    assert((resp[1] & PU_DAP_PIN_TDI) == 0u); /* TDI STILL 0 as ESP32 exits reset! */
    assert((resp[1] & PU_DAP_PIN_nRESET) != 0u);
    assert(g_spy.sio_release_calls == 0u);

    /* 1. DAP_JTAG_Sequence releases SIO override BEFORE PIO JTAG shift */
    const uint8_t jtag_seq[4] = {ID_DAP_JTAG_Sequence, 1u, 8u, 0xFFu};
    assert(pu_dap_process_packet(jtag_seq, sizeof(jtag_seq), resp, sizeof(resp), 250u) == 2u);
    assert(resp[1] == DAP_OK);
    assert(g_spy.sio_release_calls == 1u);
    assert(g_spy.override_released_before_jtag_seq == true);
    assert(g_spy.override_active == false);

    /* 2. Re-latch TDI=0 + DAP_Delay -> DAP_SWJ_Sequence releases override BEFORE PIO SWJ shift */
    assert(pu_dap_process_packet(hold_tdi_low, sizeof(hold_tdi_low), resp, sizeof(resp), 260u) == 2u);
    assert(pu_dap_process_packet(delay_5ms, sizeof(delay_5ms), resp, sizeof(resp), 270u) == 2u);
    assert(g_spy.override_active == true && g_spy.sio_release_calls == 1u);

    const uint8_t swj_seq[3] = {ID_DAP_SWJ_Sequence, 8u, 0xFFu};
    assert(pu_dap_process_packet(swj_seq, sizeof(swj_seq), resp, sizeof(resp), 280u) == 2u);
    assert(resp[1] == DAP_OK);
    assert(g_spy.sio_release_calls == 2u);
    assert(g_spy.override_released_before_swj_seq == true);
    assert(g_spy.override_active == false);

    /* 3. Re-latch TDI=0 + DAP_Delay -> DAP_Disconnect releases override BEFORE port_off */
    assert(pu_dap_process_packet(hold_tdi_low, sizeof(hold_tdi_low), resp, sizeof(resp), 290u) == 2u);
    assert(pu_dap_process_packet(delay_5ms, sizeof(delay_5ms), resp, sizeof(resp), 300u) == 2u);
    assert(g_spy.override_active == true && g_spy.sio_release_calls == 2u);

    const uint8_t disc[1] = {ID_DAP_Disconnect};
    assert(pu_dap_process_packet(disc, sizeof(disc), resp, sizeof(resp), 310u) == 2u);
    assert(resp[1] == DAP_OK);
    assert(g_spy.sio_release_calls == 3u);
    assert(g_spy.override_released_before_port_off == true);
    assert(g_spy.override_active == false);

    /* 4. End-to-end check on built-in kHostSimWireHal (zero spy) */
    pu_dap_init_for_test();
    const uint8_t sample_only[7] = {ID_DAP_SWJ_Pins, 0u, 0u, 0u, 0u, 0u, 0u};
    assert(pu_dap_process_packet(conn_jtag, sizeof(conn_jtag), resp, sizeof(resp), 400u) == 2u);
    assert(pu_dap_process_packet(sample_only, sizeof(sample_only), resp, sizeof(resp), 405u) == 2u);
    assert((resp[1] & PU_DAP_PIN_TDI) != 0u); /* JTAG PIO idle sets TDI=1 */

    assert(pu_dap_process_packet(hold_tdi_low, sizeof(hold_tdi_low), resp, sizeof(resp), 410u) == 2u);
    assert(pu_dap_process_packet(delay_5ms, sizeof(delay_5ms), resp, sizeof(resp), 420u) == 2u);
    assert(pu_dap_process_packet(sample_only, sizeof(sample_only), resp, sizeof(resp), 430u) == 2u);
    assert((resp[1] & PU_DAP_PIN_TDI) == 0u); /* TDI=0 still held across DAP_Delay */

    assert(pu_dap_process_packet(jtag_seq, sizeof(jtag_seq), resp, sizeof(resp), 440u) == 2u);
    assert(pu_dap_process_packet(sample_only, sizeof(sample_only), resp, sizeof(resp), 450u) == 2u);
    assert((resp[1] & PU_DAP_PIN_TDI) != 0u); /* Released back to PIO JTAG idle TDI=1 */
}

typedef struct {
    uint32_t rdbuff;
    unsigned ap_reads;
    unsigned rdbuff_reads;
} posted_ap_sim_t;

static posted_ap_sim_t g_posted_ap;

static uint8_t posted_ap_swd_transfer(uint8_t req_hdr,
                                      uint32_t *data,
                                      uint8_t turnaround_cycles,
                                      bool force_data_phase) {
    (void)turnaround_cycles;
    (void)force_data_phase;
    bool is_ap = ((req_hdr & DAP_TRANSFER_APnDP) != 0u);
    bool is_read = ((req_hdr & DAP_TRANSFER_RnW) != 0u);

    if (is_ap && is_read) {
        /* ADIv5 posted AP read: returns previous RDBUFF and triggers next AP read */
        if (data != NULL) {
            *data = g_posted_ap.rdbuff;
        }
        static const uint32_t kSeq[3] = {0x00000000u, 0x00000001u, 0x00000002u};
        g_posted_ap.rdbuff = (g_posted_ap.ap_reads < 3u) ? kSeq[g_posted_ap.ap_reads] : 0xDEADBEEFu;
        g_posted_ap.ap_reads++;
        return DAP_TRANSFER_OK;
    }

    if (!is_ap && is_read && ((req_hdr & (DAP_TRANSFER_A2 | DAP_TRANSFER_A3)) ==
                              (DAP_TRANSFER_A2 | DAP_TRANSFER_A3))) {
        if (data != NULL) {
            *data = g_posted_ap.rdbuff;
        }
        g_posted_ap.rdbuff_reads++;
        return DAP_TRANSFER_OK;
    }

    if (is_read && data != NULL) {
        *data = 0u;
    }
    return DAP_TRANSFER_OK;
}

static void test_dap_transfer_ap_match_value_repolls_ap_register(void) {
    uint8_t resp[64];
    pu_dap_wire_hal_t hal;
    memset(&hal, 0, sizeof(hal));
    hal.sys_clock_hz = 125000000u;
    hal.swd_transfer = posted_ap_swd_transfer;

    /* Case 1: match_retry = 2 -> AP yields 0x0, 0x1, 0x2; matches 0x2 on final DP RDBUFF without orphaned 4th AP read */
    pu_dap_init_for_test();
    pu_dap_init(&hal);
    memset(&g_posted_ap, 0, sizeof(g_posted_ap));
    g_posted_ap.rdbuff = 0xCAFEBABEu;

    const uint8_t conn_swd[2] = {ID_DAP_Connect, DAP_PORT_SWD};
    assert(pu_dap_process_packet(conn_swd, sizeof(conn_swd), resp, sizeof(resp), 100u) == 2u);

    const uint8_t cfg_retry2[6] = {ID_DAP_TransferConfigure, 0u, 100u, 0u, 2u, 0u};
    assert(pu_dap_process_packet(cfg_retry2, sizeof(cfg_retry2), resp, sizeof(resp), 110u) == 2u);
    assert(resp[1] == DAP_OK);

    const uint8_t match_req[] = {
        ID_DAP_Transfer, 0u, 2u,
        DAP_TRANSFER_MATCH_MASK, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        (uint8_t)(DAP_TRANSFER_APnDP | DAP_TRANSFER_RnW | DAP_TRANSFER_MATCH_VALUE),
        0x02u, 0x00u, 0x00u, 0x00u
    };
    size_t n = pu_dap_process_packet(match_req, sizeof(match_req), resp, sizeof(resp), 120u);
    assert(n == 3u && resp[0] == ID_DAP_Transfer);
    assert(resp[1] == 2u);
    assert(resp[2] == DAP_TRANSFER_OK);
    assert(g_posted_ap.ap_reads == 3u);     /* Prime + 2 AP polls; NO orphaned 4th AP read! */
    assert(g_posted_ap.rdbuff_reads == 1u); /* Final m_retry == 0 read came cleanly from DP RDBUFF */

    /* Case 2: match_retry = 1 -> only 2 AP reads (0x0, 0x1), fails with DAP_TRANSFER_MISMATCH */
    memset(&g_posted_ap, 0, sizeof(g_posted_ap));
    g_posted_ap.rdbuff = 0xCAFEBABEu;
    const uint8_t cfg_retry1[6] = {ID_DAP_TransferConfigure, 0u, 100u, 0u, 1u, 0u};
    assert(pu_dap_process_packet(cfg_retry1, sizeof(cfg_retry1), resp, sizeof(resp), 130u) == 2u);
    n = pu_dap_process_packet(match_req, sizeof(match_req), resp, sizeof(resp), 140u);
    assert(n == 3u && resp[1] == 1u);
    assert(resp[2] == (uint8_t)(DAP_TRANSFER_OK | DAP_TRANSFER_MISMATCH));
    assert(g_posted_ap.ap_reads == 2u);
    assert(g_posted_ap.rdbuff_reads == 1u);

    /* Case 3: match_retry = 5 -> matches 0x2 early while m_retry > 0 */
    memset(&g_posted_ap, 0, sizeof(g_posted_ap));
    g_posted_ap.rdbuff = 0xCAFEBABEu;
    const uint8_t cfg_retry5[6] = {ID_DAP_TransferConfigure, 0u, 100u, 0u, 5u, 0u};
    assert(pu_dap_process_packet(cfg_retry5, sizeof(cfg_retry5), resp, sizeof(resp), 150u) == 2u);
    n = pu_dap_process_packet(match_req, sizeof(match_req), resp, sizeof(resp), 160u);
    assert(n == 3u && resp[1] == 2u && resp[2] == DAP_TRANSFER_OK);
}

static uint32_t read_u32_le_test(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_dap_swj_pins_wait_masks_non_drivable_pins(void) {
    pu_dap_init_for_test();
    uint8_t resp[64];

    const uint8_t conn_swd[2] = {ID_DAP_Connect, DAP_PORT_SWD};
    assert(pu_dap_process_packet(conn_swd, sizeof(conn_swd), resp, sizeof(resp), 100u) == 2u);
    assert(resp[1] == DAP_PORT_SWD);

    const uint8_t ts_read[4] = {
        ID_DAP_Transfer, 0u, 1u, (uint8_t)(DAP_TRANSFER_RnW | DAP_TRANSFER_TIMESTAMP)
    };
    assert(pu_dap_process_packet(ts_read, sizeof(ts_read), resp, sizeof(resp), 110u) == 11u);
    uint32_t ts0 = read_u32_le_test(&resp[3]);

    /* Selecting unwired nTRST=0 with 1,000,000 us wait must return immediately (0 wait stall) */
    const uint8_t ntrst_zero[7] = {
        ID_DAP_SWJ_Pins, 0x00u, PU_DAP_PIN_nTRST, 0x40u, 0x42u, 0x0Fu, 0x00u
    };
    assert(pu_dap_process_packet(ntrst_zero, sizeof(ntrst_zero), resp, sizeof(resp), 120u) == 2u);
    assert((resp[1] & PU_DAP_PIN_nTRST) != 0u);

    assert(pu_dap_process_packet(ts_read, sizeof(ts_read), resp, sizeof(resp), 130u) == 11u);
    uint32_t ts1 = read_u32_le_test(&resp[3]);
    assert((ts1 - ts0) < 1000u);

    /* Selecting pin_select = 0xFF with pin_output = 0x00 and 1,000,000 us wait must also finish immediately */
    const uint8_t all_pins_zero[7] = {
        ID_DAP_SWJ_Pins, 0x00u, 0xFFu, 0x40u, 0x42u, 0x0Fu, 0x00u
    };
    assert(pu_dap_process_packet(all_pins_zero, sizeof(all_pins_zero), resp, sizeof(resp), 140u) == 2u);
    assert((resp[1] & (PU_DAP_PIN_SWCLK_TCK | PU_DAP_PIN_SWDIO_TMS |
                       PU_DAP_PIN_TDI | PU_DAP_PIN_nRESET)) == 0u);
    assert((resp[1] & (PU_DAP_PIN_nTRST | PU_DAP_PIN_TDO)) ==
           (PU_DAP_PIN_nTRST | PU_DAP_PIN_TDO));

    assert(pu_dap_process_packet(ts_read, sizeof(ts_read), resp, sizeof(resp), 150u) == 11u);
    uint32_t ts2 = read_u32_le_test(&resp[3]);
    assert((ts2 - ts1) < 1000u);
}

static void test_dap_clock_64bit_math(void) {
    pu_dap_init_for_test();
    uint8_t resp[64];

    /* 0 Hz must be rejected cleanly */
    const uint8_t clk_zero[5] = {ID_DAP_SWJ_Clock, 0u, 0u, 0u, 0u};
    size_t n = pu_dap_process_packet(clk_zero, sizeof(clk_zero), resp, sizeof(resp), 100u);
    assert(n == 2u && resp[0] == ID_DAP_SWJ_Clock && resp[1] == DAP_ERROR);
    uint32_t eff_khz = 999u;
    assert(pu_dap_compute_pio_clkdiv_q16_8(125000000u, 0u, &eff_khz) == 0u);

    /* 1 Hz and 500 Hz clamp to 1 kHz floor */
    const uint8_t clk_1hz[5] = {ID_DAP_SWJ_Clock, 1u, 0u, 0u, 0u};
    n = pu_dap_process_packet(clk_1hz, sizeof(clk_1hz), resp, sizeof(resp), 200u);
    assert(n == 2u && resp[0] == ID_DAP_SWJ_Clock && resp[1] == DAP_OK);
    uint32_t div_1hz = pu_dap_compute_pio_clkdiv_q16_8(125000000u, 1u, &eff_khz);
    assert(div_1hz >= 256u && div_1hz <= 0xFFFFFFu);
    assert(eff_khz == 1u);

    uint32_t div_500hz = pu_dap_compute_pio_clkdiv_q16_8(125000000u, 500u, &eff_khz);
    assert(div_500hz == div_1hz && eff_khz == 1u);

    /* High MHz (UINT32_MAX) clamps cleanly without 32-bit overflow */
    const uint8_t clk_max[5] = {ID_DAP_SWJ_Clock, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
    n = pu_dap_process_packet(clk_max, sizeof(clk_max), resp, sizeof(resp), 300u);
    assert(n == 2u && resp[0] == ID_DAP_SWJ_Clock && resp[1] == DAP_OK);
    uint32_t div_max = pu_dap_compute_pio_clkdiv_q16_8(125000000u, UINT32_MAX, &eff_khz);
    assert(div_max >= 256u);
    assert(eff_khz <= (PU_DAP_MAX_CLOCK_HZ / 1000u));

    /* Legacy delay-to-kHz helper parity with origin/linux-focused clock_math.h */
    assert(pu_dap_delay_to_khz(125000000u, 624u) == 100u);
    assert(pu_dap_delay_to_khz(125000000u, UINT32_MAX) == 1u);
    assert(pu_dap_delay_to_khz(125000000u, 62500000u) == 1u);
}

static void test_dap_bounds_jtag_and_batch_guards(void) {
    init_spy_dap();
    uint8_t resp[64];

    /* JTAG chain > 8 devices, 0 devices, 0 IR length, >32 IR length all rejected */
    uint8_t jtag_9[11] = {ID_DAP_JTAG_Configure, 9u, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    assert(pu_dap_process_packet(jtag_9, sizeof(jtag_9), resp, sizeof(resp), 100u) == 1u && resp[0] == ID_DAP_Invalid);
    uint8_t jtag_0[2] = {ID_DAP_JTAG_Configure, 0u};
    assert(pu_dap_process_packet(jtag_0, sizeof(jtag_0), resp, sizeof(resp), 100u) == 1u && resp[0] == ID_DAP_Invalid);
    uint8_t jtag_ir0[3] = {ID_DAP_JTAG_Configure, 1u, 0u};
    assert(pu_dap_process_packet(jtag_ir0, sizeof(jtag_ir0), resp, sizeof(resp), 100u) == 1u && resp[0] == ID_DAP_Invalid);
    uint8_t jtag_ir33[3] = {ID_DAP_JTAG_Configure, 1u, 33u};
    assert(pu_dap_process_packet(jtag_ir33, sizeof(jtag_ir33), resp, sizeof(resp), 100u) == 1u && resp[0] == ID_DAP_Invalid);

    /* Valid JTAG session: Connect(JTAG) -> JTAG_Configure(2 TAPs: IR 4, 5) -> JTAG_IDCODE(0) -> JTAG_Sequence -> Disconnect */
    pu_dap_init_for_test();
    const uint8_t conn_jtag[2] = {ID_DAP_Connect, DAP_PORT_JTAG};
    assert(pu_dap_process_packet(conn_jtag, 2u, resp, sizeof(resp), 200u) == 2u && resp[1] == DAP_PORT_JTAG);
    const uint8_t jtag_ok[4] = {ID_DAP_JTAG_Configure, 2u, 4u, 5u};
    assert(pu_dap_process_packet(jtag_ok, sizeof(jtag_ok), resp, sizeof(resp), 210u) == 2u && resp[1] == DAP_OK);
    const uint8_t jtag_id[2] = {ID_DAP_JTAG_IDCODE, 0u};
    assert(pu_dap_process_packet(jtag_id, sizeof(jtag_id), resp, sizeof(resp), 220u) == 6u && resp[1] == DAP_OK);
    const uint8_t jtag_seq[4] = {ID_DAP_JTAG_Sequence, 1u, (uint8_t)(0x80u | 8u) /* capture 8 bits, TMS=0 */, 0xA5u};
    assert(pu_dap_process_packet(jtag_seq, sizeof(jtag_seq), resp, sizeof(resp), 230u) == 3u && resp[1] == DAP_OK);
    const uint8_t disc[1] = {ID_DAP_Disconnect};
    assert(pu_dap_process_packet(disc, 1u, resp, sizeof(resp), 240u) == 2u && resp[1] == DAP_OK);

    /* SWJ_Sequence with bit_count == 0 (256 bits) requires 2 + 32 = 34 bytes */
    uint8_t swj_256[34];
    memset(swj_256, 0xFF, sizeof(swj_256));
    swj_256[0] = ID_DAP_SWJ_Sequence;
    swj_256[1] = 0u; /* 256 bits */
    assert(pu_dap_process_packet(swj_256, 33u, resp, sizeof(resp), 300u) == 1u && resp[0] == ID_DAP_Invalid);
    assert(pu_dap_process_packet(conn_jtag, 2u, resp, sizeof(resp), 310u) == 2u);
    assert(pu_dap_process_packet(swj_256, 34u, resp, sizeof(resp), 320u) == 2u && resp[1] == DAP_OK);
    (void)pu_dap_process_packet(disc, 1u, resp, sizeof(resp), 330u);

    /* TransferBlock read overflow: 16 words -> 4 + 64 = 68B > 64B rejected; 15 words -> 64B valid */
    const uint8_t blk_16[5] = {ID_DAP_TransferBlock, 0u, 16u, 0u, DAP_TRANSFER_RnW};
    assert(pu_dap_process_packet(blk_16, sizeof(blk_16), resp, sizeof(resp), 400u) == 1u && resp[0] == ID_DAP_Invalid);
    const uint8_t blk_15[5] = {ID_DAP_TransferBlock, 0u, 15u, 0u, DAP_TRANSFER_RnW};
    assert(pu_dap_process_packet(blk_15, sizeof(blk_15), resp, sizeof(resp), 410u) >= 4u && resp[0] == ID_DAP_TransferBlock);

    /* Overflowing batch rejected BEFORE executing any sub-command */
    init_spy_dap();
    const uint8_t overflow_batch[] = {
        ID_DAP_ExecuteCommands, 3u,
        ID_DAP_Connect, DAP_PORT_SWD,
        ID_DAP_TransferBlock, 0u, 10u, 0u, DAP_TRANSFER_RnW, /* 44B reply */
        ID_DAP_TransferBlock, 0u, 10u, 0u, DAP_TRANSFER_RnW  /* +44B -> 92B > 64B! */
    };
    assert(pu_dap_process_packet(overflow_batch, sizeof(overflow_batch), resp, sizeof(resp), 500u) == 1u);
    assert(resp[0] == ID_DAP_Invalid);
    assert(g_spy.port_setup_calls == 0u && g_spy.swd_transfer_calls == 0u);

    /* Nested ExecuteCommands rejected */
    const uint8_t nested_batch[] = {ID_DAP_ExecuteCommands, 1u, ID_DAP_QueueCommands, 1u, ID_DAP_Disconnect};
    assert(pu_dap_process_packet(nested_batch, sizeof(nested_batch), resp, sizeof(resp), 510u) == 1u);
    assert(resp[0] == ID_DAP_Invalid);
}

static void test_50000_fuzzed_packets(void) {
    pu_dap_init_for_test();
    uint32_t seed = 0xC0FFEE42u;

    for (uint32_t i = 0; i < 50000u; ++i) {
        size_t req_len = xorshift32(&seed) % 69u; /* 0..68 including oversized >64 */
        size_t resp_cap = (i % 17u == 0u) ? (xorshift32(&seed) % 65u) : 64u;

        uint8_t *req = (uint8_t *)malloc(req_len != 0u ? req_len : 1u);
        uint8_t *resp = (uint8_t *)malloc(resp_cap != 0u ? resp_cap : 1u);
        assert(req != NULL && resp != NULL);

        for (size_t j = 0; j < req_len; ++j) {
            req[j] = (uint8_t)xorshift32(&seed);
        }

        size_t out = pu_dap_process_packet(req, req_len, resp, resp_cap, (uint64_t)i * 100u);
        assert(out <= resp_cap);
        assert(out <= PU_DAP_PACKET_SIZE);
        if (resp_cap == 0u) {
            assert(out == 0u);
        }

        free(resp);
        free(req);
    }
}

int main(void) {
    test_dap_info_and_basic_commands();
    test_dap_guard_snapshot_survives_buffer_reuse();
    test_dap_cross_interface_contention_and_deadline_renewal();
    test_dap_swj_pins_handoff_and_bounded_wait();
    test_dap_swj_pins_esp32_tdi_hold_across_delay_and_release();
    test_dap_transfer_ap_match_value_repolls_ap_register();
    test_dap_swj_pins_wait_masks_non_drivable_pins();
    test_dap_clock_64bit_math();
    test_dap_bounds_jtag_and_batch_guards();
    test_50000_fuzzed_packets();
    puts("test_dap_guard_fuzz: ALL PASSED (incl. 50,000 fuzzed packets)");
    return 0;
}
