#pragma once
#include <cstdint>
bool tud_cdc_n_connected(uint8_t);
uint32_t tud_cdc_n_write_available(uint8_t);
uint32_t tud_cdc_n_available(uint8_t);
uint32_t tud_cdc_n_read(uint8_t, void *, uint32_t);
uint32_t tud_cdc_n_write(uint8_t, const void *, uint32_t);
void tud_cdc_n_write_flush(uint8_t);
