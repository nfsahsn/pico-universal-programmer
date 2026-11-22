/*
 * Immutable A/B CRC32 chainloader for Pico Universal (RP2040).
 *
 * Boot sequence:
 *   1. Sample active-LOW recovery button (PU_PIN_RECOVERY_BTN) with pull-up debounce.
 *   2. Evaluate two-bank journal & 3-strike trial rollback policy via pu_bootloader_select_slot().
 *   3. Enter RP2040 ROM USB BOOTSEL recovery if requested or no valid slot exists.
 *   4. Quiesce peripherals (preserving XIP QSPI flash), install VTOR + MSP, and branch via handover.S.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bootloader/image_format.h"
#include "bootloader/image_validator.h"
#include "bootloader/journal.h"
#include "config/board_pinout.h"
#include "config/flash_layout.h"

#if !defined(PU_HOST_TEST)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "hardware/gpio.h"
#include "hardware/resets.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

extern void pu_bootloader_handover(uint32_t vtor, uint32_t initial_msp, uint32_t entry_point)
    __attribute__((noreturn));

#if !defined(PU_HOST_TEST)
#define PU_RECOVERY_DEBOUNCE_SAMPLES (8u)
#define PU_RECOVERY_SAMPLE_DELAY_US  (500u)

static bool sample_recovery_button_pressed(void) {
    gpio_init(PU_PIN_RECOVERY_BTN);
    gpio_set_dir(PU_PIN_RECOVERY_BTN, GPIO_IN);
    gpio_pull_up(PU_PIN_RECOVERY_BTN);
    /* Allow internal pull-up to settle external pin capacitance. */
    busy_wait_us_32(100u);

    unsigned pressed_samples = 0u;
    for (unsigned i = 0u; i < PU_RECOVERY_DEBOUNCE_SAMPLES; ++i) {
        if (!gpio_get(PU_PIN_RECOVERY_BTN)) {
            ++pressed_samples;
        }
        busy_wait_us_32(PU_RECOVERY_SAMPLE_DELAY_US);
    }

    gpio_disable_pulls(PU_PIN_RECOVERY_BTN);
    gpio_deinit(PU_PIN_RECOVERY_BTN);
    return pressed_samples == PU_RECOVERY_DEBOUNCE_SAMPLES;
}

static void quiesce_peripherals_for_handover(void) {
    /*
     * Reset peripheral blocks that the application expects in power-on state while
     * keeping XIP, IO_QSPI, PADS_QSPI, PLLs, clocks, and SRAM alive for XIP execution.
     */
    const uint32_t peripheral_mask =
        RESETS_RESET_ADC_BITS |
        RESETS_RESET_DMA_BITS |
        RESETS_RESET_I2C0_BITS |
        RESETS_RESET_I2C1_BITS |
        RESETS_RESET_PIO0_BITS |
        RESETS_RESET_PIO1_BITS |
        RESETS_RESET_PWM_BITS |
        RESETS_RESET_SPI0_BITS |
        RESETS_RESET_SPI1_BITS |
        RESETS_RESET_UART0_BITS |
        RESETS_RESET_UART1_BITS |
        RESETS_RESET_USBCTRL_BITS;

    reset_block(peripheral_mask);
    unreset_block_wait(
        RESETS_RESET_DMA_BITS |
        RESETS_RESET_PIO0_BITS |
        RESETS_RESET_PIO1_BITS |
        RESETS_RESET_UART0_BITS);
}

static void __attribute__((noreturn)) enter_bootsel_recovery(void) {
    reset_usb_boot(0u, 0u);
    for (;;) {
        __asm volatile("wfi");
    }
}
#endif /* !defined(PU_HOST_TEST) */

int main(void) {
#if !defined(PU_HOST_TEST)
    bool recovery_pressed = sample_recovery_button_pressed();
    pu_boot_decision_t decision = pu_bootloader_select_slot(recovery_pressed);

    if (decision.action != PU_BOOT_ACTION_BOOT_SLOT) {
        enter_bootsel_recovery();
    }

    uint32_t slot_addr = (decision.selected_slot == PU_SLOT_A) ? SLOT_APP_A_ADDR : SLOT_APP_B_ADDR;
    uint32_t vtor = slot_addr + PU_IMAGE_HEADER_SIZE;

    (void)save_and_disable_interrupts();
    quiesce_peripherals_for_handover();
    pu_bootloader_handover(vtor, decision.image_info.initial_msp, decision.image_info.entry_point);
#else
    return 0;
#endif
}
