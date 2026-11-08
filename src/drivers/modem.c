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
 * OpenDMR, driver: the HR_C7000 baseband, codec and DACs for FM. After OpenDM32 hardware/HR-C7000.c
 * (HRC7000Init() without the DMR, DTMF and measuring ADC parts; HRC7000SetFMRx(), HRC7000SetFMTx(),
 * HRC7000SetFmAudio(), HRC7000SetFrequencyCalibration(), HRC7000SetMicGainFM()) and interfaces/dac.c, DM32 branches.
 */

#include "hw.h"
#include "hal.h"
#include "modem.h"

// Baseband (MODEM_REG_BASE, C7000_DRV.h)
#define AUDIO_CONTROL    REG(MODEM_REG_BASE + 0x0080) // [31] prompt FIFO full, [7] prompt path, [6] FM audio path
#define DAC_CONTROL      REG(MODEM_REG_BASE + 0x0070)
#define ADC_CONTROL      REG(MODEM_REG_BASE + 0x0074)
#define LINEOUT_CTRL     REG(MODEM_REG_BASE + 0x0088)
#define WORK_MODE        REG(MODEM_REG_BASE + 0x0100)
#define RF_MODE          REG(MODEM_REG_BASE + 0x0104)
#define SIG_CENTER       REG(MODEM_REG_BASE + 0x0108)
#define RF_CONTROL       REG(MODEM_REG_BASE + 0x0110)
#define RF_MOD_BIAS_CTRL REG(MODEM_REG_BASE + 0x0114)
#define DEV_LIMITER      REG(MODEM_REG_BASE + 0x0118)
#define RSSI_BIAS_VALUE  REG(MODEM_REG_BASE + 0x011C)
#define FM_BANDWIDTH     REG(MODEM_REG_BASE + 0x0500)
#define FM_DEV_COEF      REG(MODEM_REG_BASE + 0x0504)
#define FM_PTT           REG(MODEM_REG_BASE + 0x0560)

// Codec (8-bit registers)
#define CODEC_BASE 0x160009C0
#define CODEC(off) (*(volatile uint8_t *)(CODEC_BASE + (off)))
#define AICR_DAC   CODEC(0x08)
#define AICR_ADC   CODEC(0x09)
#define CR_MIC1    CODEC(0x0B)
#define CR_MIC2    CODEC(0x0C)
#define CR_DAC     CODEC(0x0D)
#define CR_ADC     CODEC(0x0F)
#define CR_VIC     CODEC(0x12)
#define CR_CK      CODEC(0x13)
#define GCR_DACL   CODEC(0x1F)
#define GCR_MIC1   CODEC(0x23)
#define GCR_ADCL   CODEC(0x25)
#define CR_DAC_AGC CODEC(0x2B)
#define CR_ADC_AGC CODEC(0x2F)
#define DR_ADC_AGC CODEC(0x30)

// DACs (0x140F0000): B drives the PA bias
#define DAC_BASE       0x140F0000
#define DAC_PD_CTRL    REG(DAC_BASE + 0x00)
#define DAC_PD_MODE_EN REG(DAC_BASE + 0x04)
#define DAC_DATA_A     REG(DAC_BASE + 0x08)
#define DAC_DATA_B     REG(DAC_BASE + 0x0C)
#define DAC_DATA_C     REG(DAC_BASE + 0x10)

#define WORK_MODE_FM       0x80
#define WORK_MODE_FM_TX    0xEE // stock, transmitting FM
#define RF_MODE_DIRECT     0x03269860 // RF_MODE bit 11 in direct mode (stock 0x301d9c0)
#define RF_CONTROL_DIRECT  0x00040A0A
#define DEFAULT_SIG_CENTER 0x6D6C0000 // without a factory table: the radio the port was developed on
#define DEFAULT_MOD_BIAS   0x01F0FFFF

void modem_init(void)
{
    IO_DIPLEX2 &= ~BITSET(28);

    // DACs: B and C on, A low power
    DAC_PD_CTRL = 0x01;
    DAC_PD_MODE_EN = 0x01;
    DAC_DATA_A = 0x0000;
    DAC_DATA_B = 0x0000; // PA bias off
    DAC_DATA_C = 0x06E2;

    FM_BANDWIDTH = 0x5F;
    FM_DEV_COEF = 0x33500;

    // Codec
    SYS_SOFT_RSTN &= ~BITSET(4);
    delay_us(2);
    AICR_DAC = 0xC0;
    AICR_ADC = 0xC0;
    CR_MIC1 = 0x00;
    CR_MIC2 = 0x70;
    CR_DAC = 0x20;
    CR_ADC = 0x20;
    CR_VIC = 0x0C;
    CR_CK = 0x40;
    GCR_DACL = 0x87;
    GCR_MIC1 = 0x05; // the DMR mic's analog boost, 20 dB, with the DMR mic gain (GCR_ADCL) at 16 dB: words at about
                     // -25 dBFS, peaks -11 (tools/debug/miclevel.py); at 0 dB they were about -50, sent too quiet
    GCR_ADCL = 0x8B;
    CR_DAC_AGC = 0x00;
    CR_ADC_AGC = 0x00;
    DR_ADC_AGC = 0x34;
    LINEOUT_CTRL = 0x01;

    // The DAC interface on from the start (else an FM start stays silent until DMR was used, main firmware note)
    DAC_CONTROL = 0x8000001B;
    ADC_CONTROL |= 0x41C3;

    RF_CONTROL = RF_CONTROL_DIRECT;
    modem_set_frequency_calibration(false, 0, 0);
    RF_MODE = RF_MODE_DIRECT;
    DEV_LIMITER = 0x0000FF00;
    RSSI_BIAS_VALUE = 0x00001381;
    WORK_MODE = WORK_MODE_FM;
}

void modem_set_frequency_calibration(bool valid, uint8_t osc_tune, uint16_t mod_bias)
{
    if (valid) {
        uint32_t osc = osc_tune;

        SIG_CENTER = (osc << 24) | (osc << 16);            // Tx frequency
        RF_MOD_BIAS_CTRL = ((osc + 384) << 16) | mod_bias; // Rx frequency
    } else {
        SIG_CENTER = DEFAULT_SIG_CENTER;
        RF_MOD_BIAS_CTRL = DEFAULT_MOD_BIAS;
    }
}

// The FM filters: 12.5 kHz 0x50 / 0x40A25, 25 kHz 0x5F / 0x40A20; bit 4 (pre / de-emphasis) on in receive too, as
// the stock firmware (without it the receive has a hiss the stock firmware doesn't have)
static void set_bandwidth(bool wide)
{
    FM_BANDWIDTH = wide ? 0x5F : 0x50;
    FM_DEV_COEF = wide ? 0x40A20 : 0x40A25;
}

void modem_fm_receive(bool wide, uint8_t dac_gain)
{
    set_bandwidth(wide);
    FM_PTT = 0x00;
    AUDIO_CONTROL = 0x40;
    GCR_DACL = dac_gain;
    DAC_PD_CTRL |= BITSET(1);
    WORK_MODE = WORK_MODE_FM;
}

// The FD6818 modulates FM from its own microphone input; the baseband stays out of it (no FM_PTT), with the values
// the stock firmware's registers had while transmitting FM: WORK_MODE 0xEE, FM_DEV_COEF 0x33500, AUDIO_CONTROL 0x02
void modem_fm_transmit(bool wide, uint8_t mic_gain)
{
    set_bandwidth(wide);
    FM_DEV_COEF = 0x33500;
    DAC_PD_CTRL &= ~BITSET(1);
    AUDIO_CONTROL = 0x02;
    WORK_MODE = WORK_MODE_FM_TX;
    FM_PTT = 0x00;
    GCR_ADCL = mic_gain | 0x80;
}

void RAM_CODE modem_fm_audio(bool on)
{
    if (on) {
        AUDIO_CONTROL |= BITSET(6);
        WORK_MODE = WORK_MODE_FM;
    } else {
        AUDIO_CONTROL &= ~BITSET(6);
    }
}

// The prompt path (beeps): PCM samples, 8 kHz, 16 bits little endian, into the FIFO at SOUND_FIFO, 32 bytes at a time
// (sound.c soundBeepTaskFunction()). The FM audio path is left off while it plays and put back as it was.
#define SOUND_FIFO ((volatile uint8_t *)0x16000980)

static bool fm_audio_before_beep = false;

void modem_beep(bool on)
{
    if (on) {
        fm_audio_before_beep = ((AUDIO_CONTROL & BITSET(6)) != 0);
        AUDIO_CONTROL = (AUDIO_CONTROL & ~BITSET(6)) | BITSET(7);
    } else {
        AUDIO_CONTROL = (AUDIO_CONTROL & ~BITSET(7)) | (fm_audio_before_beep ? BITSET(6) : 0);
    }
}

bool modem_beep_fifo_full(void)
{
    return (AUDIO_CONTROL & BITSET(31)) != 0;
}

void modem_beep_write(const uint8_t data[MODEM_BEEP_BLOCK])
{
    for (int i = 0; i < MODEM_BEEP_BLOCK; i++) {
        SOUND_FIFO[i] = data[i];
    }
}

void modem_pa_drive(uint16_t value)
{
    DAC_DATA_B = value;
}
