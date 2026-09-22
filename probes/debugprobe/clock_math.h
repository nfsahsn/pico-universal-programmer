#pragma once
#include <stdint.h>
static inline uint32_t probe_delay_to_khz(uint32_t hz, uint32_t delay) {
    uint32_t khz = hz / (2000ull * ((uint64_t)delay + 1));
    return khz ? khz : 1; // PIO clock API cannot accept zero kHz.
}
