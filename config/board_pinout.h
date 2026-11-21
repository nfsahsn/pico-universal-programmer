/*
 * Canonical RP2040 GPIO & USB identity definitions for Pico Universal.
 * Supports off-the-shelf Raspberry Pi Pico (PU_BOARD_PICO) and the 4-layer
 * production hardware (PU_BOARD_PRO_PCB).
 */

#ifndef PU_BOARD_PINOUT_H
#define PU_BOARD_PINOUT_H

#if defined(PU_BOARD_PICO) && defined(PU_BOARD_PRO_PCB)
#error "Define only one of PU_BOARD_PICO or PU_BOARD_PRO_PCB"
#endif

#if !defined(PU_BOARD_PICO) && !defined(PU_BOARD_PRO_PCB)
#define PU_BOARD_PICO         1
#endif

/* Official Raspberry Pi USB Vendor ID; PU_USB_PID is injected via CMake from
 * the approved PID allocation (0x0000u is a non-production placeholder). */
#define PU_USB_VID            0x2E8Au

#ifndef PU_USB_PID
#define PU_USB_PID            0x0000u
#endif

#if ((0 - PU_USB_PID - 1) == 1) && ((PU_USB_PID + 0) == 0)
#error "PU_USB_PID must not be empty"
#elif ((PU_USB_PID + 0) < 0) || ((PU_USB_PID + 0) > 0xFFFF)
#error "PU_USB_PID must be a valid 16-bit USB Product ID"
#endif

/* Shared target interface & recovery pins */
#define PU_PIN_UART_TX        0u
#define PU_PIN_UART_RX        1u
#define PU_PIN_SWCLK_TCK      2u
#define PU_PIN_SWDIO_TMS      3u
#define PU_PIN_JTAG_TDI       4u
#define PU_PIN_JTAG_TDO       5u
#define PU_PIN_RECOVERY_BTN   15u

#if defined(PU_BOARD_PRO_PCB)
#define PU_HAS_LEVEL_SHIFTER  1
#define PU_HAS_VTARG_SWITCH   1
#define PU_HAS_ADC_TELEMETRY  1
#define PU_PIN_SWDIO_DIR      6u
#define PU_PIN_TARGET_NRST    7u
#define PU_PIN_SWIO           8u
#define PU_PIN_SWIO_PU_EN     9u
#define PU_PIN_VTARG_EN       10u
#define PU_PIN_VTARG_FAULT_N  11u
#define PU_PIN_LED_R          12u
#define PU_PIN_LED_G          13u
#define PU_PIN_LED_B          14u
#define PU_PIN_ADC_VTREF      26u
#define PU_PIN_ADC_ITARG      27u
#else
#define PU_HAS_LEVEL_SHIFTER  0
#define PU_HAS_VTARG_SWITCH   0
#define PU_HAS_ADC_TELEMETRY  0
#define PU_PIN_TARGET_NRST    6u
#define PU_PIN_SWIO           28u
#define PU_PIN_LED_STATUS     25u
#endif

#endif /* PU_BOARD_PINOUT_H */
