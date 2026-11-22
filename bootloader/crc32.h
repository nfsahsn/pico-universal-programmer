#ifndef PU_BOOTLOADER_CRC32_H
#define PU_BOOTLOADER_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IEEE 802.3 / ISO-HDLC CRC-32 (polynomial 0xEDB88320, compatible with zlib.crc32). */
uint32_t pu_crc32(const void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* PU_BOOTLOADER_CRC32_H */
