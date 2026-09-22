#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool dap_packet_valid(const uint8_t *request, size_t length);
uint32_t dap_execute_checked(const uint8_t *request, size_t length, uint8_t *response);
