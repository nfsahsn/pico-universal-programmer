/* RP2040 only. Called before SDK runtime initialization after a hardware reset.
 * r0 = validated VTOR; r1 = validated MSP; r2 = validated Thumb reset handler.
 * No C stack accesses are permitted after installing the destination MSP. */
.syntax unified
.cpu cortex-m0plus
.thumb
.section .text.bootloader_enter_image, "ax", %progbits
.global bootloader_enter_image
.type bootloader_enter_image, %function
.thumb_func
bootloader_enter_image:
    cpsid i
    ldr r3, =0xe000e010 /* SysTick control */
    movs r4, #0
    str r4, [r3]
    ldr r3, =0xe000e180 /* NVIC ICER */
    mvns r4, r4
    str r4, [r3]
    ldr r3, =0xe000e280 /* NVIC ICPR */
    str r4, [r3]
    ldr r3, =0xe000ed04 /* clear pending PendSV and SysTick */
    ldr r4, =0x0a000000
    str r4, [r3]
    ldr r3, =0xe000ed08 /* VTOR */
    str r0, [r3]
    movs r3, #0
    msr control, r3
    dsb
    isb
    msr msp, r1
    dsb
    isb
    cpsie i
    bx r2
.size bootloader_enter_image, .-bootloader_enter_image
