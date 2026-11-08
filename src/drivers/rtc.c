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
 * OpenDMR, driver: the real time clock, on the internal I2C2 (DesignWare I2C, RTC at 0x70). From
 * OpenDM32 user/DRV/C7000_DRV.c (I2C2_Init(), I2C2_Read(), I2C2_Write(), RTC_GetTime() ...).
 *
 * RTC registers: read 0x00 seconds, 0x01 minutes, 0x02 hours, 0x03 / 0x04 days since 1970 (low, high); write 0x0C - 0x0E
 * the time, 0x0F / 0x10 the days, 0x12 the valid flag, 0x11 = 7 starts it. The waits of the main firmware had no
 * timeout: here a clock that doesn't answer fails the call instead of stopping the radio.
 */

#include "hw.h"
#include "hal.h"
#include "rtc.h"

#define I2C2_BASE          0x14080000
#define I2C2_CON           REG(I2C2_BASE + 0x00)
#define I2C2_TAR           REG(I2C2_BASE + 0x04)
#define I2C2_DATA_CMD      REG(I2C2_BASE + 0x10)
#define I2C2_FS_SCL_HCNT   REG(I2C2_BASE + 0x1C)
#define I2C2_FS_SCL_LCNT   REG(I2C2_BASE + 0x20)
#define I2C2_INTR_MASK     REG(I2C2_BASE + 0x30)
#define I2C2_RX_TL         REG(I2C2_BASE + 0x38)
#define I2C2_TX_TL         REG(I2C2_BASE + 0x3C)
#define I2C2_CLR_INTR      REG(I2C2_BASE + 0x40)
#define I2C2_ENABLE        REG(I2C2_BASE + 0x6C)
#define I2C2_STATUS        REG(I2C2_BASE + 0x70)
#define I2C2_ENABLE_STATUS REG(I2C2_BASE + 0x9C)
#define I2C2_START         REG(I2C2_BASE + 0xA0)

#define STATUS_ACTIVITY 0x01
#define STATUS_TFE      0x04 // transmit FIFO empty
#define STATUS_RFNE     0x08 // receive FIFO not empty
#define CMD_READ        0x100

#define RTC_ADDRESS 0x70
#define TIMEOUT_US  2000

static bool wait(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    for (uint32_t us = 0; us < TIMEOUT_US; us++) {
        if ((*reg & mask) == value) {
            return true;
        }
        delay_us(1);
    }
    return false;
}

void rtc_init(void)
{
    I2C2_ENABLE = 0x00;
    I2C2_CON = 0x65;
    I2C2_INTR_MASK = 0x40;
    I2C2_FS_SCL_HCNT = 0x61;
    I2C2_FS_SCL_LCNT = 0x68;
    I2C2_TX_TL = 0x06;
    I2C2_RX_TL = 0x06;
}

static bool write_register(uint8_t reg, uint8_t value)
{
    bool ok;

    I2C2_ENABLE = 0x00;
    (void)I2C2_CLR_INTR;
    I2C2_TAR = RTC_ADDRESS;
    I2C2_ENABLE = 0x01;
    I2C2_DATA_CMD = reg;
    I2C2_DATA_CMD = value;
    I2C2_START = 0x01;
    ok = wait(&I2C2_STATUS, STATUS_ACTIVITY | STATUS_TFE, STATUS_TFE);
    I2C2_START = 0x00;
    I2C2_ENABLE = 0x00;
    return wait(&I2C2_ENABLE_STATUS, 0x01, 0x00) && ok;
}

static bool read_register(uint8_t reg, uint8_t *value)
{
    bool ok;

    while (I2C2_STATUS & STATUS_RFNE) {
        (void)I2C2_DATA_CMD;
    }
    I2C2_ENABLE = 0x00;
    (void)I2C2_CLR_INTR;
    I2C2_TAR = RTC_ADDRESS;
    I2C2_ENABLE = 0x01;
    I2C2_DATA_CMD = reg;
    I2C2_DATA_CMD = CMD_READ;
    I2C2_START = 0x01;
    ok = wait(&I2C2_STATUS, STATUS_RFNE, STATUS_RFNE);
    *value = (uint8_t)I2C2_DATA_CMD;
    I2C2_START = 0x00;
    I2C2_ENABLE = 0x00;
    return ok;
}

bool rtc_read(uint32_t *seconds)
{
    uint8_t s, m, h, day_low, day_high;

    if (!read_register(0x00, &s) || !read_register(0x01, &m) || !read_register(0x02, &h) ||
        !read_register(0x03, &day_low) || !read_register(0x04, &day_high)) {
        return false;
    }
    *seconds = ((((uint32_t)day_high << 8) | day_low) * 86400UL) + (h * 3600UL) + (m * 60UL) + s;
    return true;
}

bool rtc_write(uint32_t seconds)
{
    uint32_t days = seconds / 86400UL;
    uint32_t in_day = seconds % 86400UL;

    return write_register(0x12, 0x00) &&                        // not valid while it is set
           write_register(0x0C, (in_day % 60) & 0x3F) &&        // seconds
           write_register(0x0D, ((in_day / 60) % 60) & 0x3F) && // minutes
           write_register(0x0E, (in_day / 3600) & 0x1F) &&      // hours
           write_register(0x0F, days & 0xFF) && write_register(0x10, (days >> 8) & 0xFF) &&
           write_register(0x12, 0x01) && // valid
           write_register(0x11, 0x07);   // start
}
