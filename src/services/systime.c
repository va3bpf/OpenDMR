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
 * OpenDMR: the 1 ms timer (timer block 1, vector 34, "TIMER2" of the main firmware), the power switch check
 * (power.c), the rotary encoder (rotary.c), the battery sample each second (battery.c) and the monitor.
 *
 * The monitor: when the FreeRTOS tick or the main task stops for 2 s, this interrupt answers the cable requests
 * itself (cable_link_handle_pending()) and records what it interrupted, so a stalled radio can still be read with dm32dbg.py.
 *
 * The watchdog (from systime_watchdog_start(), at the end of the start): fed here while the tick and the main task run.
 * After a stall of SYSTIME_WATCHDOG_STALL_MS it is no longer fed and resets the radio SYSTIME_WATCHDOG_MS later, as
 * it does when this interrupt stops (interrupts off for good, an exception: hal/irq.c). Not while the cable link is
 * in use (dm32dbg.py reading a stalled radio) or systime_watchdog_hold is set (written with dm32dbg.py). What
 * stopped is kept through the reset (the reset record, in RAM not cleared at the start), then in systime_last_reset.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "systime.h"
#include "cable_link.h"
#include "power.h"
#include "rotary.h"
#include "battery.h"

#define SYSTIME_MONITOR_STALL_MS  2000
#define SYSTIME_WATCHDOG_STALL_MS 5000  // a stall this long: the watchdog no longer fed
#define SYSTIME_WATCHDOG_MS       3000  // the watchdog's timeout: the reset 8 s into a stall
#define SYSTIME_WATCHDOG_CABLE_MS 10000 // a cable request this recent: still fed (a stalled radio read with dm32dbg.py)
#define SYSTIME_RESET_MAGIC       0x57444F47u // "WDOG"

volatile uint32_t systime_ms = 0;
volatile uint32_t systime_app_passes = 0;

// Monitor record: stalls seen, and at the last one: what stopped (bit 0 tick, bit 1 main task), the interrupted PC,
// the FreeRTOS tick count, the current task (TCB address), systime_ms
volatile uint32_t systime_monitor_stalls = 0;
volatile uint32_t systime_monitor_last[5];

// The reset record, kept through the watchdog reset: magic, what (bits 0, 1 as systime_monitor_last[0] for a stall;
// 0x100 + the number for an exception), then for a stall the PC, tick, task and systime_ms as systime_monitor_last,
// for an exception the PC, PSR and LR; ~magic. Set before the watchdog resets, cleared when it is fed again.
// systime_last_reset: the record found at the start (all 0: the last start was not a watchdog reset of ours);
// systime_watchdog_resets: the resets of ours since the power on (kept through them as well)
#define SYSTIME_RECORD_WORDS 7
static volatile uint32_t reset_record[SYSTIME_RECORD_WORDS] __attribute__((section(".noinit")));
static volatile uint32_t reset_count[2] __attribute__((section(".noinit"))); // the count, ~the count
volatile uint32_t systime_last_reset[SYSTIME_RECORD_WORDS];
volatile uint32_t systime_watchdog_resets = 0;
volatile uint32_t systime_watchdog_hold = 0; // not 0: the watchdog fed whatever stalls
static volatile bool watchdog_on = false;

uint32_t RAM_CODE systime_get_ms(void)
{
    return systime_ms;
}

static void RAM_CODE systime_isr(void)
{
    static TickType_t last_tick = 0;
    static uint32_t last_passes = 0;
    static uint32_t tick_stalled_ms = 0;
    static uint32_t passes_stalled_ms = 0;
    extern void *volatile pxCurrentTCB;

    (void)TIMER_EOI(1);
    systime_ms++;
    power_check_1ms();
    rotary_update_1ms();
    if ((systime_ms % 1000) == 0) {
        battery_sample_1s();
    }

    TickType_t tick = xTaskGetTickCountFromISR();

    tick_stalled_ms = ((tick != last_tick) ? 0 : (tick_stalled_ms + 1));
    last_tick = tick;
    passes_stalled_ms = ((systime_app_passes != last_passes) ? 0 : (passes_stalled_ms + 1));
    last_passes = systime_app_passes;

    if ((tick_stalled_ms >= SYSTIME_MONITOR_STALL_MS) || (passes_stalled_ms >= SYSTIME_MONITOR_STALL_MS)) {
        if ((tick_stalled_ms == SYSTIME_MONITOR_STALL_MS) || (passes_stalled_ms == SYSTIME_MONITOR_STALL_MS)) {
            uint32_t epc;

            __asm__ volatile("mfcr %0, epc" : "=r"(epc));
            systime_monitor_stalls++;
            systime_monitor_last[0] = ((tick_stalled_ms >= SYSTIME_MONITOR_STALL_MS) ? 1 : 0) |
                                      ((passes_stalled_ms >= SYSTIME_MONITOR_STALL_MS) ? 2 : 0);
            systime_monitor_last[1] = epc;
            systime_monitor_last[2] = tick;
            systime_monitor_last[3] = (uint32_t)pxCurrentTCB;
            systime_monitor_last[4] = systime_ms;
        }

        if ((systime_ms % 5) == 0) {
            cable_link_handle_pending(true);
        }
    }

    if (watchdog_on) {
        static bool recorded = false;
        bool stalled = (tick_stalled_ms >= SYSTIME_WATCHDOG_STALL_MS) || (passes_stalled_ms >= SYSTIME_WATCHDOG_STALL_MS);

        if (!stalled || systime_watchdog_hold || cable_link_used_within(SYSTIME_WATCHDOG_CABLE_MS)) {
            if (recorded) {
                reset_record[0] = 0;
                recorded = false;
            }
            watchdog_feed();
        } else if (!recorded) {
            reset_record[1] = systime_monitor_last[0];
            reset_record[2] = systime_monitor_last[1];
            reset_record[3] = systime_monitor_last[2];
            reset_record[4] = systime_monitor_last[3];
            reset_record[5] = systime_monitor_last[4];
            reset_record[6] = ~SYSTIME_RESET_MAGIC;
            reset_record[0] = SYSTIME_RESET_MAGIC;
            recorded = true;
        }
    }
}

// An exception (hal/irq.c irq_exception(), interrupts off for good): recorded; the watchdog, if on, resets the radio
void systime_record_exception(uint32_t number, uint32_t epc, uint32_t epsr, uint32_t lr)
{
    reset_record[1] = 0x100 + number;
    reset_record[2] = epc;
    reset_record[3] = epsr;
    reset_record[4] = lr;
    reset_record[5] = systime_ms;
    reset_record[6] = ~SYSTIME_RESET_MAGIC;
    reset_record[0] = SYSTIME_RESET_MAGIC;
}

// From now on a stall resets the radio (main(), the start done)
void systime_watchdog_start(void)
{
    watchdog_start(SYSTIME_WATCHDOG_MS);
    watchdog_on = true;
}

// The 1 ms timer watched from the FreeRTOS tick (TIMER0): when systime_ms stops for SYSTIME_WATCH_MS, what the
// interrupt controller and timer 1 say (seen once: no power off and no cable link, the screen working): count, then
// PIC_MASK, PIC_MASK_1, PIC_INT_ST, PIC_INT_ST_1, TIMER_CONTROL(1), TIMER_CURRENT(1), TIMER_INTSTAT(1), the tick
#define SYSTIME_WATCH_MS 100

volatile uint32_t systime_watch_stops = 0;
volatile uint32_t systime_watch_last[8];

void RAM_CODE vApplicationTickHook(void)
{
    static uint32_t last_ms = 0;
    static uint32_t still = 0;
    uint32_t ms = systime_ms;

    if (ms != last_ms) {
        last_ms = ms;
        still = 0;
        return;
    }
    if (++still == SYSTIME_WATCH_MS) {
        systime_watch_stops++;
        systime_watch_last[0] = PIC_MASK;
        systime_watch_last[1] = PIC_MASK_1;
        systime_watch_last[2] = PIC_INT_ST;
        systime_watch_last[3] = PIC_INT_ST_1;
        systime_watch_last[4] = TIMER_CONTROL(1);
        systime_watch_last[5] = TIMER_CURRENT(1);
        systime_watch_last[6] = TIMER_INTSTAT(1);
        systime_watch_last[7] = xTaskGetTickCountFromISR();
    }
}

// The milliseconds and the 1 ms timer's count down; a reload not counted yet (its interrupt pending) is one more ms
uint32_t RAM_CODE systime_get_us(void)
{
    uint32_t psr = irq_save();
    uint32_t load = TIMER_LOADCOUNT(1);
    uint32_t ms = systime_ms;
    uint32_t current = TIMER_CURRENT(1);

    if ((TIMER_INTSTAT(1) & 0x01) && (current > (load / 2))) {
        ms++;
    }
    irq_restore(psr);
    return (ms * 1000) + (((load - current) * 1000) / load);
}

void systime_init(void)
{
    // The reset record: found when the last reset was the watchdog's after a stall or an exception (garbage after a
    // power on, so both checked; a firmware with the record elsewhere finds garbage as well)
    bool counted = (reset_count[1] == ~reset_count[0]);

    if ((reset_record[0] == SYSTIME_RESET_MAGIC) && (reset_record[6] == ~SYSTIME_RESET_MAGIC)) {
        for (int i = 0; i < SYSTIME_RECORD_WORDS; i++) {
            systime_last_reset[i] = reset_record[i];
        }
        systime_watchdog_resets = (counted ? reset_count[0] : 0) + 1;
    } else {
        systime_watchdog_resets = (counted ? reset_count[0] : 0);
    }
    reset_record[0] = 0;
    reset_count[0] = systime_watchdog_resets;
    reset_count[1] = ~systime_watchdog_resets;

    irq_register(2, systime_isr); // vector 34
    TIMER_CONTROL(1) = 0x02;      // reload mode
    TIMER_LOADCOUNT(1) = clock_apb_hz / 1000;
    TIMER_CONTROL(1) |= 0x01;
}
