#include "dap_guard.h"
#include "clock_math.h"
#include "DAP.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls;
static uint8_t *shared_request;
uint32_t DAP_ProcessCommand(const uint8_t *request, uint8_t *response) {
    calls++;
    response[0] = request[0]; response[1] = 0xff;
    if (shared_request) {
        memset(shared_request, 0xff, 12); // Model USB resetting the request ring.
        shared_request = NULL;
    }
    return (1u << 16) | 2; // Target errors may report incomplete consumption.
}
int main(void) {
    uint8_t response[64], request[64] = {0x15, 9};
    assert(!dap_packet_valid(request, sizeof(request))); // Eight JTAG TAPs maximum.
    request[1] = 8;
    memset(request + 2, 4, 8);
    assert(dap_packet_valid(request, 10));
    assert(!dap_packet_valid(request, 9));
    uint8_t block[] = {6, 0, 16, 0, 2};
    assert(!dap_packet_valid(block, sizeof(block))); // Reply would overflow.
    block[2] = 15;
    assert(dap_packet_valid(block, sizeof(block)));
    uint8_t write[] = {5, 0, 1, 0, 1, 2, 3, 4};
    assert(dap_packet_valid(write, sizeof(write)));
    for (size_t n = 0; n < sizeof(write); ++n) {
        assert(!dap_packet_valid(write, n));
        assert(dap_execute_checked(write, n, response) == 1 && calls == 0);
    }
    uint8_t batch[] = {0x7f, 2, 5, 0, 1, 0, 1, 2, 3, 4, 2, 1};
    assert(dap_execute_checked(batch, sizeof(batch), response) == 6);
    assert(calls == 2 && response[2] == 5 && response[4] == 2);
    uint8_t stable_batch[sizeof(batch)];
    memcpy(stable_batch, batch, sizeof(batch));
    shared_request = batch;
    assert(dap_execute_checked(batch, sizeof(batch), response) == 6);
    assert(response[2] == 5 && response[4] == 2);
    memcpy(batch, stable_batch, sizeof(batch));
    batch[1] = 3;
    assert(!dap_packet_valid(batch, sizeof(batch)));
    uint8_t input_sequence[] = {0x1d, 8, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    assert(!dap_packet_valid(input_sequence, sizeof(input_sequence)));
    assert(probe_delay_to_khz(125000000, 624) == 100);
    assert(probe_delay_to_khz(125000000, UINT32_MAX) == 1);
    assert(probe_delay_to_khz(125000000, 62500000) == 1);
    // Exact-sized allocations let ASan detect even one-byte truncated reads.
    uint32_t seed = 0xfeed1234;
    for (unsigned trial = 0; trial < 50000; ++trial) {
        size_t n = trial % 65;
        uint8_t *bytes = malloc(n ? n : 1);
        for (size_t i = 0; i < n; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            bytes[i] = seed;
        }
        dap_packet_valid(bytes, n);
        free(bytes);
    }
    puts("CMSIS-DAP packet checks passed: truncation, JTAG chain limits, response bounds, atomic batches, 50000 random packets, clock extremes");
}
