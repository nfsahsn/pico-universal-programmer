/*
 * Host unit tests for Pico Universal composite USB descriptors, BOS, MS OS 2.0
 * WCID WinUSB descriptor set, and deterministic 16-hex-char serial formatting.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config/board_pinout.h"
#include "firmware/usb_descriptors.h"

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void encode_ascii_to_utf16le(const char *ascii, size_t char_count_with_nulls, uint8_t *out) {
    for (size_t i = 0; i < char_count_with_nulls; ++i) {
        out[2u * i] = (uint8_t)ascii[i];
        out[2u * i + 1u] = 0x00u;
    }
}

static void test_device_descriptor_and_strings(void) {
    const uint8_t *dev = pu_usb_get_device_descriptor();
    assert(dev != NULL);

    assert(dev[0] == 18u);                  /* bLength */
    assert(dev[1] == 0x01u);                /* TUSB_DESC_DEVICE */
    assert(read_le16(&dev[2]) == 0x0210u);  /* bcdUSB = 2.10 for BOS & MS OS 2.0 */
    assert(dev[4] == 0xEFu);                /* TUSB_CLASS_MISC */
    assert(dev[5] == 0x02u);                /* MISC_SUBCLASS_COMMON */
    assert(dev[6] == 0x01u);                /* MISC_PROTOCOL_IAD */
    assert(dev[7] == 64u);                  /* bMaxPacketSize0 */
    assert(read_le16(&dev[8]) == (uint16_t)PU_USB_VID);
    assert(read_le16(&dev[8]) == 0x2E8Au);
    assert(read_le16(&dev[10]) == (uint16_t)PU_USB_PID);
    assert(dev[14] == PU_STRID_MANUFACTURER);
    assert(dev[15] == PU_STRID_PRODUCT);
    assert(dev[16] == PU_STRID_SERIAL);
    assert(dev[17] == 1u);                  /* bNumConfigurations */

    const char *product = pu_usb_get_string_ascii(PU_STRID_PRODUCT);
    assert(product != NULL);
    assert(strcmp(product, "Pico Universal CMSIS-DAP") == 0);
    assert(strncmp(product, "Pico Universal", 14) == 0);
    assert(strstr(product, "CMSIS-DAP") != NULL);

    const char *dap_itf_str = pu_usb_get_string_ascii(PU_STRID_DAP_ITF);
    assert(dap_itf_str != NULL);
    assert(strstr(dap_itf_str, "CMSIS-DAP") != NULL);

    const char *serial = pu_usb_get_serial_string();
    assert(serial != NULL);
    assert(strlen(serial) == 16u);
    for (size_t i = 0; i < 16u; ++i) {
        char c = serial[i];
        assert((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    }
}

typedef struct {
    bool seen;
    uint8_t cls;
    uint8_t subcls;
    uint8_t proto;
    uint8_t num_eps;
    uint8_t str_idx;
    uint8_t ep_addrs[3];
    uint8_t ep_attrs[3];
    uint16_t ep_sizes[3];
    uint8_t ep_seen;
} parsed_itf_t;

typedef struct {
    uint8_t first_itf;
    uint8_t itf_count;
    uint8_t cls;
    uint8_t subcls;
} parsed_iad_t;

static void test_configuration_descriptor_layout(void) {
    assert(ITF_NUM_DAP == 0);
    assert(ITF_NUM_CDC_SWIO == 1);
    assert(ITF_NUM_CDC_SWIO_DATA == 2);
    assert(ITF_NUM_CDC_UART == 3);
    assert(ITF_NUM_CDC_UART_DATA == 4);
    assert(ITF_NUM_CTRL == 5);
    assert(ITF_NUM_TOTAL == 6);

    size_t cfg_len = 0;
    const uint8_t *cfg = pu_usb_get_config_descriptor(&cfg_len);
    assert(cfg != NULL);
    assert(cfg_len >= 9u);

    assert(cfg[0] == 9u);                   /* bLength */
    assert(cfg[1] == 0x02u);                /* TUSB_DESC_CONFIGURATION */
    assert(read_le16(&cfg[2]) == cfg_len);  /* exact wTotalLength match */
    assert(cfg[4] == 6u);                   /* bNumInterfaces */
    assert(cfg[5] == 1u);                   /* bConfigurationValue */
    assert((cfg[7] & 0x80u) != 0u);         /* USB 2.0 reserved bit 7 set */

    parsed_itf_t itfs[6];
    memset(itfs, 0, sizeof(itfs));
    parsed_iad_t iads[4];
    size_t iad_count = 0;
    int current_itf = -1;

    size_t off = 9u;
    while (off < cfg_len) {
        uint8_t dlen = cfg[off];
        assert(dlen >= 2u);
        assert(off + dlen <= cfg_len);
        uint8_t dtype = cfg[off + 1u];

        if (dtype == 0x0Bu) { /* INTERFACE_ASSOCIATION */
            assert(dlen == 8u);
            assert(iad_count < 4u);
            iads[iad_count].first_itf = cfg[off + 2u];
            iads[iad_count].itf_count = cfg[off + 3u];
            iads[iad_count].cls = cfg[off + 4u];
            iads[iad_count].subcls = cfg[off + 5u];
            iad_count++;
        } else if (dtype == 0x04u) { /* INTERFACE */
            assert(dlen == 9u);
            uint8_t inum = cfg[off + 2u];
            uint8_t alt = cfg[off + 3u];
            assert(inum < 6u);
            assert(alt == 0u);
            assert(!itfs[inum].seen);
            itfs[inum].seen = true;
            itfs[inum].num_eps = cfg[off + 4u];
            itfs[inum].cls = cfg[off + 5u];
            itfs[inum].subcls = cfg[off + 6u];
            itfs[inum].proto = cfg[off + 7u];
            itfs[inum].str_idx = cfg[off + 8u];
            current_itf = (int)inum;
        } else if (dtype == 0x05u) { /* ENDPOINT */
            assert(dlen == 7u);
            assert(current_itf >= 0 && current_itf < 6);
            parsed_itf_t *itf = &itfs[current_itf];
            assert(itf->ep_seen < 3u);
            itf->ep_addrs[itf->ep_seen] = cfg[off + 2u];
            itf->ep_attrs[itf->ep_seen] = cfg[off + 3u];
            itf->ep_sizes[itf->ep_seen] = read_le16(&cfg[off + 4u]);
            itf->ep_seen++;
        }

        off += dlen;
    }
    assert(off == cfg_len);

    for (int i = 0; i < 6; ++i) {
        assert(itfs[i].seen);
        assert(itfs[i].ep_seen == itfs[i].num_eps);
    }

    /* Two CDC IADs: IF 1..2 (SWIO GDB) and IF 3..4 (Target UART) */
    assert(iad_count == 2u);
    assert(iads[0].first_itf == ITF_NUM_CDC_SWIO && iads[0].itf_count == 2u);
    assert(iads[0].cls == 0x02u && iads[0].subcls == 0x02u);
    assert(iads[1].first_itf == ITF_NUM_CDC_UART && iads[1].itf_count == 2u);
    assert(iads[1].cls == 0x02u && iads[1].subcls == 0x02u);

    /* IF 0: CMSIS-DAP v2 Bulk Vendor (EP1 OUT 0x01 first, EP1 IN 0x81 second) */
    assert(itfs[0].cls == 0xFFu && itfs[0].subcls == 0x00u && itfs[0].proto == 0x00u);
    assert(itfs[0].num_eps == 2u);
    assert(itfs[0].ep_addrs[0] == 0x01u && itfs[0].ep_attrs[0] == 0x02u && itfs[0].ep_sizes[0] == 64u);
    assert(itfs[0].ep_addrs[1] == 0x81u && itfs[0].ep_attrs[1] == 0x02u && itfs[0].ep_sizes[1] == 64u);

    /* IF 1..2: CDC SWIO GDB / PUID1 (EP2 IN 0x82 INT, EP3 OUT 0x03, EP3 IN 0x83) */
    assert(itfs[1].cls == 0x02u && itfs[1].subcls == 0x02u && itfs[1].num_eps == 1u);
    assert(itfs[1].ep_addrs[0] == 0x82u && itfs[1].ep_attrs[0] == 0x03u);
    assert(itfs[2].cls == 0x0Au && itfs[2].num_eps == 2u);
    assert(itfs[2].ep_addrs[0] == 0x03u && itfs[2].ep_attrs[0] == 0x02u && itfs[2].ep_sizes[0] == 64u);
    assert(itfs[2].ep_addrs[1] == 0x83u && itfs[2].ep_attrs[1] == 0x02u && itfs[2].ep_sizes[1] == 64u);

    /* IF 3..4: CDC Target UART (EP4 IN 0x84 INT, EP5 OUT 0x05, EP5 IN 0x85) */
    assert(itfs[3].cls == 0x02u && itfs[3].subcls == 0x02u && itfs[3].num_eps == 1u);
    assert(itfs[3].ep_addrs[0] == 0x84u && itfs[3].ep_attrs[0] == 0x03u);
    assert(itfs[4].cls == 0x0Au && itfs[4].num_eps == 2u);
    assert(itfs[4].ep_addrs[0] == 0x05u && itfs[4].ep_attrs[0] == 0x02u && itfs[4].ep_sizes[0] == 64u);
    assert(itfs[4].ep_addrs[1] == 0x85u && itfs[4].ep_attrs[1] == 0x02u && itfs[4].ep_sizes[1] == 64u);

    /* IF 5: Control/Telemetry Vendor (EP6 OUT 0x06, EP6 IN 0x86, 64 B Bulk) */
    assert(itfs[5].cls == 0xFFu && itfs[5].subcls == 0x00u && itfs[5].proto == 0x00u);
    assert(itfs[5].num_eps == 2u);
    assert(itfs[5].ep_addrs[0] == 0x06u && itfs[5].ep_attrs[0] == 0x02u && itfs[5].ep_sizes[0] == 64u);
    assert(itfs[5].ep_addrs[1] == 0x86u && itfs[5].ep_attrs[1] == 0x02u && itfs[5].ep_sizes[1] == 64u);
}

static void verify_ms_os_20_function_subset(const uint8_t *sub,
                                            size_t sub_len,
                                            uint8_t expected_itf,
                                            uint8_t guid_utf16_out[80]) {
    assert(sub_len == 160u);
    assert(read_le16(&sub[0]) == 8u);
    assert(read_le16(&sub[2]) == 0x0002u); /* MS_OS_20_SUBSET_HEADER_FUNCTION */
    assert(sub[4] == expected_itf);
    assert(sub[5] == 0u);
    assert(read_le16(&sub[6]) == sub_len);

    /* Compatible ID descriptor (offset 8, length 20) */
    const uint8_t *cid = sub + 8u;
    assert(read_le16(&cid[0]) == 20u);
    assert(read_le16(&cid[2]) == 0x0003u); /* MS_OS_20_FEATURE_COMPATBLE_ID */
    assert(memcmp(&cid[4], "WINUSB\0\0", 8u) == 0);
    for (size_t i = 12u; i < 20u; ++i) {
        assert(cid[i] == 0x00u);
    }

    /* Registry property descriptor (offset 28, length 132) */
    const uint8_t *reg = sub + 28u;
    assert(read_le16(&reg[0]) == 132u);
    assert(read_le16(&reg[2]) == 0x0004u); /* MS_OS_20_FEATURE_REG_PROPERTY */
    assert(read_le16(&reg[4]) == 0x0007u); /* REG_MULTI_SZ */
    assert(read_le16(&reg[6]) == 42u);     /* "DeviceInterfaceGUIDs\0" in UTF-16LE */

    uint8_t expected_prop_name[42];
    encode_ascii_to_utf16le("DeviceInterfaceGUIDs", 21u, expected_prop_name);
    assert(memcmp(&reg[8], expected_prop_name, 42u) == 0);

    assert(read_le16(&reg[50]) == 80u);    /* "{GUID}\0\0" in UTF-16LE */
    memcpy(guid_utf16_out, &reg[52], 80u);
    /* Must start with '{' and end with "}\0\0" in UTF-16LE */
    assert(guid_utf16_out[0] == '{' && guid_utf16_out[1] == 0x00u);
    assert(guid_utf16_out[74] == '}' && guid_utf16_out[75] == 0x00u);
    assert(guid_utf16_out[76] == 0x00u && guid_utf16_out[77] == 0x00u);
    assert(guid_utf16_out[78] == 0x00u && guid_utf16_out[79] == 0x00u);
}

static void test_bos_and_ms_os_20_wcid_descriptors(void) {
    size_t bos_len = 0;
    const uint8_t *bos = pu_usb_get_bos_descriptor(&bos_len);
    size_t ms_len = 0;
    const uint8_t *ms = pu_usb_get_ms_os_20_descriptor(&ms_len);

    assert(bos != NULL && ms != NULL);
    assert(bos_len == 33u);
    assert(bos[0] == 5u);
    assert(bos[1] == 0x0Fu);                /* TUSB_DESC_BOS */
    assert(read_le16(&bos[2]) == bos_len);
    assert(bos[4] == 1u);                   /* bNumDeviceCaps */

    /* MS OS 2.0 Platform Capability */
    assert(bos[5] == 28u);
    assert(bos[6] == 0x10u);                /* TUSB_DESC_DEVICE_CAPABILITY */
    assert(bos[7] == 0x05u);                /* DEVICE_CAPABILITY_PLATFORM */
    assert(bos[8] == 0x00u);

    static const uint8_t kMsOs20Uuid[16] = {
        0xDFu, 0x60u, 0xDDu, 0xD8u, 0x89u, 0x45u, 0xC7u, 0x4Cu,
        0x9Cu, 0xD2u, 0x65u, 0x9Du, 0x9Eu, 0x64u, 0x8Au, 0x9Fu
    };
    assert(memcmp(&bos[9], kMsOs20Uuid, 16u) == 0);
    assert(read_le32(&bos[25]) == 0x06030000u);
    assert(read_le16(&bos[29]) == ms_len);
    assert(bos[31] == PU_MS_OS_20_VENDOR_CODE);
    assert(bos[31] != 0u);
    assert(bos[32] == 0x00u);

    /* MS OS 2.0 Descriptor Set Header + Configuration Subset + 2 Function Subsets */
    assert(ms_len == 338u);
    assert(read_le16(&ms[0]) == 10u);
    assert(read_le16(&ms[2]) == 0x0000u);   /* MS_OS_20_SET_HEADER_DESCRIPTOR */
    assert(read_le32(&ms[4]) == 0x06030000u);
    assert(read_le16(&ms[8]) == ms_len);

    assert(read_le16(&ms[10]) == 8u);
    assert(read_le16(&ms[12]) == 0x0001u);  /* MS_OS_20_SUBSET_HEADER_CONFIGURATION */
    assert(ms[14] == 0u);
    assert(ms[15] == 0u);
    assert(read_le16(&ms[16]) == ms_len - 10u);

    uint8_t guid_if0[80];
    uint8_t guid_if5[80];
    verify_ms_os_20_function_subset(ms + 18u, 160u, ITF_NUM_DAP, guid_if0);
    verify_ms_os_20_function_subset(ms + 178u, 160u, ITF_NUM_CTRL, guid_if5);

    /* Exact ARM CMSIS-DAP v2 DeviceInterfaceGUID on IF 0 */
    uint8_t expected_dap_guid[80];
    static const char kArmCmsisDapGuid[40] = "{CDB3B5AD-293B-4663-AA36-1AAE46463776}\0";
    encode_ascii_to_utf16le(kArmCmsisDapGuid, 40u, expected_dap_guid);
    assert(memcmp(guid_if0, expected_dap_guid, 80u) == 0);

    /* IF 5 Control/Telemetry must have a distinct valid GUID */
    assert(memcmp(guid_if0, guid_if5, 80u) != 0);
}

static void test_serial_hex_formatting(void) {
    const uint8_t sample_uid[8] = {0xE6u, 0x61u, 0x41u, 0x03u, 0xE7u, 0x1Au, 0x9Bu, 0x0Fu};
    char out[17];
    memset(out, 'X', sizeof(out));
    pu_usb_format_serial_hex(sample_uid, out);
    assert(strcmp(out, "E6614103E71A9B0F") == 0);

    const uint8_t zero_uid[8] = {0};
    pu_usb_format_serial_hex(zero_uid, out);
    assert(strcmp(out, "0000000000000000") == 0);

    const uint8_t ff_uid[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
    pu_usb_format_serial_hex(ff_uid, out);
    assert(strcmp(out, "FFFFFFFFFFFFFFFF") == 0);

    /* Null pointer guards */
    pu_usb_format_serial_hex(NULL, out);
    assert(strcmp(out, "0000000000000000") == 0);
    pu_usb_format_serial_hex(sample_uid, NULL);
}

int main(void) {
    test_device_descriptor_and_strings();
    test_configuration_descriptor_layout();
    test_bos_and_ms_os_20_wcid_descriptors();
    test_serial_hex_formatting();
    puts("test_usb_descriptors: ALL PASSED");
    return 0;
}
