/*
 * IF 5 Vendor Control, Telemetry & Hardware Fault Protection implementation.
 */

#include "firmware/control_telemetry.h"

#include <string.h>

#include "bootloader/journal.h"
#include "config/board_pinout.h"
#include "firmware/engine_owner.h"
#include "firmware/swio_engine.h"
#include "firmware/uart_bridge.h"

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#if !defined(PU_HOST_TEST)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

static pu_ctrl_hal_t s_hal;
static volatile bool s_vtarg_enabled = false;
static volatile bool s_bootsel_pending = false;

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 2:1 precision 1% voltage divider -> full-scale 6600 mV across 4096 ADC counts */
uint16_t pu_telemetry_adc_to_vtref_mv(uint16_t raw_adc) {
    if (raw_adc > 4095u) {
        raw_adc = 4095u;
    }
    return (uint16_t)(((uint32_t)raw_adc * 6600u + 2048u) / 4096u);
}

/* INA180A1 (20 V/V) * 0.1 Ohm shunt = 2.0 V/A -> I_mA = V_adc_mV / 2 */
uint16_t pu_telemetry_adc_to_itarg_ma(uint16_t raw_adc) {
    if (raw_adc > 4095u) {
        raw_adc = 4095u;
    }
    return (uint16_t)(((uint32_t)raw_adc * 3300u + 4096u) / 8192u);
}

void pu_ctrl_init(const pu_ctrl_hal_t *hal) {
    if (hal != NULL) {
        s_hal = *hal;
    } else {
        memset(&s_hal, 0, sizeof(s_hal));
    }
    s_vtarg_enabled = false;
    s_bootsel_pending = false;
}

#ifdef PU_HOST_TEST
static bool s_host_test_vtarg = false;
static void host_test_set_vtarg(bool en) { s_host_test_vtarg = en; }
static bool host_test_oc_active(void) { return false; }
static uint16_t host_test_adc_vtref(void) { return 2048u; }
static uint16_t host_test_adc_itarg(void) { return 0u; }
static uint64_t host_test_now_us(void) { return 10000u; }

void pu_ctrl_init_for_test(void) {
    static const pu_ctrl_hal_t kDefaultHostHal = {
        .board_is_pro_pcb = true,
        .has_level_shifter = true,
        .has_vtarg_switch = true,
        .has_adc_telemetry = true,
        .active_slot = 0u,
        .set_vtarg_power = host_test_set_vtarg,
        .is_overcurrent_active = host_test_oc_active,
        .read_adc_vtref_raw = host_test_adc_vtref,
        .read_adc_itarg_raw = host_test_adc_itarg,
        .request_ch32_unbrick = NULL,
        .enter_bootsel = NULL,
        .now_us = host_test_now_us
    };
    s_host_test_vtarg = false;
    pu_ctrl_init(&kDefaultHostHal);
}
#endif

void pu_telemetry_on_overcurrent_irq(void) {
    s_vtarg_enabled = false;
    if (s_hal.set_vtarg_power != NULL) {
        s_hal.set_vtarg_power(false);
    }
    pu_engine_latch_fault(PU_FAULT_OVERCURRENT);
}

bool pu_ctrl_is_bootsel_pending(void) {
    return __atomic_load_n(&s_bootsel_pending, __ATOMIC_ACQUIRE);
}

bool pu_ctrl_poll_deferred_bootsel(void) {
    if (!__atomic_exchange_n(&s_bootsel_pending, false, __ATOMIC_ACQ_REL)) {
        return false;
    }
    s_vtarg_enabled = false;
    if (s_hal.set_vtarg_power != NULL) {
        s_hal.set_vtarg_power(false);
    }
    pu_engine_on_usb_reset();
    if (s_hal.enter_bootsel != NULL) {
        s_hal.enter_bootsel();
    }
    return true;
}

static size_t write_status_reply(uint8_t *resp, size_t cap, uint8_t cmd, pu_ctrl_status_t st) {
    if (resp == NULL || cap < 2u) {
        return 0u;
    }
    resp[0] = cmd;
    resp[1] = (uint8_t)st;
    return 2u;
}

size_t pu_ctrl_handle_request(const uint8_t *req,
                              size_t len,
                              uint8_t *resp,
                              size_t cap) {
    if (req == NULL || len == 0u || resp == NULL || cap < 2u) {
        return 0u;
    }

    uint8_t cmd = req[0];
    switch (cmd) {
    case PU_CTRL_CMD_GET_INFO: {
        if (cap < 13u) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_INVALID_ARG);
        }
        uint8_t caps = 0u;
        if (s_hal.has_level_shifter) {
            caps |= PU_CAP_LEVEL_SHIFTER;
        }
        if (s_hal.has_vtarg_switch) {
            caps |= PU_CAP_VTARG_SWITCH;
        }
        if (s_hal.has_adc_telemetry) {
            caps |= PU_CAP_ADC_TELEMETRY;
        }
        resp[0] = cmd;
        resp[1] = (uint8_t)PU_CTRL_STATUS_OK;
        resp[2] = PU_FW_VERSION_MAJOR;
        resp[3] = PU_FW_VERSION_MINOR;
        resp[4] = PU_FW_VERSION_PATCH;
        resp[5] = s_hal.board_is_pro_pcb ? 1u : 0u;
        resp[6] = caps;
        resp[7] = (uint8_t)pu_engine_current();
        resp[8] = s_hal.active_slot;
        write_u32_le(&resp[9], pu_engine_fault_mask());
        return 13u;
    }

    case PU_CTRL_CMD_GET_TELEMETRY: {
        if (cap < 13u) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_INVALID_ARG);
        }
        uint16_t vtref_mv = 0u;
        uint16_t itarg_ma = 0u;
        if (s_hal.has_adc_telemetry) {
            if (s_hal.read_adc_vtref_raw != NULL) {
                vtref_mv = pu_telemetry_adc_to_vtref_mv(s_hal.read_adc_vtref_raw());
            }
            if (s_hal.read_adc_itarg_raw != NULL) {
                itarg_ma = pu_telemetry_adc_to_itarg_ma(s_hal.read_adc_itarg_raw());
            }
        }
        bool oc_now = (s_hal.is_overcurrent_active != NULL) ? s_hal.is_overcurrent_active() : false;
        bool oc_flag = oc_now || ((pu_engine_fault_mask() & PU_FAULT_OVERCURRENT) != 0u);

        resp[0] = cmd;
        resp[1] = (uint8_t)PU_CTRL_STATUS_OK;
        write_u16_le(&resp[2], vtref_mv);
        write_u16_le(&resp[4], itarg_ma);
        resp[6] = s_vtarg_enabled ? 1u : 0u;
        resp[7] = oc_flag ? 1u : 0u;
        resp[8] = s_hal.has_adc_telemetry ? 1u : 0u;
        write_u32_le(&resp[9], pu_uart_bridge_rx_drop_count());
        return 13u;
    }

    case PU_CTRL_CMD_SET_VTARG: {
        if (len < 2u || cap < 3u) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_INVALID_ARG);
        }
        if (!s_hal.has_vtarg_switch || s_hal.set_vtarg_power == NULL) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_UNSUPPORTED);
        }
        bool want_on = (req[1] != 0u);
        if (want_on) {
            bool oc_now = (s_hal.is_overcurrent_active != NULL) ? s_hal.is_overcurrent_active() : false;
            if (pu_engine_current() == PU_ENGINE_FAULT || oc_now) {
                s_vtarg_enabled = false;
                s_hal.set_vtarg_power(false);
                return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_FAULT_LATCHED);
            }
        }
        /* Record desired state BEFORE driving the switch so a synchronous overcurrent
         * ISR inside set_vtarg_power(true) is never clobbered back to true. */
        s_vtarg_enabled = want_on;
        s_hal.set_vtarg_power(want_on);
        if (want_on) {
            bool oc_post = (s_hal.is_overcurrent_active != NULL) ? s_hal.is_overcurrent_active() : false;
            if (!s_vtarg_enabled || pu_engine_current() == PU_ENGINE_FAULT || oc_post) {
                s_vtarg_enabled = false;
                s_hal.set_vtarg_power(false);
                if (pu_engine_current() != PU_ENGINE_FAULT) {
                    pu_engine_latch_fault(PU_FAULT_OVERCURRENT);
                }
                return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_FAULT_LATCHED);
            }
        }
        resp[0] = cmd;
        resp[1] = (uint8_t)PU_CTRL_STATUS_OK;
        resp[2] = want_on ? 1u : 0u;
        return 3u;
    }

    case PU_CTRL_CMD_CLEAR_FAULT: {
        if (s_hal.is_overcurrent_active != NULL && s_hal.is_overcurrent_active()) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_FAULT_LATCHED);
        }
        pu_status_t st = pu_engine_clear_fault();
        if (st == PU_ERR_ENGINE_BUSY) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_BUSY);
        }
        if (st != PU_OK) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_ERROR);
        }
        return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_OK);
    }

    case PU_CTRL_CMD_CH32_UNBRICK: {
        uint64_t now_us = (s_hal.now_us != NULL) ? s_hal.now_us() : 0u;
        pu_status_t st = (s_hal.request_ch32_unbrick != NULL)
                             ? s_hal.request_ch32_unbrick(now_us)
                             : pu_ch32_power_cycle_unbrick(now_us);
        if (st == PU_OK) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_OK);
        }
        if (st == PU_ERR_ENGINE_BUSY) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_BUSY);
        }
        if (st == PU_ERR_FAULT_LATCHED) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_FAULT_LATCHED);
        }
        if (st == PU_ERR_INVALID_ARG) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_UNSUPPORTED);
        }
        return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_ERROR);
    }

    case PU_CTRL_CMD_ENTER_BOOTSEL: {
        if (len < 5u || read_u32_le(&req[1]) != PU_CTRL_BOOTSEL_MAGIC) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_INVALID_ARG);
        }
        /* Defer ROM jump until caller flushes this USB OK status reply cleanly */
        __atomic_store_n(&s_bootsel_pending, true, __ATOMIC_RELEASE);
        return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_OK);
    }

    case PU_CTRL_CMD_SELF_TEST: {
        if (cap < 6u) {
            return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_INVALID_ARG);
        }
        uint32_t bits = 0u;
        pu_engine_state_t es = pu_engine_current();
        if (es != PU_ENGINE_FAULT) {
            bits |= PU_SELFTEST_ENGINE_OK | PU_SELFTEST_NO_FAULT_OK;
        }
        pu_journal_state_t jstate;
        if (pu_journal_load(&jstate)) {
            bits |= PU_SELFTEST_JOURNAL_OK;
        }
        if (!s_hal.has_adc_telemetry) {
            bits |= PU_SELFTEST_ADC_RAIL_OK;
        } else if (s_hal.read_adc_vtref_raw != NULL && s_hal.read_adc_itarg_raw != NULL) {
            uint16_t vtref_mv = pu_telemetry_adc_to_vtref_mv(s_hal.read_adc_vtref_raw());
            uint16_t itarg_ma = pu_telemetry_adc_to_itarg_ma(s_hal.read_adc_itarg_raw());
            if (vtref_mv <= 5600u && itarg_ma <= 550u) {
                bits |= PU_SELFTEST_ADC_RAIL_OK;
            }
        }
        resp[0] = cmd;
        resp[1] = (uint8_t)PU_CTRL_STATUS_OK;
        write_u32_le(&resp[2], bits);
        return 6u;
    }

    default:
        return write_status_reply(resp, cap, cmd, PU_CTRL_STATUS_UNSUPPORTED);
    }
}

#if !defined(PU_HOST_TEST)
static void rp2040_set_vtarg_power(bool enabled) {
#if PU_HAS_VTARG_SWITCH
    gpio_put(PU_PIN_VTARG_EN, enabled);
#else
    (void)enabled;
#endif
}

static bool rp2040_is_overcurrent_active(void) {
#if PU_HAS_VTARG_SWITCH
    /* VTARG_FAULT_N is active-LOW open-drain fault flag from AP22811/TPS2553 */
    return !gpio_get(PU_PIN_VTARG_FAULT_N);
#else
    return false;
#endif
}

static uint16_t rp2040_adc_sample_averaged(uint input) {
#if PU_HAS_ADC_TELEMETRY
    adc_select_input(input);
    (void)adc_read(); /* Discard first conversion to eliminate mux charge cross-talk */
    uint32_t sum = 0u;
    for (uint i = 0u; i < 4u; ++i) {
        sum += adc_read();
    }
    return (uint16_t)((sum + 2u) / 4u);
#else
    (void)input;
    return 0u;
#endif
}

static uint16_t rp2040_read_adc_vtref_raw(void) {
    return rp2040_adc_sample_averaged(0u); /* GP26 = ADC0 */
}

static uint16_t rp2040_read_adc_itarg_raw(void) {
    return rp2040_adc_sample_averaged(1u); /* GP27 = ADC1 */
}

static void rp2040_enter_bootsel(void) {
    reset_usb_boot(0u, 0u);
}

static uint64_t rp2040_ctrl_now_us(void) {
    return time_us_64();
}

#if PU_HAS_VTARG_SWITCH
static void rp2040_vtarg_fault_gpio_cb(uint gpio, uint32_t events) {
    if (gpio == PU_PIN_VTARG_FAULT_N && (events & GPIO_IRQ_EDGE_FALL) != 0u) {
        pu_telemetry_on_overcurrent_irq();
    }
}
#endif

void pu_ctrl_rp2040_init(uint8_t active_slot,
                         pu_status_t (*request_ch32_unbrick_cb)(uint64_t now_us)) {
#if PU_HAS_VTARG_SWITCH
    gpio_init(PU_PIN_VTARG_EN);
    gpio_put(PU_PIN_VTARG_EN, false);
    gpio_set_dir(PU_PIN_VTARG_EN, GPIO_OUT);

    gpio_init(PU_PIN_VTARG_FAULT_N);
    gpio_set_dir(PU_PIN_VTARG_FAULT_N, GPIO_IN);
    gpio_pull_up(PU_PIN_VTARG_FAULT_N);
    busy_wait_us_32(100u); /* Settle internal pull-up before sampling/arming IRQ */
#endif
#if PU_HAS_ADC_TELEMETRY
    adc_init();
    adc_gpio_init(PU_PIN_ADC_VTREF);
    adc_gpio_init(PU_PIN_ADC_ITARG);
#endif

    pu_ctrl_hal_t hal;
    memset(&hal, 0, sizeof(hal));
#if defined(PU_BOARD_PRO_PCB)
    hal.board_is_pro_pcb = true;
#else
    hal.board_is_pro_pcb = false;
#endif
    hal.has_level_shifter = (PU_HAS_LEVEL_SHIFTER != 0);
    hal.has_vtarg_switch = (PU_HAS_VTARG_SWITCH != 0);
    hal.has_adc_telemetry = (PU_HAS_ADC_TELEMETRY != 0);
    hal.active_slot = active_slot;
    hal.set_vtarg_power = (PU_HAS_VTARG_SWITCH != 0) ? rp2040_set_vtarg_power : NULL;
    hal.is_overcurrent_active = (PU_HAS_VTARG_SWITCH != 0) ? rp2040_is_overcurrent_active : NULL;
    hal.read_adc_vtref_raw = (PU_HAS_ADC_TELEMETRY != 0) ? rp2040_read_adc_vtref_raw : NULL;
    hal.read_adc_itarg_raw = (PU_HAS_ADC_TELEMETRY != 0) ? rp2040_read_adc_itarg_raw : NULL;
    hal.request_ch32_unbrick = request_ch32_unbrick_cb;
    hal.enter_bootsel = rp2040_enter_bootsel;
    hal.now_us = rp2040_ctrl_now_us;
    pu_ctrl_init(&hal);

#if PU_HAS_VTARG_SWITCH
    /* Catch already-asserted level-LOW hardware faults before enabling edge IRQ */
    if (!gpio_get(PU_PIN_VTARG_FAULT_N)) {
        pu_telemetry_on_overcurrent_irq();
    }
    gpio_set_irq_enabled_with_callback(PU_PIN_VTARG_FAULT_N, GPIO_IRQ_EDGE_FALL, true,
                                       rp2040_vtarg_fault_gpio_cb);
#endif
}
#endif
