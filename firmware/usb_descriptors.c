/*
 * Composite USB 2.10 Descriptors, BOS, and Microsoft OS 2.0 (WCID) WinUSB
 * Descriptors for Pico Universal (VID 0x2E8A, PID PU_USB_PID).
 */

#include "firmware/usb_descriptors.h"

#include <stdbool.h>
#include <string.h>

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#if !defined(PU_HOST_TEST)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "firmware/tusb_config.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "tusb.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#define PU_U16_LE(v) (uint8_t)((uint16_t)(v) & 0xFFu), (uint8_t)(((uint16_t)(v) >> 8) & 0xFFu)
#define PU_U32_LE(v) \
    (uint8_t)((uint32_t)(v) & 0xFFu), \
    (uint8_t)(((uint32_t)(v) >> 8) & 0xFFu), \
    (uint8_t)(((uint32_t)(v) >> 16) & 0xFFu), \
    (uint8_t)(((uint32_t)(v) >> 24) & 0xFFu)

/* Descriptor lengths */
#define PU_DESC_CONFIG_HDR_LEN      9u
#define PU_DESC_VENDOR_ITF_LEN      23u
#define PU_DESC_CDC_ITF_LEN         66u
#define PU_CONFIG_TOTAL_LEN \
    (PU_DESC_CONFIG_HDR_LEN + PU_DESC_VENDOR_ITF_LEN + PU_DESC_CDC_ITF_LEN + \
     PU_DESC_CDC_ITF_LEN + PU_DESC_VENDOR_ITF_LEN)

#define PU_BOS_TOTAL_LEN            (5u + 28u)

#define PU_MS_OS_20_SET_HDR_LEN     10u
#define PU_MS_OS_20_CFG_HDR_LEN     8u
#define PU_MS_OS_20_FUNC_HDR_LEN    8u
#define PU_MS_OS_20_COMPAT_ID_LEN   20u
#define PU_MS_OS_20_REG_PROP_LEN    132u
#define PU_MS_OS_20_FUNC_SUBSET_LEN \
    (PU_MS_OS_20_FUNC_HDR_LEN + PU_MS_OS_20_COMPAT_ID_LEN + PU_MS_OS_20_REG_PROP_LEN)
#define PU_MS_OS_20_CFG_SUBSET_LEN \
    (PU_MS_OS_20_CFG_HDR_LEN + 2u * PU_MS_OS_20_FUNC_SUBSET_LEN)
#define PU_MS_OS_20_TOTAL_LEN \
    (PU_MS_OS_20_SET_HDR_LEN + PU_MS_OS_20_CFG_SUBSET_LEN)

/* Standard USB Device Descriptor (18 bytes, bcdUSB 2.10, IAD Misc class) */
static const uint8_t s_desc_device[18] = {
    18u,                            /* bLength */
    0x01u,                          /* bDescriptorType: DEVICE */
    PU_U16_LE(0x0210u),             /* bcdUSB: USB 2.10 (required for BOS / MS OS 2.0) */
    0xEFu,                          /* bDeviceClass: TUSB_CLASS_MISC */
    0x02u,                          /* bDeviceSubClass: MISC_SUBCLASS_COMMON */
    0x01u,                          /* bDeviceProtocol: MISC_PROTOCOL_IAD */
    64u,                            /* bMaxPacketSize0 */
    PU_U16_LE(PU_USB_VID),          /* idVendor: 0x2E8A (Raspberry Pi) */
    PU_U16_LE(PU_USB_PID),          /* idProduct: approved PU_USB_PID */
    PU_U16_LE(0x0100u),             /* bcdDevice: 1.00 */
    PU_STRID_MANUFACTURER,          /* iManufacturer */
    PU_STRID_PRODUCT,               /* iProduct */
    PU_STRID_SERIAL,                /* iSerialNumber */
    0x01u                           /* bNumConfigurations */
};

#define PU_VENDOR_ITF_DESC(itf_num, str_idx, ep_out, ep_in, ep_size) \
    9u, 0x04u, (itf_num), 0x00u, 0x02u, 0xFFu, 0x00u, 0x00u, (str_idx), \
    /* Bulk OUT endpoint FIRST per ARM CMSIS-DAP v2 specification */ \
    7u, 0x05u, (ep_out), 0x02u, PU_U16_LE(ep_size), 0x00u, \
    /* Bulk IN endpoint SECOND */ \
    7u, 0x05u, (ep_in), 0x02u, PU_U16_LE(ep_size), 0x00u

#define PU_CDC_ACM_ITF_DESC(itf_num, str_idx, ep_notif, ep_notif_sz, ep_out, ep_in, ep_sz) \
    /* Interface Association Descriptor (IAD) */ \
    8u, 0x0Bu, (itf_num), 0x02u, 0x02u, 0x02u, 0x00u, 0x00u, \
    /* CDC Communication Interface */ \
    9u, 0x04u, (itf_num), 0x00u, 0x01u, 0x02u, 0x02u, 0x00u, (str_idx), \
    /* CDC Header Functional Descriptor (bcdCDC 1.20) */ \
    5u, 0x24u, 0x00u, PU_U16_LE(0x0120u), \
    /* CDC Call Management Functional Descriptor */ \
    5u, 0x24u, 0x01u, 0x00u, (uint8_t)((itf_num) + 1u), \
    /* CDC ACM Functional Descriptor (line coding + send break) */ \
    4u, 0x24u, 0x02u, 0x06u, \
    /* CDC Union Functional Descriptor */ \
    5u, 0x24u, 0x06u, (itf_num), (uint8_t)((itf_num) + 1u), \
    /* Notification Interrupt IN Endpoint */ \
    7u, 0x05u, (ep_notif), 0x03u, PU_U16_LE(ep_notif_sz), 16u, \
    /* CDC Data Interface */ \
    9u, 0x04u, (uint8_t)((itf_num) + 1u), 0x00u, 0x02u, 0x0Au, 0x00u, 0x00u, 0x00u, \
    /* Data Bulk OUT Endpoint */ \
    7u, 0x05u, (ep_out), 0x02u, PU_U16_LE(ep_sz), 0x00u, \
    /* Data Bulk IN Endpoint */ \
    7u, 0x05u, (ep_in), 0x02u, PU_U16_LE(ep_sz), 0x00u

/* Full-Speed Composite Configuration Descriptor (187 bytes across IF 0..5) */
static const uint8_t s_desc_configuration[PU_CONFIG_TOTAL_LEN] = {
    /* Configuration Header (500 mA bus power matching 500 mA PPTC polyfuse) */
    9u, 0x02u, PU_U16_LE(PU_CONFIG_TOTAL_LEN), ITF_NUM_TOTAL, 0x01u, 0x00u, 0x80u, 250u,

    /* IF 0: CMSIS-DAP v2 Bulk Vendor (EP1 OUT 0x01, EP1 IN 0x81) */
    PU_VENDOR_ITF_DESC(ITF_NUM_DAP, PU_STRID_DAP_ITF,
                       PU_EP_DAP_OUT, PU_EP_DAP_IN, PU_EP_DAP_SIZE),

    /* IF 1..2: CH32V003 SWIO GDB / PUID1 CDC-ACM (EP2 IN 0x82, EP3 OUT 0x03, EP3 IN 0x83) */
    PU_CDC_ACM_ITF_DESC(ITF_NUM_CDC_SWIO, PU_STRID_CDC_SWIO_ITF,
                        PU_EP_CDC_SWIO_NOTIF, PU_EP_CDC_NOTIF_SIZE,
                        PU_EP_CDC_SWIO_OUT, PU_EP_CDC_SWIO_IN, PU_EP_CDC_DATA_SIZE),

    /* IF 3..4: Target UART Bridge CDC-ACM (EP4 IN 0x84, EP5 OUT 0x05, EP5 IN 0x85) */
    PU_CDC_ACM_ITF_DESC(ITF_NUM_CDC_UART, PU_STRID_CDC_UART_ITF,
                        PU_EP_CDC_UART_NOTIF, PU_EP_CDC_NOTIF_SIZE,
                        PU_EP_CDC_UART_OUT, PU_EP_CDC_UART_IN, PU_EP_CDC_DATA_SIZE),

    /* IF 5: Control, Telemetry & Diagnostics Vendor (EP6 OUT 0x06, EP6 IN 0x86) */
    PU_VENDOR_ITF_DESC(ITF_NUM_CTRL, PU_STRID_CTRL_ITF,
                       PU_EP_CTRL_OUT, PU_EP_CTRL_IN, PU_EP_CTRL_SIZE)
};

/* Binary Device Object Store (BOS) Descriptor advertising MS OS 2.0 WCID */
static const uint8_t s_desc_bos[PU_BOS_TOTAL_LEN] = {
    /* BOS Header (5 bytes) */
    5u, 0x0Fu, PU_U16_LE(PU_BOS_TOTAL_LEN), 1u,

    /* MS OS 2.0 Platform Capability Descriptor (28 bytes) */
    28u, 0x10u, 0x05u, 0x00u,
    /* MS OS 2.0 Platform Capability UUID: {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} */
    0xDFu, 0x60u, 0xDDu, 0xD8u, 0x89u, 0x45u, 0xC7u, 0x4Cu,
    0x9Cu, 0xD2u, 0x65u, 0x9Du, 0x9Eu, 0x64u, 0x8Au, 0x9Fu,
    /* dwWindowsVersion: Windows 8.1+ (0x06030000) */
    PU_U32_LE(0x06030000u),
    /* wMSOSDescriptorSetTotalLength, bMS_VendorCode, bAltEnumCode */
    PU_U16_LE(PU_MS_OS_20_TOTAL_LEN), PU_MS_OS_20_VENDOR_CODE, 0x00u
};

#define PU_UTF16LE_PROP_DEVICE_INTERFACE_GUIDS \
    'D', 0x00u, 'e', 0x00u, 'v', 0x00u, 'i', 0x00u, 'c', 0x00u, 'e', 0x00u, 'I', 0x00u, \
    'n', 0x00u, 't', 0x00u, 'e', 0x00u, 'r', 0x00u, 'f', 0x00u, 'a', 0x00u, 'c', 0x00u, \
    'e', 0x00u, 'G', 0x00u, 'U', 0x00u, 'I', 0x00u, 'D', 0x00u, 's', 0x00u, 0x00u, 0x00u

/*
 * MS OS 2.0 Function Subset helper:
 *   - Function Subset Header (8B)
 *   - Compatible ID "WINUSB\0\0" (20B)
 *   - Registry Property REG_MULTI_SZ "DeviceInterfaceGUIDs\0" (132B)
 */
#define PU_MS_OS_20_WINUSB_FUNCTION(first_itf, ...) \
    PU_U16_LE(PU_MS_OS_20_FUNC_HDR_LEN), PU_U16_LE(0x0002u), \
    (first_itf), 0x00u, PU_U16_LE(PU_MS_OS_20_FUNC_SUBSET_LEN), \
    PU_U16_LE(PU_MS_OS_20_COMPAT_ID_LEN), PU_U16_LE(0x0003u), \
    'W', 'I', 'N', 'U', 'S', 'B', 0x00u, 0x00u, \
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, \
    PU_U16_LE(PU_MS_OS_20_REG_PROP_LEN), PU_U16_LE(0x0004u), \
    PU_U16_LE(0x0007u), PU_U16_LE(0x002Au), \
    PU_UTF16LE_PROP_DEVICE_INTERFACE_GUIDS, \
    PU_U16_LE(0x0050u), \
    __VA_ARGS__

/*
 * Microsoft OS 2.0 WCID Descriptor Set (338 bytes):
 *   - IF 0 (CMSIS-DAP v2): ARM standard GUID {CDB3B5AD-293B-4663-AA36-1AAE46463776}
 *   - IF 5 (Control/Telemetry): Pico Universal GUID {50554354-2E8A-4D53-9A05-1AAE46463777}
 */
static const uint8_t s_desc_ms_os_20[PU_MS_OS_20_TOTAL_LEN] = {
    /* Descriptor Set Header (10B) */
    PU_U16_LE(PU_MS_OS_20_SET_HDR_LEN), PU_U16_LE(0x0000u),
    PU_U32_LE(0x06030000u), PU_U16_LE(PU_MS_OS_20_TOTAL_LEN),

    /* Configuration Subset Header (8B, configuration index 0) */
    PU_U16_LE(PU_MS_OS_20_CFG_HDR_LEN), PU_U16_LE(0x0001u),
    0x00u, 0x00u, PU_U16_LE(PU_MS_OS_20_CFG_SUBSET_LEN),

    /* Function Subset 0: IF 0 CMSIS-DAP v2 -> {CDB3B5AD-293B-4663-AA36-1AAE46463776}\0\0 */
    PU_MS_OS_20_WINUSB_FUNCTION(
        ITF_NUM_DAP,
        '{', 0x00u, 'C', 0x00u, 'D', 0x00u, 'B', 0x00u, '3', 0x00u, 'B', 0x00u, '5', 0x00u, 'A', 0x00u, 'D', 0x00u, '-', 0x00u,
        '2', 0x00u, '9', 0x00u, '3', 0x00u, 'B', 0x00u, '-', 0x00u, '4', 0x00u, '6', 0x00u, '6', 0x00u, '3', 0x00u, '-', 0x00u,
        'A', 0x00u, 'A', 0x00u, '3', 0x00u, '6', 0x00u, '-', 0x00u, '1', 0x00u, 'A', 0x00u, 'A', 0x00u, 'E', 0x00u, '4', 0x00u,
        '6', 0x00u, '4', 0x00u, '6', 0x00u, '3', 0x00u, '7', 0x00u, '7', 0x00u, '6', 0x00u, '}', 0x00u, 0x00u, 0x00u, 0x00u, 0x00u),

    /* Function Subset 1: IF 5 Control/Telemetry -> {50554354-2E8A-4D53-9A05-1AAE46463777}\0\0 */
    PU_MS_OS_20_WINUSB_FUNCTION(
        ITF_NUM_CTRL,
        '{', 0x00u, '5', 0x00u, '0', 0x00u, '5', 0x00u, '5', 0x00u, '4', 0x00u, '3', 0x00u, '5', 0x00u, '4', 0x00u, '-', 0x00u,
        '2', 0x00u, 'E', 0x00u, '8', 0x00u, 'A', 0x00u, '-', 0x00u, '4', 0x00u, 'D', 0x00u, '5', 0x00u, '3', 0x00u, '-', 0x00u,
        '9', 0x00u, 'A', 0x00u, '0', 0x00u, '5', 0x00u, '-', 0x00u, '1', 0x00u, 'A', 0x00u, 'A', 0x00u, 'E', 0x00u, '4', 0x00u,
        '6', 0x00u, '4', 0x00u, '6', 0x00u, '3', 0x00u, '7', 0x00u, '7', 0x00u, '7', 0x00u, '}', 0x00u, 0x00u, 0x00u, 0x00u, 0x00u)
};

#if defined(__cplusplus)
static_assert(sizeof(s_desc_configuration) == 187u, "Unexpected configuration descriptor size");
static_assert(sizeof(s_desc_bos) == 33u, "Unexpected BOS descriptor size");
static_assert(sizeof(s_desc_ms_os_20) == 338u, "Unexpected MS OS 2.0 descriptor size");
#else
_Static_assert(sizeof(s_desc_configuration) == 187u, "Unexpected configuration descriptor size");
_Static_assert(sizeof(s_desc_bos) == 33u, "Unexpected BOS descriptor size");
_Static_assert(sizeof(s_desc_ms_os_20) == 338u, "Unexpected MS OS 2.0 descriptor size");
#endif

#if !defined(PU_HOST_TEST)
_Static_assert(PU_CONFIG_TOTAL_LEN ==
                   (TUD_CONFIG_DESC_LEN + 2u * TUD_CDC_DESC_LEN + 2u * TUD_VENDOR_DESC_LEN),
               "Configuration length mismatch vs TinyUSB templates");
_Static_assert(PU_BOS_TOTAL_LEN == (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN),
               "BOS length mismatch vs TinyUSB templates");
#endif

static char s_serial_hex[17];
static bool s_serial_initialized = false;

void pu_usb_format_serial_hex(const uint8_t uid[8], char out[17]) {
    static const char kHexDigits[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
    };

    if (out == NULL) {
        return;
    }
    if (uid == NULL) {
        memset(out, '0', 16u);
        out[16] = '\0';
        return;
    }

    for (size_t i = 0u; i < 8u; ++i) {
        uint8_t byte = uid[i];
        out[2u * i] = kHexDigits[(byte >> 4) & 0x0Fu];
        out[2u * i + 1u] = kHexDigits[byte & 0x0Fu];
    }
    out[16] = '\0';
}

#if !defined(PU_HOST_TEST)
/*
 * Cache QSPI NOR flash 64-bit unique ID into SRAM before main()/Core 1/USB IRQs
 * start so runtime USB string queries never stall XIP flash.
 */
static void __attribute__((constructor)) pu_usb_cache_hardware_serial_on_boot(void) {
    uint8_t uid[8] = {0};
    uint32_t saved_irq = save_and_disable_interrupts();
    flash_get_unique_id(uid);
    restore_interrupts(saved_irq);
    pu_usb_format_serial_hex(uid, s_serial_hex);
    s_serial_initialized = true;
}
#endif

const char *pu_usb_get_serial_string(void) {
    if (!s_serial_initialized) {
#if defined(PU_HOST_TEST)
        static const uint8_t kHostTestUid[8] = {
            0xE6u, 0x60u, 0x58u, 0x38u, 0x83u, 0x27u, 0x86u, 0x28u
        };
        pu_usb_format_serial_hex(kHostTestUid, s_serial_hex);
        s_serial_initialized = true;
#else
        pu_usb_cache_hardware_serial_on_boot();
#endif
    }
    return s_serial_hex;
}

const char *pu_usb_get_string_ascii(uint8_t index) {
    switch (index) {
    case PU_STRID_MANUFACTURER:
        return "Pico Universal";
    case PU_STRID_PRODUCT:
        return "Pico Universal CMSIS-DAP";
    case PU_STRID_SERIAL:
        return pu_usb_get_serial_string();
    case PU_STRID_DAP_ITF:
        return "Pico Universal CMSIS-DAP v2";
    case PU_STRID_CDC_SWIO_ITF:
        return "Pico Universal CH32 SWIO GDB";
    case PU_STRID_CDC_UART_ITF:
        return "Pico Universal Target UART";
    case PU_STRID_CTRL_ITF:
        return "Pico Universal Control";
    default:
        return NULL;
    }
}

const uint8_t *pu_usb_get_device_descriptor(void) {
    return s_desc_device;
}

const uint8_t *pu_usb_get_config_descriptor(size_t *len) {
    if (len != NULL) {
        *len = sizeof(s_desc_configuration);
    }
    return s_desc_configuration;
}

const uint8_t *pu_usb_get_bos_descriptor(size_t *len) {
    if (len != NULL) {
        *len = sizeof(s_desc_bos);
    }
    return s_desc_bos;
}

const uint8_t *pu_usb_get_ms_os_20_descriptor(size_t *len) {
    if (len != NULL) {
        *len = sizeof(s_desc_ms_os_20);
    }
    return s_desc_ms_os_20;
}

#if !defined(PU_HOST_TEST)
static uint16_t s_desc_str_utf16[32u + 1u];

uint8_t const *tud_descriptor_device_cb(void) {
    return pu_usb_get_device_descriptor();
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    if (index != 0u) {
        return NULL;
    }
    return pu_usb_get_config_descriptor(NULL);
}

uint8_t const *tud_descriptor_bos_cb(void) {
    return pu_usb_get_bos_descriptor(NULL);
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count = 0u;

    if (index == PU_STRID_LANGID) {
        s_desc_str_utf16[1] = 0x0409u; /* English (US) */
        chr_count = 1u;
    } else {
        const char *ascii = pu_usb_get_string_ascii(index);
        if (ascii == NULL) {
            return NULL;
        }
        chr_count = strlen(ascii);
        const size_t max_chars = (sizeof(s_desc_str_utf16) / sizeof(s_desc_str_utf16[0])) - 1u;
        if (chr_count > max_chars) {
            chr_count = max_chars;
        }
        for (size_t i = 0u; i < chr_count; ++i) {
            s_desc_str_utf16[1u + i] = (uint16_t)(uint8_t)ascii[i];
        }
    }

    s_desc_str_utf16[0] = (uint16_t)(((uint16_t)TUSB_DESC_STRING << 8) | (uint16_t)(2u * chr_count + 2u));
    return s_desc_str_utf16;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport,
                                uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request == NULL) {
        return false;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->bRequest == PU_MS_OS_20_VENDOR_CODE &&
        request->wIndex == PU_MS_OS_20_INDEX) {
        size_t ms_len = 0u;
        const uint8_t *ms_desc = pu_usb_get_ms_os_20_descriptor(&ms_len);
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)ms_desc, (uint16_t)ms_len);
    }

    return false;
}
#endif /* !defined(PU_HOST_TEST) */
