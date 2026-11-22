/*
 * Composite USB 2.10 Descriptors, BOS, and Microsoft OS 2.0 (WCID) WinUSB
 * Descriptors for Pico Universal (VID 0x2E8A, PID PU_USB_PID).
 */

#ifndef PU_FIRMWARE_USB_DESCRIPTORS_H
#define PU_FIRMWARE_USB_DESCRIPTORS_H

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("config/board_pinout.h")
#include "config/board_pinout.h"
#else
#include "../config/board_pinout.h"
#endif
#else
#include "config/board_pinout.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Composite USB interface numbering (6 interfaces across EP0..EP6) */
enum {
    ITF_NUM_DAP = 0,
    ITF_NUM_CDC_SWIO = 1,
    ITF_NUM_CDC_SWIO_DATA = 2,
    ITF_NUM_CDC_UART = 3,
    ITF_NUM_CDC_UART_DATA = 4,
    ITF_NUM_CTRL = 5,
    ITF_NUM_TOTAL = 6
};

/* TinyUSB class instance indices */
#define PU_TUD_VENDOR_IDX_DAP       0u
#define PU_TUD_VENDOR_IDX_CTRL      1u
#define PU_TUD_CDC_IDX_SWIO         0u
#define PU_TUD_CDC_IDX_UART         1u

/* Endpoint addresses & packet sizes */
#define PU_EP_DAP_OUT               0x01u
#define PU_EP_DAP_IN                0x81u
#define PU_EP_DAP_SIZE              64u

#define PU_EP_CDC_SWIO_NOTIF        0x82u
#define PU_EP_CDC_SWIO_OUT          0x03u
#define PU_EP_CDC_SWIO_IN           0x83u

#define PU_EP_CDC_UART_NOTIF        0x84u
#define PU_EP_CDC_UART_OUT          0x05u
#define PU_EP_CDC_UART_IN           0x85u

#define PU_EP_CDC_NOTIF_SIZE        8u
#define PU_EP_CDC_DATA_SIZE         64u

#define PU_EP_CTRL_OUT              0x06u
#define PU_EP_CTRL_IN               0x86u
#define PU_EP_CTRL_SIZE             64u

/* USB String Descriptor Indices */
enum {
    PU_STRID_LANGID = 0,
    PU_STRID_MANUFACTURER = 1,
    PU_STRID_PRODUCT = 2,
    PU_STRID_SERIAL = 3,
    PU_STRID_DAP_ITF = 4,
    PU_STRID_CDC_SWIO_ITF = 5,
    PU_STRID_CDC_UART_ITF = 6,
    PU_STRID_CTRL_ITF = 7,
    PU_STRID_COUNT = 8
};

/* Vendor control request code advertised in BOS for MS OS 2.0 Descriptor Set */
#define PU_MS_OS_20_VENDOR_CODE     0x20u
#define PU_MS_OS_20_INDEX           0x07u

/* Pure C descriptor-inspection & serial helpers (available on host & RP2040) */
const uint8_t *pu_usb_get_device_descriptor(void);
const uint8_t *pu_usb_get_config_descriptor(size_t *len);
const uint8_t *pu_usb_get_bos_descriptor(size_t *len);
const uint8_t *pu_usb_get_ms_os_20_descriptor(size_t *len);
void pu_usb_format_serial_hex(const uint8_t uid[8], char out[17]);
const char *pu_usb_get_string_ascii(uint8_t index);
const char *pu_usb_get_serial_string(void);

#ifdef __cplusplus
}
#endif

#endif /* PU_FIRMWARE_USB_DESCRIPTORS_H */
