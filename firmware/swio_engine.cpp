/*
 * Hardened WCH CH32V003 1-Wire SWIO Engine, PUID1 Non-Destructive Identity
 * & Cross-Page SoftBreak for Pico Universal.
 */

#include "firmware/swio_engine.h"

#include <cstdio>
#include <cstring>

namespace {
/* Assembled via pioasm from firmware/pio/singlewire.pio (22 instructions) */
constexpr uint16_t kPuSwioFallbackInstructions[] = {
    0x80a0, 0x6037, 0x7041, 0x1166, 0x05e2, 0x0008, 0xbb42, 0x03e2,
    0x004a, 0x0014, 0x002e, 0xe020, 0x80a0, 0x0002, 0xe03f, 0xb242,
    0xa342, 0x4b01, 0x004f, 0x8020, 0xe042, 0x0f95,
};
static_assert(sizeof(kPuSwioFallbackInstructions) / sizeof(kPuSwioFallbackInstructions[0]) <= 24u,
              "singlewire.pio must occupy <= 24 PIO1 instruction words");

#if !defined(PU_HOST_TEST) && defined(__has_include)
#if __has_include("singlewire.pio.h")
[[maybe_unused]] constexpr bool pio_fallback_matches_assembled(const uint16_t *a,
                                                               const uint16_t *b,
                                                               size_t n) {
    for (size_t i = 0u; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}
#endif
#endif
} // namespace

#if !defined(PU_HOST_TEST)

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/time.h"
#if defined(__has_include) && __has_include("singlewire.pio.h")
#include "singlewire.pio.h"
static_assert(sizeof(pu_swio_program_instructions) == sizeof(kPuSwioFallbackInstructions),
              "singlewire.pio.h instruction count must match fallback array");
#if defined(__pio_const)
static_assert(pio_fallback_matches_assembled(
                  pu_swio_program_instructions,
                  kPuSwioFallbackInstructions,
                  sizeof(kPuSwioFallbackInstructions) / sizeof(kPuSwioFallbackInstructions[0])),
              "singlewire.pio.h instructions must match fallback array byte-for-byte");
#endif
#else
#define pu_swio_wrap_target 0
#define pu_swio_wrap 21
#define pu_swio_offset_frame_start 0u
#define pu_swio_program_instructions kPuSwioFallbackInstructions
static constexpr struct pio_program pu_swio_program = {
    pu_swio_program_instructions,
    22,
    -1,
#if defined(PICO_PIO_VERSION)
    0,
#if PICO_PIO_VERSION > 0
    0x0
#endif
#endif
};
static inline pio_sm_config pu_swio_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + pu_swio_wrap_target, offset + pu_swio_wrap);
    sm_config_set_sideset(&c, 1, false, true);
    return c;
}
static inline void pu_swio_program_init(PIO pio, uint sm, uint offset, uint32_t clkdiv_q16_8) {
    pio_sm_config c = pu_swio_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, PU_PIN_SWIO);
    sm_config_set_in_pins(&c, PU_PIN_SWIO);
    sm_config_set_out_shift(&c, false, false, 32u);
    sm_config_set_in_shift(&c, false, false, 32u);
    c.clkdiv = (clkdiv_q16_8 << PIO_SM0_CLKDIV_FRAC_LSB);
#if defined(PU_PIN_SWIO_PU_EN)
    gpio_init(PU_PIN_SWIO_PU_EN);
    gpio_put(PU_PIN_SWIO_PU_EN, true);
    gpio_set_dir(PU_PIN_SWIO_PU_EN, GPIO_OUT);
#endif
    pio_gpio_init(pio, PU_PIN_SWIO);
    gpio_pull_up(PU_PIN_SWIO);
    pio_sm_set_pins_with_mask(pio, sm, 0u, (1u << PU_PIN_SWIO));
    pio_sm_set_consecutive_pindirs(pio, sm, PU_PIN_SWIO, 1u, false);
    pio_sm_init(pio, sm, offset + pu_swio_offset_frame_start, &c);
    pio_sm_set_enabled(pio, sm, true);
}
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static_assert(sizeof(pu_swio_program_instructions) / sizeof(pu_swio_program_instructions[0]) <= 24u,
              "singlewire.pio must occupy <= 24 PIO1 instruction words");

#endif /* !defined(PU_HOST_TEST) */

namespace {

/* -------------------------------------------------------------------------- */
/* WCH QingKe V2 Debug Module (DM) & CH32V003 Hardware Constants              */
/* -------------------------------------------------------------------------- */

constexpr uint8_t kDmData0        = 0x04u;
constexpr uint8_t kDmControl      = 0x10u;
constexpr uint8_t kDmStatus       = 0x11u;
constexpr uint8_t kDmHartInfo     = 0x12u;
constexpr uint8_t kDmAbstractCs   = 0x16u;
constexpr uint8_t kDmCommand      = 0x17u;
constexpr uint8_t kDmProgBuf0     = 0x20u;
constexpr uint8_t kDmProgBuf1     = 0x21u;
constexpr uint8_t kWchDmCpbr      = 0x7Cu;
constexpr uint8_t kWchDmCfgr      = 0x7Du;
constexpr uint8_t kWchDmShdwCfgr  = 0x7Eu;

/* DMCONTROL (0x10) bitfields */
constexpr uint32_t kDmCtrlDmActive     = (1u << 0);
constexpr uint32_t kDmCtrlNdmReset     = (1u << 1);
constexpr uint32_t kDmCtrlAckHaveReset = (1u << 28);
constexpr uint32_t kDmCtrlResumeReq    = (1u << 30);
constexpr uint32_t kDmCtrlHaltReq      = (1u << 31);

/* DMSTATUS (0x11) QingKe V2 Table 3-5 bitfields */
constexpr uint32_t kDmStatVersionMask     = 0x0000000Fu;
constexpr uint32_t kDmStatVersionQingKeV2 = 0x00000002u;
constexpr uint32_t kDmStatReservedMask    = 0xFFB00070u; /* [31:23], [21:20], [6:4] must be 0 */
constexpr uint32_t kDmStatAuthenticated   = (1u << 7);
constexpr uint32_t kDmStatAnyHalted       = (1u << 8);
constexpr uint32_t kDmStatAllHalted       = (1u << 9);
constexpr uint32_t kDmStatAnyRunning      = (1u << 10);
constexpr uint32_t kDmStatAllRunning      = (1u << 11);
constexpr uint32_t kDmStatAnyUnavail      = (1u << 12);
constexpr uint32_t kDmStatAllUnavail      = (1u << 13);
constexpr uint32_t kDmStatAnyNonexistent  = (1u << 14);
constexpr uint32_t kDmStatAllNonexistent  = (1u << 15);
constexpr uint32_t kDmStatAnyResumeAck    = (1u << 16);
constexpr uint32_t kDmStatAllResumeAck    = (1u << 17);
constexpr uint32_t kDmStatAnyHaveReset    = (1u << 18);
constexpr uint32_t kDmStatAllHaveReset    = (1u << 19);
constexpr uint32_t kDmStatImpebreak       = (1u << 22);

/* ABSTRACTCS (0x16) bitfields */
constexpr uint32_t kAbsCsCmdErrMask = (7u << 8);
constexpr uint32_t kAbsCsBusy       = (1u << 12);

/* WCH CFGR / SHDWCFGR (0x7D / 0x7E) */
constexpr uint32_t kWchCfgrKeyMask   = 0xFFFF0000u;
constexpr uint32_t kWchCfgrKey       = 0x5AA50000u;
constexpr uint32_t kWchCfgrOutEnBit  = 0x00000400u;

/* RISC-V Debug Spec regno & RV32EC PROGBUF opcodes */
constexpr uint16_t kRegDcsr = 0x07B0u;
constexpr uint16_t kRegDpc  = 0x07B1u;
constexpr uint16_t kRegGpr0 = 0x1000u;
constexpr uint16_t kRegX8   = 0x1008u;
constexpr uint16_t kRegX9   = 0x1009u;

constexpr uint32_t kDcsrStepBit    = (1u << 2);
constexpr uint32_t kDcsrEbreakM    = (1u << 15);

constexpr uint32_t kInsnEbreak     = 0x00100073u;
constexpr uint16_t kInsnCEbreak    = 0x9002u;
constexpr uint32_t kInsnLwX9FromX8  = 0x00042483u; /* lw  x9, 0(x8) */
constexpr uint32_t kInsnLhuX9FromX8 = 0x00045483u; /* lhu x9, 0(x8) */
constexpr uint32_t kInsnLbuX9FromX8 = 0x00044483u; /* lbu x9, 0(x8) */
constexpr uint32_t kInsnSwX9ToX8    = 0x00942023u; /* sw  x9, 0(x8) */
constexpr uint32_t kInsnShX9ToX8    = 0x00941023u; /* sh  x9, 0(x8) */
constexpr uint32_t kInsnSbX9ToX8    = 0x00940023u; /* sb  x9, 0(x8) */

/* CH32V003 Electronic Signature & Flash Controller MMIO */
constexpr uint32_t kCh32DbgmcuIdcodeAddr = 0x1FFFF7C4u;
constexpr uint32_t kCh32FlashSizeAddr    = 0x1FFFF7E0u;
constexpr uint32_t kCh32Uid0Addr         = 0x1FFFF7E8u;

constexpr uint32_t kCh32FlashRegBase   = 0x40022000u;
constexpr uint32_t kCh32FlashKeyr      = 0x40022004u;
constexpr uint32_t kCh32FlashStatr     = 0x4002200Cu;
constexpr uint32_t kCh32FlashCtlr      = 0x40022010u;
constexpr uint32_t kCh32FlashAddr      = 0x40022014u;
constexpr uint32_t kCh32FlashModeKeyr  = 0x40022024u;

constexpr uint32_t kCh32FlashKey1      = 0x45670123u;
constexpr uint32_t kCh32FlashKey2      = 0xCDEF89ABu;

constexpr uint32_t kFlashStatrBsy      = (1u << 0);
constexpr uint32_t kFlashStatrWrprterr = (1u << 2);
constexpr uint32_t kFlashStatrEop      = (1u << 4);

constexpr uint32_t kFlashCtlrStrt      = (1u << 6);
constexpr uint32_t kFlashCtlrLock      = (1u << 7);
constexpr uint32_t kFlashCtlrFlock     = (1u << 15);
constexpr uint32_t kFlashCtlrFtpg      = (1u << 16);
constexpr uint32_t kFlashCtlrFter      = (1u << 17);
constexpr uint32_t kFlashCtlrBufLoad   = (1u << 18);
constexpr uint32_t kFlashCtlrBufRst    = (1u << 19);

/* -------------------------------------------------------------------------- */
/* RP2040 PIO1 Hardware Backend vs Host QingKe V2 Wire Simulation             */
/* -------------------------------------------------------------------------- */

#if !defined(PU_HOST_TEST)

constexpr uint kSwioPioSm = 0u;
bool s_pio_loaded = false;
uint s_pio_offset = 0u;

uint32_t compute_swio_clkdiv_q16_8(uint32_t sys_hz) {
    const uint32_t effective_sys = (sys_hz != 0u) ? sys_hz : 125000000u;
    /* Target ~10.42 MHz PIO clock (clkdiv=12.0 at 125 MHz -> 96 ns/cyc, 288 ns '1' low,
     * 656 ns RX sample): centered in empirically characterized CH32V003 silicon window. */
    constexpr uint64_t kTargetPioHz = 10416667ull;
    uint64_t div_q16_8 = ((uint64_t)effective_sys * 256ull + kTargetPioHz - 1ull) / kTargetPioHz;
    if (div_q16_8 < 256ull) {
        div_q16_8 = 256ull;
    } else if (div_q16_8 > 0xFFFFFFull) {
        div_q16_8 = 0xFFFFFFull;
    }
    return static_cast<uint32_t>(div_q16_8);
}

bool rp2040_swio_push_bounded(uint32_t word, uint32_t timeout_us) {
    const uint64_t t0 = time_us_64();
    while (pio_sm_is_tx_fifo_full(pio1, kSwioPioSm)) {
        const uint64_t now = time_us_64();
        if ((now >= t0) && ((now - t0) >= timeout_us)) {
            return false;
        }
    }
    pio_sm_put(pio1, kSwioPioSm, word);
    return true;
}

void rp2040_swio_drain_bounded(uint32_t timeout_us) {
    if (!s_pio_loaded) {
        return;
    }
    const uint64_t t0 = time_us_64();
    while (!pio_sm_is_tx_fifo_empty(pio1, kSwioPioSm)) {
        const uint64_t now = time_us_64();
        if ((now >= t0) && ((now - t0) >= timeout_us)) {
            break;
        }
    }
    busy_wait_us_32(12u);
    while (!pio_sm_is_rx_fifo_empty(pio1, kSwioPioSm)) {
        (void)pio_sm_get(pio1, kSwioPioSm);
    }
}

void rp2040_swio_bus_close() {
    if (s_pio_loaded) {
        rp2040_swio_drain_bounded(PU_SWIO_WIRE_TIMEOUT_US);
        pio_sm_set_enabled(pio1, kSwioPioSm, false);
        pio_sm_clear_fifos(pio1, kSwioPioSm);
        pio_remove_program(pio1, &pu_swio_program, s_pio_offset);
        s_pio_loaded = false;
        s_pio_offset = 0u;
    }
    gpio_init(PU_PIN_SWIO);
    gpio_set_dir(PU_PIN_SWIO, GPIO_IN);
    gpio_disable_pulls(PU_PIN_SWIO);
#if defined(PU_PIN_SWIO_PU_EN)
    gpio_init(PU_PIN_SWIO_PU_EN);
    gpio_put(PU_PIN_SWIO_PU_EN, false);
    gpio_set_dir(PU_PIN_SWIO_PU_EN, GPIO_OUT);
#endif
}

void rp2040_swio_bus_open() {
    rp2040_swio_bus_close();
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t clkdiv_q16_8 = compute_swio_clkdiv_q16_8(sys_hz);
    s_pio_offset = pio_add_program(pio1, &pu_swio_program);
    s_pio_loaded = true;
    pu_swio_program_init(pio1, kSwioPioSm, s_pio_offset, clkdiv_q16_8);
}

void rp2040_swio_bus_reset() {
    if (!s_pio_loaded) {
        rp2040_swio_bus_open();
    }
    rp2040_swio_drain_bounded(PU_SWIO_WIRE_TIMEOUT_US);
    pio_sm_set_enabled(pio1, kSwioPioSm, false);
    pio_sm_clear_fifos(pio1, kSwioPioSm);

    /* Hold SWIO open-drain LOW for 300 us (>= 256T even at 8 MHz / TDIV=3 cold boot) */
    pio_sm_set_pins_with_mask(pio1, kSwioPioSm, 0u, (1u << PU_PIN_SWIO));
    pio_sm_set_consecutive_pindirs(pio1, kSwioPioSm, PU_PIN_SWIO, 1u, true);
    busy_wait_us_32(300u);
    pio_sm_set_consecutive_pindirs(pio1, kSwioPioSm, PU_PIN_SWIO, 1u, false);
    busy_wait_us_32(20u);

    pio_sm_exec(pio1, kSwioPioSm, pio_encode_jmp(s_pio_offset + pu_swio_offset_frame_start));
    pio_sm_set_enabled(pio1, kSwioPioSm, true);
}

bool rp2040_swio_write_reg(uint8_t reg_addr, uint32_t val, uint32_t timeout_us) {
    if (!s_pio_loaded) {
        return false;
    }
    const uint32_t eff_timeout = (timeout_us != 0u) ? timeout_us : PU_SWIO_WIRE_TIMEOUT_US;
    const uint32_t hdr = (2u << 9) | (1u << 8) |
                         (static_cast<uint32_t>(reg_addr & 0x7Fu) << 1) | 1u;

    /* Wait until >= 2 of the 4 TX FIFO slots are free, then push header + data
     * atomically with local IRQs masked so no ISR can inject a >= 18T stop gap! */
    const uint64_t push_t0 = time_us_64();
    for (;;) {
        const uint32_t irq_state = save_and_disable_interrupts();
        if (pio_sm_get_tx_fifo_level(pio1, kSwioPioSm) <= 2u) {
            pio_sm_put(pio1, kSwioPioSm, hdr);
            pio_sm_put(pio1, kSwioPioSm, val);
            restore_interrupts(irq_state);
            break;
        }
        restore_interrupts(irq_state);
        const uint64_t now = time_us_64();
        if ((now >= push_t0) && ((now - push_t0) >= eff_timeout)) {
            return false;
        }
    }

    const uint64_t drain_t0 = time_us_64();
    while (!pio_sm_is_tx_fifo_empty(pio1, kSwioPioSm)) {
        const uint64_t now = time_us_64();
        if ((now >= drain_t0) && ((now - drain_t0) >= eff_timeout)) {
            return false;
        }
    }
    busy_wait_us_32(50u);
    return true;
}

bool rp2040_swio_read_reg(uint8_t reg_addr, uint32_t *out_val, uint32_t timeout_us) {
    if (!s_pio_loaded || out_val == nullptr) {
        return false;
    }
    while (!pio_sm_is_rx_fifo_empty(pio1, kSwioPioSm)) {
        (void)pio_sm_get(pio1, kSwioPioSm);
    }
    const uint32_t eff_timeout = (timeout_us != 0u) ? timeout_us : PU_SWIO_WIRE_TIMEOUT_US;
    const uint32_t hdr = (1u << 9) | (1u << 8) |
                         (static_cast<uint32_t>(reg_addr & 0x7Fu) << 1) | 0u;
    if (!rp2040_swio_push_bounded(hdr, eff_timeout)) {
        *out_val = 0u;
        return false;
    }
    const uint64_t t0 = time_us_64();
    while (pio_sm_is_rx_fifo_empty(pio1, kSwioPioSm)) {
        const uint64_t now = time_us_64();
        if ((now >= t0) && ((now - t0) >= eff_timeout)) {
            *out_val = 0u;
            return false;
        }
    }
    *out_val = pio_sm_get(pio1, kSwioPioSm);
    return true;
}

#if PU_HAS_VTARG_SWITCH
void rp2040_swio_set_vtarg(bool enabled) {
    /* Pin direction is initialized once by pu_ctrl_rp2040_init(); avoid gpio_init() glitch. */
    gpio_put(PU_PIN_VTARG_EN, enabled);
}
#endif

void rp2040_swio_delay_us(uint32_t delay_us) {
    busy_wait_us_32(delay_us);
}

uint64_t rp2040_swio_now_us() {
    return time_us_64();
}

#else /* defined(PU_HOST_TEST) */

struct FakeCh32Target {
    pu_fake_wire_mode_t mode;
    bool powered;
    bool dm_active;
    bool halted;
    bool resume_ack;
    bool have_reset;
    bool cfgr_outen;
    bool shdw_outen;
    uint8_t cmderr;
    uint32_t data0;
    uint32_t progbuf[8];
    uint32_t gpr[16];
    uint32_t dpc;
    uint32_t dcsr;

    /* Simulated CH32V003 Flash Controller & Memory */
    uint8_t flash[PU_CH32_FLASH_SIZE];
    uint8_t original_flash[PU_CH32_FLASH_SIZE];
    uint8_t sram[PU_CH32_SRAM_SIZE];
    uint8_t page_latch[PU_CH32_FLASH_PAGE_SIZE];
    uint32_t staged_flash_word;
    uint32_t staged_flash_addr;
    uint32_t flash_ctlr;
    uint32_t flash_statr;
    uint32_t flash_addr;
    uint8_t keyr_step;
    uint8_t modekeyr_step;
    uint32_t flash_writes;
    uint32_t page_prog_seq;
    int fail_on_write_index;
    uint64_t sim_time_us;
};

FakeCh32Target s_fake;

uint8_t fake_mem_read8(uint32_t addr) {
    if (addr < PU_CH32_FLASH_SIZE) {
        return s_fake.flash[addr];
    }
    if (addr >= PU_CH32_FLASH_BASE && addr < PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE) {
        return s_fake.flash[addr - PU_CH32_FLASH_BASE];
    }
    if (addr >= PU_CH32_SRAM_BASE && addr < PU_CH32_SRAM_BASE + PU_CH32_SRAM_SIZE) {
        return s_fake.sram[addr - PU_CH32_SRAM_BASE];
    }
    return 0xFFu;
}

uint32_t fake_mem_read32(uint32_t addr) {
    if (addr == kCh32DbgmcuIdcodeAddr) {
        return 0x00300500u; /* CH32V003F4P6 ID */
    }
    if (addr == kCh32FlashSizeAddr) {
        return 0x00000010u; /* 16 KiB */
    }
    if (addr == kCh32Uid0Addr) {
        return 0x32563033u;
    }
    if (addr == kCh32FlashStatr) {
        return s_fake.flash_statr;
    }
    if (addr == kCh32FlashCtlr) {
        return s_fake.flash_ctlr;
    }
    if (addr == kCh32FlashAddr) {
        return s_fake.flash_addr;
    }
    return static_cast<uint32_t>(fake_mem_read8(addr)) |
           (static_cast<uint32_t>(fake_mem_read8(addr + 1u)) << 8) |
           (static_cast<uint32_t>(fake_mem_read8(addr + 2u)) << 16) |
           (static_cast<uint32_t>(fake_mem_read8(addr + 3u)) << 24);
}

void fake_mem_write8(uint32_t addr, uint8_t val) {
    if (addr >= PU_CH32_SRAM_BASE && addr < PU_CH32_SRAM_BASE + PU_CH32_SRAM_SIZE) {
        s_fake.sram[addr - PU_CH32_SRAM_BASE] = val;
    }
}

void fake_flash_mmio_write32(uint32_t addr, uint32_t val) {
    if (addr == kCh32FlashKeyr) {
        if (s_fake.keyr_step == 0u && val == kCh32FlashKey1) {
            s_fake.keyr_step = 1u;
        } else if (s_fake.keyr_step == 1u && val == kCh32FlashKey2) {
            s_fake.flash_ctlr &= ~kFlashCtlrLock;
            s_fake.keyr_step = 0u;
        } else {
            s_fake.keyr_step = 0u;
        }
        return;
    }
    if (addr == kCh32FlashModeKeyr) {
        if (s_fake.modekeyr_step == 0u && val == kCh32FlashKey1) {
            s_fake.modekeyr_step = 1u;
        } else if (s_fake.modekeyr_step == 1u && val == kCh32FlashKey2) {
            s_fake.flash_ctlr &= ~kFlashCtlrFlock;
            s_fake.modekeyr_step = 0u;
        } else {
            s_fake.modekeyr_step = 0u;
        }
        return;
    }
    if (addr == kCh32FlashStatr) {
        if ((val & kFlashStatrEop) != 0u) {
            s_fake.flash_statr &= ~kFlashStatrEop;
        }
        if ((val & kFlashStatrWrprterr) != 0u) {
            s_fake.flash_statr &= ~kFlashStatrWrprterr;
        }
        return;
    }
    if (addr == kCh32FlashAddr) {
        s_fake.flash_addr = val;
        return;
    }
    if (addr == kCh32FlashCtlr) {
        if ((val & kFlashCtlrLock) != 0u) {
            s_fake.flash_ctlr |= (kFlashCtlrLock | kFlashCtlrFlock);
            return;
        }
        const bool fast_unlocked =
            (s_fake.flash_ctlr & (kFlashCtlrLock | kFlashCtlrFlock)) == 0u;
        s_fake.flash_ctlr = (s_fake.flash_ctlr & (kFlashCtlrLock | kFlashCtlrFlock)) |
                            (val & ~(kFlashCtlrStrt | kFlashCtlrBufLoad | kFlashCtlrBufRst));
        if (!fast_unlocked) {
            s_fake.flash_statr |= kFlashStatrWrprterr;
            return;
        }
        if ((val & kFlashCtlrBufRst) != 0u) {
            std::memset(s_fake.page_latch, 0xFF, sizeof(s_fake.page_latch));
            s_fake.flash_statr |= kFlashStatrEop;
        }
        if ((val & kFlashCtlrBufLoad) != 0u) {
            uint32_t off = s_fake.staged_flash_addr & 0x3Cu;
            s_fake.page_latch[off + 0u] = static_cast<uint8_t>(s_fake.staged_flash_word & 0xFFu);
            s_fake.page_latch[off + 1u] = static_cast<uint8_t>((s_fake.staged_flash_word >> 8) & 0xFFu);
            s_fake.page_latch[off + 2u] = static_cast<uint8_t>((s_fake.staged_flash_word >> 16) & 0xFFu);
            s_fake.page_latch[off + 3u] = static_cast<uint8_t>((s_fake.staged_flash_word >> 24) & 0xFFu);
            s_fake.flash_statr |= kFlashStatrEop;
        }
        if ((val & (kFlashCtlrFter | kFlashCtlrStrt)) == (kFlashCtlrFter | kFlashCtlrStrt)) {
            uint32_t flash_off = 0u;
            bool ok_addr = false;
            if (s_fake.flash_addr < PU_CH32_FLASH_SIZE) {
                flash_off = s_fake.flash_addr & ~(PU_CH32_FLASH_PAGE_SIZE - 1u);
                ok_addr = true;
            } else if (s_fake.flash_addr >= PU_CH32_FLASH_BASE &&
                       s_fake.flash_addr < PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE) {
                flash_off = (s_fake.flash_addr - PU_CH32_FLASH_BASE) & ~(PU_CH32_FLASH_PAGE_SIZE - 1u);
                ok_addr = true;
            }
            if (ok_addr) {
                std::memset(s_fake.flash + flash_off, 0xFF, PU_CH32_FLASH_PAGE_SIZE);
                ++s_fake.flash_writes;
                s_fake.flash_statr |= kFlashStatrEop;
            } else {
                s_fake.flash_statr |= kFlashStatrWrprterr;
            }
        }
        if ((val & (kFlashCtlrFtpg | kFlashCtlrStrt)) == (kFlashCtlrFtpg | kFlashCtlrStrt)) {
            if (s_fake.fail_on_write_index >= 0 &&
                static_cast<uint32_t>(s_fake.fail_on_write_index) == s_fake.page_prog_seq) {
                /* One-shot simulated flash write error on target page index */
                s_fake.fail_on_write_index = -1;
                ++s_fake.page_prog_seq;
                s_fake.flash_statr |= kFlashStatrWrprterr;
                return;
            }
            ++s_fake.page_prog_seq;
            uint32_t flash_off = 0u;
            bool ok_addr = false;
            if (s_fake.flash_addr < PU_CH32_FLASH_SIZE) {
                flash_off = s_fake.flash_addr & ~(PU_CH32_FLASH_PAGE_SIZE - 1u);
                ok_addr = true;
            } else if (s_fake.flash_addr >= PU_CH32_FLASH_BASE &&
                       s_fake.flash_addr < PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE) {
                flash_off = (s_fake.flash_addr - PU_CH32_FLASH_BASE) & ~(PU_CH32_FLASH_PAGE_SIZE - 1u);
                ok_addr = true;
            }
            if (ok_addr) {
                for (size_t i = 0u; i < PU_CH32_FLASH_PAGE_SIZE; ++i) {
                    s_fake.flash[flash_off + i] &= s_fake.page_latch[i];
                }
                ++s_fake.flash_writes;
                s_fake.flash_statr |= kFlashStatrEop;
            } else {
                s_fake.flash_statr |= kFlashStatrWrprterr;
            }
        }
    }
}

void fake_mem_write32(uint32_t addr, uint32_t val) {
    if (addr >= kCh32FlashRegBase && addr < kCh32FlashRegBase + 0x40u) {
        fake_flash_mmio_write32(addr, val);
        return;
    }
    if ((addr < PU_CH32_FLASH_SIZE) ||
        (addr >= PU_CH32_FLASH_BASE && addr < PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE)) {
        if ((s_fake.flash_ctlr & kFlashCtlrFtpg) != 0u) {
            s_fake.staged_flash_addr = addr;
            s_fake.staged_flash_word = val;
        }
        return;
    }
    fake_mem_write8(addr + 0u, static_cast<uint8_t>(val & 0xFFu));
    fake_mem_write8(addr + 1u, static_cast<uint8_t>((val >> 8) & 0xFFu));
    fake_mem_write8(addr + 2u, static_cast<uint8_t>((val >> 16) & 0xFFu));
    fake_mem_write8(addr + 3u, static_cast<uint8_t>((val >> 24) & 0xFFu));
}

void fake_exec_progbuf() {
    for (size_t i = 0u; i < 8u; ++i) {
        const uint32_t insn = s_fake.progbuf[i];
        if (insn == kInsnEbreak || (insn & 0xFFFFu) == kInsnCEbreak) {
            return;
        }
        const uint32_t addr = s_fake.gpr[8];
        if (insn == kInsnLwX9FromX8) {
            if ((addr & 3u) != 0u) {
                s_fake.cmderr = 3u;
                return;
            }
            s_fake.gpr[9] = fake_mem_read32(addr);
        } else if (insn == kInsnLhuX9FromX8) {
            if ((addr & 1u) != 0u) {
                s_fake.cmderr = 3u;
                return;
            }
            s_fake.gpr[9] = static_cast<uint32_t>(fake_mem_read8(addr)) |
                            (static_cast<uint32_t>(fake_mem_read8(addr + 1u)) << 8);
        } else if (insn == kInsnLbuX9FromX8) {
            if (addr >= 0x40000000u || (addr >= 0x1FFFF000u && addr <= 0x1FFFFFFFu)) {
                s_fake.cmderr = 3u;
                return;
            }
            s_fake.gpr[9] = fake_mem_read8(addr);
        } else if (insn == kInsnSwX9ToX8) {
            if ((addr & 3u) != 0u) {
                s_fake.cmderr = 3u;
                return;
            }
            fake_mem_write32(addr, s_fake.gpr[9]);
        } else if (insn == kInsnShX9ToX8) {
            if ((addr & 1u) != 0u) {
                s_fake.cmderr = 3u;
                return;
            }
            fake_mem_write8(addr + 0u, static_cast<uint8_t>(s_fake.gpr[9] & 0xFFu));
            fake_mem_write8(addr + 1u, static_cast<uint8_t>((s_fake.gpr[9] >> 8) & 0xFFu));
        } else if (insn == kInsnSbX9ToX8) {
            if (addr >= 0x40000000u) {
                s_fake.cmderr = 3u;
                return;
            }
            fake_mem_write8(addr, static_cast<uint8_t>(s_fake.gpr[9] & 0xFFu));
        } else {
            s_fake.cmderr = 3u;
            return;
        }
        s_fake.gpr[0] = 0u;
    }
}

void host_fake_bus_open() {}
void host_fake_bus_close() {}
void host_fake_bus_reset() {}

void host_fake_set_vtarg(bool enabled) {
    if (!enabled) {
        s_fake.powered = false;
    } else {
        s_fake.powered = true;
        s_fake.dm_active = false;
        s_fake.halted = false;
        s_fake.cfgr_outen = false;
        s_fake.shdw_outen = false;
        if (s_fake.mode == PU_WIRE_SILENT_HIGH_FF || s_fake.mode == PU_WIRE_SILENT_LOW_00) {
            /* Keep hardware fault modes sticky unless healthy */
        }
    }
}

void host_fake_delay_us(uint32_t delay_us) {
    s_fake.sim_time_us += delay_us;
}

uint64_t host_fake_now_us() {
    s_fake.sim_time_us += 1u;
    return s_fake.sim_time_us;
}

bool fake_pc_hits_ebreak(uint32_t pc) {
    const uint16_t hw0 = static_cast<uint16_t>(fake_mem_read8(pc)) |
                         (static_cast<uint16_t>(fake_mem_read8(pc + 1u)) << 8);
    if (hw0 == kInsnCEbreak) {
        return true;
    }
    if ((hw0 & 0x3u) == 0x3u && fake_mem_read32(pc) == kInsnEbreak) {
        return true;
    }
    return false;
}

bool host_fake_read_reg(uint8_t reg_addr, uint32_t *out_val, uint32_t /*timeout_us*/) {
    if (out_val == nullptr) {
        return false;
    }
    if (!s_fake.powered || s_fake.mode == PU_WIRE_SILENT_HIGH_FF) {
        *out_val = 0xFFFFFFFFu;
        return true;
    }
    if (s_fake.mode == PU_WIRE_SILENT_LOW_00) {
        *out_val = 0x00000000u;
        return true;
    }
    if (!s_fake.cfgr_outen) {
        /* Target SWIO output driver disabled -> pull-up floats line to all-1s */
        *out_val = 0xFFFFFFFFu;
        return true;
    }

    switch (reg_addr) {
    case kDmData0:
        *out_val = s_fake.data0;
        return true;
    case kDmControl:
        *out_val = s_fake.dm_active ? kDmCtrlDmActive : 0u;
        return true;
    case kDmStatus: {
        if (!s_fake.halted && s_fake.dm_active) {
            const uint16_t hw0 = static_cast<uint16_t>(fake_mem_read8(s_fake.dpc)) |
                                 (static_cast<uint16_t>(fake_mem_read8(s_fake.dpc + 1u)) << 8);
            const uint32_t step_bytes = ((hw0 & 0x3u) == 0x3u) ? 4u : 2u;
            uint32_t next_pc = s_fake.dpc + step_bytes;
            if (next_pc >= PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE - 4u) {
                next_pc = PU_CH32_FLASH_BASE;
            } else if (s_fake.dpc < PU_CH32_FLASH_SIZE &&
                       next_pc >= PU_CH32_FLASH_SIZE - 4u) {
                next_pc = 0u;
            }
            s_fake.dpc = next_pc;
            if ((s_fake.dcsr & kDcsrEbreakM) != 0u && fake_pc_hits_ebreak(s_fake.dpc)) {
                s_fake.halted = true;
            }
        }
        uint32_t st = kDmStatImpebreak | kDmStatVersionQingKeV2 | kDmStatAuthenticated;
        if (s_fake.halted) {
            st |= (kDmStatAnyHalted | kDmStatAllHalted);
        } else {
            st |= (kDmStatAnyRunning | kDmStatAllRunning);
        }
        if (s_fake.resume_ack) {
            st |= (kDmStatAnyResumeAck | kDmStatAllResumeAck);
        }
        if (s_fake.have_reset) {
            st |= (kDmStatAnyHaveReset | kDmStatAllHaveReset);
        }
        *out_val = st;
        return true;
    }
    case kDmHartInfo:
        *out_val = 0x00112380u;
        return true;
    case kDmAbstractCs:
        *out_val = (8u << 24) | (static_cast<uint32_t>(s_fake.cmderr) << 8) | 2u;
        return true;
    case kWchDmCpbr:
        *out_val = 0x00010001u | (s_fake.cfgr_outen ? kWchCfgrOutEnBit : 0u);
        return true;
    case kWchDmCfgr:
        *out_val = (s_fake.cfgr_outen ? kWchCfgrOutEnBit : 0u);
        return true;
    case kWchDmShdwCfgr:
        *out_val = (s_fake.shdw_outen ? kWchCfgrOutEnBit : 0u);
        return true;
    default:
        if (reg_addr >= kDmProgBuf0 && reg_addr < kDmProgBuf0 + 8u) {
            *out_val = s_fake.progbuf[reg_addr - kDmProgBuf0];
            return true;
        }
        *out_val = 0u;
        return true;
    }
}

bool host_fake_write_reg(uint8_t reg_addr, uint32_t val, uint32_t /*timeout_us*/) {
    if (!s_fake.powered ||
        s_fake.mode == PU_WIRE_SILENT_HIGH_FF ||
        s_fake.mode == PU_WIRE_SILENT_LOW_00) {
        return true;
    }

    if (reg_addr == kWchDmShdwCfgr) {
        if ((val & kWchCfgrKeyMask) == kWchCfgrKey) {
            bool want_outen = (val & kWchCfgrOutEnBit) != 0u;
            if (!want_outen && s_fake.mode == PU_WIRE_OUTEN_RESET_FAIL) {
                s_fake.shdw_outen = true; /* Simulate stuck SHDWCFGR.OUTEN */
            } else {
                s_fake.shdw_outen = want_outen;
            }
        }
        return true;
    }
    if (reg_addr == kWchDmCfgr) {
        if ((val & kWchCfgrKeyMask) == kWchCfgrKey && (val & kWchCfgrOutEnBit) != 0u) {
            s_fake.cfgr_outen = s_fake.shdw_outen;
        }
        return true;
    }

    if (reg_addr == kDmControl) {
        if ((val & kDmCtrlDmActive) == 0u) {
            s_fake.dm_active = false;
            return true;
        }
        s_fake.dm_active = true;
        if ((val & kDmCtrlAckHaveReset) != 0u) {
            s_fake.have_reset = false;
        }
        if ((val & kDmCtrlNdmReset) != 0u) {
            s_fake.dpc = PU_CH32_FLASH_BASE;
            s_fake.have_reset = true;
            if ((val & kDmCtrlHaltReq) != 0u) {
                s_fake.halted = true;
            }
        }
        if ((val & kDmCtrlHaltReq) != 0u) {
            s_fake.halted = true;
            s_fake.resume_ack = false;
        } else if ((val & kDmCtrlResumeReq) != 0u) {
            if (s_fake.mode != PU_WIRE_RESUME_FAIL) {
                if ((s_fake.dcsr & kDcsrStepBit) != 0u) {
                    const uint16_t hw0 = static_cast<uint16_t>(fake_mem_read8(s_fake.dpc)) |
                                         (static_cast<uint16_t>(fake_mem_read8(s_fake.dpc + 1u)) << 8);
                    const uint32_t w0  = fake_mem_read32(s_fake.dpc);
                    if (hw0 != kInsnCEbreak && w0 != kInsnEbreak) {
                        s_fake.dpc += ((hw0 & 0x3u) == 0x3u) ? 4u : 2u;
                    }
                    s_fake.halted = true;
                    s_fake.resume_ack = true;
                } else if ((s_fake.dcsr & kDcsrEbreakM) != 0u &&
                           fake_pc_hits_ebreak(s_fake.dpc)) {
                    s_fake.halted = true;
                    s_fake.resume_ack = true;
                } else {
                    s_fake.halted = false;
                    s_fake.resume_ack = true;
                }
            }
        }
        return true;
    }

    if (!s_fake.dm_active) {
        return true;
    }

    if (reg_addr == kDmData0) {
        s_fake.data0 = val;
        return true;
    }
    if (reg_addr >= kDmProgBuf0 && reg_addr < kDmProgBuf0 + 8u) {
        s_fake.progbuf[reg_addr - kDmProgBuf0] = val;
        return true;
    }
    if (reg_addr == kDmAbstractCs) {
        if ((val & kAbsCsCmdErrMask) != 0u) {
            s_fake.cmderr = 0u;
        }
        return true;
    }
    if (reg_addr == kDmCommand) {
        if (!s_fake.halted) {
            s_fake.cmderr = 4u;
            return true;
        }
        const uint8_t cmdtype = static_cast<uint8_t>((val >> 24) & 0xFFu);
        if (cmdtype != 0u) {
            s_fake.cmderr = 2u;
            return true;
        }
        const bool postexec = (val & (1u << 18)) != 0u;
        const bool transfer = (val & (1u << 17)) != 0u;
        const bool write    = (val & (1u << 16)) != 0u;
        const uint16_t regno = static_cast<uint16_t>(val & 0xFFFFu);
        if (transfer) {
            if (regno >= kRegGpr0 && regno < kRegGpr0 + 16u) {
                const size_t idx = regno - kRegGpr0;
                if (write) {
                    if (idx != 0u) {
                        s_fake.gpr[idx] = s_fake.data0;
                    }
                } else {
                    s_fake.data0 = (idx == 0u) ? 0u : s_fake.gpr[idx];
                }
            } else if (regno == kRegDpc) {
                if (write) s_fake.dpc = s_fake.data0;
                else s_fake.data0 = s_fake.dpc;
            } else if (regno == kRegDcsr) {
                if (write) s_fake.dcsr = s_fake.data0;
                else s_fake.data0 = s_fake.dcsr;
            } else {
                s_fake.cmderr = 2u;
                return true;
            }
        }
        if (postexec && s_fake.cmderr == 0u) {
            fake_exec_progbuf();
        }
        return true;
    }

    return true;
}

const pu_swio_wire_hal_t kHostFakeSwioHal = {
    125000000u,
    host_fake_bus_open,
    host_fake_bus_close,
    host_fake_bus_reset,
    host_fake_read_reg,
    host_fake_write_reg,
    host_fake_set_vtarg,
    host_fake_delay_us,
    host_fake_now_us,
};

#endif /* defined(PU_HOST_TEST) */

/* -------------------------------------------------------------------------- */
/* SWIO Engine State, Wire Helpers & Strict DMSTATUS Disambiguation           */
/* -------------------------------------------------------------------------- */

pu_swio_wire_hal_t s_wire = {};
bool s_wire_initialized = false;
bool s_has_custom_vtarg_cb = false;

enum class DmStateClass {
    kInvalidNoReply = 0,
    kRunning,
    kHalted,
    kUnexpected,
};

struct SoftBreakpoint {
    bool active;
    uint32_t offset; /* 0 .. PU_CH32_FLASH_SIZE - 2 */
    uint8_t size;    /* 2 or 4 */
};

constexpr size_t kMaxBreakpoints    = 32u;
constexpr size_t kMaxGdbRxLen       = 2048u;
constexpr size_t kMaxGdbRxRingCap   = 8192u;
constexpr size_t kMaxGdbTxLen       = 4352u;
constexpr size_t kMaxSingleRspReply = 2112u;
constexpr size_t kMaxMemReadLen     = 1024u;

SoftBreakpoint s_breakpoints[kMaxBreakpoints] = {};
uint8_t s_clean_pages[PU_CH32_FLASH_PAGE_COUNT][PU_CH32_FLASH_PAGE_SIZE] = {};
uint8_t s_page_bp_refcount[PU_CH32_FLASH_PAGE_COUNT] = {};
bool s_page_dirty_restore[PU_CH32_FLASH_PAGE_COUNT] = {};

bool s_vflash_dirty = false;
uint32_t s_vflash_page_idx = 0u;
uint8_t s_vflash_buf[PU_CH32_FLASH_PAGE_SIZE] = {};
uint64_t s_vflash_mask = 0u;

enum class GdbRxState {
    kIdle = 0,
    kBody,
    kCsum1,
    kCsum2,
};

bool s_gdb_session_open = false;
bool s_gdb_running = false;
bool s_entered_running_in_feed = false;
bool s_target_attached_halted = false;
pu_status_t s_last_session_err = PU_OK;

GdbRxState s_rx_state = GdbRxState::kIdle;
bool s_rx_escaped = false;
bool s_rx_error = false;
uint8_t s_rx_wire_csum = 0u;
uint8_t s_rx_expected_csum = 0u;
uint8_t s_rx_buf[kMaxGdbRxLen] = {};
size_t s_rx_len = 0u;

uint8_t s_rx_ring[kMaxGdbRxRingCap] = {};
size_t s_rx_ring_head = 0u;
size_t s_rx_ring_tail = 0u;
size_t s_rx_ring_count = 0u;

char s_tx_ring[kMaxGdbTxLen] = {};
size_t s_tx_head = 0u;
size_t s_tx_tail = 0u;
size_t s_tx_count = 0u;
char s_last_framed_pkt[kMaxSingleRspReply] = {};
size_t s_last_framed_len = 0u;

void ensure_wire_initialized() {
    if (s_wire_initialized) {
        return;
    }
    pu_swio_init(nullptr);
}

uint64_t wire_now_us(uint64_t fallback_us) {
    if (s_wire.now_us != nullptr) {
        return s_wire.now_us();
    }
    return fallback_us;
}

void wire_bus_open() {
    if (s_wire.bus_open != nullptr) {
        s_wire.bus_open();
    }
    if (s_wire.bus_reset != nullptr) {
        s_wire.bus_reset();
    }
}

void wire_bus_close() {
    if (s_wire.bus_close != nullptr) {
        s_wire.bus_close();
    }
}

bool wire_read(uint8_t reg, uint32_t *out) {
    if (s_wire.read_reg == nullptr || out == nullptr) {
        return false;
    }
    return s_wire.read_reg(reg, out, PU_SWIO_WIRE_TIMEOUT_US);
}

bool wire_write(uint8_t reg, uint32_t val) {
    if (s_wire.write_reg == nullptr) {
        return false;
    }
    return s_wire.write_reg(reg, val, PU_SWIO_WIRE_TIMEOUT_US);
}

/*
 * Review Focus #2: Strict QingKe V2 Table 3-5 DMSTATUS disambiguation.
 * Rejects 0xFFFFFFFF (floating high), 0x00000000 (shorted low), non-zero
 * reserved bits, wrong DM version, unauthenticated/read-protected state,
 * and contradictory or dual-zero halt/run combinations.
 */
DmStateClass classify_dmstatus(uint32_t dmstatus) {
    if (dmstatus == 0xFFFFFFFFu || dmstatus == 0x00000000u) {
        return DmStateClass::kInvalidNoReply;
    }
    if ((dmstatus & kDmStatReservedMask) != 0u) {
        return DmStateClass::kInvalidNoReply;
    }
    if ((dmstatus & kDmStatVersionMask) != kDmStatVersionQingKeV2) {
        return DmStateClass::kInvalidNoReply;
    }
    if ((dmstatus & kDmStatAuthenticated) == 0u) {
        return DmStateClass::kInvalidNoReply;
    }
    if ((dmstatus & (kDmStatAnyNonexistent | kDmStatAllNonexistent |
                     kDmStatAnyUnavail | kDmStatAllUnavail)) != 0u) {
        return DmStateClass::kInvalidNoReply;
    }

    const bool any_halted  = (dmstatus & kDmStatAnyHalted) != 0u;
    const bool all_halted  = (dmstatus & kDmStatAllHalted) != 0u;
    const bool any_running = (dmstatus & kDmStatAnyRunning) != 0u;
    const bool all_running = (dmstatus & kDmStatAllRunning) != 0u;
    const bool any_resack  = (dmstatus & kDmStatAnyResumeAck) != 0u;
    const bool all_resack  = (dmstatus & kDmStatAllResumeAck) != 0u;
    const bool any_haverst = (dmstatus & kDmStatAnyHaveReset) != 0u;
    const bool all_haverst = (dmstatus & kDmStatAllHaveReset) != 0u;

    if (any_halted != all_halted || any_running != all_running ||
        any_resack != all_resack || any_haverst != all_haverst) {
        return DmStateClass::kInvalidNoReply;
    }

    if (any_running && all_running && !any_halted && !all_halted) {
        return DmStateClass::kRunning;
    }
    if (any_halted && all_halted && !any_running && !all_running) {
        return DmStateClass::kHalted;
    }
    return DmStateClass::kInvalidNoReply;
}

bool enable_swio_link_and_dm() {
    const uint32_t max_tries = (s_wire.bus_reset != nullptr) ? 4u : 1u;
    for (uint32_t attempt = 0u; attempt < max_tries; ++attempt) {
        if (attempt > 0u && s_wire.bus_reset != nullptr) {
            s_wire.bus_reset();
        }
        /* Write SHDWCFGR + CFGR + DMCONTROL twice so SDI wake-up edge never drops OUTEN=1 */
        if (!wire_write(kWchDmShdwCfgr, kWchCfgrKey | kWchCfgrOutEnBit) ||
            !wire_write(kWchDmCfgr, kWchCfgrKey | kWchCfgrOutEnBit) ||
            !wire_write(kWchDmShdwCfgr, kWchCfgrKey | kWchCfgrOutEnBit) ||
            !wire_write(kWchDmCfgr, kWchCfgrKey | kWchCfgrOutEnBit) ||
            !wire_write(kDmControl, kDmCtrlDmActive) ||
            !wire_write(kDmControl, kDmCtrlDmActive)) {
            return false;
        }
        if (max_tries == 1u) {
            return true;
        }
        uint32_t cfgr = 0xFFFFFFFFu;
        if (!wire_read(kWchDmCfgr, &cfgr)) {
            return false;
        }
        if (cfgr != 0xFFFFFFFFu && (cfgr & kWchCfgrOutEnBit) != 0u) {
            return true;
        }
    }
    return true;
}

/*
 * Verified power-on interface reset:
 *   1. Stage SHDWCFGR = 0x5AA50000 (OUTEN = 0) while CFGR.OUTEN is still 1.
 *   2. Read back CFGR and SHDWCFGR; confirm CFGR.OUTEN == 1 and SHDWCFGR.OUTEN == 0.
 *   3. Clear DMCONTROL.DMACTIVE = 0.
 *   4. Latch WCH_DM_CFGR = 0x5AA50400 so OUTEN returns to power-on disabled state.
 */
bool reset_interface_to_power_on() {
    if (!wire_write(kWchDmShdwCfgr, kWchCfgrKey)) {
        return false;
    }
    uint32_t cfgr_val = 0xFFFFFFFFu;
    uint32_t shdw_val = 0xFFFFFFFFu;
    if (!wire_read(kWchDmCfgr, &cfgr_val) || !wire_read(kWchDmShdwCfgr, &shdw_val)) {
        return false;
    }
    const bool verified =
        (cfgr_val != 0xFFFFFFFFu) &&
        (shdw_val != 0xFFFFFFFFu) &&
        ((cfgr_val & kWchCfgrOutEnBit) != 0u) &&
        ((shdw_val & kWchCfgrOutEnBit) == 0u);

    (void)wire_write(kDmControl, 0x00000000u);
    (void)wire_write(kWchDmCfgr, kWchCfgrKey | kWchCfgrOutEnBit);
    return verified;
}

bool wait_abstract_ready() {
    for (uint32_t attempt = 0u; attempt < 64u; ++attempt) {
        uint32_t abscs = 0xFFFFFFFFu;
        if (!wire_read(kDmAbstractCs, &abscs) || abscs == 0xFFFFFFFFu || abscs == 0x00000000u) {
            return false;
        }
        if ((abscs & kAbsCsBusy) == 0u) {
            if ((abscs & kAbsCsCmdErrMask) != 0u) {
                (void)wire_write(kDmAbstractCs, kAbsCsCmdErrMask);
                return false;
            }
            return true;
        }
    }
    return false;
}

bool dm_read_reg(uint16_t regno, uint32_t *out_val) {
    const uint32_t cmd = (2u << 20) | (1u << 17) | static_cast<uint32_t>(regno);
    if (!wire_write(kDmCommand, cmd) || !wait_abstract_ready()) {
        return false;
    }
    return wire_read(kDmData0, out_val);
}

bool dm_write_reg(uint16_t regno, uint32_t val) {
    if (!wire_write(kDmData0, val)) {
        return false;
    }
    const uint32_t cmd = (2u << 20) | (1u << 17) | (1u << 16) | static_cast<uint32_t>(regno);
    return wire_write(kDmCommand, cmd) && wait_abstract_ready();
}

bool s_progbuf_cache_valid = false;
uint32_t s_progbuf0_cached = 0u;
uint32_t s_progbuf1_cached = 0u;

void invalidate_progbuf_cache() {
    s_progbuf_cache_valid = false;
    s_progbuf0_cached = 0u;
    s_progbuf1_cached = 0u;
}

bool set_progbuf_pair(uint32_t insn0, uint32_t insn1) {
    if (!s_progbuf_cache_valid || s_progbuf0_cached != insn0) {
        if (!wire_write(kDmProgBuf0, insn0)) {
            invalidate_progbuf_cache();
            return false;
        }
        s_progbuf0_cached = insn0;
    }
    if (!s_progbuf_cache_valid || s_progbuf1_cached != insn1) {
        if (!wire_write(kDmProgBuf1, insn1)) {
            invalidate_progbuf_cache();
            return false;
        }
        s_progbuf1_cached = insn1;
    }
    s_progbuf_cache_valid = true;
    return true;
}

struct ScopedScratchRegs {
    bool valid = false;
    uint32_t saved_x8 = 0u;
    uint32_t saved_x9 = 0u;

    bool save() {
        invalidate_progbuf_cache();
        if (!dm_read_reg(kRegX8, &saved_x8) || !dm_read_reg(kRegX9, &saved_x9)) {
            return false;
        }
        valid = true;
        return true;
    }

    bool restore() {
        if (!valid) {
            return true;
        }
        valid = false;
        const bool ok8 = dm_write_reg(kRegX8, saved_x8);
        const bool ok9 = dm_write_reg(kRegX9, saved_x9);
        (void)wire_write(kDmProgBuf0, kInsnEbreak);
        (void)wire_write(kDmProgBuf1, kInsnEbreak);
        (void)wire_write(kDmData0, 0u);
        invalidate_progbuf_cache();
        return ok8 && ok9;
    }

    ~ScopedScratchRegs() {
        (void)restore();
    }
};

bool exec_load_with_saved_scratch(uint32_t load_insn, uint32_t addr, uint32_t *out_val) {
    if (!set_progbuf_pair(load_insn, kInsnEbreak) ||
        !wire_write(kDmData0, addr)) {
        return false;
    }
    const uint32_t exec_cmd = (2u << 20) | (1u << 18) | (1u << 17) | (1u << 16) | kRegX8;
    if (!wire_write(kDmCommand, exec_cmd) || !wait_abstract_ready()) {
        return false;
    }
    return dm_read_reg(kRegX9, out_val);
}

bool exec_store_with_saved_scratch(uint32_t store_insn, uint32_t addr, uint32_t val) {
    if (!dm_write_reg(kRegX9, val) ||
        !set_progbuf_pair(store_insn, kInsnEbreak) ||
        !wire_write(kDmData0, addr)) {
        return false;
    }
    const uint32_t exec_cmd = (2u << 20) | (1u << 18) | (1u << 17) | (1u << 16) | kRegX8;
    return wire_write(kDmCommand, exec_cmd) && wait_abstract_ready();
}

bool halt_target_bounded(uint64_t start_us) {
    const uint64_t t0 = wire_now_us(start_us);
    uint32_t dmstatus = 0u;
    if (!wire_read(kDmStatus, &dmstatus)) {
        return false;
    }
    DmStateClass cls = classify_dmstatus(dmstatus);
    if (cls == DmStateClass::kInvalidNoReply) {
        return false;
    }
    if (cls == DmStateClass::kHalted) {
        return true;
    }
    if (!wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlDmActive)) {
        return false;
    }
    for (uint32_t i = 0u; i < 64u; ++i) {
        if (!wire_read(kDmStatus, &dmstatus)) {
            return false;
        }
        if (classify_dmstatus(dmstatus) == DmStateClass::kHalted) {
            (void)wire_write(kDmControl, kDmCtrlDmActive);
            return true;
        }
        const uint64_t now = wire_now_us(t0 + static_cast<uint64_t>(i + 1u) * 100u);
        if ((now >= t0) && ((now - t0) >= PU_SWIO_CHECK_TIMEOUT_US)) {
            break;
        }
    }
    return false;
}

bool reset_cpu_and_halt(uint64_t start_us) {
    invalidate_progbuf_cache();
    if (!wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlNdmReset | kDmCtrlDmActive) ||
        !wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlDmActive) ||
        !halt_target_bounded(start_us) ||
        !wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlAckHaveReset | kDmCtrlDmActive) ||
        !wire_write(kDmControl, kDmCtrlDmActive)) {
        return false;
    }
    return true;
}

bool resume_target_and_verify(uint64_t start_us) {
    const uint64_t t0 = wire_now_us(start_us);
    uint32_t pre_status = 0u;
    if (wire_read(kDmStatus, &pre_status) && (pre_status & kDmStatAnyHaveReset) != 0u) {
        (void)wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlAckHaveReset | kDmCtrlDmActive);
        (void)wire_write(kDmControl, kDmCtrlDmActive);
    }
    if (!wire_write(kDmControl, kDmCtrlResumeReq | kDmCtrlDmActive)) {
        return false;
    }
    bool saw_running = false;
    for (uint32_t i = 0u; i < 64u; ++i) {
        uint32_t dmstatus = 0u;
        if (!wire_read(kDmStatus, &dmstatus)) {
            return false;
        }
        /* Real QingKe V2A silicon transitions DMSTATUS directly to 0x004C0C82
         * (ALLRUNNING=1, ANYRUNNING=1) without asserting ALLRESUMEACK. */
        if (classify_dmstatus(dmstatus) == DmStateClass::kRunning) {
            saw_running = true;
            break;
        }
        const uint64_t now = wire_now_us(t0 + static_cast<uint64_t>(i + 1u) * 100u);
        if ((now >= t0) && ((now - t0) >= PU_SWIO_CHECK_TIMEOUT_US)) {
            break;
        }
    }
    if (!saw_running) {
        return false;
    }
    if (!wire_write(kDmControl, kDmCtrlDmActive)) {
        return false;
    }
    uint32_t post_status = 0u;
    if (!wire_read(kDmStatus, &post_status)) {
        return false;
    }
    return classify_dmstatus(post_status) == DmStateClass::kRunning;
}

bool resume_target_for_gdb(uint64_t start_us, DmStateClass *out_state) {
    if (out_state == nullptr) {
        return false;
    }
    const uint64_t t0 = wire_now_us(start_us);
    uint32_t pre_status = 0u;
    if (wire_read(kDmStatus, &pre_status) && (pre_status & kDmStatAnyHaveReset) != 0u) {
        (void)wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlAckHaveReset | kDmCtrlDmActive);
        (void)wire_write(kDmControl, kDmCtrlDmActive);
    }
    uint32_t dcsr = 0u;
    if (dm_read_reg(kRegDcsr, &dcsr)) {
        const uint32_t want_dcsr = (dcsr & ~kDcsrStepBit) | kDcsrEbreakM;
        if (want_dcsr != dcsr && !dm_write_reg(kRegDcsr, want_dcsr)) {
            return false;
        }
    }
    if (!wire_write(kDmControl, kDmCtrlResumeReq | kDmCtrlDmActive)) {
        return false;
    }
    DmStateClass acked_state = DmStateClass::kInvalidNoReply;
    for (uint32_t i = 0u; i < 64u; ++i) {
        uint32_t dmstatus = 0u;
        if (!wire_read(kDmStatus, &dmstatus)) {
            return false;
        }
        const DmStateClass cls = classify_dmstatus(dmstatus);
        if (cls == DmStateClass::kRunning) {
            acked_state = DmStateClass::kRunning;
            break;
        }
        if (cls == DmStateClass::kHalted) {
            if ((dmstatus & kDmStatAllResumeAck) != 0u) {
                acked_state = DmStateClass::kHalted;
                break;
            }
            uint32_t post_dcsr = 0u;
            if (dm_read_reg(kRegDcsr, &post_dcsr) &&
                ((post_dcsr >> 6u) & 0x7u) == 1u /* CSR_DCSR_CAUSE_EBREAK */) {
                acked_state = DmStateClass::kHalted;
                break;
            }
        }
        const uint64_t now = wire_now_us(t0 + static_cast<uint64_t>(i + 1u) * 100u);
        if ((now >= t0) && ((now - t0) >= PU_SWIO_CHECK_TIMEOUT_US)) {
            break;
        }
    }
    if (acked_state == DmStateClass::kInvalidNoReply) {
        return false;
    }
    if (!wire_write(kDmControl, kDmCtrlDmActive)) {
        return false;
    }
    if (acked_state == DmStateClass::kHalted) {
        *out_state = DmStateClass::kHalted;
        return true;
    }
    uint32_t post_status = 0u;
    if (!wire_read(kDmStatus, &post_status)) {
        return false;
    }
    const DmStateClass post_cls = classify_dmstatus(post_status);
    if (post_cls != DmStateClass::kRunning && post_cls != DmStateClass::kHalted) {
        return false;
    }
    *out_state = post_cls;
    return true;
}

/* -------------------------------------------------------------------------- */
/* CH32V003 64-Byte Flash Page Erase/Program & Cross-Page SoftBreak           */
/* -------------------------------------------------------------------------- */

bool wait_flash_not_busy(uint32_t *out_statr) {
    for (uint32_t i = 0u; i < 256u; ++i) {
        uint32_t statr = 0u;
        if (!exec_load_with_saved_scratch(kInsnLwX9FromX8, kCh32FlashStatr, &statr)) {
            return false;
        }
        if ((statr & kFlashStatrBsy) == 0u) {
            if (out_statr != nullptr) {
                *out_statr = statr;
            }
            return (statr & kFlashStatrWrprterr) == 0u;
        }
    }
    return false;
}

bool unlock_ch32_fast_flash() {
    uint32_t ctlr = 0u;
    if (!exec_load_with_saved_scratch(kInsnLwX9FromX8, kCh32FlashCtlr, &ctlr)) {
        return false;
    }
    if ((ctlr & kFlashCtlrLock) != 0u) {
        if (!exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashKeyr, kCh32FlashKey1) ||
            !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashKeyr, kCh32FlashKey2)) {
            return false;
        }
    }
    if ((ctlr & (kFlashCtlrLock | kFlashCtlrFlock)) != 0u) {
        if (!exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashModeKeyr, kCh32FlashKey1) ||
            !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashModeKeyr, kCh32FlashKey2)) {
            return false;
        }
    }
    if (!exec_load_with_saved_scratch(kInsnLwX9FromX8, kCh32FlashCtlr, &ctlr)) {
        return false;
    }
    return (ctlr & (kFlashCtlrLock | kFlashCtlrFlock)) == 0u;
}

void lock_ch32_flash() {
    (void)exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashStatr,
                                        kFlashStatrEop | kFlashStatrWrprterr);
    (void)exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr,
                                        kFlashCtlrLock | kFlashCtlrFlock);
}

bool erase_flash_page_64_unlocked(uint32_t page_idx) {
    const uint32_t page_addr = PU_CH32_FLASH_BASE + page_idx * PU_CH32_FLASH_PAGE_SIZE;
    if (!exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashStatr,
                                       kFlashStatrEop | kFlashStatrWrprterr) ||
        !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr, kFlashCtlrFter) ||
        !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashAddr, page_addr) ||
        !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr,
                                       kFlashCtlrFter | kFlashCtlrStrt)) {
        return false;
    }
    if (!wait_flash_not_busy(nullptr)) {
        return false;
    }
    return exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashStatr, kFlashStatrEop) &&
           exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr, 0u);
}

bool read_flash_page_64_unlocked(uint32_t page_idx, uint8_t out_page[PU_CH32_FLASH_PAGE_SIZE]) {
    const uint32_t page_addr = PU_CH32_FLASH_BASE + page_idx * PU_CH32_FLASH_PAGE_SIZE;
    for (uint32_t w = 0u; w < PU_CH32_FLASH_PAGE_SIZE / 4u; ++w) {
        uint32_t word = 0u;
        if (!exec_load_with_saved_scratch(kInsnLwX9FromX8, page_addr + w * 4u, &word)) {
            return false;
        }
        out_page[w * 4u + 0u] = static_cast<uint8_t>(word & 0xFFu);
        out_page[w * 4u + 1u] = static_cast<uint8_t>((word >> 8) & 0xFFu);
        out_page[w * 4u + 2u] = static_cast<uint8_t>((word >> 16) & 0xFFu);
        out_page[w * 4u + 3u] = static_cast<uint8_t>((word >> 24) & 0xFFu);
    }
    return true;
}

bool rewrite_flash_page_64(uint32_t page_idx, const uint8_t data[PU_CH32_FLASH_PAGE_SIZE]) {
    ScopedScratchRegs scratch;
    if (!scratch.save()) {
        return false;
    }
    if (!unlock_ch32_fast_flash()) {
        return false;
    }
    if (!erase_flash_page_64_unlocked(page_idx)) {
        lock_ch32_flash();
        return false;
    }

    const uint32_t page_addr = PU_CH32_FLASH_BASE + page_idx * PU_CH32_FLASH_PAGE_SIZE;
    if (!exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr, kFlashCtlrFtpg) ||
        !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr,
                                       kFlashCtlrFtpg | kFlashCtlrBufRst) ||
        !wait_flash_not_busy(nullptr)) {
        lock_ch32_flash();
        return false;
    }

    for (uint32_t w = 0u; w < PU_CH32_FLASH_PAGE_SIZE / 4u; ++w) {
        const uint32_t word =
            static_cast<uint32_t>(data[w * 4u + 0u]) |
            (static_cast<uint32_t>(data[w * 4u + 1u]) << 8) |
            (static_cast<uint32_t>(data[w * 4u + 2u]) << 16) |
            (static_cast<uint32_t>(data[w * 4u + 3u]) << 24);
        if (!exec_store_with_saved_scratch(kInsnSwX9ToX8, page_addr + w * 4u, word) ||
            !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr,
                                           kFlashCtlrFtpg | kFlashCtlrBufLoad) ||
            !wait_flash_not_busy(nullptr)) {
            lock_ch32_flash();
            return false;
        }
    }

    if (!exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashAddr, page_addr) ||
        !exec_store_with_saved_scratch(kInsnSwX9ToX8, kCh32FlashCtlr,
                                       kFlashCtlrFtpg | kFlashCtlrStrt) ||
        !wait_flash_not_busy(nullptr)) {
        lock_ch32_flash();
        return false;
    }

    uint8_t verify_buf[PU_CH32_FLASH_PAGE_SIZE] = {};
    const bool read_ok = read_flash_page_64_unlocked(page_idx, verify_buf);
    lock_ch32_flash();
    if (!read_ok || std::memcmp(verify_buf, data, PU_CH32_FLASH_PAGE_SIZE) != 0) {
        return false;
    }
    return scratch.restore();
}

void build_patched_page_image(uint32_t page_idx,
                              uint8_t out_img[PU_CH32_FLASH_PAGE_SIZE],
                              int extra_offset,
                              uint8_t extra_size,
                              int skip_slot) {
    std::memcpy(out_img, s_clean_pages[page_idx], PU_CH32_FLASH_PAGE_SIZE);
    const uint32_t page_start = page_idx * PU_CH32_FLASH_PAGE_SIZE;
    const uint32_t page_end   = page_start + PU_CH32_FLASH_PAGE_SIZE;

    auto overlay_trap = [&](uint32_t bp_off, uint8_t bp_sz) {
        const uint8_t trap2[2] = {0x02u, 0x90u};
        const uint8_t trap4[4] = {0x73u, 0x00u, 0x10u, 0x00u};
        const uint8_t *trap = (bp_sz == 2u) ? trap2 : trap4;
        for (uint8_t b = 0u; b < bp_sz; ++b) {
            const uint32_t pos = bp_off + b;
            if (pos >= page_start && pos < page_end) {
                out_img[pos - page_start] = trap[b];
            }
        }
    };

    for (size_t i = 0u; i < kMaxBreakpoints; ++i) {
        if (!s_breakpoints[i].active || static_cast<int>(i) == skip_slot) {
            continue;
        }
        overlay_trap(s_breakpoints[i].offset, s_breakpoints[i].size);
    }
    if (extra_offset >= 0 && (extra_size == 2u || extra_size == 4u)) {
        overlay_trap(static_cast<uint32_t>(extra_offset), extra_size);
    }
}

bool normalize_flash_range(uint32_t addr, uint32_t size, uint32_t *out_offset) {
    if (size > 0u && (size - 1u) > UINT32_MAX - addr) {
        return false;
    }
    if (addr <= PU_CH32_FLASH_SIZE && size <= PU_CH32_FLASH_SIZE - addr) {
        *out_offset = addr;
        return true;
    }
    if (addr >= PU_CH32_FLASH_BASE && addr <= PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE &&
        size <= (PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE) - addr) {
        *out_offset = addr - PU_CH32_FLASH_BASE;
        return true;
    }
    return false;
}

bool set_soft_breakpoint(uint32_t addr, uint32_t kind) {
    uint32_t offset = 0u;
    if ((kind != 2u && kind != 4u) || (addr & 1u) != 0u ||
        !normalize_flash_range(addr, kind, &offset)) {
        return false;
    }
    int free_slot = -1;
    for (size_t i = 0u; i < kMaxBreakpoints; ++i) {
        if (!s_breakpoints[i].active) {
            if (free_slot < 0) {
                free_slot = static_cast<int>(i);
            }
            continue;
        }
        if (s_breakpoints[i].offset == offset && s_breakpoints[i].size == kind) {
            return true; /* Idempotent duplicate insert */
        }
        if (offset < s_breakpoints[i].offset + s_breakpoints[i].size &&
            s_breakpoints[i].offset < offset + kind) {
            return false; /* Overlapping software breakpoint rejected */
        }
    }
    if (free_slot < 0) {
        return false;
    }

    const uint32_t p0 = offset / PU_CH32_FLASH_PAGE_SIZE;
    const uint32_t p1 = (offset + kind - 1u) / PU_CH32_FLASH_PAGE_SIZE;

    /* Snapshot clean original flash pages before any mutation */
    {
        ScopedScratchRegs scratch;
        if (!scratch.save()) {
            return false;
        }
        for (uint32_t p = p0; p <= p1; ++p) {
            if (s_page_bp_refcount[p] == 0u && !s_page_dirty_restore[p]) {
                if (!read_flash_page_64_unlocked(p, s_clean_pages[p])) {
                    return false;
                }
            }
        }
        if (!scratch.restore()) {
            return false;
        }
    }

    for (uint32_t p = p0; p <= p1; ++p) {
        uint8_t desired[PU_CH32_FLASH_PAGE_SIZE] = {};
        build_patched_page_image(p, desired, static_cast<int>(offset),
                                 static_cast<uint8_t>(kind), -1);
        s_page_dirty_restore[p] = true;
        if (!rewrite_flash_page_64(p, desired)) {
            /* Transactional cross-page rollback to prior breakpoint state */
            for (uint32_t rp = p0; rp <= p; ++rp) {
                uint8_t prior[PU_CH32_FLASH_PAGE_SIZE] = {};
                build_patched_page_image(rp, prior, -1, 0u, -1);
                if (rewrite_flash_page_64(rp, prior) && s_page_bp_refcount[rp] == 0u) {
                    s_page_dirty_restore[rp] = false;
                }
            }
            return false;
        }
    }

    s_breakpoints[free_slot].active = true;
    s_breakpoints[free_slot].offset = offset;
    s_breakpoints[free_slot].size = static_cast<uint8_t>(kind);
    for (uint32_t p = p0; p <= p1; ++p) {
        ++s_page_bp_refcount[p];
    }
    return true;
}

bool clear_soft_breakpoint(uint32_t addr, uint32_t kind) {
    uint32_t offset = 0u;
    if ((kind != 2u && kind != 4u) || (addr & 1u) != 0u ||
        !normalize_flash_range(addr, kind, &offset)) {
        return false;
    }

    for (size_t i = 0u; i < kMaxBreakpoints; ++i) {
        if (!s_breakpoints[i].active || s_breakpoints[i].offset != offset) {
            continue;
        }
        if (s_breakpoints[i].size != kind) {
            return false;
        }
        const uint32_t p0 = offset / PU_CH32_FLASH_PAGE_SIZE;
        const uint32_t p1 = (offset + kind - 1u) / PU_CH32_FLASH_PAGE_SIZE;
        for (uint32_t p = p0; p <= p1; ++p) {
            uint8_t desired[PU_CH32_FLASH_PAGE_SIZE] = {};
            build_patched_page_image(p, desired, -1, 0u, static_cast<int>(i));
            s_page_dirty_restore[p] = true;
            if (!rewrite_flash_page_64(p, desired)) {
                for (uint32_t rp = p0; rp <= p; ++rp) {
                    uint8_t rollback_img[PU_CH32_FLASH_PAGE_SIZE] = {};
                    build_patched_page_image(rp, rollback_img, -1, 0u, -1);
                    (void)rewrite_flash_page_64(rp, rollback_img);
                }
                return false;
            }
        }
        s_breakpoints[i].active = false;
        s_breakpoints[i].size = 0u;
        for (uint32_t p = p0; p <= p1; ++p) {
            if (s_page_bp_refcount[p] > 0u) {
                --s_page_bp_refcount[p];
            }
            if (s_page_bp_refcount[p] == 0u) {
                s_page_dirty_restore[p] = false;
            }
        }
        return true;
    }
    return true; /* Duplicate removals are idempotent */
}

bool has_unrestored_soft_breakpoints() {
    for (size_t i = 0u; i < kMaxBreakpoints; ++i) {
        if (s_breakpoints[i].active) {
            return true;
        }
    }
    for (size_t p = 0u; p < PU_CH32_FLASH_PAGE_COUNT; ++p) {
        if (s_page_bp_refcount[p] != 0u || s_page_dirty_restore[p]) {
            return true;
        }
    }
    return false;
}

bool restore_all_soft_breakpoints(uint64_t now_us) {
    if (!has_unrestored_soft_breakpoints()) {
        return true;
    }
    if (!halt_target_bounded(now_us)) {
        return false;
    }
    bool all_ok = true;
    for (uint32_t p = 0u; p < PU_CH32_FLASH_PAGE_COUNT; ++p) {
        if (s_page_bp_refcount[p] != 0u || s_page_dirty_restore[p]) {
            s_page_dirty_restore[p] = true;
            if (rewrite_flash_page_64(p, s_clean_pages[p])) {
                s_page_bp_refcount[p] = 0u;
                s_page_dirty_restore[p] = false;
            } else {
                all_ok = false;
            }
        }
    }
    for (size_t i = 0u; i < kMaxBreakpoints; ++i) {
        if (!s_breakpoints[i].active) {
            continue;
        }
        const uint32_t p0 = s_breakpoints[i].offset / PU_CH32_FLASH_PAGE_SIZE;
        const uint32_t p1 = (s_breakpoints[i].offset + s_breakpoints[i].size - 1u) /
                            PU_CH32_FLASH_PAGE_SIZE;
        if (!s_page_dirty_restore[p0] && !s_page_dirty_restore[p1]) {
            s_breakpoints[i].active = false;
            s_breakpoints[i].size = 0u;
        }
    }
    return all_ok;
}

void clear_vflash_cache() {
    s_vflash_dirty = false;
    s_vflash_page_idx = 0u;
    s_vflash_mask = 0u;
    std::memset(s_vflash_buf, 0xFF, sizeof(s_vflash_buf));
}

bool flush_vflash_cache() {
    if (!s_vflash_dirty) {
        return true;
    }
    const uint32_t page_idx = s_vflash_page_idx;
    const bool had_bp_snapshot =
        (s_page_bp_refcount[page_idx] != 0u || s_page_dirty_restore[page_idx]);

    uint8_t clean_merged[PU_CH32_FLASH_PAGE_SIZE] = {};
    if (s_vflash_mask != UINT64_MAX) {
        if (had_bp_snapshot) {
            std::memcpy(clean_merged, s_clean_pages[page_idx], PU_CH32_FLASH_PAGE_SIZE);
        } else {
            ScopedScratchRegs scratch;
            if (!scratch.save() || !read_flash_page_64_unlocked(page_idx, clean_merged) ||
                !scratch.restore()) {
                return false;
            }
        }
        for (uint32_t i = 0u; i < PU_CH32_FLASH_PAGE_SIZE; ++i) {
            if ((s_vflash_mask & (UINT64_C(1) << i)) != 0u) {
                clean_merged[i] = s_vflash_buf[i];
            }
        }
    } else {
        std::memcpy(clean_merged, s_vflash_buf, PU_CH32_FLASH_PAGE_SIZE);
    }

    uint8_t prev_clean[PU_CH32_FLASH_PAGE_SIZE] = {};
    std::memcpy(prev_clean, s_clean_pages[page_idx], PU_CH32_FLASH_PAGE_SIZE);
    std::memcpy(s_clean_pages[page_idx], clean_merged, PU_CH32_FLASH_PAGE_SIZE);

    uint8_t hw_img[PU_CH32_FLASH_PAGE_SIZE] = {};
    build_patched_page_image(page_idx, hw_img, -1, 0u, -1);
    if (!rewrite_flash_page_64(page_idx, hw_img)) {
        std::memcpy(s_clean_pages[page_idx], prev_clean, PU_CH32_FLASH_PAGE_SIZE);
        return false;
    }
    if (s_page_bp_refcount[page_idx] == 0u) {
        s_page_dirty_restore[page_idx] = false;
    }
    clear_vflash_cache();
    return true;
}

bool stage_vflash_byte(uint32_t flash_offset, uint8_t byte_val) {
    const uint32_t page_idx = flash_offset / PU_CH32_FLASH_PAGE_SIZE;
    const uint32_t in_page  = flash_offset % PU_CH32_FLASH_PAGE_SIZE;
    if (s_vflash_dirty && s_vflash_page_idx != page_idx) {
        if (!flush_vflash_cache()) {
            return false;
        }
    }
    if (!s_vflash_dirty) {
        s_vflash_dirty = true;
        s_vflash_page_idx = page_idx;
        s_vflash_mask = 0u;
        std::memset(s_vflash_buf, 0xFF, sizeof(s_vflash_buf));
    }
    s_vflash_buf[in_page] = byte_val;
    s_vflash_mask |= (UINT64_C(1) << in_page);
    return true;
}

/* -------------------------------------------------------------------------- */
/* GDB RSP Parser, Hex Helpers & Command Dispatch                             */
/* -------------------------------------------------------------------------- */

int from_hex_char(uint8_t c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}

char to_hex_char(uint8_t v) {
    static constexpr char kHex[] = "0123456789abcdef";
    return kHex[v & 0x0Fu];
}

uint32_t bswap32_u(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}

size_t tx_free_space() {
    return kMaxGdbTxLen - s_tx_count;
}

void tx_enqueue_char(char c) {
    if (s_tx_count < kMaxGdbTxLen) {
        s_tx_ring[s_tx_tail] = c;
        s_tx_tail = (s_tx_tail + 1u) % kMaxGdbTxLen;
        ++s_tx_count;
    }
}

size_t tx_drain_to(uint8_t *out, size_t max_bytes) {
    if (out == nullptr || max_bytes == 0u) {
        return 0u;
    }
    size_t n = 0u;
    while (s_tx_count > 0u && n < max_bytes) {
        out[n++] = static_cast<uint8_t>(s_tx_ring[s_tx_head]);
        s_tx_head = (s_tx_head + 1u) % kMaxGdbTxLen;
        --s_tx_count;
    }
    return n;
}

bool rx_ring_push(uint8_t b) {
    if (s_rx_ring_count >= kMaxGdbRxRingCap) {
        return false;
    }
    s_rx_ring[s_rx_ring_tail] = b;
    s_rx_ring_tail = (s_rx_ring_tail + 1u) % kMaxGdbRxRingCap;
    ++s_rx_ring_count;
    return true;
}

uint8_t rx_ring_pop() {
    const uint8_t b = s_rx_ring[s_rx_ring_head];
    s_rx_ring_head = (s_rx_ring_head + 1u) % kMaxGdbRxRingCap;
    --s_rx_ring_count;
    return b;
}

void tx_enqueue_framed_packet(const char *payload, size_t payload_len) {
    size_t pos = 0u;
    auto push_pkt = [&](char c) {
        if (pos < sizeof(s_last_framed_pkt)) {
            s_last_framed_pkt[pos++] = c;
        }
        tx_enqueue_char(c);
    };

    push_pkt('$');
    uint8_t csum = 0u;
    for (size_t i = 0u; i < payload_len; ++i) {
        const uint8_t b = static_cast<uint8_t>(payload[i]);
        if (b == '$' || b == '#' || b == '}' || b == '*') {
            push_pkt('}');
            csum = static_cast<uint8_t>(csum + static_cast<uint8_t>('}'));
            const uint8_t esc = static_cast<uint8_t>(b ^ 0x20u);
            push_pkt(static_cast<char>(esc));
            csum = static_cast<uint8_t>(csum + esc);
        } else {
            push_pkt(static_cast<char>(b));
            csum = static_cast<uint8_t>(csum + b);
        }
    }
    push_pkt('#');
    push_pkt(to_hex_char(static_cast<uint8_t>(csum >> 4)));
    push_pkt(to_hex_char(static_cast<uint8_t>(csum & 0x0Fu)));
    s_last_framed_len = pos;
}

void tx_enqueue_cstr_packet(const char *str) {
    tx_enqueue_framed_packet(str, std::strlen(str));
}

struct PktCursor {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    bool err;

    bool at_end() const { return pos == len; }

    bool match_exact(const char *exact) {
        const size_t n = std::strlen(exact);
        if (len - pos != n) {
            return false;
        }
        if (std::memcmp(buf + pos, exact, n) != 0) {
            return false;
        }
        pos += n;
        return true;
    }

    bool match_prefix(const char *prefix) {
        const size_t n = std::strlen(prefix);
        if (len - pos < n) {
            return false;
        }
        if (std::memcmp(buf + pos, prefix, n) != 0) {
            return false;
        }
        pos += n;
        return true;
    }

    bool take_char(char expected) {
        if (pos >= len || buf[pos] != static_cast<uint8_t>(expected)) {
            err = true;
            return false;
        }
        ++pos;
        return true;
    }

    uint32_t take_hex_u32(size_t max_digits = 8u) {
        if (pos >= len || from_hex_char(buf[pos]) < 0) {
            err = true;
            return 0u;
        }
        uint64_t acc = 0u;
        size_t digits = 0u;
        while (pos < len && digits < max_digits) {
            int d = from_hex_char(buf[pos]);
            if (d < 0) {
                break;
            }
            acc = (acc << 4) | static_cast<uint64_t>(d);
            ++pos;
            ++digits;
        }
        if (pos < len && from_hex_char(buf[pos]) >= 0) {
            err = true; /* Overflow beyond 32-bit hex literal */
            return 0u;
        }
        return static_cast<uint32_t>(acc);
    }
};

bool ensure_gdb_swio_session(uint64_t now_us) {
    bool lost_prior_lease = false;
    if (s_gdb_session_open) {
        pu_status_t st = pu_engine_renew_deadline(PU_ENGINE_SWIO, now_us, PU_SWIO_SESSION_TIMEOUT_US);
        if (st == PU_OK) {
            s_last_session_err = PU_OK;
            return true;
        }
        /* Session ownership lost due to timeout, USB reset, or hardware fault:
         * close local PIO1 bus and preserve s_breakpoints/s_clean_pages so any
         * ebreak patches still in flash can be restored on reconnect/disconnect. */
        wire_bus_close();
        s_gdb_session_open = false;
        s_gdb_running = false;
        s_target_attached_halted = false;
        clear_vflash_cache();
        lost_prior_lease = true;
        if (st == PU_ERR_FAULT_LATCHED) {
            s_last_session_err = st;
            return false;
        }
        /* If prior lease expired back to IDLE, fall through to re-acquire cleanly */
    }

    pu_status_t acq = pu_engine_acquire(PU_ENGINE_SWIO, now_us, PU_SWIO_SESSION_TIMEOUT_US);
    if (acq != PU_OK) {
        s_last_session_err = acq;
        return false;
    }
    wire_bus_open();
    if (!enable_swio_link_and_dm()) {
        wire_bus_close();
        (void)pu_engine_release(PU_ENGINE_SWIO);
        s_last_session_err = PU_ERR_TIMEOUT;
        return false;
    }
    s_gdb_session_open = true;
    s_gdb_running = false;
    s_target_attached_halted = false;
    s_last_session_err = PU_OK;
    if (lost_prior_lease || has_unrestored_soft_breakpoints()) {
        (void)restore_all_soft_breakpoints(now_us);
    }
    return true;
}

bool ensure_gdb_target_halted(uint64_t now_us) {
    if (!halt_target_bounded(now_us)) {
        s_target_attached_halted = false;
        return false;
    }
    uint32_t dmstatus = 0u;
    if (!wire_read(kDmStatus, &dmstatus) || classify_dmstatus(dmstatus) != DmStateClass::kHalted) {
        s_target_attached_halted = false;
        return false;
    }
    if ((dmstatus & kDmStatAnyHaveReset) != 0u) {
        (void)wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlAckHaveReset | kDmCtrlDmActive);
        (void)wire_write(kDmControl, kDmCtrlDmActive);
    }
    s_gdb_running = false;
    if (!s_target_attached_halted) {
        uint32_t dcsr = 0u;
        if (dm_read_reg(kRegDcsr, &dcsr)) {
            (void)dm_write_reg(kRegDcsr, dcsr | kDcsrEbreakM);
        }
        s_target_attached_halted = true;
    }
    return true;
}

bool is_readable_ch32_addr(uint32_t addr, uint32_t len) {
    if (len == 0u) {
        return true;
    }
    if ((len - 1u) > UINT32_MAX - addr) {
        return false;
    }
    const uint32_t end = addr + (len - 1u);
    if (end < PU_CH32_FLASH_SIZE) {
        return true;
    }
    if (addr >= PU_CH32_FLASH_BASE && end < PU_CH32_FLASH_BASE + PU_CH32_FLASH_SIZE) {
        return true;
    }
    if (addr >= 0x1FFFF000u && end <= 0x1FFFFFFFu) {
        return true;
    }
    if (addr >= PU_CH32_SRAM_BASE && end < PU_CH32_SRAM_BASE + PU_CH32_SRAM_SIZE) {
        return true;
    }
    if (addr >= 0x40000000u && end < 0x40028000u) {
        return true;
    }
    if (addr >= 0xE0000000u && end < 0xE0100000u) {
        return true;
    }
    return false;
}

bool is_writable_ch32_ram_or_mmio(uint32_t addr, uint32_t len) {
    if (len == 0u) {
        return true;
    }
    if ((len - 1u) > UINT32_MAX - addr) {
        return false;
    }
    const uint32_t end = addr + (len - 1u);
    if (addr >= PU_CH32_SRAM_BASE && end < PU_CH32_SRAM_BASE + PU_CH32_SRAM_SIZE) {
        return true;
    }
    if (addr >= 0x40000000u && end < 0x40028000u) {
        return true;
    }
    if (addr >= 0xE0000000u && end < 0xE0100000u) {
        return true;
    }
    return false;
}

uint8_t overlay_clean_flash_byte(uint32_t addr, uint8_t raw_byte) {
    uint32_t flash_off = 0u;
    if (normalize_flash_range(addr, 1u, &flash_off)) {
        const uint32_t page_idx = flash_off / PU_CH32_FLASH_PAGE_SIZE;
        if (s_page_bp_refcount[page_idx] != 0u || s_page_dirty_restore[page_idx]) {
            return s_clean_pages[page_idx][flash_off % PU_CH32_FLASH_PAGE_SIZE];
        }
    }
    return raw_byte;
}

bool read_target_clean_range(uint32_t addr, uint8_t *out_bytes, uint32_t len) {
    uint32_t i = 0u;
    while (i < len) {
        const uint32_t cur_addr = addr + i;
        const uint32_t rem = len - i;
        if ((cur_addr & 3u) == 0u && rem >= 4u) {
            uint32_t word = 0u;
            if (!exec_load_with_saved_scratch(kInsnLwX9FromX8, cur_addr, &word)) {
                return false;
            }
            for (uint32_t b = 0u; b < 4u; ++b) {
                const uint8_t raw = static_cast<uint8_t>((word >> (b * 8u)) & 0xFFu);
                out_bytes[i + b] = overlay_clean_flash_byte(cur_addr + b, raw);
            }
            i += 4u;
        } else if ((cur_addr & 1u) == 0u && rem >= 2u) {
            uint32_t hw = 0u;
            if (!exec_load_with_saved_scratch(kInsnLhuX9FromX8, cur_addr, &hw)) {
                return false;
            }
            for (uint32_t b = 0u; b < 2u; ++b) {
                const uint8_t raw = static_cast<uint8_t>((hw >> (b * 8u)) & 0xFFu);
                out_bytes[i + b] = overlay_clean_flash_byte(cur_addr + b, raw);
            }
            i += 2u;
        } else {
            uint32_t byte_val = 0u;
            if (!exec_load_with_saved_scratch(kInsnLbuX9FromX8, cur_addr, &byte_val)) {
                return false;
            }
            out_bytes[i] = overlay_clean_flash_byte(cur_addr, static_cast<uint8_t>(byte_val & 0xFFu));
            i += 1u;
        }
    }
    return true;
}

bool write_target_range(uint32_t addr, const uint8_t *bytes, uint32_t len) {
    uint32_t i = 0u;
    while (i < len) {
        const uint32_t cur_addr = addr + i;
        const uint32_t rem = len - i;
        if ((cur_addr & 3u) == 0u && rem >= 4u) {
            const uint32_t word =
                static_cast<uint32_t>(bytes[i + 0u]) |
                (static_cast<uint32_t>(bytes[i + 1u]) << 8) |
                (static_cast<uint32_t>(bytes[i + 2u]) << 16) |
                (static_cast<uint32_t>(bytes[i + 3u]) << 24);
            if (!exec_store_with_saved_scratch(kInsnSwX9ToX8, cur_addr, word)) {
                return false;
            }
            i += 4u;
        } else if ((cur_addr & 1u) == 0u && rem >= 2u) {
            const uint32_t hw =
                static_cast<uint32_t>(bytes[i + 0u]) |
                (static_cast<uint32_t>(bytes[i + 1u]) << 8);
            if (!exec_store_with_saved_scratch(kInsnShX9ToX8, cur_addr, hw)) {
                return false;
            }
            i += 2u;
        } else {
            if (!exec_store_with_saved_scratch(kInsnSbX9ToX8, cur_addr, bytes[i])) {
                return false;
            }
            i += 1u;
        }
    }
    return true;
}

bool ascii_iequals(const char *a, size_t n, const char *b) {
    if (std::strlen(b) != n) {
        return false;
    }
    for (size_t i = 0u; i < n; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

void encode_ascii_as_hex_packet(const char *msg) {
    char hex_out[512] = {};
    size_t pos = 0u;
    for (size_t i = 0u; msg[i] != '\0' && pos + 2u < sizeof(hex_out); ++i) {
        const uint8_t b = static_cast<uint8_t>(msg[i]);
        hex_out[pos++] = to_hex_char(static_cast<uint8_t>(b >> 4));
        hex_out[pos++] = to_hex_char(static_cast<uint8_t>(b & 0x0Fu));
    }
    tx_enqueue_framed_packet(hex_out, pos);
}

void handle_qrcmd(PktCursor &cur, uint64_t now_us) {
    const size_t rem = cur.len - cur.pos;
    if ((rem % 2u) != 0u || rem > 128u) {
        tx_enqueue_cstr_packet("E01");
        return;
    }
    char cmd_str[65] = {};
    size_t cmd_len = 0u;
    while (cur.pos < cur.len) {
        int hi = from_hex_char(cur.buf[cur.pos++]);
        int lo = from_hex_char(cur.buf[cur.pos++]);
        if (hi < 0 || lo < 0) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        cmd_str[cmd_len++] = static_cast<char>((hi << 4) | lo);
    }
    /* Trim surrounding whitespace */
    size_t start = 0u;
    while (start < cmd_len && (cmd_str[start] == ' ' || cmd_str[start] == '\t')) {
        ++start;
    }
    while (cmd_len > start &&
           (cmd_str[cmd_len - 1u] == ' ' || cmd_str[cmd_len - 1u] == '\t' ||
            cmd_str[cmd_len - 1u] == '\r' || cmd_str[cmd_len - 1u] == '\n')) {
        --cmd_len;
    }
    const char *trimmed = cmd_str + start;
    const size_t trimmed_len = cmd_len - start;

    if (ascii_iequals(trimmed, trimmed_len, "puid1") ||
        ascii_iequals(trimmed, trimmed_len, "check")) {
        if (s_gdb_session_open) {
            encode_ascii_as_hex_packet("PUID1:ERR_ALREADY_HALTED\n");
            return;
        }
        pu_ch32_check_result_t r = pu_ch32_non_destructive_check(now_us);
        char line[192] = {};
        switch (r.status) {
        case PU_CH32_OK:
            std::snprintf(line, sizeof(line),
                          "PUID1:OK CHIP_ID=0x%08X FLASH_KB=%u FLASH_WRITES=%u RESUMED=1 OUTEN_OFF=1\n",
                          static_cast<unsigned>(r.chip_id),
                          static_cast<unsigned>(r.flash_kb),
                          static_cast<unsigned>(r.flash_writes));
            s_last_session_err = PU_OK;
            break;
        case PU_CH32_ERR_BUSY:
            std::snprintf(line, sizeof(line), "PUID1:ERR_ENGINE_BUSY\n");
            s_last_session_err = PU_ERR_ENGINE_BUSY;
            break;
        case PU_CH32_ERR_FAULT:
            std::snprintf(line, sizeof(line), "PUID1:ERR_FAULT_LATCHED\n");
            s_last_session_err = PU_ERR_FAULT_LATCHED;
            break;
        case PU_CH32_ERR_NO_REPLY:
            std::snprintf(line, sizeof(line),
                          "PUID1:ERR_NO_REPLY DIAG=0x%02X CAUSES=POWER,GROUND,PULLUP,PD1_REUSE,READ_PROTECT\n",
                          static_cast<unsigned>(r.diagnosis_mask));
            s_last_session_err = PU_OK;
            break;
        case PU_CH32_ERR_ALREADY_HALTED:
            std::snprintf(line, sizeof(line), "PUID1:ERR_ALREADY_HALTED\n");
            s_last_session_err = PU_OK;
            break;
        case PU_CH32_ERR_DETACH_FAILED:
            std::snprintf(line, sizeof(line),
                          "PUID1:ERR_DETACH_FAILED RESUMED=%u OUTEN_OFF=%u\n",
                          r.resumed_running ? 1u : 0u,
                          r.outen_disabled_verified ? 1u : 0u);
            s_last_session_err = PU_OK;
            break;
        default:
            std::snprintf(line, sizeof(line), "PUID1:ERR_UNEXPECTED_STATE\n");
            s_last_session_err = PU_OK;
            break;
        }
        encode_ascii_as_hex_packet(line);
        return;
    }

    if (ascii_iequals(trimmed, trimmed_len, "reset") ||
        ascii_iequals(trimmed, trimmed_len, "reset halt")) {
        if (!ensure_gdb_swio_session(now_us) || !ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        clear_vflash_cache();
        s_target_attached_halted = false;
        if (!restore_all_soft_breakpoints(now_us) ||
            !reset_cpu_and_halt(now_us) ||
            !ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (ascii_iequals(trimmed, trimmed_len, "unbrick")) {
        if (s_gdb_session_open) {
            encode_ascii_as_hex_packet("UNBRICK:ERR_ENGINE_BUSY\n");
            return;
        }
        const pu_status_t acq = pu_engine_acquire(PU_ENGINE_RECOVERY, now_us, PU_SWIO_RECOVERY_TIMEOUT_US);
        if (acq == PU_ERR_ENGINE_BUSY) {
            encode_ascii_as_hex_packet("UNBRICK:ERR_ENGINE_BUSY\n");
            return;
        }
        if (acq == PU_ERR_FAULT_LATCHED) {
            encode_ascii_as_hex_packet("UNBRICK:ERR_FAULT_LATCHED\n");
            return;
        }
        if (acq != PU_OK) {
            encode_ascii_as_hex_packet("UNBRICK:ERR_TIMEOUT\n");
            return;
        }

        wire_bus_open();
#if !defined(PU_HOST_TEST)
        /* Pulse NRST (GP6 -> PD7 if wired) LOW for 10 ms then release into POR catch loop */
        gpio_init(PU_PIN_TARGET_NRST);
        gpio_put(PU_PIN_TARGET_NRST, false);
        gpio_set_dir(PU_PIN_TARGET_NRST, GPIO_OUT);
        busy_wait_us_32(10000u);
        gpio_set_dir(PU_PIN_TARGET_NRST, GPIO_IN);
        gpio_pull_up(PU_PIN_TARGET_NRST);
#endif
        const uint64_t t0 = wire_now_us(now_us);
        bool caught = false;
        for (uint32_t attempt = 0u; attempt < 4096u; ++attempt) {
            if (pu_engine_current() == PU_ENGINE_FAULT) {
                wire_bus_close();
                encode_ascii_as_hex_packet("UNBRICK:ERR_FAULT_LATCHED\n");
                return;
            }
            const uint64_t cur = wire_now_us(t0 + static_cast<uint64_t>(attempt + 1u) * 50u);
            (void)pu_engine_renew_deadline(PU_ENGINE_RECOVERY, cur, PU_SWIO_RECOVERY_TIMEOUT_US);
            if (s_wire.bus_reset != nullptr && ((attempt & 0x7u) == 0u)) {
                s_wire.bus_reset();
            }
            (void)wire_write(kWchDmShdwCfgr, kWchCfgrKey | kWchCfgrOutEnBit);
            (void)wire_write(kWchDmCfgr, kWchCfgrKey | kWchCfgrOutEnBit);
            if (wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlDmActive)) {
                uint32_t dmst = 0u;
                if (wire_read(kDmStatus, &dmst) &&
                    classify_dmstatus(dmst) == DmStateClass::kHalted) {
                    caught = reset_cpu_and_halt(cur);
                    break;
                }
            }
            if ((cur >= t0) && ((cur - t0) >= PU_SWIO_RECOVERY_TIMEOUT_US)) {
                break;
            }
        }
        wire_bus_close();
        (void)pu_engine_release(PU_ENGINE_RECOVERY);
        encode_ascii_as_hex_packet(caught ? "UNBRICK:OK\n" : "UNBRICK:ERR_TIMEOUT\n");
        return;
    }

    tx_enqueue_cstr_packet("");
}

void handle_valid_gdb_packet(uint64_t now_us) {
    static constexpr char kMemoryMapXml[] =
        "<?xml version=\"1.0\"?>"
        "<!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory Map V1.0//EN\""
        " \"http://sourceware.org/gdb/gdb-memory-map.dtd\">"
        "<memory-map>"
        "<memory type=\"flash\" start=\"0x00000000\" length=\"0x4000\">"
        "<property name=\"blocksize\">0x40</property></memory>"
        "<memory type=\"flash\" start=\"0x08000000\" length=\"0x4000\">"
        "<property name=\"blocksize\">0x40</property></memory>"
        "<memory type=\"rom\" start=\"0x1ffff000\" length=\"0x1000\"/>"
        "<memory type=\"ram\" start=\"0x20000000\" length=\"0x800\"/>"
        "<memory type=\"ram\" start=\"0x40000000\" length=\"0x28000\"/>"
        "<memory type=\"ram\" start=\"0xe0000000\" length=\"0x100000\"/>"
        "</memory-map>";

    PktCursor cur{s_rx_buf, s_rx_len, 0u, false};
    if (cur.len == 0u) {
        tx_enqueue_cstr_packet("");
        return;
    }

    if (cur.match_prefix("qRcmd,")) {
        handle_qrcmd(cur, now_us);
        return;
    }

    if (cur.match_exact("k")) {
        clear_vflash_cache();
        if (s_gdb_session_open || has_unrestored_soft_breakpoints()) {
            if (ensure_gdb_swio_session(now_us)) {
                (void)restore_all_soft_breakpoints(now_us);
                (void)resume_target_and_verify(now_us);
                (void)reset_interface_to_power_on();
            }
        }
        wire_bus_close();
        if (pu_engine_current() == PU_ENGINE_SWIO) {
            (void)pu_engine_release(PU_ENGINE_SWIO);
        }
        s_gdb_session_open = false;
        s_gdb_running = false;
        s_target_attached_halted = false;
        return;
    }

    if (!ensure_gdb_swio_session(now_us)) {
        tx_enqueue_cstr_packet("Eeb");
        return;
    }

    if (cur.match_prefix("qSupported")) {
        tx_enqueue_cstr_packet("PacketSize=3ff;qXfer:memory-map:read+");
        return;
    }
    if (cur.match_exact("vMustReplyEmpty")) {
        tx_enqueue_cstr_packet("");
        return;
    }
    if (cur.match_exact("!")) {
        tx_enqueue_cstr_packet("OK");
        return;
    }
    if (cur.match_prefix("Hg") || cur.match_prefix("Hc")) {
        tx_enqueue_cstr_packet("OK");
        return;
    }
    if (cur.match_exact("qfThreadInfo")) {
        tx_enqueue_cstr_packet("m1");
        return;
    }
    if (cur.match_exact("qsThreadInfo")) {
        tx_enqueue_cstr_packet("l");
        return;
    }
    if (cur.match_prefix("qAttached")) {
        tx_enqueue_cstr_packet("1");
        return;
    }
    if (cur.match_prefix("qXfer:memory-map:read::")) {
        const uint32_t offset = cur.take_hex_u32();
        cur.take_char(',');
        const uint32_t length = cur.take_hex_u32();
        const size_t total = std::strlen(kMemoryMapXml);
        if (cur.err || !cur.at_end() || length == 0u || offset > total) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        size_t count = total - offset;
        if (count > length) {
            count = length;
        }
        if (count > 1024u) {
            count = 1024u;
        }
        char reply[1028] = {};
        reply[0] = (offset + count == total) ? 'l' : 'm';
        std::memcpy(reply + 1, kMemoryMapXml + offset, count);
        tx_enqueue_framed_packet(reply, count + 1u);
        return;
    }

    if (cur.match_prefix("?")) {
        if (!cur.at_end() || !ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
        } else {
            tx_enqueue_cstr_packet("T05");
        }
        return;
    }

    if (cur.match_prefix("D")) {
        if (!cur.at_end()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        clear_vflash_cache();
        (void)restore_all_soft_breakpoints(now_us);
        (void)resume_target_and_verify(now_us);
        (void)reset_interface_to_power_on();
        wire_bus_close();
        (void)pu_engine_release(PU_ENGINE_SWIO);
        s_gdb_session_open = false;
        s_gdb_running = false;
        s_target_attached_halted = false;
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("g")) {
        if (!cur.at_end() || !ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        char regs_hex[17u * 8u + 1u] = {};
        for (uint16_t i = 0u; i < 17u; ++i) {
            uint32_t val = 0u;
            const uint16_t regno = (i < 16u) ? static_cast<uint16_t>(kRegGpr0 + i) : kRegDpc;
            if (!dm_read_reg(regno, &val)) {
                tx_enqueue_cstr_packet("E01");
                return;
            }
            const uint32_t le = bswap32_u(val);
            std::snprintf(regs_hex + i * 8u, 9u, "%08x", static_cast<unsigned>(le));
        }
        tx_enqueue_framed_packet(regs_hex, 17u * 8u);
        return;
    }

    if (cur.match_prefix("G")) {
        if (cur.len - cur.pos != 17u * 8u) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        uint32_t vals[17] = {};
        for (size_t i = 0u; i < 17u; ++i) {
            uint32_t raw = 0u;
            for (size_t d = 0u; d < 8u; ++d) {
                int nib = from_hex_char(cur.buf[cur.pos++]);
                if (nib < 0) {
                    tx_enqueue_cstr_packet("E01");
                    return;
                }
                raw = (raw << 4) | static_cast<uint32_t>(nib);
            }
            vals[i] = bswap32_u(raw);
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        for (uint16_t i = 0u; i < 16u; ++i) {
            if (!dm_write_reg(static_cast<uint16_t>(kRegGpr0 + i), vals[i])) {
                tx_enqueue_cstr_packet("E01");
                return;
            }
        }
        if (!dm_write_reg(kRegDpc, vals[16])) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("p")) {
        const uint32_t reg_idx = cur.take_hex_u32();
        if (cur.err || !cur.at_end() || (reg_idx > 16u && reg_idx != 0x20u)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        uint32_t val = 0u;
        const uint16_t regno = (reg_idx < 16u) ? static_cast<uint16_t>(kRegGpr0 + reg_idx) : kRegDpc;
        if (!dm_read_reg(regno, &val)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        char out_hex[9] = {};
        std::snprintf(out_hex, sizeof(out_hex), "%08x", static_cast<unsigned>(bswap32_u(val)));
        tx_enqueue_framed_packet(out_hex, 8u);
        return;
    }

    if (cur.match_prefix("P")) {
        const uint32_t reg_idx = cur.take_hex_u32();
        cur.take_char('=');
        if (cur.err || cur.len - cur.pos != 8u || (reg_idx > 16u && reg_idx != 0x20u)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        uint32_t raw = 0u;
        for (size_t d = 0u; d < 8u; ++d) {
            int nib = from_hex_char(cur.buf[cur.pos++]);
            if (nib < 0) {
                tx_enqueue_cstr_packet("E01");
                return;
            }
            raw = (raw << 4) | static_cast<uint32_t>(nib);
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        const uint16_t regno = (reg_idx < 16u) ? static_cast<uint16_t>(kRegGpr0 + reg_idx) : kRegDpc;
        if (!dm_write_reg(regno, bswap32_u(raw))) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("m")) {
        const uint32_t addr = cur.take_hex_u32();
        cur.take_char(',');
        const uint32_t len = cur.take_hex_u32();
        if (cur.err || !cur.at_end() || len > kMaxMemReadLen ||
            (len > 0u && (len - 1u) > UINT32_MAX - addr) ||
            !is_readable_ch32_addr(addr, len)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        ScopedScratchRegs scratch;
        if (!scratch.save()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        uint8_t raw_bytes[kMaxMemReadLen] = {};
        if (!read_target_clean_range(addr, raw_bytes, len)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        char hex_out[kMaxMemReadLen * 2u] = {};
        for (uint32_t i = 0u; i < len; ++i) {
            const uint8_t b = raw_bytes[i];
            hex_out[i * 2u + 0u] = to_hex_char(static_cast<uint8_t>(b >> 4));
            hex_out[i * 2u + 1u] = to_hex_char(static_cast<uint8_t>(b & 0x0Fu));
        }
        if (!scratch.restore()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_framed_packet(hex_out, len * 2u);
        return;
    }

    if (cur.match_prefix("M")) {
        const uint32_t addr = cur.take_hex_u32();
        cur.take_char(',');
        const uint32_t len = cur.take_hex_u32();
        cur.take_char(':');
        const size_t rem = cur.len - cur.pos;
        if (cur.err || len > kMaxMemReadLen ||
            (len > 0u && (len - 1u) > UINT32_MAX - addr) ||
            rem != static_cast<size_t>(len) * 2u ||
            !is_writable_ch32_ram_or_mmio(addr, len)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        uint8_t bytes[kMaxMemReadLen] = {};
        for (uint32_t i = 0u; i < len; ++i) {
            int hi = from_hex_char(cur.buf[cur.pos++]);
            int lo = from_hex_char(cur.buf[cur.pos++]);
            if (hi < 0 || lo < 0) {
                tx_enqueue_cstr_packet("E01");
                return;
            }
            bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        ScopedScratchRegs scratch;
        if (!scratch.save()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!write_target_range(addr, bytes, len)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!scratch.restore()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    /* QingKe V2A has zero hardware trigger comparators; GDB 10..16 converts 'break' inside
     * <memory type="flash"> into Z1/z1 automatically, so route both Z0/Z1 to SoftBreak. */
    if (cur.match_prefix("Z0,") || cur.match_prefix("Z1,")) {
        const uint32_t addr = cur.take_hex_u32();
        cur.take_char(',');
        const uint32_t kind = cur.take_hex_u32();
        if (cur.err || !cur.at_end() || (kind != 2u && kind != 4u)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us) || !set_soft_breakpoint(addr, kind)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("z0,") || cur.match_prefix("z1,")) {
        const uint32_t addr = cur.take_hex_u32();
        cur.take_char(',');
        const uint32_t kind = cur.take_hex_u32();
        if (cur.err || !cur.at_end() || (kind != 2u && kind != 4u)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us) || !clear_soft_breakpoint(addr, kind)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("vFlashErase:")) {
        const uint32_t addr = cur.take_hex_u32();
        cur.take_char(',');
        const uint32_t size = cur.take_hex_u32();
        uint32_t flash_off = 0u;
        if (cur.err || !cur.at_end() || size == 0u ||
            !normalize_flash_range(addr, size, &flash_off) ||
            (flash_off % PU_CH32_FLASH_PAGE_SIZE) != 0u ||
            (size % PU_CH32_FLASH_PAGE_SIZE) != 0u) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        const uint32_t first_page = flash_off / PU_CH32_FLASH_PAGE_SIZE;
        const uint32_t num_pages  = size / PU_CH32_FLASH_PAGE_SIZE;
        const uint32_t end_page   = first_page + num_pages;
        if (s_vflash_dirty && s_vflash_page_idx >= first_page && s_vflash_page_idx < end_page) {
            clear_vflash_cache();
        }
        for (size_t i = 0u; i < kMaxBreakpoints; ++i) {
            if (!s_breakpoints[i].active) {
                continue;
            }
            const uint32_t bp_p0 = s_breakpoints[i].offset / PU_CH32_FLASH_PAGE_SIZE;
            const uint32_t bp_p1 = (s_breakpoints[i].offset + s_breakpoints[i].size - 1u) /
                                   PU_CH32_FLASH_PAGE_SIZE;
            const bool p0_erased = (bp_p0 >= first_page && bp_p0 < end_page);
            const bool p1_erased = (bp_p1 >= first_page && bp_p1 < end_page);
            if (!p0_erased && !p1_erased) {
                continue;
            }
            if (bp_p0 != bp_p1 && !p0_erased) {
                uint8_t unpatched[PU_CH32_FLASH_PAGE_SIZE] = {};
                build_patched_page_image(bp_p0, unpatched, -1, 0u, static_cast<int>(i));
                if (!rewrite_flash_page_64(bp_p0, unpatched)) {
                    tx_enqueue_cstr_packet("E01");
                    return;
                }
            }
            if (bp_p0 != bp_p1 && !p1_erased) {
                uint8_t unpatched[PU_CH32_FLASH_PAGE_SIZE] = {};
                build_patched_page_image(bp_p1, unpatched, -1, 0u, static_cast<int>(i));
                if (!rewrite_flash_page_64(bp_p1, unpatched)) {
                    tx_enqueue_cstr_packet("E01");
                    return;
                }
            }
            s_breakpoints[i].active = false;
            s_breakpoints[i].size = 0u;
            for (uint32_t p = bp_p0; p <= bp_p1; ++p) {
                if (s_page_bp_refcount[p] > 0u) {
                    --s_page_bp_refcount[p];
                }
                if (s_page_bp_refcount[p] == 0u && !(p >= first_page && p < end_page)) {
                    s_page_dirty_restore[p] = false;
                }
            }
        }

        ScopedScratchRegs scratch;
        if (!scratch.save() || !unlock_ch32_fast_flash()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        bool ok = true;
        for (uint32_t p = 0u; p < num_pages; ++p) {
            const uint32_t page_idx = first_page + p;
            if (!erase_flash_page_64_unlocked(page_idx)) {
                ok = false;
                break;
            }
            std::memset(s_clean_pages[page_idx], 0xFF, PU_CH32_FLASH_PAGE_SIZE);
            s_page_bp_refcount[page_idx] = 0u;
            s_page_dirty_restore[page_idx] = false;
        }
        lock_ch32_flash();
        if (!scratch.restore() || !ok) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("vFlashWrite:")) {
        const uint32_t addr = cur.take_hex_u32();
        cur.take_char(':');
        const uint32_t bin_len = static_cast<uint32_t>(cur.len - cur.pos);
        uint32_t flash_off = 0u;
        if (cur.err || !normalize_flash_range(addr, bin_len, &flash_off)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        for (uint32_t i = 0u; i < bin_len; ++i) {
            if (!stage_vflash_byte(flash_off + i, cur.buf[cur.pos + i])) {
                tx_enqueue_cstr_packet("E01");
                return;
            }
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("vFlashDone")) {
        if (!cur.at_end() || !ensure_gdb_target_halted(now_us) || !flush_vflash_cache()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("OK");
        return;
    }

    if (cur.match_prefix("qCRC:")) {
        const uint32_t addr = cur.take_hex_u32();
        cur.take_char(',');
        const uint32_t len = cur.take_hex_u32();
        uint32_t flash_off = 0u;
        const bool in_flash = normalize_flash_range(addr, len, &flash_off);
        const bool in_ram =
            !(len > 0u && (len - 1u) > UINT32_MAX - addr) &&
            addr >= PU_CH32_SRAM_BASE && addr <= PU_CH32_SRAM_BASE + PU_CH32_SRAM_SIZE &&
            len <= (PU_CH32_SRAM_BASE + PU_CH32_SRAM_SIZE) - addr;
        if (cur.err || !cur.at_end() || (!in_flash && !in_ram)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        ScopedScratchRegs scratch;
        if (!scratch.save()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        uint32_t crc = 0xFFFFFFFFu;
        uint32_t offset = 0u;
        while (offset < len) {
            uint8_t chunk[256] = {};
            const uint32_t n_chunk = (len - offset > sizeof(chunk))
                                         ? static_cast<uint32_t>(sizeof(chunk))
                                         : (len - offset);
            if (!read_target_clean_range(addr + offset, chunk, n_chunk)) {
                tx_enqueue_cstr_packet("E01");
                return;
            }
            for (uint32_t i = 0u; i < n_chunk; ++i) {
                crc ^= static_cast<uint32_t>(chunk[i]) << 24;
                for (unsigned bit = 0u; bit < 8u; ++bit) {
                    crc = (crc << 1) ^ ((crc & 0x80000000u) != 0u ? 0x04C11DB7u : 0u);
                }
            }
            offset += n_chunk;
        }
        if (!scratch.restore()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        char reply[12] = {};
        std::snprintf(reply, sizeof(reply), "C%08x", static_cast<unsigned>(crc));
        tx_enqueue_cstr_packet(reply);
        return;
    }

    if (cur.match_exact("qC")) {
        tx_enqueue_cstr_packet("QC1");
        return;
    }

    if (cur.match_prefix("s")) {
        const bool has_addr = !cur.at_end();
        const uint32_t new_pc = has_addr ? cur.take_hex_u32() : 0u;
        if (cur.err || !cur.at_end()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (has_addr && !dm_write_reg(kRegDpc, new_pc)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        uint32_t step_pc = new_pc;
        if (!has_addr && !dm_read_reg(kRegDpc, &step_pc)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }

        /* Temporarily unpatch active software breakpoint covering step_pc if present */
        int bp_slot = -1;
        uint32_t bp_p0 = 0u;
        uint32_t bp_p1 = 0u;
        uint32_t pc_flash_off = 0u;
        if (normalize_flash_range(step_pc, 2u, &pc_flash_off)) {
            for (size_t i = 0u; i < kMaxBreakpoints; ++i) {
                if (s_breakpoints[i].active &&
                    pc_flash_off >= s_breakpoints[i].offset &&
                    pc_flash_off < s_breakpoints[i].offset + s_breakpoints[i].size) {
                    bp_slot = static_cast<int>(i);
                    bp_p0 = s_breakpoints[i].offset / PU_CH32_FLASH_PAGE_SIZE;
                    bp_p1 = (s_breakpoints[i].offset + s_breakpoints[i].size - 1u) /
                            PU_CH32_FLASH_PAGE_SIZE;
                    break;
                }
            }
        }
        if (bp_slot >= 0) {
            for (uint32_t p = bp_p0; p <= bp_p1; ++p) {
                uint8_t unpatched[PU_CH32_FLASH_PAGE_SIZE] = {};
                build_patched_page_image(p, unpatched, -1, 0u, bp_slot);
                if (!rewrite_flash_page_64(p, unpatched)) {
                    tx_enqueue_cstr_packet("E01");
                    return;
                }
            }
        }

        uint32_t dcsr = 0u;
        const bool step_ok =
            dm_read_reg(kRegDcsr, &dcsr) &&
            dm_write_reg(kRegDcsr, dcsr | kDcsrStepBit | kDcsrEbreakM) &&
            wire_write(kDmControl, kDmCtrlResumeReq | kDmCtrlDmActive) &&
            halt_target_bounded(now_us) &&
            dm_write_reg(kRegDcsr, (dcsr & ~kDcsrStepBit) | kDcsrEbreakM);

        bool repatch_ok = true;
        if (bp_slot >= 0) {
            for (uint32_t p = bp_p0; p <= bp_p1; ++p) {
                uint8_t repatched[PU_CH32_FLASH_PAGE_SIZE] = {};
                build_patched_page_image(p, repatched, -1, 0u, -1);
                if (!rewrite_flash_page_64(p, repatched)) {
                    repatch_ok = false;
                }
            }
        }

        if (!step_ok || !repatch_ok) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        tx_enqueue_cstr_packet("T05");
        return;
    }

    if (cur.match_prefix("c")) {
        const bool has_addr = !cur.at_end();
        const uint32_t new_pc = has_addr ? cur.take_hex_u32() : 0u;
        if (cur.err || !cur.at_end()) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (!ensure_gdb_target_halted(now_us)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (has_addr && !dm_write_reg(kRegDpc, new_pc)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        DmStateClass post_state = DmStateClass::kInvalidNoReply;
        if (!resume_target_for_gdb(now_us, &post_state)) {
            tx_enqueue_cstr_packet("E01");
            return;
        }
        if (post_state == DmStateClass::kHalted) {
            s_gdb_running = false;
            s_target_attached_halted = true;
            tx_enqueue_cstr_packet("T05");
        } else {
            s_gdb_running = true;
            s_entered_running_in_feed = true;
            s_target_attached_halted = false;
        }
        return;
    }

    tx_enqueue_cstr_packet("");
}

void feed_single_gdb_byte(uint8_t b, uint64_t now_us) {
    switch (s_rx_state) {
    case GdbRxState::kIdle:
        if (b == '$') {
            s_rx_state = GdbRxState::kBody;
            s_rx_escaped = false;
            s_rx_error = false;
            s_rx_wire_csum = 0u;
            s_rx_len = 0u;
        } else if (b == '-') {
            if (s_last_framed_len > 0u) {
                for (size_t i = 0u; i < s_last_framed_len; ++i) {
                    tx_enqueue_char(s_last_framed_pkt[i]);
                }
            }
        } else if (b == 0x03u) {
            if (ensure_gdb_swio_session(now_us) && ensure_gdb_target_halted(now_us)) {
                s_gdb_running = false;
                tx_enqueue_cstr_packet("T02");
            } else {
                s_gdb_running = false;
                tx_enqueue_cstr_packet("E01");
            }
        }
        break;

    case GdbRxState::kBody:
        if (b == '$') {
            /* Interrupted frame: cleanly resynchronize onto the new '$' */
            s_rx_escaped = false;
            s_rx_error = false;
            s_rx_wire_csum = 0u;
            s_rx_len = 0u;
        } else if (!s_rx_escaped && b == '#') {
            s_rx_state = GdbRxState::kCsum1;
        } else {
            s_rx_wire_csum = static_cast<uint8_t>(s_rx_wire_csum + b);
            if (!s_rx_escaped && b == '}') {
                s_rx_escaped = true;
            } else {
                const uint8_t decoded = s_rx_escaped ? static_cast<uint8_t>(b ^ 0x20u) : b;
                s_rx_escaped = false;
                if (s_rx_len < kMaxGdbRxLen) {
                    s_rx_buf[s_rx_len++] = decoded;
                } else {
                    s_rx_error = true;
                }
            }
        }
        break;

    case GdbRxState::kCsum1: {
        if (b == '$') {
            s_rx_state = GdbRxState::kBody;
            s_rx_escaped = false;
            s_rx_error = false;
            s_rx_wire_csum = 0u;
            s_rx_len = 0u;
            break;
        }
        const int d1 = from_hex_char(b);
        if (d1 < 0) {
            s_rx_error = true;
            s_rx_expected_csum = 0u;
        } else {
            s_rx_expected_csum = static_cast<uint8_t>(d1 << 4);
        }
        s_rx_state = GdbRxState::kCsum2;
        break;
    }

    case GdbRxState::kCsum2: {
        if (b == '$') {
            s_rx_state = GdbRxState::kBody;
            s_rx_escaped = false;
            s_rx_error = false;
            s_rx_wire_csum = 0u;
            s_rx_len = 0u;
            break;
        }
        const int d2 = from_hex_char(b);
        if (d2 < 0) {
            s_rx_error = true;
        } else {
            s_rx_expected_csum = static_cast<uint8_t>(s_rx_expected_csum | static_cast<uint8_t>(d2));
        }
        s_rx_state = GdbRxState::kIdle;
        if (s_rx_error || s_rx_escaped || s_rx_expected_csum != s_rx_wire_csum) {
            tx_enqueue_char('-');
        } else {
            tx_enqueue_char('+');
            handle_valid_gdb_packet(now_us);
        }
        break;
    }
    }
}

void pump_rx_ring(uint64_t now_us) {
    while (s_rx_ring_count > 0u) {
        const uint8_t next_b = s_rx_ring[s_rx_ring_head];
        const bool may_emit_tx =
            (s_rx_state == GdbRxState::kCsum2) ||
            (s_rx_state == GdbRxState::kIdle && (next_b == '-' || next_b == 0x03u));
        if (may_emit_tx && tx_free_space() < kMaxSingleRspReply) {
            break;
        }
        feed_single_gdb_byte(rx_ring_pop(), now_us);
    }
}

void poll_gdb_session_state(uint64_t now_us) {
    if (!s_gdb_session_open) {
        return;
    }
    const pu_status_t st =
        pu_engine_renew_deadline(PU_ENGINE_SWIO, now_us, PU_SWIO_SESSION_TIMEOUT_US);
    if (st != PU_OK) {
        wire_bus_close();
        s_gdb_session_open = false;
        s_gdb_running = false;
        s_target_attached_halted = false;
        clear_vflash_cache();
        return;
    }
    if (s_gdb_running && !s_entered_running_in_feed && tx_free_space() >= 16u) {
        uint32_t dmstatus = 0u;
        if (wire_read(kDmStatus, &dmstatus) &&
            classify_dmstatus(dmstatus) == DmStateClass::kHalted) {
            (void)wire_write(kDmControl, kDmCtrlDmActive);
            s_gdb_running = false;
            s_target_attached_halted = true;
            tx_enqueue_cstr_packet("T05");
        }
    }
}

} // namespace

/* -------------------------------------------------------------------------- */
/* Public C-Compatible API                                                    */
/* -------------------------------------------------------------------------- */

extern "C" {

void pu_swio_init(const pu_swio_wire_hal_t *hal) {
    pu_ch32_on_disconnect();
    std::memset(s_breakpoints, 0, sizeof(s_breakpoints));
    std::memset(s_page_bp_refcount, 0, sizeof(s_page_bp_refcount));
    std::memset(s_page_dirty_restore, 0, sizeof(s_page_dirty_restore));
    if (hal != nullptr) {
        s_wire = *hal;
        if (s_wire.sys_clock_hz == 0u) {
            s_wire.sys_clock_hz = 125000000u;
        }
        s_has_custom_vtarg_cb = (hal->set_vtarg_power != nullptr);
        s_wire_initialized = true;
        return;
    }

    s_has_custom_vtarg_cb = false;
#if !defined(PU_HOST_TEST)
    std::memset(&s_wire, 0, sizeof(s_wire));
    uint32_t clk = clock_get_hz(clk_sys);
    s_wire.sys_clock_hz = (clk != 0u) ? clk : 125000000u;
    s_wire.bus_open = rp2040_swio_bus_open;
    s_wire.bus_close = rp2040_swio_bus_close;
    s_wire.bus_reset = rp2040_swio_bus_reset;
    s_wire.read_reg = rp2040_swio_read_reg;
    s_wire.write_reg = rp2040_swio_write_reg;
#if PU_HAS_VTARG_SWITCH
    s_wire.set_vtarg_power = rp2040_swio_set_vtarg;
#else
    s_wire.set_vtarg_power = nullptr;
#endif
    s_wire.delay_us = rp2040_swio_delay_us;
    s_wire.now_us = rp2040_swio_now_us;
#else
    s_wire = kHostFakeSwioHal;
#if !PU_HAS_VTARG_SWITCH
    s_wire.set_vtarg_power = nullptr;
#endif
#endif
    s_wire_initialized = true;
}

pu_ch32_check_result_t pu_ch32_non_destructive_check(uint64_t now_us) {
    ensure_wire_initialized();
    pu_ch32_check_result_t res = {};
    res.status = PU_CH32_ERR_UNEXPECTED_STATE;

    const pu_status_t acq = pu_engine_acquire(PU_ENGINE_SWIO, now_us, PU_SWIO_CHECK_TIMEOUT_US);
    if (acq == PU_ERR_ENGINE_BUSY) {
        res.status = PU_CH32_ERR_BUSY;
        return res;
    }
    if (acq == PU_ERR_FAULT_LATCHED) {
        res.status = PU_CH32_ERR_FAULT;
        return res;
    }
    if (acq != PU_OK) {
        res.status = PU_CH32_ERR_UNEXPECTED_STATE;
        return res;
    }

    struct ScopedEngineRelease {
        ~ScopedEngineRelease() {
            wire_bus_close();
            (void)pu_engine_release(PU_ENGINE_SWIO);
        }
    } release_guard;

    wire_bus_open();
    if (!enable_swio_link_and_dm()) {
        res.status = PU_CH32_ERR_NO_REPLY;
        res.diagnosis_mask = PU_CH32_DIAG_ALL_FIVE_CAUSES;
        return res;
    }

    uint32_t initial_dmstatus = 0u;
    if (!wire_read(kDmStatus, &initial_dmstatus)) {
        res.status = PU_CH32_ERR_NO_REPLY;
        res.diagnosis_mask = PU_CH32_DIAG_ALL_FIVE_CAUSES;
        return res;
    }

    const DmStateClass init_cls = classify_dmstatus(initial_dmstatus);
    if (init_cls == DmStateClass::kInvalidNoReply) {
        res.status = PU_CH32_ERR_NO_REPLY;
        res.diagnosis_mask = PU_CH32_DIAG_ALL_FIVE_CAUSES;
        return res;
    }
    if (init_cls == DmStateClass::kHalted) {
        /* Never clobber or force-resume an already-halted debug target */
        res.status = PU_CH32_ERR_ALREADY_HALTED;
        return res;
    }
    if (init_cls != DmStateClass::kRunning) {
        res.status = PU_CH32_ERR_UNEXPECTED_STATE;
        return res;
    }

    if (!halt_target_bounded(now_us)) {
        (void)reset_interface_to_power_on();
        res.status = PU_CH32_ERR_NO_REPLY;
        res.diagnosis_mask = PU_CH32_DIAG_ALL_FIVE_CAUSES;
        return res;
    }

    uint32_t chip_id = 0u;
    uint32_t flash_sig = 0u;
    bool read_ok = false;
    {
        ScopedScratchRegs scratch;
        if (scratch.save()) {
            read_ok = exec_load_with_saved_scratch(kInsnLwX9FromX8, kCh32DbgmcuIdcodeAddr, &chip_id);
            if (read_ok && (chip_id == 0u || chip_id == 0xFFFFFFFFu)) {
                read_ok = exec_load_with_saved_scratch(kInsnLwX9FromX8, kCh32Uid0Addr, &chip_id);
            }
            if (read_ok) {
                read_ok = exec_load_with_saved_scratch(kInsnLwX9FromX8, kCh32FlashSizeAddr, &flash_sig);
            }
            if (!scratch.restore()) {
                read_ok = false;
            }
        }
    }

    const bool resumed = resume_target_and_verify(now_us);
    const bool outen_off = reset_interface_to_power_on();

    res.resumed_running = resumed;
    res.outen_disabled_verified = outen_off;
    res.flash_writes = 0u;

    if (!resumed || !outen_off) {
        res.status = PU_CH32_ERR_DETACH_FAILED;
        return res;
    }
    if (!read_ok || chip_id == 0u || chip_id == 0xFFFFFFFFu) {
        res.status = PU_CH32_ERR_UNEXPECTED_STATE;
        return res;
    }

    const uint32_t kb = flash_sig & 0xFFFFu;
    res.status = PU_CH32_OK;
    res.chip_id = chip_id;
    res.flash_kb = (kb != 0u && kb != 0xFFFFu) ? kb : 16u;
    return res;
}

size_t pu_ch32_gdb_feed(const uint8_t *in,
                        size_t len,
                        uint8_t *out,
                        size_t out_cap,
                        uint64_t now_us) {
    ensure_wire_initialized();
    s_entered_running_in_feed = false;

    size_t written = 0u;
    size_t in_pos = 0u;

    for (;;) {
        if (in != nullptr) {
            while (in_pos < len && rx_ring_push(in[in_pos])) {
                ++in_pos;
            }
        }
        pump_rx_ring(now_us);
        if (out != nullptr && written < out_cap) {
            written += tx_drain_to(out + written, out_cap - written);
        }
        pump_rx_ring(now_us);
        if (out != nullptr && written < out_cap) {
            written += tx_drain_to(out + written, out_cap - written);
        }
        if (in == nullptr || in_pos >= len || s_rx_ring_count >= kMaxGdbRxRingCap) {
            break;
        }
    }

    poll_gdb_session_state(now_us);
    s_entered_running_in_feed = false;
    if (out != nullptr && written < out_cap) {
        written += tx_drain_to(out + written, out_cap - written);
    }
    return written;
}

size_t pu_ch32_gdb_poll(uint8_t *out, size_t out_cap, uint64_t now_us) {
    return pu_ch32_gdb_feed(nullptr, 0u, out, out_cap, now_us);
}

pu_status_t pu_ch32_power_cycle_unbrick(uint64_t now_us) {
    ensure_wire_initialized();
#if !PU_HAS_VTARG_SWITCH
    if (!s_has_custom_vtarg_cb || s_wire.set_vtarg_power == nullptr) {
        return PU_ERR_INVALID_ARG;
    }
#else
    if (s_wire.set_vtarg_power == nullptr) {
        return PU_ERR_INVALID_ARG;
    }
#endif

    const pu_status_t acq = pu_engine_acquire(PU_ENGINE_RECOVERY, now_us, PU_SWIO_RECOVERY_TIMEOUT_US);
    if (acq != PU_OK) {
        return acq;
    }

    struct ScopedRecoveryRelease {
        ~ScopedRecoveryRelease() {
            wire_bus_close();
            (void)pu_engine_release(PU_ENGINE_RECOVERY);
        }
    } release_guard;

    wire_bus_close();
    s_wire.set_vtarg_power(false);
    if (s_wire.delay_us != nullptr) {
        s_wire.delay_us(50000u);
    }

    if (pu_engine_current() == PU_ENGINE_FAULT) {
        return PU_ERR_FAULT_LATCHED;
    }

    /* Open SWIO bus and turn VTARG ON with ZERO blind sleep: immediately poll
     * DMCONTROL = 0x80000001 across POR so PD1-reusing sketches never reach main(). */
    wire_bus_open();
    s_wire.set_vtarg_power(true);

    const uint64_t t0 = wire_now_us(now_us);
    bool caught = false;
    for (uint32_t attempt = 0u; attempt < 4096u; ++attempt) {
        if (pu_engine_current() == PU_ENGINE_FAULT) {
            return PU_ERR_FAULT_LATCHED;
        }
        (void)wire_write(kWchDmShdwCfgr, kWchCfgrKey | kWchCfgrOutEnBit);
        (void)wire_write(kWchDmCfgr, kWchCfgrKey | kWchCfgrOutEnBit);
        if (wire_write(kDmControl, kDmCtrlHaltReq | kDmCtrlDmActive)) {
            uint32_t dmstatus = 0u;
            if (wire_read(kDmStatus, &dmstatus) &&
                classify_dmstatus(dmstatus) == DmStateClass::kHalted) {
                caught = true;
                break;
            }
        }
        if (s_wire.bus_reset != nullptr && ((attempt & 0x7u) == 0x7u)) {
            s_wire.bus_reset();
        }
        const uint64_t now = wire_now_us(t0 + static_cast<uint64_t>(attempt + 1u) * 50u);
        if ((now >= t0) && ((now - t0) >= PU_SWIO_RECOVERY_TIMEOUT_US)) {
            break;
        }
    }
    if (!caught) {
        return PU_ERR_TIMEOUT;
    }
    if (!reset_cpu_and_halt(now_us)) {
        return PU_ERR_TIMEOUT;
    }
    /* Do NOT call reset_interface_to_power_on(): leave chip HALTED with DMACTIVE=1. */
    return PU_OK;
}

void pu_ch32_on_disconnect(void) {
    invalidate_progbuf_cache();
    clear_vflash_cache();
    const uint64_t now = wire_now_us(0u);
    const bool had_bps = has_unrestored_soft_breakpoints();
    if (pu_engine_current() == PU_ENGINE_SWIO) {
        if (s_gdb_session_open || had_bps) {
            (void)enable_swio_link_and_dm();
            (void)restore_all_soft_breakpoints(now);
            if (s_target_attached_halted || had_bps) {
                (void)resume_target_and_verify(now);
            }
            (void)reset_interface_to_power_on();
        }
        wire_bus_close();
        (void)pu_engine_release(PU_ENGINE_SWIO);
    } else if (had_bps &&
               pu_engine_acquire(PU_ENGINE_SWIO, now, PU_SWIO_CHECK_TIMEOUT_US) == PU_OK) {
        wire_bus_open();
        if (enable_swio_link_and_dm()) {
            (void)restore_all_soft_breakpoints(now);
            (void)resume_target_and_verify(now);
            (void)reset_interface_to_power_on();
        }
        wire_bus_close();
        (void)pu_engine_release(PU_ENGINE_SWIO);
    } else if (pu_engine_current() != PU_ENGINE_RECOVERY) {
        wire_bus_close();
    }
    s_gdb_session_open = false;
    s_gdb_running = false;
    s_target_attached_halted = false;
    s_last_session_err = PU_OK;
    s_rx_state = GdbRxState::kIdle;
    s_rx_escaped = false;
    s_rx_error = false;
    s_rx_len = 0u;
    s_rx_ring_head = 0u;
    s_rx_ring_tail = 0u;
    s_rx_ring_count = 0u;
    s_tx_head = 0u;
    s_tx_tail = 0u;
    s_tx_count = 0u;
    s_last_framed_len = 0u;
}

#ifdef PU_HOST_TEST

void pu_swio_set_fake_wire_mode(pu_fake_wire_mode_t mode) {
    pu_ch32_on_disconnect();
    std::memset(s_breakpoints, 0, sizeof(s_breakpoints));
    std::memset(s_page_bp_refcount, 0, sizeof(s_page_bp_refcount));
    std::memset(s_page_dirty_restore, 0, sizeof(s_page_dirty_restore));
    const pu_engine_state_t cur_eng = pu_engine_current();
    if (cur_eng == PU_ENGINE_FAULT) {
        (void)pu_engine_clear_fault();
    } else if (cur_eng != PU_ENGINE_IDLE) {
        (void)pu_engine_release(cur_eng);
    }

    std::memset(&s_fake, 0, sizeof(s_fake));
    s_fake.mode = mode;
    s_fake.powered = true;
    s_fake.halted = (mode == PU_WIRE_ALREADY_HALTED);
    s_fake.dpc = PU_CH32_FLASH_BASE;
    s_fake.flash_ctlr = kFlashCtlrLock | kFlashCtlrFlock;
    s_fake.fail_on_write_index = -1;
    std::memset(s_fake.page_latch, 0xFF, sizeof(s_fake.page_latch));

    for (size_t i = 0u; i < PU_CH32_FLASH_SIZE; ++i) {
        s_fake.flash[i] = static_cast<uint8_t>((i * 37u + 0x5Au) ^ (i >> 6));
    }
    std::memcpy(s_fake.original_flash, s_fake.flash, PU_CH32_FLASH_SIZE);

    for (size_t i = 0u; i < 16u; ++i) {
        s_fake.gpr[i] = (i == 0u) ? 0u : static_cast<uint32_t>(0xA5000000u + i);
    }
    for (size_t i = 0u; i < 8u; ++i) {
        s_fake.progbuf[i] = kInsnEbreak;
    }

    pu_swio_init(&kHostFakeSwioHal);
}

void pu_swio_inject_fake_flash_write_fail(int fail_on_write_index) {
    s_fake.page_prog_seq = 0u;
    s_fake.fail_on_write_index = fail_on_write_index;
}

bool pu_swio_fake_flash_matches_original(void) {
    return std::memcmp(s_fake.flash, s_fake.original_flash, PU_CH32_FLASH_SIZE) == 0;
}

const uint16_t *pu_swio_get_pio_instructions(size_t *out_count) {
    if (out_count != nullptr) {
        *out_count = sizeof(kPuSwioFallbackInstructions) / sizeof(kPuSwioFallbackInstructions[0]);
    }
    return kPuSwioFallbackInstructions;
}

pu_status_t pu_ch32_gdb_send_checked(const char *packet_without_csum,
                                     uint8_t *out,
                                     size_t out_cap,
                                     uint64_t now_us) {
    if (packet_without_csum == nullptr || out == nullptr || out_cap == 0u) {
        return PU_ERR_INVALID_ARG;
    }

    size_t raw_len = std::strlen(packet_without_csum);
    size_t start = 0u;
    if (raw_len > 0u && packet_without_csum[0] == '$') {
        start = 1u;
    }
    size_t end = raw_len;
    if (end > start && packet_without_csum[end - 1u] == '#') {
        --end;
    }

    uint8_t framed[kMaxGdbRxLen + 64u] = {};
    size_t fpos = 0u;
    framed[fpos++] = '$';
    uint8_t csum = 0u;
    for (size_t i = start; i < end; ++i) {
        const uint8_t b = static_cast<uint8_t>(packet_without_csum[i]);
        if (b == '$' || b == '#' || b == '}' || b == '*') {
            if (fpos + 2u >= sizeof(framed) - 4u) {
                return PU_ERR_INVALID_ARG;
            }
            framed[fpos++] = '}';
            csum = static_cast<uint8_t>(csum + static_cast<uint8_t>('}'));
            const uint8_t esc = static_cast<uint8_t>(b ^ 0x20u);
            framed[fpos++] = esc;
            csum = static_cast<uint8_t>(csum + esc);
        } else {
            if (fpos + 1u >= sizeof(framed) - 4u) {
                return PU_ERR_INVALID_ARG;
            }
            framed[fpos++] = b;
            csum = static_cast<uint8_t>(csum + b);
        }
    }
    framed[fpos++] = '#';
    framed[fpos++] = static_cast<uint8_t>(to_hex_char(static_cast<uint8_t>(csum >> 4)));
    framed[fpos++] = static_cast<uint8_t>(to_hex_char(static_cast<uint8_t>(csum & 0x0Fu)));

    s_last_session_err = PU_OK;
    const size_t n = pu_ch32_gdb_feed(framed, fpos, out, out_cap - 1u, now_us);
    out[n] = '\0';

    if (s_last_session_err != PU_OK) {
        return s_last_session_err;
    }
    if (n == 0u || out[0] == '-') {
        return PU_ERR_INVALID_ARG;
    }
    const char *dollar = std::strchr(reinterpret_cast<const char *>(out), '$');
    if (dollar == nullptr) {
        return PU_ERR_INVALID_ARG;
    }
    if (dollar[1] == 'E') {
        return PU_ERR_INVALID_ARG;
    }
    return PU_OK;
}

#endif /* PU_HOST_TEST */

} // extern "C"
