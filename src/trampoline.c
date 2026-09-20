#include "hardware/structs/watchdog.h"
#include "hardware/regs/m0plus.h"
#include "hardware/regs/addressmap.h"
#include <stdint.h>

void __attribute__((section(".scratch_y"), noinline)) jump_trampoline(void) {
    uint32_t vtor = *((volatile uint32_t *)0x4005800c); // scratch[0]
    uint32_t sp   = *((volatile uint32_t *)0x40058010); // scratch[1]
    uint32_t pc   = *((volatile uint32_t *)0x40058014); // scratch[2]
    
    *((volatile uint32_t *)(PPB_BASE + M0PLUS_VTOR_OFFSET)) = vtor;
    
    __asm volatile (
        "msr msp, %0\n"
        "isb\n"
        "bx %1\n"
        : : "r"(sp), "r"(pc) : "memory"
    );
}
