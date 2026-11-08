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
 * OpenDMR, hardware abstraction: the functions (registers and pins: hw.h).
 */

#ifndef DM32_HAL_H
#define DM32_HAL_H

#include <stdint.h>
#include <stdbool.h>

extern uint32_t clock_sys_hz;
extern uint32_t clock_apb_hz;

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);
void gpio_output(uint32_t pin, bool level);
void gpio_input(uint32_t pin);
void gpio_set(uint32_t pin);
void gpio_reset(uint32_t pin);

void clock_init(void);
void watchdog_start(uint32_t timeout_ms); // a chip reset unless fed within timeout_ms (watchdog.c)
void watchdog_feed(void);
void watchdog_stop(void);
void watchdog_reset(void) __attribute__((noreturn)); // a whole chip reset, as at power on, after about 1 s
void pins_init(void);

typedef void (*irq_handler_t)(void);
void irq_init(void);
void irq_register(int irq, irq_handler_t handler); // unmasks it

// Interrupts off and back as they were: for code that runs in a task or in an interrupt (the FreeRTOS critical
// sections are for tasks only)
static inline uint32_t irq_save(void)
{
    uint32_t psr;

    __asm__ volatile("mfcr %0, psr\n\tpsrclr ie, fe" : "=r"(psr));
    return psr;
}

static inline void irq_restore(uint32_t psr)
{
    __asm__ volatile("mtcr %0, psr" : : "r"(psr));
}
void systick_init(uint32_t hz);                    // TIMER0, the FreeRTOS tick

void irq_exception(uint32_t number, uint32_t epc, uint32_t epsr, uint32_t lr); // startup/crt0.S

#endif
