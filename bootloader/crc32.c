#include "bootloader/crc32.h"

uint32_t pu_crc32(const void *data, size_t length) {
    if (data == NULL || length == 0u) {
        return 0u;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xEDB88320u);
        }
    }

    return ~crc;
}
