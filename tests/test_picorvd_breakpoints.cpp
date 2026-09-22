#include "SoftBreak.h"
#include "targets.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdarg>
void printf_color(const char*, const char*, ...) {}

int main() {
    RVDebug cpu;
    WCHFlash flash;
    cpu.flash = flash.bytes;
    for (unsigned i = 0; i < sizeof(flash.bytes); ++i) flash.bytes[i] = i;
    uint8_t original[sizeof(flash.bytes)];
    memcpy(original, flash.bytes, sizeof(original));
    SoftBreak soft(&cpu, &flash);
    soft.init();
    // A 32-bit RISC-V instruction can start at a halfword and straddle pages.
    int breakpoint = soft.set_breakpoint(62, 4);
    assert(breakpoint >= 0 && soft.set_breakpoint(62, 4) == breakpoint);
    assert(soft.set_breakpoint(64, 2) < 0); // Overlap must not corrupt saved bytes.
    assert(soft.resume());
    const uint8_t trap[] = {0x73, 0, 0x10, 0};
    assert(memcmp(flash.bytes + 62, trap, 4) == 0);
    soft.halt();
    assert(memcmp(original, flash.bytes, sizeof(original)) == 0);
    assert(soft.clear_breakpoint(62, 4) >= 0);
    assert(soft.clear_breakpoint(62, 4) >= 0);
    assert(soft.set_breakpoint(16382, 2) >= 0); // Final instruction is valid.
    assert(soft.set_breakpoint(16382, 4) < 0);
    assert(soft.resume());
    // Disconnecting while running must restore all modified flash pages.
    soft.clear_all_breakpoints();
    assert(memcmp(original, flash.bytes, sizeof(original)) == 0);
    assert(soft.resume());
    assert(memcmp(original, flash.bytes, sizeof(original)) == 0);
    // Reset must synchronize the software halt state with the hardware.
    soft.reset();
    assert(soft.is_halted() && cpu.halted);
    assert(soft.set_breakpoint(100, 2) >= 0);
    assert(soft.resume());
    assert(flash.bytes[100] == 2 && flash.bytes[101] == 0x90);
    soft.halt();
    soft.clear_all_breakpoints();
    assert(memcmp(original, flash.bytes, sizeof(original)) == 0);
    puts("PicoRVD breakpoint tests passed: cross-page and unaligned instructions, duplicate packets, bounds, disconnect and reset");
}
