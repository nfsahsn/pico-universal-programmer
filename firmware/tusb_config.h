/*
 * TinyUSB device configuration for Pico Universal composite USB probe:
 *   - 2 Vendor Bulk interfaces: IF 0 (CMSIS-DAP v2), IF 5 (Control/Telemetry)
 *   - 2 CDC-ACM interfaces:     IF 1..2 (CH32 SWIO GDB/PUID1), IF 3..4 (Target UART)
 */

#ifndef PU_FIRMWARE_TUSB_CONFIG_H
#define PU_FIRMWARE_TUSB_CONFIG_H

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT          0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU              OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG            0
#endif

#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#endif

#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         BOARD_TUD_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

/* Composite device classes: 2 Vendor (DAP v2 + Control) + 2 CDC (SWIO + UART) */
#define CFG_TUD_VENDOR            2
#define CFG_TUD_CDC               2
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0

/* Preserve discrete 64-byte CMSIS-DAPv2 & IF 5 Bulk packet boundaries:
 *   - RX_BUFSIZE = 64 holds EP OUT in USB NAK until the prior packet is drained,
 *     preventing byte-stream merging across pipelined Bulk OUT packets.
 *   - TX_BUFSIZE = 0 writes directly to EP IN buffer (1 packet per transfer),
 *     preventing response coalescing and spurious 64-byte ZLP injection. */
#define CFG_TUD_VENDOR_EPSIZE     64
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 0

#define CFG_TUD_CDC_EP_BUFSIZE    64
#define CFG_TUD_CDC_RX_BUFSIZE    512
#define CFG_TUD_CDC_TX_BUFSIZE    512

#ifdef __cplusplus
}
#endif

#endif /* PU_FIRMWARE_TUSB_CONFIG_H */
