#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
struct RVDebug {
    uint32_t regs[16] = {}, pc = 0;
    unsigned writes = 0;
    uint8_t ram[2048] = {};
    uint8_t *flash = nullptr;
    bool corrupt = false;
    bool halted = true;
    void halt() { halted = true; }
    void resume() { halted = false; }
    void reset() { halted = true; pc = 0; }
    void step() { assert(halted); pc += 2; }
    int get_gpr_count() { return 16; }
    uint32_t get_gpr(int index) { assert(index >= 0 && index < 16); return regs[index]; }
    void set_gpr(int index, uint32_t value) { assert(index >= 0 && index < 16); regs[index] = value; writes++; }
    uint32_t get_dpc() { return pc; }
    void set_dpc(uint32_t value) { pc = value; writes++; }
    struct Status { bool ALLHALTED; };
    Status get_dmstatus() { return {halted}; }
    uint8_t get_mem_u8(uint32_t address) { return flash && address < 16384 ? (flash[address] ^ (corrupt && address == 0 ? 1 : 0)) : ram[address % sizeof(ram)]; }
    uint16_t get_mem_u16(uint32_t address) { return get_mem_u8(address) | (get_mem_u8(address + 1) << 8); }
    uint32_t get_mem_u32(uint32_t address) { return get_mem_u16(address) | (uint32_t(get_mem_u16(address + 2)) << 16); }
    void get_block_aligned(uint32_t address, void *data, int size) { for (int i = 0; i < size; ++i) static_cast<uint8_t *>(data)[i] = get_mem_u8(address + i); }
    void set_mem_u8(uint32_t address, uint8_t value) { ram[address % sizeof(ram)] = value; writes++; }
    void set_block_aligned(uint32_t address, void *data, int size) { memcpy(ram + address % sizeof(ram), data, size); writes++; }
};
struct WCHFlash {
    uint8_t bytes[16384];
    unsigned erases = 0, writes = 0;
    WCHFlash() { memset(bytes, 0xff, sizeof(bytes)); }
    int get_flash_base() { return 0; }
    int get_flash_size() { return sizeof(bytes); }
    int get_page_size() { return 64; }
    int get_page_count() { return sizeof(bytes) / 64; }
    int get_sector_size() { return 1024; }
    void wipe_chip() { memset(bytes, 0xff, sizeof(bytes)); erases++; }
    void wipe_page(int address) { assert(address >= 0 && address + 64 <= 16384); memset(bytes + address, 0xff, 64); erases++; }
    void wipe_sector(int address) { assert(address >= 0 && address + 1024 <= 16384); memset(bytes + address, 0xff, 1024); erases++; }
    void write_flash(int address, uint8_t *data, int size) { assert(address >= 0 && address + size <= 16384); memcpy(bytes + address, data, size); writes++; }
};
#ifndef TEST_REAL_SOFTBREAK
struct SoftBreak {
    unsigned resumes = 0, breakpoint_calls = 0;
    int breakpoint_result = 0;
    bool resume() { resumes++; return true; }
    void halt() {}
    void step() {}
    void reset() {}
    void set_dpc(uint32_t) {}
    void clear_all_breakpoints() {}
    int clear_breakpoint(uint32_t, uint32_t) { breakpoint_calls++; return breakpoint_result; }
    int set_breakpoint(uint32_t, uint32_t) { breakpoint_calls++; return breakpoint_result; }
};
#endif
