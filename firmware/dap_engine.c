/*
 * Hardened CMSIS-DAP v2 Engine (PIO0 SWD/JTAG + dap_guard + engine_owner).
 */

#include "firmware/dap_engine.h"

#include <string.h>

#include "firmware/engine_owner.h"
#include "firmware/usb_descriptors.h"

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#define PU_DAP_CAPABILITIES_MASK    0x13u /* Bit0=SWD, Bit1=JTAG, Bit4=Atomic Commands */
#define PU_DAP_CMSIS_VER_STR        "2.1.0"
#define PU_DAP_FW_VER_STR           "1.0.0"
#define PU_DAP_PIO_OP_TIMEOUT_US    50000u

/* JTAG IEEE 1149.1 / ARM ADIv5 IR opcodes */
#define PU_JTAG_IR_ABORT            0x08u
#define PU_JTAG_IR_DPACC            0x0Au
#define PU_JTAG_IR_APACC            0x0Bu
#define PU_JTAG_IR_IDCODE           0x0Eu
#define PU_JTAG_IR_BYPASS           0xFFFFFFFFu

#if defined(__cplusplus)
static_assert(PU_PIN_JTAG_TDI == PU_PIN_SWDIO_TMS + 1u,
              "JTAG TMS and TDI must occupy consecutive GPIOs for PIO0 out pins, 2");
#else
_Static_assert(PU_PIN_JTAG_TDI == PU_PIN_SWDIO_TMS + 1u,
               "JTAG TMS and TDI must occupy consecutive GPIOs for PIO0 out pins, 2");
#endif

/* -------------------------------------------------------------------------- */
/* 64-bit Clock Divider Math                                                  */
/* -------------------------------------------------------------------------- */

uint32_t pu_dap_delay_to_khz(uint32_t sys_hz, uint32_t delay) {
    uint64_t denom = 2000ull * ((uint64_t)delay + 1ull);
    uint64_t khz = (uint64_t)sys_hz / denom;
    return (khz != 0ull) ? (uint32_t)khz : 1u;
}

uint32_t pu_dap_compute_pio_clkdiv_q16_8(uint32_t sys_hz,
                                         uint32_t target_hz,
                                         uint32_t *out_effective_khz) {
    if (sys_hz == 0u || target_hz == 0u) {
        if (out_effective_khz != NULL) {
            *out_effective_khz = 0u;
        }
        return 0u;
    }

    uint32_t max_pio_hz = sys_hz / 4u;
    if (max_pio_hz < PU_DAP_MIN_CLOCK_HZ) {
        max_pio_hz = PU_DAP_MIN_CLOCK_HZ;
    }
    uint32_t ceiling_hz = (max_pio_hz < PU_DAP_MAX_CLOCK_HZ) ? max_pio_hz : PU_DAP_MAX_CLOCK_HZ;

    uint32_t clamped_hz = target_hz;
    if (clamped_hz < PU_DAP_MIN_CLOCK_HZ) {
        clamped_hz = PU_DAP_MIN_CLOCK_HZ; /* 1 kHz floor */
    }
    if (clamped_hz > ceiling_hz) {
        clamped_hz = ceiling_hz;
    }

    /* 4 PIO cycles per SWCLK/TCK period; round divider UP so wire <= clamped_hz */
    uint64_t numer = (uint64_t)sys_hz * 256ull;
    uint64_t denom = 4ull * (uint64_t)clamped_hz;
    uint64_t div_q16_8 = (numer + denom - 1ull) / denom;

    if (div_q16_8 < 256ull) {
        div_q16_8 = 256ull;
    } else if (div_q16_8 > 0xFFFFFFull) {
        div_q16_8 = 0xFFFFFFull;
    }

    if (out_effective_khz != NULL) {
        uint64_t eff_hz = ((uint64_t)sys_hz * 64ull) / div_q16_8;
        uint32_t eff_khz = (uint32_t)(eff_hz / 1000ull);
        *out_effective_khz = (eff_khz != 0u) ? eff_khz : 1u;
    }

    return (uint32_t)div_q16_8;
}

/* -------------------------------------------------------------------------- */
/* Platform Wire HAL: RP2040 PIO0 Hardware Backend vs Host Simulation         */
/* -------------------------------------------------------------------------- */

#define PU_DAP_DRIVABLE_PINS_MASK \
    ((uint8_t)(PU_DAP_PIN_SWCLK_TCK | PU_DAP_PIN_SWDIO_TMS | PU_DAP_PIN_TDI | PU_DAP_PIN_nRESET))

static uint8_t dap_wait_for_drivable_pins(uint8_t pin_output,
                                          uint8_t pin_select,
                                          uint32_t wait_us,
                                          uint8_t (*sample_fn)(void),
                                          uint64_t (*now_us_fn)(void)) {
    const uint8_t drivable_mask = (uint8_t)(pin_select & PU_DAP_DRIVABLE_PINS_MASK);
    uint8_t sampled = sample_fn();
    if (wait_us != 0u) {
        uint64_t start = now_us_fn();
        while (((sampled ^ pin_output) & drivable_mask) != 0u) {
            if ((now_us_fn() - start) >= (uint64_t)wait_us) {
                break;
            }
            sampled = sample_fn();
        }
    }
    return sampled;
}

#if !defined(PU_HOST_TEST)

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/time.h"
#if defined(__has_include) && __has_include("swd.pio.h") && __has_include("jtag.pio.h")
#include "swd.pio.h"
#include "jtag.pio.h"
#else
/* Assembled directly from firmware/pio/swd.pio (16 words) & jtag.pio (7 words) */
#define pu_swd_wrap_target 0
#define pu_swd_wrap 15
#define pu_swd_offset_get_cmd 0u
static const uint16_t pu_swd_program_instructions[] = {
    0x90a0, 0x6028, 0x6041, 0x006a, 0xe001, 0x6081, 0x80a0, 0x7101,
    0x1947, 0x1000, 0x7081, 0xe000, 0xb142, 0x5901, 0x104c, 0x9020,
};
static const struct pio_program pu_swd_program = {
    .instructions = pu_swd_program_instructions,
    .length = 16,
    .origin = -1,
};
static inline void pu_swd_program_init(PIO pio, uint sm, uint offset, uint32_t clkdiv_q16_8) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + pu_swd_wrap_target, offset + pu_swd_wrap);
    sm_config_set_sideset(&c, 2, true, false);
    sm_config_set_sideset_pins(&c, PU_PIN_SWCLK_TCK);
    sm_config_set_out_pins(&c, PU_PIN_SWDIO_TMS, 1u);
    sm_config_set_in_pins(&c, PU_PIN_SWDIO_TMS);
#if PU_HAS_LEVEL_SHIFTER
    sm_config_set_set_pins(&c, PU_PIN_SWDIO_DIR, 1u);
#else
    sm_config_set_set_pins(&c, PU_PIN_SWDIO_TMS, 0u);
#endif
    sm_config_set_out_shift(&c, true, false, 32u);
    sm_config_set_in_shift(&c, true, false, 32u);
    c.clkdiv = (clkdiv_q16_8 << PIO_SM0_CLKDIV_FRAC_LSB);
    pio_gpio_init(pio, PU_PIN_SWCLK_TCK);
    pio_gpio_init(pio, PU_PIN_SWDIO_TMS);
    gpio_pull_up(PU_PIN_SWDIO_TMS);
#if PU_HAS_LEVEL_SHIFTER
    pio_gpio_init(pio, PU_PIN_SWDIO_DIR);
    pio_sm_set_pins_with_mask(pio, sm,
                              (1u << PU_PIN_SWDIO_TMS) | (1u << PU_PIN_SWDIO_DIR),
                              (1u << PU_PIN_SWCLK_TCK) | (1u << PU_PIN_SWDIO_TMS) | (1u << PU_PIN_SWDIO_DIR));
    pio_sm_set_consecutive_pindirs(pio, sm, PU_PIN_SWCLK_TCK, 2u, true);
    pio_sm_set_consecutive_pindirs(pio, sm, PU_PIN_SWDIO_DIR, 1u, true);
#else
    pio_sm_set_pins_with_mask(pio, sm,
                              (1u << PU_PIN_SWDIO_TMS),
                              (1u << PU_PIN_SWCLK_TCK) | (1u << PU_PIN_SWDIO_TMS));
    pio_sm_set_consecutive_pindirs(pio, sm, PU_PIN_SWCLK_TCK, 2u, true);
#endif
    pio_sm_init(pio, sm, offset + pu_swd_offset_get_cmd, &c);
    pio_sm_set_enabled(pio, sm, true);
}

#define pu_jtag_wrap_target 0
#define pu_jtag_wrap 6
#define pu_jtag_offset_get_cmd 0u
static const uint16_t pu_jtag_program_instructions[] = {
    0x80a0, 0x6024, 0x80a0, 0x6102, 0x5101, 0x0043, 0x8020,
};
static const struct pio_program pu_jtag_program = {
    .instructions = pu_jtag_program_instructions,
    .length = 7,
    .origin = -1,
};
static inline void pu_jtag_program_init(PIO pio, uint sm, uint offset, uint32_t clkdiv_q16_8) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + pu_jtag_wrap_target, offset + pu_jtag_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, PU_PIN_SWCLK_TCK);
    sm_config_set_out_pins(&c, PU_PIN_SWDIO_TMS, 2u);
    sm_config_set_in_pins(&c, PU_PIN_JTAG_TDO);
    sm_config_set_out_shift(&c, true, false, 32u);
    sm_config_set_in_shift(&c, true, false, 32u);
    c.clkdiv = (clkdiv_q16_8 << PIO_SM0_CLKDIV_FRAC_LSB);
    pio_gpio_init(pio, PU_PIN_SWCLK_TCK);
    pio_gpio_init(pio, PU_PIN_SWDIO_TMS);
    pio_gpio_init(pio, PU_PIN_JTAG_TDI);
    pio_gpio_init(pio, PU_PIN_JTAG_TDO);
    gpio_pull_up(PU_PIN_JTAG_TDO);
#if PU_HAS_LEVEL_SHIFTER
    gpio_init(PU_PIN_SWDIO_DIR);
    gpio_put(PU_PIN_SWDIO_DIR, true);
    gpio_set_dir(PU_PIN_SWDIO_DIR, GPIO_OUT);
#endif
    pio_sm_set_pins_with_mask(pio, sm,
                              (1u << PU_PIN_SWDIO_TMS) | (1u << PU_PIN_JTAG_TDI),
                              (1u << PU_PIN_SWCLK_TCK) | (1u << PU_PIN_SWDIO_TMS) | (1u << PU_PIN_JTAG_TDI));
    pio_sm_set_consecutive_pindirs(pio, sm, PU_PIN_SWCLK_TCK, 3u, true);
    pio_sm_set_consecutive_pindirs(pio, sm, PU_PIN_JTAG_TDO, 1u, false);
    pio_sm_init(pio, sm, offset + pu_jtag_offset_get_cmd, &c);
    pio_sm_set_enabled(pio, sm, true);
}
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

_Static_assert(sizeof(pu_swd_program_instructions) / sizeof(pu_swd_program_instructions[0]) <= 16u,
               "swd.pio must be <= 16 instructions");
_Static_assert(sizeof(pu_jtag_program_instructions) / sizeof(pu_jtag_program_instructions[0]) <= 16u,
               "jtag.pio must be <= 16 instructions");

static const uint kPioSm = 0u;
static const struct pio_program *s_loaded_prog = NULL;
static uint s_loaded_offset = 0u;
static uint32_t s_hw_clkdiv_q16_8 = (31u << 8);
static uint32_t s_hw_khz = 1000u;

static bool rp2040_pio_push_bounded(PIO pio, uint sm, uint32_t word) {
    uint64_t deadline = time_us_64() + PU_DAP_PIO_OP_TIMEOUT_US;
    while (pio_sm_is_tx_fifo_full(pio, sm)) {
        if (time_us_64() >= deadline) {
            return false;
        }
    }
    pio_sm_put(pio, sm, word);
    return true;
}

static bool rp2040_pio_pop_bounded(PIO pio, uint sm, uint32_t *out_word) {
    uint64_t deadline = time_us_64() + PU_DAP_PIO_OP_TIMEOUT_US;
    while (pio_sm_is_rx_fifo_empty(pio, sm)) {
        if (time_us_64() >= deadline) {
            *out_word = 0u;
            return false;
        }
    }
    *out_word = pio_sm_get(pio, sm);
    return true;
}

static void rp2040_release_overrides(void) {
    gpio_set_outover(PU_PIN_SWCLK_TCK, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(PU_PIN_SWCLK_TCK, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(PU_PIN_SWDIO_TMS, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(PU_PIN_SWDIO_TMS, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(PU_PIN_JTAG_TDI, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(PU_PIN_JTAG_TDI, GPIO_OVERRIDE_NORMAL);
#if PU_HAS_LEVEL_SHIFTER
    gpio_set_outover(PU_PIN_SWDIO_DIR, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(PU_PIN_SWDIO_DIR, GPIO_OVERRIDE_NORMAL);
#endif
}

static void rp2040_pio_drain(void) {
    if (s_loaded_prog == NULL) {
        return;
    }
    uint64_t deadline = time_us_64() + PU_DAP_PIO_OP_TIMEOUT_US;
    while (!pio_sm_is_tx_fifo_empty(pio0, kPioSm)) {
        if (time_us_64() >= deadline) {
            break;
        }
    }
    uint32_t settle_us = (36000u + s_hw_khz - 1u) / s_hw_khz;
    if (settle_us < 2u) {
        settle_us = 2u;
    }
    busy_wait_us_32(settle_us);
    while (!pio_sm_is_rx_fifo_empty(pio0, kPioSm)) {
        (void)pio_sm_get(pio0, kPioSm);
    }
}

static void rp2040_unload_pio_program(void) {
    if (s_loaded_prog != NULL) {
        rp2040_pio_drain();
        pio_sm_set_enabled(pio0, kPioSm, false);
        pio_sm_clear_fifos(pio0, kPioSm);
        pio_remove_program(pio0, s_loaded_prog, s_loaded_offset);
        s_loaded_prog = NULL;
        s_loaded_offset = 0u;
    }
}

static void rp2040_port_off(void) {
    rp2040_unload_pio_program();
    rp2040_release_overrides();

    const uint pins[] = {
        PU_PIN_SWCLK_TCK,
        PU_PIN_SWDIO_TMS,
        PU_PIN_JTAG_TDI,
        PU_PIN_JTAG_TDO,
        PU_PIN_TARGET_NRST
    };
    for (size_t i = 0u; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_disable_pulls(pins[i]);
    }
    gpio_pull_up(PU_PIN_TARGET_NRST);
#if PU_HAS_LEVEL_SHIFTER
    gpio_init(PU_PIN_SWDIO_DIR);
    gpio_put(PU_PIN_SWDIO_DIR, false); /* Target->Host passive default */
    gpio_set_dir(PU_PIN_SWDIO_DIR, GPIO_OUT);
#endif
}

static void rp2040_port_setup(uint8_t port) {
    rp2040_unload_pio_program();
    rp2040_release_overrides();

    gpio_init(PU_PIN_TARGET_NRST);
    gpio_put(PU_PIN_TARGET_NRST, false);
    gpio_set_dir(PU_PIN_TARGET_NRST, GPIO_IN);
    gpio_pull_up(PU_PIN_TARGET_NRST);

    if (port == DAP_PORT_SWD) {
        gpio_init(PU_PIN_JTAG_TDI);
        gpio_set_dir(PU_PIN_JTAG_TDI, GPIO_IN);
        gpio_init(PU_PIN_JTAG_TDO);
        gpio_set_dir(PU_PIN_JTAG_TDO, GPIO_IN);

        s_loaded_offset = pio_add_program(pio0, &pu_swd_program);
        s_loaded_prog = &pu_swd_program;
        pu_swd_program_init(pio0, kPioSm, s_loaded_offset, s_hw_clkdiv_q16_8);
    } else if (port == DAP_PORT_JTAG) {
        s_loaded_offset = pio_add_program(pio0, &pu_jtag_program);
        s_loaded_prog = &pu_jtag_program;
        pu_jtag_program_init(pio0, kPioSm, s_loaded_offset, s_hw_clkdiv_q16_8);
    }
}

static void rp2040_set_clock(uint32_t clkdiv_q16_8, uint32_t effective_khz) {
    s_hw_clkdiv_q16_8 = clkdiv_q16_8;
    s_hw_khz = (effective_khz != 0u) ? effective_khz : 1u;
    if (s_loaded_prog != NULL) {
        rp2040_pio_drain();
        pio0->sm[kPioSm].clkdiv = (clkdiv_q16_8 << PIO_SM0_CLKDIV_FRAC_LSB);
        pio_sm_clkdiv_restart(pio0, kPioSm);
    }
}

static void rp2040_swd_write_bits(uint32_t bit_count, uint32_t word) {
    rp2040_release_overrides();
    uint32_t cmd = ((bit_count - 1u) & 0xFFu) | (1u << 8) | (1u << 9);
    (void)rp2040_pio_push_bounded(pio0, kPioSm, cmd);
    (void)rp2040_pio_push_bounded(pio0, kPioSm, word);
}

static uint32_t rp2040_swd_read_bits(uint32_t bit_count) {
    rp2040_release_overrides();
    uint32_t cmd = ((bit_count - 1u) & 0xFFu);
    uint32_t rx = 0u;
    (void)rp2040_pio_push_bounded(pio0, kPioSm, cmd);
    (void)rp2040_pio_pop_bounded(pio0, kPioSm, &rx);
    return (bit_count >= 32u) ? rx : (rx >> (32u - bit_count));
}

static void rp2040_jtag_sequence(uint32_t bit_count,
                                 bool tms,
                                 const uint8_t *tdi_data,
                                 uint8_t *tdo_data) {
    rp2040_release_overrides();
    if (tdo_data != NULL) {
        memset(tdo_data, 0, (bit_count + 7u) / 8u);
    }
    uint32_t offset = 0u;
    while (offset < bit_count) {
        uint32_t chunk = bit_count - offset;
        if (chunk > 16u) {
            chunk = 16u;
        }
        uint32_t packed = 0u;
        for (uint32_t i = 0u; i < chunk; ++i) {
            uint32_t bit_idx = offset + i;
            uint32_t tdi_bit = 0u;
            if (tdi_data != NULL) {
                tdi_bit = (tdi_data[bit_idx / 8u] >> (bit_idx % 8u)) & 1u;
            }
            uint32_t pair = (tms ? 1u : 0u) | (tdi_bit << 1);
            packed |= (pair << (2u * i));
        }
        (void)rp2040_pio_push_bounded(pio0, kPioSm, (chunk - 1u) & 0x0Fu);
        (void)rp2040_pio_push_bounded(pio0, kPioSm, packed);
        uint32_t rx = 0u;
        (void)rp2040_pio_pop_bounded(pio0, kPioSm, &rx);
        uint32_t tdo_chunk = rx >> (32u - chunk);
        if (tdo_data != NULL) {
            for (uint32_t i = 0u; i < chunk; ++i) {
                if ((tdo_chunk & (1u << i)) != 0u) {
                    uint32_t bit_idx = offset + i;
                    tdo_data[bit_idx / 8u] |= (uint8_t)(1u << (bit_idx % 8u));
                }
            }
        }
        offset += chunk;
    }
}

static void rp2040_swj_sequence(uint32_t bit_count, const uint8_t *data) {
    if (s_loaded_prog == &pu_jtag_program) {
        /* JTAG mode: clock each bit onto TMS with TDI=0 */
        for (uint32_t i = 0u; i < bit_count; ++i) {
            bool tms = ((data[i / 8u] >> (i % 8u)) & 1u) != 0u;
            uint8_t zero = 0u;
            rp2040_jtag_sequence(1u, tms, &zero, NULL);
        }
        return;
    }
    uint32_t offset = 0u;
    while (offset < bit_count) {
        uint32_t chunk = bit_count - offset;
        if (chunk > 32u) {
            chunk = 32u;
        }
        uint32_t word = 0u;
        for (uint32_t i = 0u; i < chunk; ++i) {
            uint32_t idx = offset + i;
            if ((data[idx / 8u] & (1u << (idx % 8u))) != 0u) {
                word |= (1u << i);
            }
        }
        rp2040_swd_write_bits(chunk, word);
        offset += chunk;
    }
    rp2040_pio_drain();
}

static void rp2040_swd_sequence(uint32_t bit_count,
                                bool capture,
                                const uint8_t *tx_data,
                                uint8_t *rx_data) {
    if (capture && rx_data != NULL) {
        memset(rx_data, 0, (bit_count + 7u) / 8u);
    }
    uint32_t offset = 0u;
    while (offset < bit_count) {
        uint32_t chunk = bit_count - offset;
        if (chunk > 32u) {
            chunk = 32u;
        }
        if (capture) {
            uint32_t bits = rp2040_swd_read_bits(chunk);
            if (rx_data != NULL) {
                for (uint32_t i = 0u; i < chunk; ++i) {
                    if ((bits & (1u << i)) != 0u) {
                        uint32_t idx = offset + i;
                        rx_data[idx / 8u] |= (uint8_t)(1u << (idx % 8u));
                    }
                }
            }
        } else {
            uint32_t word = 0u;
            for (uint32_t i = 0u; i < chunk; ++i) {
                uint32_t idx = offset + i;
                if (tx_data != NULL && (tx_data[idx / 8u] & (1u << (idx % 8u))) != 0u) {
                    word |= (1u << i);
                }
            }
            rp2040_swd_write_bits(chunk, word);
        }
        offset += chunk;
    }
    if (!capture) {
        rp2040_pio_drain();
    }
}

static uint32_t parity32_even(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

static uint8_t rp2040_swd_transfer(uint8_t req_hdr,
                                   uint32_t *data,
                                   uint8_t turnaround_cycles,
                                   bool force_data_phase) {
    uint32_t hdr_nibble = (uint32_t)(req_hdr & 0x0Fu);
    uint32_t hdr_parity = parity32_even(hdr_nibble);
    uint32_t packet = 0x81u | (hdr_nibble << 1) | (hdr_parity << 5);

    rp2040_swd_write_bits(8u, packet);
    uint32_t ack_and_trn = rp2040_swd_read_bits((uint32_t)turnaround_cycles + 3u);
    uint8_t ack = (uint8_t)((ack_and_trn >> turnaround_cycles) & 0x07u);

    if (ack == DAP_TRANSFER_OK) {
        if ((req_hdr & DAP_TRANSFER_RnW) != 0u) {
            uint32_t val = rp2040_swd_read_bits(32u);
            uint32_t p_and_trn = rp2040_swd_read_bits(1u + (uint32_t)turnaround_cycles);
            uint32_t rx_parity = p_and_trn & 1u;
            if (parity32_even(val) != rx_parity) {
                ack = DAP_TRANSFER_ERROR;
            }
            if (data != NULL) {
                *data = val;
            }
        } else {
            (void)rp2040_swd_read_bits(turnaround_cycles);
            uint32_t val = (data != NULL) ? *data : 0u;
            rp2040_swd_write_bits(32u, val);
            rp2040_swd_write_bits(1u, parity32_even(val));
        }
        return ack;
    }

    if (ack == DAP_TRANSFER_WAIT || ack == DAP_TRANSFER_FAULT) {
        if (force_data_phase && (req_hdr & DAP_TRANSFER_RnW) != 0u) {
            (void)rp2040_swd_read_bits(32u);
            (void)rp2040_swd_read_bits(1u + (uint32_t)turnaround_cycles);
        } else {
            (void)rp2040_swd_read_bits(turnaround_cycles);
            if (force_data_phase && (req_hdr & DAP_TRANSFER_RnW) == 0u) {
                rp2040_swd_write_bits(32u, 0u);
                rp2040_swd_write_bits(1u, 0u);
            }
        }
        return ack;
    }

    /* Protocol error: clock through turnaround + data phase recovery */
    (void)rp2040_swd_read_bits(32u);
    (void)rp2040_swd_read_bits(1u + (uint32_t)turnaround_cycles);
    return DAP_TRANSFER_ERROR;
}

static void rp2040_sio_override_begin(void) {
    /* Explicit marker after rp2040_pio_drain before SIO GPIO overrides */
}

static uint8_t rp2040_sample_pins(void) {
    uint8_t pins = PU_DAP_PIN_nTRST;
    if (gpio_get(PU_PIN_SWCLK_TCK)) {
        pins |= PU_DAP_PIN_SWCLK_TCK;
    }
    if (gpio_get(PU_PIN_SWDIO_TMS)) {
        pins |= PU_DAP_PIN_SWDIO_TMS;
    }
    if (gpio_get(PU_PIN_JTAG_TDI)) {
        pins |= PU_DAP_PIN_TDI;
    }
    if (gpio_get(PU_PIN_JTAG_TDO)) {
        pins |= PU_DAP_PIN_TDO;
    }
    if (gpio_get(PU_PIN_TARGET_NRST)) {
        pins |= PU_DAP_PIN_nRESET;
    }
    return pins;
}

static uint64_t rp2040_now_us_64(void) {
    return time_us_64();
}

static uint8_t rp2040_swj_pins_io(uint8_t pin_output, uint8_t pin_select, uint32_t wait_us) {
    if ((pin_select & PU_DAP_PIN_SWCLK_TCK) != 0u) {
        gpio_set_outover(PU_PIN_SWCLK_TCK,
                         (pin_output & PU_DAP_PIN_SWCLK_TCK) ? GPIO_OVERRIDE_HIGH : GPIO_OVERRIDE_LOW);
        gpio_set_oeover(PU_PIN_SWCLK_TCK, GPIO_OVERRIDE_HIGH);
    }
    if ((pin_select & PU_DAP_PIN_SWDIO_TMS) != 0u) {
#if PU_HAS_LEVEL_SHIFTER
        gpio_set_outover(PU_PIN_SWDIO_DIR, GPIO_OVERRIDE_HIGH);
        gpio_set_oeover(PU_PIN_SWDIO_DIR, GPIO_OVERRIDE_HIGH);
#endif
        gpio_set_outover(PU_PIN_SWDIO_TMS,
                         (pin_output & PU_DAP_PIN_SWDIO_TMS) ? GPIO_OVERRIDE_HIGH : GPIO_OVERRIDE_LOW);
        gpio_set_oeover(PU_PIN_SWDIO_TMS, GPIO_OVERRIDE_HIGH);
    }
    if ((pin_select & PU_DAP_PIN_TDI) != 0u) {
        gpio_set_outover(PU_PIN_JTAG_TDI,
                         (pin_output & PU_DAP_PIN_TDI) ? GPIO_OVERRIDE_HIGH : GPIO_OVERRIDE_LOW);
        gpio_set_oeover(PU_PIN_JTAG_TDI, GPIO_OVERRIDE_HIGH);
    }
    if ((pin_select & PU_DAP_PIN_nRESET) != 0u) {
        if ((pin_output & PU_DAP_PIN_nRESET) == 0u) {
            gpio_put(PU_PIN_TARGET_NRST, false);
            gpio_set_dir(PU_PIN_TARGET_NRST, GPIO_OUT);
        } else {
            gpio_set_dir(PU_PIN_TARGET_NRST, GPIO_IN);
            gpio_pull_up(PU_PIN_TARGET_NRST);
        }
    }

    return dap_wait_for_drivable_pins(pin_output, pin_select, wait_us,
                                      rp2040_sample_pins, rp2040_now_us_64);
}

static void rp2040_sio_override_end(void) {
    rp2040_release_overrides();
}

static void rp2040_delay_us(uint32_t delay_us) {
    busy_wait_us_32(delay_us);
}

static uint32_t rp2040_get_timestamp_us(void) {
    return (uint32_t)time_us_64();
}

#else /* defined(PU_HOST_TEST) */

static uint32_t s_sim_dp_regs[4];
static uint32_t s_sim_ap_regs[4];
static uint8_t s_sim_pio_pins = PU_DAP_PIN_SWDIO_TMS;
static uint8_t s_sim_override_mask = 0u;
static uint8_t s_sim_override_out = 0u;
static bool s_sim_nreset_low = false;
static uint32_t s_sim_time_us = 0u;

static void host_fake_drain_pio(void) {}
static void host_fake_tristate(void) {}
static void host_fake_set_swio_pu(bool en) { (void)en; }

static const pu_pin_hal_t kHostFakePinHal = {
    .drain_pio = host_fake_drain_pio,
    .tristate_debug_pins = host_fake_tristate,
    .set_swio_pullup = host_fake_set_swio_pu,
    .set_vtarg_power = NULL
};

static void host_sim_port_setup(uint8_t port) {
    s_sim_override_mask = 0u;
    s_sim_override_out = 0u;
    s_sim_nreset_low = false;
    s_sim_pio_pins = (port == DAP_PORT_JTAG)
                         ? (uint8_t)(PU_DAP_PIN_SWDIO_TMS | PU_DAP_PIN_TDI)
                         : (uint8_t)PU_DAP_PIN_SWDIO_TMS;
}

static void host_sim_port_off(void) {
    s_sim_override_mask = 0u;
    s_sim_override_out = 0u;
    s_sim_nreset_low = false;
    s_sim_pio_pins = PU_DAP_PIN_SWDIO_TMS;
}

static void host_sim_sio_override_end(void) {
    s_sim_override_mask = 0u;
}

static uint8_t host_sim_swd_transfer(uint8_t req_hdr,
                                     uint32_t *data,
                                     uint8_t turnaround_cycles,
                                     bool force_data_phase) {
    (void)turnaround_cycles;
    (void)force_data_phase;
    uint32_t reg_idx = ((uint32_t)req_hdr >> 2) & 0x03u;
    uint32_t *bank = ((req_hdr & DAP_TRANSFER_APnDP) != 0u) ? s_sim_ap_regs : s_sim_dp_regs;
    if ((req_hdr & DAP_TRANSFER_RnW) != 0u) {
        if (data != NULL) {
            *data = bank[reg_idx];
        }
    } else if (data != NULL) {
        bank[reg_idx] = *data;
    }
    return DAP_TRANSFER_OK;
}

static void host_sim_jtag_sequence(uint32_t bit_count,
                                   bool tms,
                                   const uint8_t *tdi_data,
                                   uint8_t *tdo_data) {
    (void)tms;
    if (tdo_data == NULL) {
        return;
    }
    uint32_t bytes = (bit_count + 7u) / 8u;
    if (tdi_data != NULL) {
        memcpy(tdo_data, tdi_data, bytes);
    } else {
        memset(tdo_data, 0x5Au, bytes);
    }
    /* Return JTAG ACK = OK/FAULT (0b010) in bottom 3 bits for 35-bit DPACC/APACC scans */
    if (bit_count == 35u && bytes >= 1u) {
        tdo_data[0] = (uint8_t)((tdo_data[0] & ~0x07u) | 0x02u);
    }
}

static uint8_t host_sim_sample_pins(void) {
    uint8_t pio_or_sio = (uint8_t)((s_sim_pio_pins & ~s_sim_override_mask) |
                                   (s_sim_override_out & s_sim_override_mask));
    uint8_t pins = (uint8_t)(pio_or_sio | PU_DAP_PIN_TDO | PU_DAP_PIN_nTRST);
    if (!s_sim_nreset_low) {
        pins |= PU_DAP_PIN_nRESET;
    }
    return pins;
}

static uint64_t host_sim_now_us_64(void) {
    s_sim_time_us += 10u;
    return (uint64_t)s_sim_time_us;
}

static uint8_t host_sim_swj_pins_io(uint8_t pin_output, uint8_t pin_select, uint32_t wait_us) {
    const uint8_t sio_pins = (uint8_t)(PU_DAP_PIN_SWCLK_TCK | PU_DAP_PIN_SWDIO_TMS | PU_DAP_PIN_TDI);
    uint8_t sel_sio = (uint8_t)(pin_select & sio_pins);
    s_sim_override_mask |= sel_sio;
    s_sim_override_out = (uint8_t)((s_sim_override_out & ~sel_sio) | (pin_output & sel_sio));
    if ((pin_select & PU_DAP_PIN_nRESET) != 0u) {
        s_sim_nreset_low = ((pin_output & PU_DAP_PIN_nRESET) == 0u);
    }
    return dap_wait_for_drivable_pins(pin_output, pin_select, wait_us,
                                      host_sim_sample_pins, host_sim_now_us_64);
}

static void host_sim_delay_us(uint32_t delay_us) {
    s_sim_time_us += delay_us;
}

static uint32_t host_sim_timestamp_us(void) {
    s_sim_time_us += 10u;
    return s_sim_time_us;
}

static const pu_dap_wire_hal_t kHostSimWireHal = {
    .sys_clock_hz = 125000000u,
    .port_setup = host_sim_port_setup,
    .port_off = host_sim_port_off,
    .set_clock = NULL,
    .swj_sequence = NULL,
    .swd_sequence = NULL,
    .swd_transfer = host_sim_swd_transfer,
    .jtag_sequence = host_sim_jtag_sequence,
    .pio_drain = NULL,
    .sio_override_begin = NULL,
    .swj_pins_io = host_sim_swj_pins_io,
    .sio_override_end = host_sim_sio_override_end,
    .delay_us = host_sim_delay_us,
    .get_timestamp_us = host_sim_timestamp_us
};

#endif /* !defined(PU_HOST_TEST) */

/* -------------------------------------------------------------------------- */
/* Engine State                                                               */
/* -------------------------------------------------------------------------- */

static pu_dap_wire_hal_t s_wire;
static uint8_t s_active_port = DAP_PORT_DISABLED;
static uint32_t s_target_clock_hz = PU_DAP_DEFAULT_CLOCK_HZ;
static uint32_t s_cached_clkdiv_q16_8 = 0u;

static uint8_t s_idle_cycles = 0u;
static uint16_t s_wait_retry = 100u;
static uint16_t s_match_retry = 0u;
static uint32_t s_match_mask = 0u;

static uint8_t s_swd_turnaround = 1u;
static bool s_swd_data_phase = false;
static bool s_sio_override_active = false;
static volatile bool s_transfer_abort = false;

static uint8_t s_jtag_dev_count = 1u;
static uint8_t s_jtag_ir_len[PU_DAP_MAX_JTAG_DEVS] = {4u};
static uint16_t s_jtag_ir_before[PU_DAP_MAX_JTAG_DEVS] = {0u};
static uint16_t s_jtag_ir_after[PU_DAP_MAX_JTAG_DEVS] = {0u};

static void reset_dap_protocol_state(void) {
    s_active_port = DAP_PORT_DISABLED;
    s_target_clock_hz = PU_DAP_DEFAULT_CLOCK_HZ;
    s_cached_clkdiv_q16_8 = 0u;
    s_idle_cycles = 0u;
    s_wait_retry = 100u;
    s_match_retry = 0u;
    s_match_mask = 0u;
    s_swd_turnaround = 1u;
    s_swd_data_phase = false;
    s_sio_override_active = false;
    s_transfer_abort = false;
    s_jtag_dev_count = 1u;
    memset(s_jtag_ir_len, 0, sizeof(s_jtag_ir_len));
    memset(s_jtag_ir_before, 0, sizeof(s_jtag_ir_before));
    memset(s_jtag_ir_after, 0, sizeof(s_jtag_ir_after));
    s_jtag_ir_len[0] = 4u;
}

void pu_dap_init(const pu_dap_wire_hal_t *wire_hal) {
    reset_dap_protocol_state();
    if (wire_hal != NULL) {
        s_wire = *wire_hal;
        if (s_wire.sys_clock_hz == 0u) {
            s_wire.sys_clock_hz = 125000000u;
        }
        return;
    }

#if !defined(PU_HOST_TEST)
    memset(&s_wire, 0, sizeof(s_wire));
    uint32_t clk = clock_get_hz(clk_sys);
    s_wire.sys_clock_hz = (clk != 0u) ? clk : 125000000u;
    s_wire.port_setup = rp2040_port_setup;
    s_wire.port_off = rp2040_port_off;
    s_wire.set_clock = rp2040_set_clock;
    s_wire.swj_sequence = rp2040_swj_sequence;
    s_wire.swd_sequence = rp2040_swd_sequence;
    s_wire.swd_transfer = rp2040_swd_transfer;
    s_wire.jtag_sequence = rp2040_jtag_sequence;
    s_wire.pio_drain = rp2040_pio_drain;
    s_wire.sio_override_begin = rp2040_sio_override_begin;
    s_wire.swj_pins_io = rp2040_swj_pins_io;
    s_wire.sio_override_end = rp2040_sio_override_end;
    s_wire.delay_us = rp2040_delay_us;
    s_wire.get_timestamp_us = rp2040_get_timestamp_us;
#else
    s_wire = kHostSimWireHal;
#endif
}

#ifdef PU_HOST_TEST
void pu_dap_init_for_test(void) {
    memset(s_sim_dp_regs, 0, sizeof(s_sim_dp_regs));
    memset(s_sim_ap_regs, 0, sizeof(s_sim_ap_regs));
    s_sim_dp_regs[0] = 0x2BA01477u; /* Simulated Cortex-M SW-DP IDCODE */
    s_sim_pio_pins = PU_DAP_PIN_SWDIO_TMS;
    s_sim_override_mask = 0u;
    s_sim_override_out = 0u;
    s_sim_nreset_low = false;
    s_sim_time_us = 0u;
    pu_engine_init(&kHostFakePinHal);
    pu_dap_init(&kHostSimWireHal);
}
#endif

static pu_engine_state_t port_to_engine_state(uint8_t port) {
    if (port == DAP_PORT_SWD) {
        return PU_ENGINE_SWD;
    }
    if (port == DAP_PORT_JTAG) {
        return PU_ENGINE_JTAG;
    }
    return PU_ENGINE_IDLE;
}

static void sync_wire_clock_if_needed(void) {
    uint32_t eff_khz = 1u;
    uint32_t clkdiv = pu_dap_compute_pio_clkdiv_q16_8(s_wire.sys_clock_hz, s_target_clock_hz, &eff_khz);
    if (clkdiv != 0u && clkdiv != s_cached_clkdiv_q16_8) {
        s_cached_clkdiv_q16_8 = clkdiv;
        if (s_wire.set_clock != NULL) {
            s_wire.set_clock(clkdiv, eff_khz);
        }
    }
}

static void release_sio_overrides_if_active(void) {
    if (s_sio_override_active) {
        s_sio_override_active = false;
        if (s_wire.sio_override_end != NULL) {
            s_wire.sio_override_end();
        }
    }
}

static bool ensure_active_dap_session(uint64_t now_us) {
    if (s_active_port == DAP_PORT_DISABLED) {
        return false;
    }
    pu_engine_state_t expected = port_to_engine_state(s_active_port);
    if (pu_engine_renew_deadline(expected, now_us, PU_DAP_SESSION_TIMEOUT_US) != PU_OK) {
        /* Session lost due to timeout, USB reset, or hardware fault latch */
        s_active_port = DAP_PORT_DISABLED;
        s_cached_clkdiv_q16_8 = 0u;
        s_sio_override_active = false;
        return false;
    }
    sync_wire_clock_if_needed();
    return true;
}

void pu_dap_on_usb_disconnect(void) {
    s_transfer_abort = false;
    if (s_active_port != DAP_PORT_DISABLED) {
        pu_engine_state_t expected = port_to_engine_state(s_active_port);
        /* Always release SIO overrides and PIO0 pins even if engine_owner already
         * transitioned to IDLE (USB reset) or FAULT (overcurrent IRQ). */
        release_sio_overrides_if_active();
        if (s_wire.port_off != NULL) {
            s_wire.port_off();
        }
        if (pu_engine_current() == expected) {
            (void)pu_engine_release(expected);
        }
        s_active_port = DAP_PORT_DISABLED;
        s_cached_clkdiv_q16_8 = 0u;
        s_sio_override_active = false;
    }
}

/* -------------------------------------------------------------------------- */
/* Pre-Execution Packet Validation Guard (dap_guard)                          */
/* -------------------------------------------------------------------------- */

static size_t dap_info_reply_len(uint8_t info_id) {
    switch (info_id) {
    case DAP_ID_VENDOR:
        return 2u + strlen("Pico Universal") + 1u;
    case DAP_ID_PRODUCT:
        return 2u + strlen("Pico Universal CMSIS-DAP") + 1u;
    case DAP_ID_SER_NUM:
        return 2u + 16u + 1u;
    case DAP_ID_CMSIS_DAP_VER:
        return 2u + sizeof(PU_DAP_CMSIS_VER_STR);
    case DAP_ID_PRODUCT_FW_VER:
        return 2u + sizeof(PU_DAP_FW_VER_STR);
    case DAP_ID_CAPABILITIES:
    case DAP_ID_PACKET_COUNT:
        return 3u;
    case DAP_ID_PACKET_SIZE:
        return 4u;
    case DAP_ID_TIMESTAMP_CLOCK:
        return 6u;
    default:
        return 2u;
    }
}

static bool dap_command_bounds(const uint8_t *p,
                               size_t left,
                               size_t max_resp,
                               size_t *used_out,
                               size_t *worst_reply_out) {
    if (left == 0u) {
        return false;
    }

    size_t n = 1u;
    size_t out = 2u;
    size_t count = 0u;

    switch (p[0]) {
    case ID_DAP_Info:
        if (left < 2u) {
            return false;
        }
        n = 2u;
        out = dap_info_reply_len(p[1]);
        break;

    case ID_DAP_HostStatus:
        n = 3u;
        break;

    case ID_DAP_Connect:
    case ID_DAP_SWD_Configure:
        n = 2u;
        break;

    case ID_DAP_Disconnect:
    case ID_DAP_TransferAbort:
        n = 1u;
        break;

    case ID_DAP_ResetTarget:
        n = 1u;
        out = 3u;
        break;

    case ID_DAP_TransferConfigure:
    case ID_DAP_WriteABORT:
        n = 6u;
        break;

    case ID_DAP_Delay:
        n = 3u;
        break;

    case ID_DAP_SWJ_Pins:
        n = 7u;
        break;

    case ID_DAP_SWJ_Clock:
        n = 5u;
        break;

    case ID_DAP_JTAG_IDCODE:
        n = 2u;
        out = 6u;
        break;

    case ID_DAP_SWJ_Sequence: {
        if (left < 2u) {
            return false;
        }
        size_t bits = (p[1] != 0u) ? (size_t)p[1] : 256u;
        n = 2u + ((bits + 7u) / 8u);
        break;
    }

    case ID_DAP_JTAG_Configure:
        if (left < 2u || p[1] == 0u || p[1] > PU_DAP_MAX_JTAG_DEVS) {
            return false;
        }
        n = 2u + (size_t)p[1];
        if (n > left) {
            return false;
        }
        for (size_t i = 2u; i < n; ++i) {
            if (p[i] == 0u || p[i] > 32u) {
                return false;
            }
        }
        break;

    case ID_DAP_JTAG_Sequence:
    case ID_DAP_SWD_Sequence:
        if (left < 2u) {
            return false;
        }
        count = p[1];
        n = 2u;
        for (size_t i = 0u; i < count; ++i) {
            if (n >= left) {
                return false;
            }
            uint8_t info = p[n++];
            size_t cycles = (info & 0x3Fu) != 0u ? (size_t)(info & 0x3Fu) : 64u;
            size_t bytes = (cycles + 7u) / 8u;
            if (p[0] == ID_DAP_JTAG_Sequence || (info & 0x80u) == 0u) {
                n += bytes;
            }
            if ((info & 0x80u) != 0u) {
                out += bytes;
            }
            if (n > left || out > max_resp) {
                return false;
            }
        }
        break;

    case ID_DAP_Transfer:
        if (left < 3u) {
            return false;
        }
        count = p[2];
        n = 3u;
        out = 3u;
        for (size_t i = 0u; i < count; ++i) {
            if (n >= left) {
                return false;
            }
            uint8_t flags = p[n++];
            if ((flags & DAP_TRANSFER_RnW) == 0u || (flags & DAP_TRANSFER_MATCH_VALUE) != 0u) {
                n += 4u;
            }
            if ((flags & DAP_TRANSFER_RnW) != 0u && (flags & DAP_TRANSFER_MATCH_VALUE) == 0u) {
                out += 4u;
            }
            if ((flags & DAP_TRANSFER_TIMESTAMP) != 0u) {
                out += 4u;
            }
            if (n > left || out > max_resp) {
                return false;
            }
        }
        break;

    case ID_DAP_TransferBlock:
        if (left < 4u) {
            return false;
        }
        count = (size_t)p[2] | ((size_t)p[3] << 8);
        n = 4u;
        out = 4u;
        if (count != 0u) {
            if (left < 5u) {
                return false;
            }
            n = 5u;
            if ((p[4] & DAP_TRANSFER_RnW) != 0u) {
                out += 4u * count;
            } else {
                n += 4u * count;
            }
        }
        break;

    case ID_DAP_ExecuteCommands:
    case ID_DAP_QueueCommands:
        return false; /* Nested command batches are forbidden */

    default:
        n = 1u;
        out = 1u;
        break;
    }

    if (n > left || out > max_resp) {
        return false;
    }
    *used_out = n;
    *worst_reply_out = out;
    return true;
}

bool pu_dap_packet_valid(const uint8_t *req, size_t req_len, size_t resp_cap) {
    if (req == NULL || req_len == 0u || req_len > PU_DAP_PACKET_SIZE || resp_cap == 0u) {
        return false;
    }
    size_t max_resp = (resp_cap < PU_DAP_PACKET_SIZE) ? resp_cap : PU_DAP_PACKET_SIZE;

    size_t offset = 0u;
    size_t total_reply = 0u;
    size_t cmd_count = 1u;

    if (req[0] == ID_DAP_ExecuteCommands || req[0] == ID_DAP_QueueCommands) {
        if (req_len < 2u || req[1] == 0u) {
            return false;
        }
        cmd_count = req[1];
        offset = 2u;
        total_reply = 2u;
        if (total_reply > max_resp) {
            return false;
        }
    }

    for (size_t i = 0u; i < cmd_count; ++i) {
        size_t used = 0u;
        size_t reply = 0u;
        if (!dap_command_bounds(req + offset, req_len - offset, max_resp, &used, &reply)) {
            return false;
        }
        offset += used;
        total_reply += reply;
        if (total_reply > max_resp) {
            return false;
        }
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* JTAG & SWD Transfer Helpers                                                */
/* -------------------------------------------------------------------------- */

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

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

static void jtag_clock_tms(uint8_t tms_bits, uint32_t count) {
    if (s_wire.jtag_sequence == NULL) {
        return;
    }
    uint8_t zero = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        bool tms = ((tms_bits >> i) & 1u) != 0u;
        s_wire.jtag_sequence(1u, tms, &zero, NULL);
    }
}

static void jtag_shift_ones_bypass(uint16_t count, bool exit_on_last) {
    if (count == 0u || s_wire.jtag_sequence == NULL) {
        return;
    }
    uint8_t ones[8];
    memset(ones, 0xFF, sizeof(ones));
    uint16_t body = exit_on_last ? (uint16_t)(count - 1u) : count;
    while (body > 0u) {
        uint32_t chunk = (body > 64u) ? 64u : body;
        s_wire.jtag_sequence(chunk, false, ones, NULL);
        body = (uint16_t)(body - chunk);
    }
    if (exit_on_last) {
        s_wire.jtag_sequence(1u, true, ones, NULL);
    }
}

static void jtag_write_ir(uint8_t index, uint32_t ir_opcode) {
    if (s_wire.jtag_sequence == NULL || index >= s_jtag_dev_count) {
        return;
    }
    /* Run-Test/Idle -> Select-DR -> Select-IR -> Capture-IR -> Shift-IR (TMS: 1,1,0,0) */
    jtag_clock_tms(0x03u, 4u);
    jtag_shift_ones_bypass(s_jtag_ir_before[index], false);

    uint8_t ir_len = s_jtag_ir_len[index];
    uint8_t ir_bytes[4];
    write_u32_le(ir_bytes, ir_opcode);
    bool exit_ir = (s_jtag_ir_after[index] == 0u);
    if (ir_len > 1u) {
        s_wire.jtag_sequence((uint32_t)(ir_len - 1u), false, ir_bytes, NULL);
    }
    uint8_t last_bit = (uint8_t)((ir_opcode >> (ir_len - 1u)) & 1u);
    s_wire.jtag_sequence(1u, exit_ir, &last_bit, NULL);

    if (!exit_ir) {
        jtag_shift_ones_bypass(s_jtag_ir_after[index], true);
    }
    /* Exit1-IR -> Update-IR -> Run-Test/Idle (TMS: 1,0) */
    jtag_clock_tms(0x01u, 2u);
}

static uint8_t execute_bus_transfer(uint8_t dap_index, uint8_t req_hdr, uint32_t *data) {
    if (s_active_port == DAP_PORT_SWD) {
        if (s_wire.swd_transfer == NULL) {
            return DAP_TRANSFER_ERROR;
        }
        release_sio_overrides_if_active();
        uint8_t ack = s_wire.swd_transfer(req_hdr, data, s_swd_turnaround, s_swd_data_phase);
        if (s_idle_cycles != 0u && s_wire.swd_sequence != NULL) {
            uint8_t zeros[32] = {0};
            s_wire.swd_sequence(s_idle_cycles, false, zeros, NULL);
        }
        return ack;
    }

    if (s_active_port == DAP_PORT_JTAG) {
        if (dap_index >= s_jtag_dev_count || s_wire.jtag_sequence == NULL) {
            return DAP_TRANSFER_ERROR;
        }
        release_sio_overrides_if_active();
        uint32_t ir = ((req_hdr & DAP_TRANSFER_APnDP) != 0u) ? PU_JTAG_IR_APACC : PU_JTAG_IR_DPACC;
        jtag_write_ir(dap_index, ir);

        /* Run-Test/Idle -> Select-DR -> Capture-DR -> Shift-DR (TMS: 1,0,0) */
        jtag_clock_tms(0x01u, 3u);
        if (dap_index > 0u) {
            uint8_t zeros[1] = {0};
            for (uint8_t b = 0u; b < dap_index; ++b) {
                s_wire.jtag_sequence(1u, false, zeros, NULL);
            }
        }

        uint64_t dr_out = ((uint64_t)((req_hdr >> 1) & 0x07u)) |
                          (((uint64_t)((data != NULL) ? *data : 0u)) << 3);
        uint8_t tdi[5];
        uint8_t tdo[5] = {0};
        for (size_t i = 0u; i < 5u; ++i) {
            tdi[i] = (uint8_t)((dr_out >> (8u * i)) & 0xFFu);
        }
        uint8_t trailing = (uint8_t)(s_jtag_dev_count - 1u - dap_index);
        if (trailing == 0u) {
            s_wire.jtag_sequence(34u, false, tdi, tdo);
            uint8_t last_tdi = (uint8_t)((dr_out >> 34) & 1u);
            uint8_t last_tdo = 0u;
            s_wire.jtag_sequence(1u, true, &last_tdi, &last_tdo);
            if ((last_tdo & 1u) != 0u) {
                tdo[4] |= 0x04u;
            }
        } else {
            s_wire.jtag_sequence(35u, false, tdi, tdo);
            jtag_shift_ones_bypass(trailing, true);
        }
        /* Exit1-DR -> Update-DR -> Run-Test/Idle (TMS: 1,0) */
        jtag_clock_tms(0x01u, 2u);
        if (s_idle_cycles != 0u) {
            uint8_t zeros[32] = {0};
            uint32_t rem = s_idle_cycles;
            while (rem > 0u) {
                uint32_t c = (rem > 64u) ? 64u : rem;
                s_wire.jtag_sequence(c, false, zeros, NULL);
                rem -= c;
            }
        }

        uint64_t dr_in = 0u;
        for (size_t i = 0u; i < 5u; ++i) {
            dr_in |= ((uint64_t)tdo[i]) << (8u * i);
        }
        uint8_t jtag_ack = (uint8_t)(dr_in & 0x07u);
        if (jtag_ack == 0x01u) {
            return DAP_TRANSFER_WAIT;
        }
        if (jtag_ack != 0x02u) {
            return DAP_TRANSFER_ERROR;
        }
        if ((req_hdr & DAP_TRANSFER_RnW) != 0u && data != NULL) {
            *data = (uint32_t)((dr_in >> 3) & 0xFFFFFFFFull);
        }
        return DAP_TRANSFER_OK;
    }

    return DAP_TRANSFER_ERROR;
}

static uint8_t execute_bus_transfer_with_retry(uint8_t dap_index, uint8_t req_hdr, uint32_t *data) {
    uint16_t retries = s_wait_retry;
    uint8_t ack = DAP_TRANSFER_ERROR;
    do {
        ack = execute_bus_transfer(dap_index, req_hdr, data);
        if (ack != DAP_TRANSFER_WAIT || s_transfer_abort) {
            break;
        }
    } while (retries-- != 0u);
    return ack;
}

/* -------------------------------------------------------------------------- */
/* Individual CMSIS-DAP Command Execution                                     */
/* -------------------------------------------------------------------------- */

static size_t execute_single_command(const uint8_t *req, uint8_t *resp, uint64_t now_us) {
    uint8_t cmd = req[0];
    resp[0] = cmd;

    switch (cmd) {
    case ID_DAP_Info: {
        uint8_t info_id = req[1];
        const char *str = NULL;
        switch (info_id) {
        case DAP_ID_VENDOR:
            str = "Pico Universal";
            break;
        case DAP_ID_PRODUCT:
            str = "Pico Universal CMSIS-DAP";
            break;
        case DAP_ID_SER_NUM:
            str = pu_usb_get_serial_string();
            break;
        case DAP_ID_CMSIS_DAP_VER:
            str = PU_DAP_CMSIS_VER_STR;
            break;
        case DAP_ID_PRODUCT_FW_VER:
            str = PU_DAP_FW_VER_STR;
            break;
        case DAP_ID_CAPABILITIES:
            resp[1] = 1u;
            resp[2] = PU_DAP_CAPABILITIES_MASK;
            return 3u;
        case DAP_ID_TIMESTAMP_CLOCK:
            resp[1] = 4u;
            write_u32_le(&resp[2], 1000000u);
            return 6u;
        case DAP_ID_PACKET_COUNT:
            resp[1] = 1u;
            resp[2] = PU_DAP_PACKET_COUNT;
            return 3u;
        case DAP_ID_PACKET_SIZE:
            resp[1] = 2u;
            write_u16_le(&resp[2], PU_DAP_PACKET_SIZE);
            return 4u;
        default:
            resp[1] = 0u;
            return 2u;
        }
        size_t slen = strlen(str) + 1u;
        resp[1] = (uint8_t)slen;
        memcpy(&resp[2], str, slen);
        return 2u + slen;
    }

    case ID_DAP_HostStatus:
        resp[1] = (req[1] <= 1u) ? DAP_OK : DAP_ERROR;
        return 2u;

    case ID_DAP_Connect: {
        uint8_t req_port = req[1];
        uint8_t desired_port = (req_port == DAP_PORT_AUTODETECT) ? DAP_PORT_SWD : req_port;
        if (desired_port != DAP_PORT_SWD && desired_port != DAP_PORT_JTAG) {
            resp[1] = DAP_PORT_DISABLED;
            return 2u;
        }

        pu_engine_state_t target_engine = port_to_engine_state(desired_port);
        if (s_active_port != DAP_PORT_DISABLED) {
            pu_engine_state_t prev_engine = port_to_engine_state(s_active_port);
            if (prev_engine == target_engine &&
                pu_engine_renew_deadline(target_engine, now_us, PU_DAP_SESSION_TIMEOUT_US) == PU_OK) {
                s_cached_clkdiv_q16_8 = 0u;
                release_sio_overrides_if_active();
                if (s_wire.port_setup != NULL) {
                    s_wire.port_setup(desired_port);
                }
                sync_wire_clock_if_needed();
                resp[1] = desired_port;
                return 2u;
            }
            if (pu_engine_current() == prev_engine) {
                release_sio_overrides_if_active();
                if (s_wire.port_off != NULL) {
                    s_wire.port_off();
                }
                (void)pu_engine_release(prev_engine);
            }
            s_active_port = DAP_PORT_DISABLED;
            s_sio_override_active = false;
        }

        if (pu_engine_acquire(target_engine, now_us, PU_DAP_SESSION_TIMEOUT_US) != PU_OK) {
            resp[1] = DAP_PORT_DISABLED;
            return 2u;
        }

        s_active_port = desired_port;
        s_transfer_abort = false;
        s_cached_clkdiv_q16_8 = 0u; /* Mandatory clock-cache invalidation on SWD/JTAG entry */
        release_sio_overrides_if_active();
        if (s_wire.port_setup != NULL) {
            s_wire.port_setup(desired_port);
        }
        sync_wire_clock_if_needed();
        resp[1] = desired_port;
        return 2u;
    }

    case ID_DAP_Disconnect:
        if (s_active_port != DAP_PORT_DISABLED) {
            pu_engine_state_t expected = port_to_engine_state(s_active_port);
            if (pu_engine_current() == expected) {
                release_sio_overrides_if_active();
                if (s_wire.port_off != NULL) {
                    s_wire.port_off();
                }
                (void)pu_engine_release(expected);
            }
            s_active_port = DAP_PORT_DISABLED;
            s_cached_clkdiv_q16_8 = 0u;
            s_sio_override_active = false;
        }
        resp[1] = DAP_OK;
        return 2u;

    case ID_DAP_TransferConfigure:
        s_idle_cycles = req[1];
        s_wait_retry = read_u16_le(&req[2]);
        s_match_retry = read_u16_le(&req[4]);
        resp[1] = DAP_OK;
        return 2u;

    case ID_DAP_Transfer: {
        uint8_t dap_index = req[1];
        uint8_t req_count = req[2];
        if (!ensure_active_dap_session(now_us)) {
            resp[1] = 0u;
            resp[2] = DAP_TRANSFER_ERROR;
            return 3u;
        }
        s_transfer_abort = false;

        size_t in_off = 3u;
        size_t out_off = 3u;
        uint8_t executed = 0u;
        uint8_t ack = 0u;
        bool post_read = false;
        bool check_write = false;

        for (uint8_t i = 0u; i < req_count; ++i) {
            uint8_t tr = req[in_off++];
            uint32_t data = 0u;

            if ((tr & DAP_TRANSFER_RnW) != 0u) {
                if (post_read) {
                    uint8_t next_tr = tr;
                    bool next_is_ap_read = ((tr & (DAP_TRANSFER_APnDP | DAP_TRANSFER_MATCH_VALUE)) ==
                                            DAP_TRANSFER_APnDP);
                    if (!next_is_ap_read) {
                        next_tr = DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3; /* DP RDBUFF */
                        post_read = false;
                    }
                    ack = execute_bus_transfer_with_retry(dap_index, next_tr, &data);
                    if (ack != DAP_TRANSFER_OK) {
                        if (executed > 0u) {
                            executed--;
                        }
                        break;
                    }
                    write_u32_le(&resp[out_off], data);
                    out_off += 4u;
                    if (post_read) {
                        if ((tr & DAP_TRANSFER_TIMESTAMP) != 0u) {
                            uint32_t ts = (s_wire.get_timestamp_us != NULL) ? s_wire.get_timestamp_us() : 0u;
                            write_u32_le(&resp[out_off], ts);
                            out_off += 4u;
                        }
                        executed++;
                        continue;
                    }
                }

                if ((tr & DAP_TRANSFER_MATCH_VALUE) != 0u) {
                    uint32_t match_val = read_u32_le(&req[in_off]);
                    in_off += 4u;
                    uint16_t m_retry = s_match_retry;
                    if ((tr & DAP_TRANSFER_APnDP) != 0u) {
                        ack = execute_bus_transfer_with_retry(dap_index, tr, &data);
                        if (ack != DAP_TRANSFER_OK) {
                            break;
                        }
                    }
                    do {
                        uint8_t poll_tr = (((tr & DAP_TRANSFER_APnDP) != 0u) && (m_retry == 0u))
                                              ? (uint8_t)(DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3)
                                              : tr;
                        ack = execute_bus_transfer_with_retry(dap_index, poll_tr, &data);
                        if (ack != DAP_TRANSFER_OK) {
                            break;
                        }
                        if (((data ^ match_val) & s_match_mask) == 0u) {
                            break;
                        }
                    } while (m_retry-- != 0u && !s_transfer_abort);

                    if (ack == DAP_TRANSFER_OK && ((data ^ match_val) & s_match_mask) != 0u) {
                        ack |= DAP_TRANSFER_MISMATCH;
                        break;
                    }
                    if (ack != DAP_TRANSFER_OK) {
                        break;
                    }
                } else if ((tr & DAP_TRANSFER_APnDP) != 0u) {
                    ack = execute_bus_transfer_with_retry(dap_index, tr, &data);
                    if (ack != DAP_TRANSFER_OK) {
                        break;
                    }
                    post_read = true;
                    if ((tr & DAP_TRANSFER_TIMESTAMP) != 0u) {
                        uint32_t ts = (s_wire.get_timestamp_us != NULL) ? s_wire.get_timestamp_us() : 0u;
                        write_u32_le(&resp[out_off], ts);
                        out_off += 4u;
                    }
                } else {
                    ack = execute_bus_transfer_with_retry(dap_index, tr, &data);
                    if (ack != DAP_TRANSFER_OK) {
                        break;
                    }
                    if ((tr & DAP_TRANSFER_TIMESTAMP) != 0u) {
                        uint32_t ts = (s_wire.get_timestamp_us != NULL) ? s_wire.get_timestamp_us() : 0u;
                        write_u32_le(&resp[out_off], ts);
                        out_off += 4u;
                    }
                    write_u32_le(&resp[out_off], data);
                    out_off += 4u;
                }
                check_write = false;
            } else {
                if (post_read) {
                    ack = execute_bus_transfer_with_retry(
                        dap_index,
                        DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3,
                        &data);
                    if (ack != DAP_TRANSFER_OK) {
                        if (executed > 0u) {
                            executed--;
                        }
                        break;
                    }
                    write_u32_le(&resp[out_off], data);
                    out_off += 4u;
                    post_read = false;
                }
                data = read_u32_le(&req[in_off]);
                in_off += 4u;
                if ((tr & DAP_TRANSFER_MATCH_MASK) != 0u) {
                    s_match_mask = data;
                    ack = DAP_TRANSFER_OK;
                } else {
                    ack = execute_bus_transfer_with_retry(dap_index, tr, &data);
                    if (ack != DAP_TRANSFER_OK) {
                        break;
                    }
                    if ((tr & DAP_TRANSFER_TIMESTAMP) != 0u) {
                        uint32_t ts = (s_wire.get_timestamp_us != NULL) ? s_wire.get_timestamp_us() : 0u;
                        write_u32_le(&resp[out_off], ts);
                        out_off += 4u;
                    }
                    check_write = ((tr & DAP_TRANSFER_APnDP) != 0u);
                }
            }

            executed++;
            if (s_transfer_abort) {
                break;
            }
        }

        if (executed == req_count) {
            if (post_read) {
                uint32_t data = 0u;
                ack = execute_bus_transfer_with_retry(
                    dap_index,
                    DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3,
                    &data);
                if (ack == DAP_TRANSFER_OK) {
                    write_u32_le(&resp[out_off], data);
                    out_off += 4u;
                } else if (executed > 0u) {
                    executed--;
                }
            } else if (check_write) {
                uint32_t dummy = 0u;
                ack = execute_bus_transfer_with_retry(
                    dap_index,
                    DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3,
                    &dummy);
                if (ack != DAP_TRANSFER_OK && executed > 0u) {
                    executed--;
                }
            }
        }

        resp[1] = executed;
        resp[2] = ack;
        return out_off;
    }

    case ID_DAP_TransferBlock: {
        uint8_t dap_index = req[1];
        uint16_t req_count = read_u16_le(&req[2]);
        if (req_count == 0u) {
            write_u16_le(&resp[1], 0u);
            resp[3] = 0u;
            return 4u;
        }
        if (!ensure_active_dap_session(now_us)) {
            write_u16_le(&resp[1], 0u);
            resp[3] = DAP_TRANSFER_ERROR;
            return 4u;
        }
        s_transfer_abort = false;

        uint8_t tr = req[4];
        size_t in_off = 5u;
        size_t out_off = 4u;
        uint16_t executed = 0u;
        uint8_t ack = 0u;

        if ((tr & DAP_TRANSFER_RnW) != 0u) {
            bool is_ap = ((tr & DAP_TRANSFER_APnDP) != 0u);
            if (is_ap) {
                uint32_t dummy = 0u;
                ack = execute_bus_transfer_with_retry(dap_index, tr, &dummy);
                if (ack != DAP_TRANSFER_OK) {
                    write_u16_le(&resp[1], 0u);
                    resp[3] = ack;
                    return 4u;
                }
            }
            for (uint16_t i = 0u; i < req_count; ++i) {
                uint8_t step_tr = tr;
                if (is_ap && (i + 1u == req_count)) {
                    step_tr = DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3;
                }
                uint32_t data = 0u;
                ack = execute_bus_transfer_with_retry(dap_index, step_tr, &data);
                if (ack != DAP_TRANSFER_OK) {
                    break;
                }
                write_u32_le(&resp[out_off], data);
                out_off += 4u;
                executed++;
                if (s_transfer_abort) {
                    break;
                }
            }
        } else {
            for (uint16_t i = 0u; i < req_count; ++i) {
                uint32_t data = read_u32_le(&req[in_off]);
                in_off += 4u;
                ack = execute_bus_transfer_with_retry(dap_index, tr, &data);
                if (ack != DAP_TRANSFER_OK) {
                    break;
                }
                executed++;
                if (s_transfer_abort) {
                    break;
                }
            }
            if (executed == req_count && (tr & DAP_TRANSFER_APnDP) != 0u) {
                uint32_t dummy = 0u;
                ack = execute_bus_transfer_with_retry(
                    dap_index,
                    DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3,
                    &dummy);
                if (ack != DAP_TRANSFER_OK && executed > 0u) {
                    executed--;
                }
            }
        }

        write_u16_le(&resp[1], executed);
        resp[3] = ack;
        return out_off;
    }

    case ID_DAP_TransferAbort:
        s_transfer_abort = true;
        resp[1] = DAP_OK;
        return 2u;

    case ID_DAP_WriteABORT: {
        uint8_t dap_index = req[1];
        uint32_t abort_val = read_u32_le(&req[2]);
        if (!ensure_active_dap_session(now_us)) {
            resp[1] = DAP_ERROR;
            return 2u;
        }
        if (s_active_port == DAP_PORT_SWD) {
            uint8_t ack = execute_bus_transfer(dap_index, 0x00u /* DP ABORT Write */, &abort_val);
            resp[1] = (ack == DAP_TRANSFER_OK) ? DAP_OK : DAP_ERROR;
        } else if (s_active_port == DAP_PORT_JTAG && dap_index < s_jtag_dev_count && s_wire.jtag_sequence != NULL) {
            release_sio_overrides_if_active();
            jtag_write_ir(dap_index, PU_JTAG_IR_ABORT);
            jtag_clock_tms(0x01u, 3u);
            if (dap_index > 0u) {
                uint8_t zero = 0u;
                for (uint8_t b = 0u; b < dap_index; ++b) {
                    s_wire.jtag_sequence(1u, false, &zero, NULL);
                }
            }
            uint64_t dr = ((uint64_t)abort_val) << 3;
            uint8_t tdi[5];
            for (size_t i = 0u; i < 5u; ++i) {
                tdi[i] = (uint8_t)((dr >> (8u * i)) & 0xFFu);
            }
            uint8_t trailing = (uint8_t)(s_jtag_dev_count - 1u - dap_index);
            if (trailing == 0u) {
                s_wire.jtag_sequence(34u, false, tdi, NULL);
                uint8_t last = (uint8_t)((dr >> 34) & 1u);
                s_wire.jtag_sequence(1u, true, &last, NULL);
            } else {
                s_wire.jtag_sequence(35u, false, tdi, NULL);
                jtag_shift_ones_bypass(trailing, true);
            }
            jtag_clock_tms(0x01u, 2u);
            resp[1] = DAP_OK;
        } else {
            resp[1] = DAP_ERROR;
        }
        return 2u;
    }

    case ID_DAP_Delay: {
        uint16_t delay = read_u16_le(&req[1]);
        if (s_wire.delay_us != NULL) {
            s_wire.delay_us(delay);
        }
        resp[1] = DAP_OK;
        return 2u;
    }

    case ID_DAP_ResetTarget:
        resp[1] = DAP_OK;
        resp[2] = 0x00u; /* No target-specific reset script; standard SWJ_Pins nRESET used */
        return 3u;

    case ID_DAP_SWJ_Pins: {
        if (!ensure_active_dap_session(now_us)) {
            resp[1] = 0x00u;
            return 2u;
        }
        uint8_t pin_output = req[1];
        uint8_t pin_select = req[2];
        uint32_t wait_us = read_u32_le(&req[3]);
        if (wait_us > PU_DAP_MAX_PIN_WAIT_US) {
            wait_us = PU_DAP_MAX_PIN_WAIT_US;
        }

        /* Enforced PIO->SIO handoff; GPIO overrides remain latched across DAP_Delay &
         * subsequent SWJ_Pins calls until the next PIO wire transfer, port_setup, or port_off. */
        const uint8_t sio_pins =
            (uint8_t)(PU_DAP_PIN_SWCLK_TCK | PU_DAP_PIN_SWDIO_TMS | PU_DAP_PIN_TDI);
        if (!s_sio_override_active) {
            if (s_wire.pio_drain != NULL) {
                s_wire.pio_drain();
            }
            if ((pin_select & sio_pins) != 0u) {
                if (s_wire.sio_override_begin != NULL) {
                    s_wire.sio_override_begin();
                }
                s_sio_override_active = true;
            }
        }
        uint8_t pin_in = 0u;
        if (s_wire.swj_pins_io != NULL) {
            pin_in = s_wire.swj_pins_io(pin_output, pin_select, wait_us);
        }
        resp[1] = pin_in;
        return 2u;
    }

    case ID_DAP_SWJ_Clock: {
        uint32_t hz = read_u32_le(&req[1]);
        uint32_t eff_khz = 0u;
        uint32_t clkdiv = pu_dap_compute_pio_clkdiv_q16_8(s_wire.sys_clock_hz, hz, &eff_khz);
        if (clkdiv == 0u) {
            resp[1] = DAP_ERROR;
            return 2u;
        }
        s_target_clock_hz = hz;
        if (s_active_port != DAP_PORT_DISABLED) {
            (void)ensure_active_dap_session(now_us);
        }
        resp[1] = DAP_OK;
        return 2u;
    }

    case ID_DAP_SWJ_Sequence: {
        if (!ensure_active_dap_session(now_us)) {
            resp[1] = DAP_ERROR;
            return 2u;
        }
        uint32_t bit_count = (req[1] != 0u) ? (uint32_t)req[1] : 256u;
        release_sio_overrides_if_active();
        if (s_wire.swj_sequence != NULL) {
            s_wire.swj_sequence(bit_count, &req[2]);
        }
        resp[1] = DAP_OK;
        return 2u;
    }

    case ID_DAP_SWD_Configure: {
        uint8_t cfg = req[1];
        if ((cfg & ~0x07u) != 0u) {
            resp[1] = DAP_ERROR;
            return 2u;
        }
        s_swd_turnaround = (uint8_t)((cfg & 0x03u) + 1u);
        s_swd_data_phase = ((cfg & 0x04u) != 0u);
        resp[1] = DAP_OK;
        return 2u;
    }

    case ID_DAP_SWD_Sequence: {
        if (s_active_port != DAP_PORT_SWD || !ensure_active_dap_session(now_us)) {
            resp[1] = DAP_ERROR;
            return 2u;
        }
        release_sio_overrides_if_active();
        uint8_t seq_count = req[1];
        size_t in_off = 2u;
        size_t out_off = 2u;
        resp[1] = DAP_OK;

        for (uint8_t i = 0u; i < seq_count; ++i) {
            uint8_t info = req[in_off++];
            uint32_t cycles = ((info & 0x3Fu) != 0u) ? (uint32_t)(info & 0x3Fu) : 64u;
            size_t bytes = (cycles + 7u) / 8u;
            bool capture = ((info & 0x80u) != 0u);
            if (capture) {
                memset(&resp[out_off], 0, bytes);
                if (s_wire.swd_sequence != NULL) {
                    s_wire.swd_sequence(cycles, true, NULL, &resp[out_off]);
                }
                out_off += bytes;
            } else {
                if (s_wire.swd_sequence != NULL) {
                    s_wire.swd_sequence(cycles, false, &req[in_off], NULL);
                }
                in_off += bytes;
            }
        }
        return out_off;
    }

    case ID_DAP_JTAG_Sequence: {
        if (s_active_port != DAP_PORT_JTAG || !ensure_active_dap_session(now_us)) {
            resp[1] = DAP_ERROR;
            return 2u;
        }
        release_sio_overrides_if_active();
        uint8_t seq_count = req[1];
        size_t in_off = 2u;
        size_t out_off = 2u;
        resp[1] = DAP_OK;

        for (uint8_t i = 0u; i < seq_count; ++i) {
            uint8_t info = req[in_off++];
            uint32_t cycles = ((info & 0x3Fu) != 0u) ? (uint32_t)(info & 0x3Fu) : 64u;
            size_t bytes = (cycles + 7u) / 8u;
            bool tms = ((info & 0x40u) != 0u);
            bool capture = ((info & 0x80u) != 0u);
            uint8_t *tdo_ptr = capture ? &resp[out_off] : NULL;
            if (capture) {
                memset(tdo_ptr, 0, bytes);
            }
            if (s_wire.jtag_sequence != NULL) {
                s_wire.jtag_sequence(cycles, tms, &req[in_off], tdo_ptr);
            }
            in_off += bytes;
            if (capture) {
                out_off += bytes;
            }
        }
        return out_off;
    }

    case ID_DAP_JTAG_Configure: {
        uint8_t count = req[1];
        s_jtag_dev_count = count;
        uint16_t total_ir = 0u;
        for (uint8_t i = 0u; i < count; ++i) {
            s_jtag_ir_before[i] = total_ir;
            s_jtag_ir_len[i] = req[2u + i];
            total_ir = (uint16_t)(total_ir + req[2u + i]);
        }
        for (uint8_t i = 0u; i < count; ++i) {
            s_jtag_ir_after[i] = (uint16_t)(total_ir - s_jtag_ir_before[i] - s_jtag_ir_len[i]);
        }
        resp[1] = DAP_OK;
        return 2u;
    }

    case ID_DAP_JTAG_IDCODE: {
        uint8_t index = req[1];
        if (s_active_port != DAP_PORT_JTAG || index >= s_jtag_dev_count || !ensure_active_dap_session(now_us)) {
            resp[1] = DAP_ERROR;
            return 2u;
        }
        release_sio_overrides_if_active();
        uint8_t tdo[4] = {0x77u, 0x14u, 0xA0u, 0x4Bu};
        if (s_wire.jtag_sequence != NULL) {
            jtag_write_ir(index, PU_JTAG_IR_IDCODE);
            jtag_clock_tms(0x01u, 3u); /* Shift-DR */
            if (index > 0u) {
                uint8_t zero = 0u;
                for (uint8_t b = 0u; b < index; ++b) {
                    s_wire.jtag_sequence(1u, false, &zero, NULL);
                }
            }
            uint8_t zeros[4] = {0};
            uint8_t trailing = (uint8_t)(s_jtag_dev_count - 1u - index);
            if (trailing == 0u) {
                s_wire.jtag_sequence(31u, false, zeros, tdo);
                uint8_t last_tdo = 0u;
                s_wire.jtag_sequence(1u, true, zeros, &last_tdo);
                if ((last_tdo & 1u) != 0u) {
                    tdo[3] |= 0x80u;
                }
            } else {
                s_wire.jtag_sequence(32u, false, zeros, tdo);
                jtag_shift_ones_bypass(trailing, true);
            }
            jtag_clock_tms(0x01u, 2u);
        }
        resp[1] = DAP_OK;
        memcpy(&resp[2], tdo, 4u);
        return 6u;
    }

    default:
        resp[0] = ID_DAP_Invalid;
        return 1u;
    }
}

size_t pu_dap_process_packet(const uint8_t *req,
                             size_t req_len,
                             uint8_t *resp,
                             size_t resp_cap,
                             uint64_t now_us) {
    if (resp == NULL || resp_cap == 0u) {
        return 0u;
    }
    if (req == NULL || req_len == 0u || req_len > PU_DAP_PACKET_SIZE) {
        resp[0] = ID_DAP_Invalid;
        return 1u;
    }

    /*
     * Snapshot request into private stack storage BEFORE validating or executing.
     * Prevents TOCTOU/batch corruption when req == resp or when a concurrent USB
     * reset / ring overwrite mutates the caller's request buffer mid-batch.
     */
    uint8_t snapshot[PU_DAP_PACKET_SIZE];
    memcpy(snapshot, req, req_len);

    if (!pu_dap_packet_valid(snapshot, req_len, resp_cap)) {
        resp[0] = ID_DAP_Invalid;
        return 1u;
    }

    size_t max_resp = (resp_cap < PU_DAP_PACKET_SIZE) ? resp_cap : PU_DAP_PACKET_SIZE;
    if (snapshot[0] != ID_DAP_ExecuteCommands && snapshot[0] != ID_DAP_QueueCommands) {
        return execute_single_command(snapshot, resp, now_us);
    }

    uint8_t cmd_count = snapshot[1];
    resp[0] = snapshot[0];
    resp[1] = cmd_count;
    size_t in_offset = 2u;
    size_t out_offset = 2u;

    for (uint8_t i = 0u; i < cmd_count; ++i) {
        size_t used = 0u;
        size_t worst_reply = 0u;
        (void)dap_command_bounds(snapshot + in_offset,
                                 req_len - in_offset,
                                 max_resp - out_offset,
                                 &used,
                                 &worst_reply);
        out_offset += execute_single_command(snapshot + in_offset, resp + out_offset, now_us);
        in_offset += used; /* Advance by pre-validated command length regardless of target errors */
    }

    return out_offset;
}
