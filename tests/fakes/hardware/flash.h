#ifndef TEST_FLASH_H
#define TEST_FLASH_H
#include <stdint.h>
#include <stddef.h>
#define FLASH_PAGE_SIZE 256
#define FLASH_SECTOR_SIZE 4096
void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);
#endif
