#include "bootloader/image_validator.h"

#include <string.h>

#include "bootloader/crc32.h"
#include "bootloader/image_format.h"
#include "config/flash_layout.h"

#define PU_RP2040_SRAM_BASE      (0x20000000u)
#define PU_RP2040_SRAM_TOP       (0x20042000u)
#define PU_MIN_PAYLOAD_SIZE      (16u)
#define PU_DESC_CRC_DATA_BYTES   (28u)

#ifdef PU_HOST_TEST
static const uint8_t *s_test_flash = NULL;
static size_t s_test_flash_size = 0u;

void pu_image_bind_test_flash(const uint8_t *flash_buf, size_t flash_size) {
    s_test_flash = flash_buf;
    s_test_flash_size = flash_size;
}
#endif

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static const uint8_t *resolve_slot_ptr(uint32_t slot_addr, uint32_t slot_size) {
    if ((slot_addr & (FLASH_SECTOR_SIZE - 1u)) != 0u) {
        return NULL;
    }
    if (slot_addr < FLASH_BASE_ADDR) {
        return NULL;
    }
    if (slot_size < (PU_IMAGE_HEADER_SIZE + PU_MIN_PAYLOAD_SIZE) || slot_size > FLASH_TOTAL_SIZE) {
        return NULL;
    }

    uint32_t offset = slot_addr - FLASH_BASE_ADDR;
    if (offset > FLASH_TOTAL_SIZE - slot_size) {
        return NULL;
    }

#ifdef PU_HOST_TEST
    if (s_test_flash == NULL || (size_t)slot_size > s_test_flash_size) {
        return NULL;
    }
    if ((size_t)offset > s_test_flash_size - (size_t)slot_size) {
        return NULL;
    }
    return &s_test_flash[offset];
#else
    return (const uint8_t *)(uintptr_t)slot_addr;
#endif
}

bool pu_image_validate_slot(uint32_t slot_addr, uint32_t slot_size, pu_image_info_t *info) {
    const uint8_t *slot_bytes = resolve_slot_ptr(slot_addr, slot_size);
    if (slot_bytes == NULL) {
        return false;
    }

    uint32_t magic = read_u32_le(slot_bytes + 0u);
    uint32_t header_version = read_u32_le(slot_bytes + 4u);
    uint32_t image_version = read_u32_le(slot_bytes + 8u);
    uint32_t desc_slot_addr = read_u32_le(slot_bytes + 12u);
    uint32_t image_size = read_u32_le(slot_bytes + 16u);
    uint32_t payload_crc32 = read_u32_le(slot_bytes + 20u);
    uint32_t flags = read_u32_le(slot_bytes + 24u);
    uint32_t header_crc32 = read_u32_le(slot_bytes + 28u);

    if (magic != PU_IMAGE_DESC_MAGIC ||
        header_version != PU_IMAGE_DESC_VERSION ||
        desc_slot_addr != slot_addr ||
        flags != 0u) {
        return false;
    }

    uint32_t max_payload = slot_size - PU_IMAGE_HEADER_SIZE;
    if (image_size < PU_MIN_PAYLOAD_SIZE ||
        (image_size & 1u) != 0u ||
        image_size > max_payload) {
        return false;
    }

    if (pu_crc32(slot_bytes, PU_DESC_CRC_DATA_BYTES) != header_crc32) {
        return false;
    }

    const uint8_t *payload = slot_bytes + PU_IMAGE_HEADER_SIZE;
    uint32_t initial_msp = read_u32_le(payload + 0u);
    uint32_t entry_point = read_u32_le(payload + 4u);

    /* Initial MSP must point into RP2040 264 KiB SRAM and be 8-byte AAPCS aligned. */
    if (initial_msp <= PU_RP2040_SRAM_BASE ||
        initial_msp > PU_RP2040_SRAM_TOP ||
        (initial_msp & 0x7u) != 0u) {
        return false;
    }

    /* Cortex-M0+ executes Thumb-only; bit 0 of Reset_Handler must be 1. */
    if ((entry_point & 1u) == 0u) {
        return false;
    }

    uint32_t entry_addr = entry_point & ~1u;
    uint32_t payload_base = slot_addr + PU_IMAGE_HEADER_SIZE;
    uint32_t min_code_addr = payload_base + 8u;
    uint32_t max_code_addr = payload_base + image_size - 2u;
    if (entry_addr < min_code_addr || entry_addr > max_code_addr) {
        return false;
    }

    /* Entry instruction must not be unprogrammed NOR flash (0xFFFF). */
    uint32_t entry_payload_off = entry_addr - payload_base;
    if (read_u16_le(payload + entry_payload_off) == 0xFFFFu) {
        return false;
    }

    if (pu_crc32(payload, image_size) != payload_crc32) {
        return false;
    }

    if (info != NULL) {
        info->entry_point = entry_point;
        info->initial_msp = initial_msp;
        info->image_size = image_size;
        info->crc32 = payload_crc32;
        info->version = image_version;
    }

    return true;
}
