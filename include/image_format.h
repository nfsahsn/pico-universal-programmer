#ifndef PROGRAMMER_IMAGE_FORMAT_H
#define PROGRAMMER_IMAGE_FORMAT_H
#include <stdint.h>

/* Last 256-byte page of each probe slot. All words are little-endian. CRC-32
 * detects accidental corruption, not malicious firmware or authenticity. */
#define IMAGE_DESCRIPTOR_MAGIC 0x5550494Du
#define IMAGE_DESCRIPTOR_VERSION 1u
#define IMAGE_DESCRIPTOR_PAGE_SIZE 256u
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t base;
    uint32_t length;
    uint32_t image_crc32;
    uint32_t mode;
    uint32_t flags;
    uint32_t header_crc32;
} image_descriptor_t;
_Static_assert(sizeof(image_descriptor_t) == 32, "image descriptor ABI");
#endif
