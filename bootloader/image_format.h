#ifndef PU_BOOTLOADER_IMAGE_FORMAT_H
#define PU_BOOTLOADER_IMAGE_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Slot image layout:
 *   [slot_addr + 0x000 .. slot_addr + 0x0FF] : 256-byte descriptor header page
 *   [slot_addr + 0x100 .. + image_size)      : Payload starting with Cortex-M0+ VTOR
 */
#define PU_IMAGE_DESC_MAGIC   (0x5055494Du) /* "PUIM" */
#define PU_IMAGE_DESC_VERSION (1u)
#define PU_IMAGE_HEADER_SIZE  (256u)

typedef struct {
    uint32_t magic;
    uint32_t header_version;
    uint32_t image_version;
    uint32_t slot_addr;
    uint32_t image_size;
    uint32_t payload_crc32;
    uint32_t flags;
    uint32_t header_crc32;
} pu_image_descriptor_t;

#if defined(__cplusplus)
static_assert(sizeof(pu_image_descriptor_t) == 32u, "pu_image_descriptor_t ABI must be 32 bytes");
#else
_Static_assert(sizeof(pu_image_descriptor_t) == 32u, "pu_image_descriptor_t ABI must be 32 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* PU_BOOTLOADER_IMAGE_FORMAT_H */
