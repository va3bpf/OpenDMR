/*
 * FreeRTOS port for the C-SKY CK803S core of the DH4570 / HR_C7000 (Baofeng DM-32): context switches and the
 * interrupt dispatcher. Frame layout: see portmacro.h.
 *
 * SPDX-License-Identifier: MIT
 */

.import pxCurrentTCB
.import vTaskSwitchContext
.import ulPortInterruptNesting
.import xPortSwitchRequired
.import g_irqvector
.import g_top_irqstack

.text
.align 2

/* Restores the frame at sp and returns into it */
.macro RESTORE_FRAME
    ldw     r0, (sp, 64)
    mtcr    r0, epc
    ldw     r0, (sp, 60)
    mtcr    r0, epsr
    ldw     r15, (sp, 56)
    ldm     r0-r13, (sp)
    addi    sp, 68
    rte
.endm

/* void vPortStartFirstTask(void): runs pxCurrentTCB, never returns */
.global vPortStartFirstTask
.type   vPortStartFirstTask, %function
vPortStartFirstTask:
    psrclr  ie
    lrw     r0, pxCurrentTCB
    ldw     r0, (r0)
    ldw     sp, (r0)
    RESTORE_FRAME

/*
 * void vPortYield(void): task level switch, called as a function. The caller's PSR is saved as it was (interrupts
 * on, or off in vPortExitCritical()), the return address as its PC.
 */
.global vPortYield
.type   vPortYield, %function
vPortYield:
    mfcr    r1, psr
    psrclr  ie
    subi    sp, 68
    stm     r0-r13, (sp)
    stw     r15, (sp, 56)
    stw     r1, (sp, 60)
    stw     r15, (sp, 64)

    lrw     r2, pxCurrentTCB
    ldw     r2, (r2)
    stw     sp, (r2)

    jbsr    vTaskSwitchContext

    lrw     r2, pxCurrentTCB
    ldw     r2, (r2)
    ldw     sp, (r2)
    RESTORE_FRAME

/*
 * All the interrupts (vectors 32 - 63). The interrupted context is saved on its own stack; the first level switches to
 * the IRQ stack (a nested one, only possible if a handler turned the interrupts on, stays on it), calls the handler of
 * g_irqvector, and on its way out switches task when a handler set xPortSwitchRequired.
 */
.global VectorIrqISR
.type   VectorIrqISR, %function
VectorIrqISR:
    subi    sp, 68
    stm     r0-r13, (sp)
    stw     r15, (sp, 56)
    mfcr    r0, epsr
    stw     r0, (sp, 60)
    mfcr    r0, epc
    stw     r0, (sp, 64)

    lrw     r1, ulPortInterruptNesting
    ldw     r2, (r1)
    addi    r3, r2, 1
    stw     r3, (r1)
    bnez    r2, 1f
    mov     r8, sp                      /* the interrupted task's frame (r8 is saved by the handlers) */
    lrw     sp, g_top_irqstack
1:
    /* acknowledge the interrupt controller (PIC_COW1), as the vendor's port */
    lrw     r0, 0x17000010
    movi    r1, 4
    stw     r1, (r0)

    /* handler of the vector number in PSR bits 16 - 23, argument: its interrupt number */
    lrw     r1, g_irqvector
    mfcr    r0, psr
    lsri    r0, 16
    sextb   r0
    subi    r0, 32
    lsli    r0, 2
    add     r1, r0
    ldw     r1, (r1)
    lsri    r0, 2
    jsr     r1

    lrw     r1, ulPortInterruptNesting
    ldw     r2, (r1)
    subi    r2, 1
    stw     r2, (r1)
    bnez    r2, 2f                      /* nested: back to the interrupted handler */

    mov     sp, r8
    lrw     r1, xPortSwitchRequired
    ldw     r2, (r1)
    bez     r2, 2f
    movi    r2, 0
    stw     r2, (r1)

    lrw     r2, pxCurrentTCB
    ldw     r2, (r2)
    stw     sp, (r2)
    jbsr    vTaskSwitchContext
    lrw     r2, pxCurrentTCB
    ldw     r2, (r2)
    ldw     sp, (r2)
2:
    RESTORE_FRAME
