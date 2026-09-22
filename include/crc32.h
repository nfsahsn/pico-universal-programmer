#ifndef PROGRAMMER_CRC32_H
#define PROGRAMMER_CRC32_H
#include <stddef.h>
#include <stdint.h>
/* CRC-32/ISO-HDLC (IEEE), compatible with Python zlib.crc32. */
uint32_t programmer_crc32(const void *data, size_t length);
#endif
