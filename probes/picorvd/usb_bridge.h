#pragma once
#include "GDBServer.h"
#include "tusb.h"

// GDBServer advances its output cursor when update emits a byte. Only advance
// while USB can accept it; a full endpoint must not silently drop RSP bytes.
inline void service_gdb_usb(GDBServer &gdb) {
    bool connected = tud_cdc_n_connected(0);
    if (connected && tud_cdc_n_write_available(0) == 0) return;
    bool incoming = connected && tud_cdc_n_available(0);
    char input = 0, output = 0;
    bool outgoing = false;
    if (incoming) incoming = tud_cdc_n_read(0, &input, 1) == 1;
    gdb.update(connected, incoming, input, outgoing, output);
    if (outgoing) {
        tud_cdc_n_write(0, &output, 1);
        tud_cdc_n_write_flush(0);
    }
}
