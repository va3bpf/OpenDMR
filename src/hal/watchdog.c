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
 * OpenDMR, hardware abstraction: the watchdog (hw.h WDG_*). Its registers survive the reset it makes (only a power on
 * clears them), so clock_init() turns it off first thing; services/systime.c feeds it while the radio runs.
 * All from RAM: called from the 1 ms interrupt, which also runs while the flash is busy.
 */

#include "hw.h"
#include "hal.h"

#define WDG_COUNTS_PER_MS (0x40000000u / 60000u) // 0x40000000 counts are 60 s

// The counter counts up from WDG_OT_LOAD_H:L to the 35 bit overflow: 2^35 - counts, 7:(-counts) for up to 2^32 counts
static void RAM_CODE load(uint32_t ms)
{
    uint32_t counts = WDG_COUNTS_PER_MS * ms; // fits up to 60 s

    WDG_OT_LOAD_H = 0x7;
    WDG_OT_LOAD_L = 0u - counts;
}

void RAM_CODE watchdog_start(uint32_t timeout_ms)
{
    uint32_t psr = irq_save();

    WDG_LOCK = WDG_UNLOCK;
    WDG_EN = 0;
    load(timeout_ms);
    WDG_EN = 1;
    WDG_KNOCK = WDG_FEED;
    WDG_LOCK = 0;
    irq_restore(psr);
}

void RAM_CODE watchdog_feed(void)
{
    uint32_t psr = irq_save();

    WDG_LOCK = WDG_UNLOCK;
    WDG_KNOCK = WDG_FEED;
    WDG_LOCK = 0;
    irq_restore(psr);
}

void RAM_CODE watchdog_stop(void)
{
    uint32_t psr = irq_save();

    WDG_LOCK = WDG_UNLOCK;
    WDG_EN = 0;
    WDG_LOCK = 0;
    irq_restore(psr);
}

// A whole chip reset as at power on, by the watchdog after about 1 s (clock_init() turns it off again). The modem,
// the codec and the rest start from their reset state, unlike a jump to the bootloader. The power stays on while
// the power knob is on.
void RAM_CODE watchdog_reset(void)
{
    (void)irq_save();
    WDG_LOCK = WDG_UNLOCK;
    WDG_EN = 0;
    load(1000);
    WDG_EN = 1;
    WDG_KNOCK = WDG_FEED;
    for (;;) {
    }
}
