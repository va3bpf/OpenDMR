/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, hardware abstraction: the interrupt controller, the interrupt table and the exceptions.
 * After OpenDM32 (user/main.c, user/DRV/C7000_DRV.c, ucos3/bsp).
 */

#include "hw.h"
#include "hal.h"
#include "uart.h"
#include "systime.h"

// Interrupts: VectorIrqISR (freertos/portable/CK803/portasm.S) calls g_irqvector[n] for vector 32 + n; vectors 32 - 63
// are masked in PIC_MASK, 64 - 95 in PIC_MASK_1 (the HR_C7000's Tx burst start is 64)
#define NR_IRQS 64
volatile uint32_t g_irqvector[NR_IRQS];

static void unexpected(void)
{
}

void irq_init(void)
{
    PIC_MODE = 0x00; // high level
    PIC_PO = 0xffffffff;
    PIC_MASK = 0xffffffff;
    PIC_MASK_1 = 0xffffffff;
    PIC_MODE_1 = 0x00;
    PIC_PO_1 = 0xffffffff;
    PIC_INT_ST = 0xffffffff;
    PIC_INT_ST_1 = 0xffffffff;

    for (int i = 0; i < NR_IRQS; i++) {
        g_irqvector[i] = (uint32_t)unexpected;
    }
}

void irq_register(int irq, irq_handler_t handler)
{
    g_irqvector[irq] = (uint32_t)handler;
    if (irq < 32) {
        PIC_MASK &= ~BITSET(irq);
    } else {
        PIC_MASK_1 &= ~BITSET(irq - 32);
    }
}

// Exceptions (startup/crt0.S): stop with what happened, readable with a debugger, and said again and again on the
// programming cable (115200 8N1, polled: the interrupts are off), as the main firmware's dm32DebugException()
volatile uint32_t irq_exception_info[4];
uint32_t *irq_exception_frame = 0; // the misaligned access: r0 - r13, r15 saved by startup/crt0.S

static void put_hex(char *out, uint32_t value)
{
    for (int i = 0; i < 8; i++) {
        out[i] = "0123456789ABCDEF"[(value >> (28 - (4 * i))) & 0x0F];
    }
}

void irq_exception(uint32_t number, uint32_t epc, uint32_t epsr, uint32_t lr)
{
    char line[] = "\r\nEXCEPTION n=........ pc=........ psr=........ lr=........\r\n";

    irq_exception_info[0] = number;
    irq_exception_info[1] = epc;
    irq_exception_info[2] = epsr;
    irq_exception_info[3] = lr;
    systime_record_exception(number, epc, epsr, lr); // the watchdog, once on, resets the radio (interrupts off)
    put_hex(&line[14], number);
    put_hex(&line[26], epc);
    put_hex(&line[39], epsr);
    put_hex(&line[51], lr);
    char regs[(15 * 12) + 2];

    for (int i = 0; i < 15; i++) {
        int number = (i == 14) ? 15 : i;

        regs[(i * 12) + 0] = 'r';
        regs[(i * 12) + 1] = (char)('0' + (number / 10));
        regs[(i * 12) + 2] = (char)('0' + (number % 10));
        put_hex(&regs[(i * 12) + 3], irq_exception_frame ? irq_exception_frame[i] : 0);
        regs[(i * 12) + 11] = ' ';
    }
    regs[15 * 12] = '\r';
    regs[(15 * 12) + 1] = '\n';
    for (;;) {
        uart0_write((const uint8_t *)line, sizeof(line) - 1);
        if (irq_exception_frame) {
            uart0_write((const uint8_t *)regs, sizeof(regs));
        }
        delay_ms(1000);
    }
}
