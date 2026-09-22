/* Validate USB packet boundaries before the upstream pointer-only DAP API.
 * This describes the capabilities of the pinned, fixed board configuration. */
#include "dap_guard.h"
#include "DAP_config.h"
#include "DAP.h"
#include <string.h>

static bool command_size(const uint8_t *p, size_t left, size_t *used, size_t *reply) {
    if (!left) return false;
    size_t n = 1, out = 2, count = 0;
    switch (p[0]) {
    case ID_DAP_Info:
        if (left < 2) return false;
        n = 2;
        switch (p[1]) {
        case DAP_ID_FW_VER: out = 2 + sizeof(DAP_FW_VER); break;
        case DAP_ID_CAPABILITIES: case DAP_ID_PACKET_COUNT: out = 3; break;
        case DAP_ID_TIMESTAMP_CLOCK: out = 6; break;
        case DAP_ID_PACKET_SIZE: out = 4; break;
        default: out = 2; break; // Other configured strings/features are empty.
        }
        break;
    case ID_DAP_HostStatus: n = 3; break;
    case ID_DAP_Connect: case ID_DAP_SWD_Configure: n = 2; break;
    case ID_DAP_Disconnect: break;
    case ID_DAP_ResetTarget: out = 3; break;
    case ID_DAP_TransferConfigure: n = 6; break;
    case ID_DAP_WriteABORT: n = 6; break;
    case ID_DAP_Delay: n = 3; break;
    case ID_DAP_SWJ_Pins: n = 7; break;
    case ID_DAP_SWJ_Clock: n = 5; break;
    case ID_DAP_JTAG_IDCODE: n = 2; out = 6; break;
    case ID_DAP_SWJ_Sequence:
        if (left < 2) return false;
        n = 2 + ((p[1] ? p[1] : 256) + 7) / 8;
        break;
    case ID_DAP_JTAG_Configure:
        if (left < 2 || p[1] > DAP_JTAG_DEV_CNT) return false;
        n = 2 + p[1];
        if (n > left) return false;
        for (size_t i = 2; i < n; ++i) if (p[i] == 0 || p[i] > 32) return false;
        break;
    case ID_DAP_JTAG_Sequence: case ID_DAP_SWD_Sequence:
        if (left < 2) return false;
        count = p[1]; n = 2;
        for (size_t i = 0; i < count; ++i) {
            if (n >= left) return false;
            uint8_t info = p[n++];
            size_t bytes = (((info & 63) ? (info & 63) : 64) + 7) / 8;
            if (p[0] == ID_DAP_JTAG_Sequence || !(info & 128)) n += bytes;
            if (info & 128) out += bytes;
            if (n > left || out > DAP_PACKET_SIZE) return false;
        }
        break;
    case ID_DAP_Transfer:
        if (left < 3) return false;
        count = p[2]; n = 3; out = 3;
        for (size_t i = 0; i < count; ++i) {
            if (n >= left) return false;
            uint8_t flags = p[n++];
            if (!(flags & DAP_TRANSFER_RnW) || (flags & DAP_TRANSFER_MATCH_VALUE)) n += 4;
            if ((flags & DAP_TRANSFER_RnW) && !(flags & DAP_TRANSFER_MATCH_VALUE)) out += 4;
            if (flags & DAP_TRANSFER_TIMESTAMP) out += 4;
            if (n > left || out > DAP_PACKET_SIZE) return false;
        }
        break;
    case ID_DAP_TransferBlock:
        if (left < 4) return false;
        count = p[2] | ((size_t)p[3] << 8); n = 4; out = 4;
        if (count) {
            if (left < 5) return false;
            n = 5;
            if (p[4] & DAP_TRANSFER_RnW) out += 4 * count;
            else n += 4 * count;
        }
        break;
    case ID_DAP_ExecuteCommands: case ID_DAP_QueueCommands:
        return false; // Nested command groups are not part of the DAP API.
    default:
        out = 1; // Pinned vendor stubs and unsupported commands consume one byte.
        break;
    }
    *used = n; *reply = out;
    return n <= left && out <= DAP_PACKET_SIZE;
}

bool dap_packet_valid(const uint8_t *request, size_t length) {
    if (!length || length > DAP_PACKET_SIZE) return false;
    size_t offset = 0, response = 0, count = 1;
    if (request[0] == ID_DAP_ExecuteCommands || request[0] == ID_DAP_QueueCommands) {
        if (length < 2) return false;
        count = request[1]; offset = 2; response = 2;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t used, reply;
        if (!command_size(request + offset, length - offset, &used, &reply)) return false;
        offset += used; response += reply;
        if (response > DAP_PACKET_SIZE) return false;
    }
    return true; // Trailing USB/HID padding is allowed.
}

uint32_t dap_execute_checked(const uint8_t *request, size_t length, uint8_t *response) {
    // USB reset callbacks can clear/reuse the shared request ring while the
    // DAP task is executing. Validate and execute one private, stable snapshot.
    uint8_t snapshot[DAP_PACKET_SIZE];
    if (!length || length > sizeof(snapshot)) { response[0] = ID_DAP_Invalid; return 1; }
    memcpy(snapshot, request, length);
    request = snapshot;
    if (!dap_packet_valid(request, length)) { response[0] = ID_DAP_Invalid; return 1; }
    if (request[0] != ID_DAP_ExecuteCommands && request[0] != ID_DAP_QueueCommands)
        return DAP_ProcessCommand(request, response) & 0xffffu;
    size_t offset = 2, output = 2;
    response[0] = ID_DAP_ExecuteCommands; response[1] = request[1];
    for (size_t i = 0; i < request[1]; ++i) {
        size_t used, reply;
        command_size(request + offset, length - offset, &used, &reply);
        output += DAP_ProcessCommand(request + offset, response + output) & 0xffffu;
        offset += used; // Independent of early target errors in the upstream parser.
    }
    return output;
}
