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
 * OpenDMR, hardware abstraction: the clocks.
 * After OpenDM32 (user/main.c, user/DRV/C7000_DRV.c, ucos3/bsp).
 */

#include "hw.h"
#include "hal.h"

uint32_t clock_sys_hz;
uint32_t clock_apb_hz;

// 168 MHz from the 24 MHz crystal, APB 84 MHz (CLK_Init())
void RAM_CODE clock_init(void)
{
    // The watchdog off (a watchdog reset leaves it on, hal/hw.h), with its timeout back to the power on one (60 s)
    WDG_LOCK = WDG_UNLOCK;
    WDG_EN = 0;
    WDG_OT_LOAD_H = 0x7;
    WDG_OT_LOAD_L = 0xC0000000;
    WDG_LOCK = 0;

    SYS_SOFT_RSTN = 0x180;
    QUAD_ENABLE = 0x1000001;
    LCSFC_BAUDR = 0x02;
    CLK_MGR_REG0x10 &= ~0xffffff;                              // 168M = 24/1*28/2/2
    CLK_MGR_REG0x10 |= (1 << 18) | (28 << 6) | (2 << 3) | (2); // refdiv, fbdib, postdiv1, postdiv2
    CLK_MGR_REG0x28 = (7 << 16) | (6);                         // sdio and usb dividers
    CLK_MGR_REG0x24 &= ~0x0f;
    CLK_MGR_REG0x24 |= (1 << 1); // pll:ahb = 1:1, ahb:apb = 1:4

    clock_sys_hz = 168000000;
    clock_apb_hz = clock_sys_hz / 2;

    CLK_MGR_REG0x04 |= 0x03; // reset the apll and bpll
    do {
        delay_us(100);
    } while (!(CLK_MGR_REG0x04 & BITSET(31))); // locked

    CLK_MGR_REG0x04 &= ~0x04; // clock from the soc to the pll
    CLK_MGR_REG0x04 &= ~0x08;
    do {
        delay_us(100);
    } while (!(CLK_MGR_REG0x04 & BITSET(30)));

    CLK_MGR_REG0x2c = 0xFFFFFFFF;
    IO_DIPLEX2 &= ~0x03; // clkout as pllb
}
