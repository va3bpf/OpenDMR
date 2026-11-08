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
 * OpenDMR, driver: the SPI0 controller (DesignWare SSI). After OpenDM32 user/DRV/C7000_DRV.c
 * (SPI_DRV_Init(), SPI_DRV_TxRx(), SPI_DRV_RdWr(), SPI_DRV_Write()).
 */

#include "hw.h"
#include "spi0.h"

#define SPI0_BASE   0x140A0000
#define SPI0_CTRLR0 REG(SPI0_BASE + 0x00)
#define SPI0_CTRLR1 REG(SPI0_BASE + 0x04)
#define SPI0_SSIENR REG(SPI0_BASE + 0x08)
#define SPI0_SER    REG(SPI0_BASE + 0x10)
#define SPI0_BAUDR  REG(SPI0_BASE + 0x14)
#define SPI0_TXFTLR REG(SPI0_BASE + 0x18)
#define SPI0_RXFTLR REG(SPI0_BASE + 0x1c)
#define SPI0_SR     REG(SPI0_BASE + 0x28)
#define SPI0_IMR    REG(SPI0_BASE + 0x2c)
#define SPI0_DMACR  REG(SPI0_BASE + 0x4c)
#define SPI0_DR     REG(SPI0_BASE + 0x60)

#define SR_BUSY BITSET(0)
#define SR_TFNF BITSET(1) // transmit FIFO not full
#define SR_TFE  BITSET(2) // transmit FIFO empty

void spi0_init(void)
{
    SPI0_SSIENR = 0x00;
    SPI0_CTRLR0 = 0x07; // 8 bit frames, mode 0
    SPI0_CTRLR1 = 0x00;
    SPI0_BAUDR = 0x02;
    SPI0_TXFTLR = 0x00;
    SPI0_RXFTLR = 0x00;
    SPI0_IMR = 0x00;
    SPI0_DMACR = 0x00;
    SPI0_SSIENR = 0x01;
}

// One byte out, one byte in
static uint8_t transfer(uint8_t out)
{
    while ((SPI0_SR & SR_TFNF) == 0) {
    }
    SPI0_DR = out;
    while ((SPI0_SR & SR_TFE) == 0) {
    }
    while (SPI0_SR & SR_BUSY) {
    }
    return (uint8_t)SPI0_DR;
}

void spi0_write(const uint8_t *data, uint32_t length)
{
    SPI0_SER = 0x01;
    for (uint32_t i = 0; i < length; i++) {
        (void)transfer(data[i]);
    }
    SPI0_SER = 0x00;
}

void spi0_read(uint8_t *data, uint32_t length)
{
    SPI0_SER = 0x01;
    for (uint32_t i = 0; i < length; i++) {
        data[i] = transfer(0xFF);
    }
    SPI0_SER = 0x00;
}
