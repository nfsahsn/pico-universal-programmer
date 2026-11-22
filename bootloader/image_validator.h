#ifndef PU_BOOTLOADER_IMAGE_VALIDATOR_H
#define PU_BOOTLOADER_IMAGE_VALIDATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t entry_point;
    uint32_t initial_msp;
    uint32_t image_size;
    uint32_t crc32;
    uint32_t version;
} pu_image_info_t;

bool pu_image_validate_slot(uint32_t slot_addr, uint32_t slot_size, pu_image_info_t *info);

#ifdef PU_HOST_TEST
void pu_image_bind_test_flash(const uint8_t *flash_buf, size_t flash_size);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PU_BOOTLOADER_IMAGE_VALIDATOR_H */
