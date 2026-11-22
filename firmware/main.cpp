/*
 * Pico Universal — Unified Dual-Core Composite USB Firmware Entry Point (RP2040).
 *
 * Core 0: TinyUSB device stack (IF 0 DAP v2, IF 1..2 CH32 SWIO CDC, IF 3..4 UART CDC,
 *         IF 5 Control/Telemetry), UART bridge pumping, A/B trial journal confirmation,
 *         VTARG overcurrent IRQ supervision, status LEDs, and hardware watchdog.
 * Core 1: Deterministic real-time PIO0 (SWD/JTAG) & PIO1 (CH32 SWIO) protocol worker
 *         fed via lock-free SPSC queues, with multicore flash-lockout support.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bootloader/journal.h"
#include "config/board_pinout.h"
#include "config/flash_layout.h"
#include "firmware/control_telemetry.h"
#include "firmware/dap_engine.h"
#include "firmware/engine_owner.h"
#include "firmware/swio_engine.h"
#include "firmware/uart_bridge.h"
#include "firmware/usb_descriptors.h"

#if !defined(PU_HOST_TEST)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "firmware/tusb_config.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "tusb.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

constexpr uint32_t kWatchdogTimeoutMs = 4000u;
constexpr size_t kDapQueueDepth = 8u;
constexpr size_t kSwioRingCap = 4096u;

struct DapPacketSlot {
    uint8_t data[PU_DAP_PACKET_SIZE];
    uint16_t len;
};

struct DapPacketQueue {
    DapPacketSlot slots[kDapQueueDepth];
    volatile uint32_t head;
    volatile uint32_t tail;
};

struct ByteSpscRing {
    uint8_t buf[kSwioRingCap];
    volatile uint32_t head;
    volatile uint32_t tail;
};

DapPacketQueue g_dap_req_q{};
DapPacketQueue g_dap_resp_q{};
ByteSpscRing g_swio_rx_q{};
ByteSpscRing g_swio_tx_q{};
volatile bool g_core1_lockout_ready = false;
volatile bool g_dap_disconnect_pending = false;
volatile bool g_swio_disconnect_pending = false;
volatile bool g_ch32_unbrick_pending = false;
volatile bool g_dap_resp_flush_pending = false;
volatile bool g_swio_tx_flush_pending = false;

size_t dap_q_free(const DapPacketQueue &q) {
    uint32_t h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);
    return kDapQueueDepth - static_cast<size_t>(h - t);
}

void dap_q_drain_consumer(DapPacketQueue &q) {
    uint32_t h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    __atomic_store_n(&q.tail, h, __ATOMIC_RELEASE);
}

void byte_q_drain_consumer(ByteSpscRing &q) {
    uint32_t h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    __atomic_store_n(&q.tail, h, __ATOMIC_RELEASE);
}

pu_status_t core0_request_ch32_unbrick(uint64_t now_us) {
    (void)now_us;
#if !PU_HAS_VTARG_SWITCH
    return PU_ERR_INVALID_ARG;
#else
    if (pu_engine_current() == PU_ENGINE_FAULT) {
        return PU_ERR_FAULT_LATCHED;
    }
    if (pu_engine_current() != PU_ENGINE_IDLE) {
        return PU_ERR_ENGINE_BUSY;
    }
    if (__atomic_exchange_n(&g_ch32_unbrick_pending, true, __ATOMIC_ACQ_REL)) {
        return PU_ERR_ENGINE_BUSY;
    }
    return PU_OK;
#endif
}

bool dap_q_push(DapPacketQueue &q, const uint8_t *data, size_t len) {
    uint32_t h = q.head;
    uint32_t t = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);
    if ((h - t) >= kDapQueueDepth) {
        return false;
    }
    DapPacketSlot &slot = q.slots[h & (kDapQueueDepth - 1u)];
    size_t copy_len = (len > PU_DAP_PACKET_SIZE) ? PU_DAP_PACKET_SIZE : len;
    memcpy(slot.data, data, copy_len);
    slot.len = static_cast<uint16_t>(copy_len);
    __atomic_store_n(&q.head, h + 1u, __ATOMIC_RELEASE);
    return true;
}

bool dap_q_pop(DapPacketQueue &q, uint8_t *out, size_t *out_len) {
    uint32_t t = q.tail;
    uint32_t h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    if (h == t) {
        return false;
    }
    const DapPacketSlot &slot = q.slots[t & (kDapQueueDepth - 1u)];
    memcpy(out, slot.data, slot.len);
    *out_len = slot.len;
    __atomic_store_n(&q.tail, t + 1u, __ATOMIC_RELEASE);
    return true;
}

size_t byte_q_used(const ByteSpscRing &q) {
    uint32_t h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);
    return static_cast<size_t>((h - t) & (kSwioRingCap - 1u));
}

size_t byte_q_push(ByteSpscRing &q, const uint8_t *data, size_t len) {
    uint32_t h = q.head;
    uint32_t t = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);
    size_t avail = (kSwioRingCap - 1u) - static_cast<size_t>((h - t) & (kSwioRingCap - 1u));
    size_t n = (len < avail) ? len : avail;
    for (size_t i = 0u; i < n; ++i) {
        q.buf[h] = data[i];
        h = (h + 1u) & (kSwioRingCap - 1u);
    }
    __atomic_store_n(&q.head, h, __ATOMIC_RELEASE);
    return n;
}

size_t byte_q_pop(ByteSpscRing &q, uint8_t *out, size_t max_len) {
    uint32_t t = q.tail;
    uint32_t h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    size_t avail = static_cast<size_t>((h - t) & (kSwioRingCap - 1u));
    size_t n = (max_len < avail) ? max_len : avail;
    for (size_t i = 0u; i < n; ++i) {
        out[i] = q.buf[t];
        t = (t + 1u) & (kSwioRingCap - 1u);
    }
    __atomic_store_n(&q.tail, t, __ATOMIC_RELEASE);
    return n;
}

void hw_drain_pio(void) {
    pio_sm_set_enabled(pio0, 0u, false);
    pio_sm_clear_fifos(pio0, 0u);
    pio_sm_set_enabled(pio1, 0u, false);
    pio_sm_clear_fifos(pio1, 0u);
}

void hw_tristate_debug_pins(void) {
    const uint pins[] = {
        PU_PIN_SWCLK_TCK,
        PU_PIN_SWDIO_TMS,
        PU_PIN_JTAG_TDI,
        PU_PIN_JTAG_TDO,
        PU_PIN_TARGET_NRST,
        PU_PIN_SWIO
    };
    for (uint pin : pins) {
        gpio_set_outover(pin, GPIO_OVERRIDE_NORMAL);
        gpio_set_oeover(pin, GPIO_OVERRIDE_NORMAL);
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
    }
    gpio_pull_up(PU_PIN_TARGET_NRST);
#if PU_HAS_LEVEL_SHIFTER
    gpio_set_outover(PU_PIN_SWDIO_DIR, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(PU_PIN_SWDIO_DIR, GPIO_OVERRIDE_NORMAL);
    gpio_init(PU_PIN_SWDIO_DIR);
    gpio_put(PU_PIN_SWDIO_DIR, false);
    gpio_set_dir(PU_PIN_SWDIO_DIR, GPIO_OUT);
#endif
}

void hw_set_swio_pullup(bool enabled) {
#if defined(PU_PIN_SWIO_PU_EN)
    gpio_init(PU_PIN_SWIO_PU_EN);
    gpio_put(PU_PIN_SWIO_PU_EN, enabled);
    gpio_set_dir(PU_PIN_SWIO_PU_EN, GPIO_OUT);
#else
    (void)enabled;
#endif
}

void hw_set_vtarg_power(bool enabled) {
#if PU_HAS_VTARG_SWITCH
    gpio_put(PU_PIN_VTARG_EN, enabled);
#else
    (void)enabled;
#endif
}

const pu_pin_hal_t kRp2040PinHal = {
    hw_drain_pio,
    hw_tristate_debug_pins,
    hw_set_swio_pullup,
    (PU_HAS_VTARG_SWITCH != 0) ? hw_set_vtarg_power : nullptr
};

void leds_init(void) {
#if defined(PU_BOARD_PRO_PCB)
    const uint rgb[3] = { PU_PIN_LED_R, PU_PIN_LED_G, PU_PIN_LED_B };
    for (uint pin : rgb) {
        gpio_init(pin);
        gpio_put(pin, false);
        gpio_set_dir(pin, GPIO_OUT);
    }
#else
    gpio_init(PU_PIN_LED_STATUS);
    gpio_put(PU_PIN_LED_STATUS, true);
    gpio_set_dir(PU_PIN_LED_STATUS, GPIO_OUT);
#endif
}

void leds_update(uint64_t now_us) {
    pu_engine_state_t st = pu_engine_current();
    bool blink_fast = ((now_us / 125000ull) & 1ull) != 0ull;
#if defined(PU_BOARD_PRO_PCB)
    bool r = (st == PU_ENGINE_FAULT) ? blink_fast : (st == PU_ENGINE_RECOVERY);
    bool g = (st == PU_ENGINE_IDLE) || (st == PU_ENGINE_SWD) || (st == PU_ENGINE_JTAG);
    bool b = (st == PU_ENGINE_SWIO) || (st == PU_ENGINE_JTAG);
    gpio_put(PU_PIN_LED_R, r);
    gpio_put(PU_PIN_LED_G, g);
    gpio_put(PU_PIN_LED_B, b);
#else
    bool on = (st == PU_ENGINE_FAULT) ? blink_fast : true;
    gpio_put(PU_PIN_LED_STATUS, on);
#endif
}

uint8_t detect_running_slot(void) {
    uintptr_t pc = reinterpret_cast<uintptr_t>(&detect_running_slot);
    if (pc >= SLOT_APP_B_ADDR && pc < (SLOT_APP_B_ADDR + SLOT_APP_B_SIZE)) {
        return PU_SLOT_B;
    }
    return PU_SLOT_A;
}

void confirm_trial_boot_if_needed(uint8_t running_slot) {
    pu_journal_state_t state{};
    if (!pu_journal_load(&state)) {
        /* Seed initial baseline A/B journal record on first boot after factory UF2 flash. */
        state.active_slot = running_slot;
        state.pending_slot = PU_SLOT_NONE;
        state.boot_attempts = 0u;
        state.flags = 0u;
        (void)pu_journal_commit(&state);
        return;
    }
    if ((state.flags & PU_JOURNAL_FLAG_TRIAL) != 0u && state.pending_slot == running_slot) {
        state.active_slot = running_slot;
        state.pending_slot = PU_SLOT_NONE;
        state.boot_attempts = 0u;
        state.flags = static_cast<uint8_t>(state.flags & ~PU_JOURNAL_FLAG_TRIAL);
        (void)pu_journal_commit(&state);
    }
}

void core1_protocol_worker(void) {
    /* Allow Core 0 flash_safe_execute() to lock out Core 1 safely during journal commits. */
    multicore_lockout_victim_init();
    __atomic_store_n(&g_core1_lockout_ready, true, __ATOMIC_RELEASE);

    uint8_t dap_req[PU_DAP_PACKET_SIZE];
    uint8_t dap_resp[PU_DAP_PACKET_SIZE];
    uint8_t swio_in[64];
    uint8_t swio_out[128];
    bool fault_quiesced = false;

    for (;;) {
        uint64_t now_us = time_us_64();

        if (__atomic_exchange_n(&g_dap_disconnect_pending, false, __ATOMIC_ACQ_REL)) {
            dap_q_drain_consumer(g_dap_req_q);
            __atomic_store_n(&g_dap_resp_flush_pending, true, __ATOMIC_RELEASE);
            pu_dap_on_usb_disconnect();
        }
        if (__atomic_exchange_n(&g_swio_disconnect_pending, false, __ATOMIC_ACQ_REL)) {
            byte_q_drain_consumer(g_swio_rx_q);
            __atomic_store_n(&g_swio_tx_flush_pending, true, __ATOMIC_RELEASE);
            pu_ch32_on_disconnect();
        }

        if (pu_engine_current() == PU_ENGINE_FAULT) {
            /* Quiesce PIO0/PIO1 & drain stale queues ONCE per fault latch */
            if (!fault_quiesced) {
                __atomic_store_n(&g_ch32_unbrick_pending, false, __ATOMIC_RELEASE);
                dap_q_drain_consumer(g_dap_req_q);
                byte_q_drain_consumer(g_swio_rx_q);
                __atomic_store_n(&g_dap_resp_flush_pending, true, __ATOMIC_RELEASE);
                __atomic_store_n(&g_swio_tx_flush_pending, true, __ATOMIC_RELEASE);
                pu_dap_on_usb_disconnect();
                pu_ch32_on_disconnect();
                fault_quiesced = true;
            }
            tight_loop_contents();
            continue;
        }
        fault_quiesced = false;

        if (__atomic_exchange_n(&g_ch32_unbrick_pending, false, __ATOMIC_ACQ_REL)) {
            (void)pu_ch32_power_cycle_unbrick(now_us);
        }

        /* Only pop a DAP request once Core 0 finishes any pending response flush */
        size_t req_len = 0u;
        if (!__atomic_load_n(&g_dap_resp_flush_pending, __ATOMIC_ACQUIRE) &&
            dap_q_free(g_dap_resp_q) > 0u &&
            dap_q_pop(g_dap_req_q, dap_req, &req_len)) {
            size_t resp_len = pu_dap_process_packet(dap_req, req_len, dap_resp, sizeof(dap_resp), now_us);
            if (resp_len > 0u) {
                (void)dap_q_push(g_dap_resp_q, dap_resp, resp_len);
            }
        }

        if (!__atomic_load_n(&g_swio_tx_flush_pending, __ATOMIC_ACQUIRE)) {
            size_t swio_tx_free = (kSwioRingCap - 1u) - byte_q_used(g_swio_tx_q);
            size_t out_cap = (swio_tx_free < sizeof(swio_out)) ? swio_tx_free : sizeof(swio_out);
            if (out_cap > 0u) {
                size_t in_n = byte_q_pop(g_swio_rx_q, swio_in, sizeof(swio_in));
                size_t produced = (in_n > 0u)
                                      ? pu_ch32_gdb_feed(swio_in, in_n, swio_out, out_cap, now_us)
                                      : pu_ch32_gdb_poll(swio_out, out_cap, now_us);
                if (produced > 0u) {
                    (void)byte_q_push(g_swio_tx_q, swio_out, produced);
                }
            }
        }
    }
}

void pump_usb_endpoints(void) {
    if (__atomic_exchange_n(&g_dap_resp_flush_pending, false, __ATOMIC_ACQ_REL)) {
        dap_q_drain_consumer(g_dap_resp_q);
        tud_vendor_n_read_flush(PU_TUD_VENDOR_IDX_DAP);
    }
    if (__atomic_exchange_n(&g_swio_tx_flush_pending, false, __ATOMIC_ACQ_REL)) {
        byte_q_drain_consumer(g_swio_tx_q);
    }

    /* IF 0: CMSIS-DAP v2 Bulk OUT -> Core 1 req queue */
    if (tud_vendor_n_available(PU_TUD_VENDOR_IDX_DAP) > 0u) {
        if (dap_q_free(g_dap_req_q) > 0u) {
            uint8_t buf[PU_DAP_PACKET_SIZE];
            uint32_t n = tud_vendor_n_read(PU_TUD_VENDOR_IDX_DAP, buf, sizeof(buf));
            if (n > 0u) {
                (void)dap_q_push(g_dap_req_q, buf, n);
            }
        }
    }
    /* IF 0: Core 1 resp queue -> CMSIS-DAP v2 Bulk IN */
    if (tud_vendor_n_write_available(PU_TUD_VENDOR_IDX_DAP) >= PU_DAP_PACKET_SIZE) {
        uint8_t buf[PU_DAP_PACKET_SIZE];
        size_t n = 0u;
        if (dap_q_pop(g_dap_resp_q, buf, &n) && n > 0u) {
            (void)tud_vendor_n_write(PU_TUD_VENDOR_IDX_DAP, buf, static_cast<uint32_t>(n));
            (void)tud_vendor_n_write_flush(PU_TUD_VENDOR_IDX_DAP);
        }
    }

    /* IF 1..2: CH32V003 SWIO CDC <-> Core 1 byte queues (gate TX on tud_mounted, not DTR) */
    if (tud_cdc_n_available(PU_TUD_CDC_IDX_SWIO) > 0u) {
        size_t rx_free = (kSwioRingCap - 1u) - byte_q_used(g_swio_rx_q);
        if (rx_free > 0u) {
            uint8_t buf[64];
            size_t want = (rx_free < sizeof(buf)) ? rx_free : sizeof(buf);
            uint32_t got = tud_cdc_n_read(PU_TUD_CDC_IDX_SWIO, buf, static_cast<uint32_t>(want));
            if (got > 0u) {
                (void)byte_q_push(g_swio_rx_q, buf, got);
            }
        }
    }
    if (tud_mounted()) {
        uint32_t wr_avail = tud_cdc_n_write_available(PU_TUD_CDC_IDX_SWIO);
        if (wr_avail > 0u && byte_q_used(g_swio_tx_q) > 0u) {
            uint8_t buf[64];
            size_t want = (wr_avail < sizeof(buf)) ? static_cast<size_t>(wr_avail) : sizeof(buf);
            size_t popped = byte_q_pop(g_swio_tx_q, buf, want);
            if (popped > 0u) {
                (void)tud_cdc_n_write(PU_TUD_CDC_IDX_SWIO, buf, static_cast<uint32_t>(popped));
                (void)tud_cdc_n_write_flush(PU_TUD_CDC_IDX_SWIO);
            }
        }
    }

    /* IF 3..4: Concurrent Target UART Bridge */
    pu_uart_bridge_task();

    /* IF 5: Control & Telemetry Vendor Bulk */
    if (tud_vendor_n_available(PU_TUD_VENDOR_IDX_CTRL) > 0u &&
        tud_vendor_n_write_available(PU_TUD_VENDOR_IDX_CTRL) >= PU_EP_CTRL_SIZE) {
        uint8_t req[PU_EP_CTRL_SIZE];
        uint8_t resp[PU_EP_CTRL_SIZE];
        uint32_t n = tud_vendor_n_read(PU_TUD_VENDOR_IDX_CTRL, req, sizeof(req));
        if (n > 0u) {
            size_t out_n = pu_ctrl_handle_request(req, n, resp, sizeof(resp));
            if (out_n > 0u) {
                (void)tud_vendor_n_write(PU_TUD_VENDOR_IDX_CTRL, resp, static_cast<uint32_t>(out_n));
                (void)tud_vendor_n_write_flush(PU_TUD_VENDOR_IDX_CTRL);
            }
        }
    }
}

} // namespace

extern "C" {

void tud_umount_cb(void) {
    __atomic_store_n(&g_dap_disconnect_pending, true, __ATOMIC_RELEASE);
    __atomic_store_n(&g_swio_disconnect_pending, true, __ATOMIC_RELEASE);
    pu_engine_on_usb_reset();
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    __atomic_store_n(&g_dap_disconnect_pending, true, __ATOMIC_RELEASE);
    __atomic_store_n(&g_swio_disconnect_pending, true, __ATOMIC_RELEASE);
    pu_engine_on_usb_reset();
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == PU_TUD_CDC_IDX_SWIO && !dtr) {
        __atomic_store_n(&g_swio_disconnect_pending, true, __ATOMIC_RELEASE);
    }
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p_line_coding) {
    if (itf != PU_TUD_CDC_IDX_UART || p_line_coding == nullptr) {
        return;
    }
    uint8_t stop_bits = (p_line_coding->stop_bits == 2u) ? 2u : 1u;
    pu_uart_parity_t parity = PU_UART_PARITY_NONE;
    if (p_line_coding->parity == 1u) {
        parity = PU_UART_PARITY_ODD;
    } else if (p_line_coding->parity == 2u) {
        parity = PU_UART_PARITY_EVEN;
    }
    (void)pu_uart_bridge_apply_line_coding(p_line_coding->bit_rate,
                                           p_line_coding->data_bits,
                                           stop_bits,
                                           parity);
}

} // extern "C"

int main(void) {
    uint8_t active_slot = detect_running_slot();

    leds_init();
    pu_engine_init(&kRp2040PinHal);
    pu_dap_init(nullptr);
    pu_swio_init(nullptr);
    pu_uart_bridge_rp2040_init();
    pu_ctrl_rp2040_init(active_slot, core0_request_ch32_unbrick);

    /* Launch Core 1 protocol worker and wait until lockout victim is armed before flash ops */
    __atomic_store_n(&g_core1_lockout_ready, false, __ATOMIC_RELEASE);
    multicore_launch_core1(core1_protocol_worker);
    while (!__atomic_load_n(&g_core1_lockout_ready, __ATOMIC_ACQUIRE)) {
        tight_loop_contents();
    }

    tusb_init();
    confirm_trial_boot_if_needed(active_slot);
    watchdog_enable(kWatchdogTimeoutMs, true);

    for (;;) {
        tud_task();
        pump_usb_endpoints();
        if (pu_ctrl_is_bootsel_pending()) {
            /* Flush USB OK status packet onto the wire BEFORE shutting down into ROM BOOTSEL */
            for (int i = 0; i < 20; ++i) {
                tud_task();
                busy_wait_us_32(1000u);
            }
            (void)pu_ctrl_poll_deferred_bootsel();
        }
        uint64_t now_us = time_us_64();
        pu_engine_tick(now_us);
        leds_update(now_us);
        watchdog_update();
    }
}
#else
int main(void) {
    return 0;
}
#endif
