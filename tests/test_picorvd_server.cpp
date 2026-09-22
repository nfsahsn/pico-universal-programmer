#include "GDBServer.h"
#include "targets.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <poll.h>
#include <unistd.h>
#include "usb_bridge.h"
static bool usb_connected = true;
static uint32_t usb_room = 0, usb_reads = 0;
static std::string usb_output;
bool tud_cdc_n_connected(uint8_t) { return usb_connected; }
uint32_t tud_cdc_n_write_available(uint8_t) { return usb_room; }
uint32_t tud_cdc_n_available(uint8_t) { return 1; }
uint32_t tud_cdc_n_read(uint8_t, void *data, uint32_t) {
    usb_reads++; *static_cast<char*>(data) = '+'; return 1;
}
uint32_t tud_cdc_n_write(uint8_t, const void *data, uint32_t) {
    assert(usb_room); --usb_room; usb_output += *static_cast<const char*>(data); return 1;
}
void tud_cdc_n_write_flush(uint8_t) {}
int cmp(const char *prefix, const char *text) { return strncmp(prefix, text, strlen(prefix)); }
int from_hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
char to_hex(int n) { return "0123456789abcdef"[n & 15]; }

int main(int argc, char **) {
    RVDebug cpu;
    WCHFlash flash;
    SoftBreak soft;
    GDBServer server(&cpu, &flash, &soft);
    server.reset();
    if (argc > 1) {
        cpu.flash = flash.bytes;
        cpu.corrupt = argc > 2;
        while (true) {
            struct pollfd input = {0, POLLIN, 0};
            bool available = poll(&input, 1, 1) > 0 && (input.revents & POLLIN);
            char byte = 0, output = 0;
            bool outgoing = false;
            if (available && read(0, &byte, 1) != 1) return 0;
            if (available && server.state == GDBServer::RECV_SUFFIX2) fprintf(stderr, "Packet: %.*s\n", server.recv.size, server.recv.buf);
            server.update(true, available, byte, outgoing, output);
            if (outgoing && write(1, &output, 1) != 1) return 0;
        }
    }
    auto packet = [&](const char *text) {
        server.recv.set_packet(text);
        server.handle_packet();
    };
    packet("P1=78563412");
    assert(cpu.regs[1] == 0x12345678 && strcmp(server.send.buf, "OK") == 0);
    unsigned previous = cpu.writes;
    packet("Pff=78563412");
    assert(cpu.writes == previous && strcmp(server.send.buf, "E00") == 0);
    packet("P1=12");
    assert(cpu.writes == previous);
    packet("G1234");
    assert(cpu.writes == previous);
    std::string registers = "G";
    for (int i = 0; i < 17; ++i) registers += "78563412";
    packet(registers.c_str());
    assert(cpu.pc == 0x12345678 && cpu.regs[15] == 0x12345678);
    previous = cpu.writes;
    packet("M20000000,4:0102");
    assert(cpu.writes == previous);
    packet("M20000000,4:010203zz");
    assert(cpu.writes == previous);
    packet("M20000000,4:01020304");
    assert(cpu.ram[0] == 1 && cpu.ram[3] == 4);
    packet("vFlashErase:0,40");
    assert(flash.erases == 1);
    packet("vFlashErase:4000,40");
    assert(flash.erases == 1 && strcmp(server.send.buf, "E00") == 0);
    packet("vFlashErase:0,ffffffff");
    assert(flash.erases == 1);
    server.recv.clear();
    server.recv.put_str("vFlashWrite:0:");
    for (int i = 0; i < 64; ++i) server.recv.put(static_cast<char>(i));
    server.handle_packet();
    packet("vFlashDone");
    assert(flash.writes == 1);
    for (int i = 0; i < 64; ++i) assert(flash.bytes[i] == i);
    packet("qXfer:memory-map:read::1,5");
    assert(server.send.size == 6 && server.send.buf[0] == 'm');
    packet("qXfer:memory-map:read::ffff,5");
    assert(strcmp(server.send.buf, "E01") == 0);
    packet("qCRC:ffffffff,2");
    assert(strcmp(server.send.buf, "E01") == 0);
    previous = cpu.writes;
    packet("Mffffffff,2:0102");
    assert(cpu.writes == previous && server.send.buf[0] == 'E');
    packet("mffffffff,2");
    assert(server.send.buf[0] == 'E');
    previous = flash.erases;
    packet("vFlashErase:0,40garbage");
    assert(flash.erases == previous && server.send.buf[0] == 'E');
    packet("Z0,0,");
    assert(soft.breakpoint_calls == 0 && server.send.buf[0] == 'E');
    soft.breakpoint_result = -1;
    packet("Z0,0,2");
    assert(soft.breakpoint_calls == 1 && server.send.buf[0] == 'E');
    packet("Z1,0,2");
    assert(soft.breakpoint_calls == 1 && server.send.size == 0);
    unsigned resumes_before = soft.resumes;
    packet("cnothex");
    assert(soft.resumes == resumes_before && server.send.buf[0] == 'E');
    packet("s000000001");
    assert(server.send.buf[0] == 'E');
    server.put_flash_cache(0, 0x12);
    server.state = server.next_state = GDBServer::IDLE;
    bool outgoing = false;
    char output = 0;
    server.update(false, false, 0, outgoing, output);
    assert(server.page_bitmap == 0 && server.page_base == -1);
    previous = flash.writes;
    packet("vFlashDone");
    assert(flash.writes == previous);
    // Invalid hex checksum digits used to alias 0xff and accept a packet.
    server.state = server.next_state = GDBServer::IDLE;
    for (char c : std::string("$z") + char(0x85) + "#zz")
        server.update(true, true, c, outgoing, output);
    assert(outgoing && output == '-');
    server.state = server.next_state = GDBServer::IDLE;
    for (char c : std::string("$truncated$?#3f"))
        server.update(true, true, c, outgoing, output);
    assert(outgoing && output == '+' && strcmp(server.send.buf, "T05") == 0);
    server.send.set_packet("$#}*");
    server.state = server.next_state = GDBServer::SEND_PREFIX;
    auto response = [&]() {
        std::string wire;
        for (int count = 0; count < 100 && server.state != GDBServer::RECV_ACK; ++count) {
            bool outgoing = false;
            char output = 0;
            server.update(true, false, 0, outgoing, output);
            if (outgoing) wire += output;
        }
        assert(server.state == GDBServer::RECV_ACK);
        return wire;
    };
    std::string wire = response();
    std::string expected = "$";
    uint8_t checksum = 0;
    for (char c : std::string("$#}*")) {
        expected += '}';
        expected += char(c ^ 0x20);
        checksum += '}' + (c ^ 0x20);
    }
    expected += '#';
    expected += to_hex(checksum >> 4);
    expected += to_hex(checksum);
    assert(wire == expected);
    server.update(true, true, '-', outgoing, output);
    assert(response() == wire); // A NACK retransmits the full framed packet.
    packet("D");
    assert(soft.resumes == 2);
    server.send.set_packet("OK");
    server.state = server.next_state = GDBServer::SEND_PREFIX;
    service_gdb_usb(server);
    assert(server.state == GDBServer::SEND_PREFIX && usb_reads == 0);
    while (server.state != GDBServer::RECV_ACK) {
        usb_room = 1;
        service_gdb_usb(server);
        auto cursor = server.send.cursor2;
        service_gdb_usb(server); // Full USB buffer stalls without losing a byte.
        assert(server.send.cursor2 == cursor);
    }
    assert(usb_output == "$OK#9a");
    server.put_flash_cache(0, 0x12);
    usb_connected = false;
    service_gdb_usb(server); // Disconnect is still handled while USB has no space.
    assert(server.state == GDBServer::DISCONNECTED && server.page_bitmap == 0);
    delete[] server.page_cache;
    puts("PicoRVD server tests passed: register byte order, malformed writes, flash bounds, full-page writes, detach");
}
