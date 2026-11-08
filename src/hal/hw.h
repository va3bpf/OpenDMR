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
 * OpenDMR bring-up: the HR_C7000 / DH4570 registers and DM-32 pins used, from
 * OpenDM32 user/DRV/C7000_DRV.h (and the DM-32 pin map of the OpenDM32 forum thread, 14.06.26).
 */

#ifndef DM32_HW_H
#define DM32_HW_H

#include <stdint.h>
#include <stdbool.h>

#define BITSET(n) (0x01UL << (n))
#define BITCLR(n) (~(0x01UL << (n)))

#define RAM_CODE    __attribute__((section(".staticcode"))) // code run from RAM
#define FLASH_CONST __attribute__((section(".flashconst"))) // big constant data kept in the flash (fonts)

#define REG(a) (*(volatile uint32_t *)(a))

// System, clocks and pin mux
#define MODEM_REG_BASE  0x11000000
#define SYS_SOFT_RSTN   REG(MODEM_REG_BASE + 0x00)
#define CLK_MGR_REG0x04 REG(MODEM_REG_BASE + 0x04)
#define CLK_MGR_REG0x10 REG(MODEM_REG_BASE + 0x10)
#define CLK_MGR_REG0x24 REG(MODEM_REG_BASE + 0x24)
#define CLK_MGR_REG0x28 REG(MODEM_REG_BASE + 0x28)
#define CLK_MGR_REG0x2c REG(MODEM_REG_BASE + 0x2c)
#define LCSFC_BAUDR     REG(MODEM_REG_BASE + 0x30)
#define IO_DIPLEX0      REG(MODEM_REG_BASE + 0x34)
#define IO_DIPLEX1      REG(MODEM_REG_BASE + 0x38)
#define IO_DIPLEX2      REG(MODEM_REG_BASE + 0x3c) // 1 = GPIO; [18:13] LCD_DB7 - DB2, [7] WR, [8] RD ...
#define IOMGR_REN_REG0  REG(MODEM_REG_BASE + 0x40)
#define IOMGR_REN_REG1  REG(MODEM_REG_BASE + 0x44)
#define IOMGR_REN_REG2  REG(MODEM_REG_BASE + 0x48)
#define QUAD_ENABLE     REG(MODEM_REG_BASE + 0x5c)

// GPIO A (pins 0 - 31), B (32 - 63), C (64 - 95)
#define GPIO_BASE(pin)  (((pin) < 32) ? 0x14020000 : (((pin) < 64) ? 0x14100000 : 0x14110000))
#define GPIO_DR(pin)    REG(GPIO_BASE(pin) + 0x00)
#define GPIO_DDR(pin)   REG(GPIO_BASE(pin) + 0x04)
#define GPIO_INTEN(pin) REG(GPIO_BASE(pin) + 0x30)
#define GPIO_EXT(pin)   REG(GPIO_BASE(pin) + 0x50)
#define GPIO_BIT(pin)   BITSET((pin) & 31)
#define GPIOA_DR        REG(0x14020000)
#define GPIOA_DDR       REG(0x14020004)
#define GPIOB_DR        REG(0x14100000)
#define GPIOB_DDR       REG(0x14100004)
#define GPIOC_DR        REG(0x14110000)
#define GPIOC_DDR       REG(0x14110004)

// Timers, register blocks n * 0x14: block 0 (vector 33) the FreeRTOS tick, block 1 (vector 34) the 1 ms timer
// ("TIMER1" and "TIMER2" in the main firmware), block 2 (vector 35) the battery ADC
#define TIMER_LOADCOUNT(n) REG(0x14000000 + ((n) * 0x14) + 0x00)
#define TIMER_CURRENT(n)   REG(0x14000000 + ((n) * 0x14) + 0x04)
#define TIMER_CONTROL(n)   REG(0x14000000 + ((n) * 0x14) + 0x08)
#define TIMER_EOI(n)       REG(0x14000000 + ((n) * 0x14) + 0x0C)
#define TIMER_INTSTAT(n)   REG(0x14000000 + ((n) * 0x14) + 0x10)

// Interrupt controller: vectors 32 - 63 are interrupts 0 - 31
// Watchdog (HR_C7000 manual v2.7, WDT): the timeout counter counts up from WDG_OT_LOAD (H:L, 35 bits) to the
// overflow, then the whole chip resets; its registers keep their values through that reset (only a power on resets
// them), so a watchdog left on resets again and again
#define WDG_LOCK     REG(0x14010000 + 0x00) // WDG_UNLOCK: writes allowed, anything else: locked
#define WDG_OT_LOAD_H REG(0x14010000 + 0x04)
#define WDG_OT_LOAD_L REG(0x14010000 + 0x08)
#define WDG_EN       REG(0x14010000 + 0x10)
#define WDG_KNOCK    REG(0x14010000 + 0x14) // WDG_FEED: the count starts again from WDG_OT_LOAD
#define WDG_UNLOCK   0x5ada7200
#define WDG_FEED     0x55aadd22
#define PIC_MODE     REG(0x17000000 + 0x00)
#define PIC_PO       REG(0x17000000 + 0x04)
#define PIC_MASK     REG(0x17000000 + 0x08) // 1 = masked
#define PIC_INT_ST   REG(0x17000000 + 0x44)
#define PIC_INT_ST_1 REG(0x17000000 + 0x48)
#define PIC_MODE_1   REG(0x17000000 + 0x60)
#define PIC_PO_1     REG(0x17000000 + 0x64)
#define PIC_MASK_1   REG(0x17000000 + 0x68) // vectors 64 ..: 1 = masked
#define IRQ_TIMER0   1 // vector 33

// LCD (ST7789, 240 x 320, I8080 8 bit bus)
#define I8080_CMD    REG(0x12000000 + 0x00)
#define I8080_DATA   REG(0x12000000 + 0x04)
#define I8080_TIMING REG(0x12000000 + 0x10)
#define I8080_CTRL   REG(0x12000000 + 0x28)

// Pins
#define PIN_LCD_BKLIGHT 0x0F
#define PIN_PWR_SWCHECK 0x10
#define PIN_PWR_SW      0x11 // held high: the radio stays on
#define PIN_SPI0_CS     0x12
#define PIN_KEY_BKLIGHT 0x28
#define PIN_MIC_MUT     0x2E
#define PIN_LCD_NRESET  0x42
#define PIN_LCD_RD      0x46
#define PIN_KEYPAD_ROW0 71
#define PIN_KEYPAD_ROW4 75

#endif
