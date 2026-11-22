/*
 * Deterministic ARM Cortex-M0+ CPU handover from immutable bootloader to validated slot.
 *
 * Arguments:
 *   r0 = validated VTOR (slot_addr + 0x100, 256-byte aligned)
 *   r1 = validated initial MSP (0x20000000 < msp <= 0x20042000, 8-byte aligned)
 *   r2 = validated Reset_Handler entry point (Thumb bit 0 == 1)
 */

.syntax unified
.cpu cortex-m0plus
.thumb

.section .text.pu_bootloader_handover, "ax", %progbits
.global pu_bootloader_handover
.type pu_bootloader_handover, %function
.thumb_func
pu_bootloader_handover:
    cpsid i

    /* Disable SysTick timer and interrupt */
    ldr r3, =0xE000E010
    movs r4, #0
    str r4, [r3]

    /* Disable all 32 external NVIC IRQ lines (NVIC_ICER) */
    ldr r3, =0xE000E180
    mvns r4, r4
    str r4, [r3]

    /* Clear all pending external NVIC IRQs (NVIC_ICPR) */
    ldr r3, =0xE000E280
    str r4, [r3]

    /* Clear pending PendSV (bit 27) and SysTick (bit 25) in SCB_ICSR */
    ldr r3, =0xE000ED04
    ldr r4, =0x0A000000
    str r4, [r3]

    /* Install application vector table (SCB_VTOR) */
    ldr r3, =0xE000ED08
    str r0, [r3]

    /* Ensure privileged Thread mode using MSP */
    movs r3, #0
    msr control, r3
    dsb
    isb

    /* Install validated Main Stack Pointer; no stack accesses permitted past here */
    msr msp, r1
    dsb
    isb

    /* Match Cortex-M0+ reset state (PRIMASK=0 with all IRQ sources masked) and branch */
    movs r0, #0
    movs r1, #0
    movs r3, #0
    movs r4, #0
    cpsie i
    bx r2
.size pu_bootloader_handover, .-pu_bootloader_handover
