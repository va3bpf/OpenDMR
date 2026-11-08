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
 * OpenDMR, driver: the ADC. A conversion as OpenDM32 user/DRV/C7000_DRV.c ADC1_GetValue() /
 * ADC2_GetValue(): it ends on the channel's done flag (INTR_STA). The stock firmware (0x301bedc) waits for the state
 * register to be idle instead: run from RAM here that check came before the conversion had started and channel 1
 * read 0. Channel 1 is the VOX (0x30 bits 25:16, the stock firmware's VOX channel), channel 2 the battery (0x34 bits
 * 9:0). With the interrupts off: the input task reads the battery, the radio task the VOX.
 */

#include "hw.h"
#include "hal.h"
#include "adc.h"

#define ADC_BASE         0x140D0000
#define ADC_CTRL         REG(ADC_BASE + 0x00)
#define ADC_INTR         REG(ADC_BASE + 0x08)
#define ADC_INTR_STA     REG(ADC_BASE + 0x0C)
#define ADC_START        REG(ADC_BASE + 0x14)
#define ADC_CTRL_STATE   REG(ADC_BASE + 0x18)
#define ADC_CTRL_STOP    REG(ADC_BASE + 0x20)
#define ADC_CH_VLD       REG(ADC_BASE + 0x24)
#define ADC_PD_SEOC_TIME REG(ADC_BASE + 0x28)
#define ADC_P2S_EN       REG(ADC_BASE + 0x2C)
#define ADC_DATA_AB      REG(ADC_BASE + 0x30)
#define ADC_DATA_CD      REG(ADC_BASE + 0x34)

#define CHANNEL_VOX     1
#define CHANNEL_BATTERY 2

void adc_init(void)
{
    IO_DIPLEX2 &= ~(BITSET(20) | BITSET(21)); // the analog inputs (stock 0x301be84)
}

static uint32_t RAM_CODE convert(int channel, volatile uint32_t *data)
{
    uint32_t psr = irq_save();
    uint32_t value;

    ADC_CTRL = 0x00;
    ADC_CTRL = 0x08;
    ADC_CTRL_STOP = 0x02;
    while ((ADC_CTRL_STATE & 0x3F) != 0) {
        ADC_CTRL_STOP = 0x02;
    }
    ADC_CTRL = 0x02;
    ADC_PD_SEOC_TIME = 0xA20;
    ADC_P2S_EN = 0x00;
    ADC_INTR = 0x10000 | BITSET(channel);
    ADC_CH_VLD = BITSET(channel);
    ADC_START = 0x01;
    while ((ADC_INTR_STA & BITSET(channel)) == 0x00) {
    }
    ADC_INTR = 0;
    ADC_CTRL_STOP = 0x03;
    value = *data;
    irq_restore(psr);
    return value;
}

uint16_t RAM_CODE adc_read_battery(void)
{
    return (uint16_t)(convert(CHANNEL_BATTERY, &ADC_DATA_CD) & 0x3FF);
}

uint16_t RAM_CODE adc_read_vox(void)
{
    return (uint16_t)((convert(CHANNEL_VOX, &ADC_DATA_AB) >> 16) & 0x3FF);
}
