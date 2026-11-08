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
 * OpenDMR, driver: the FD6818B RF transceiver. After OpenDM32 hardware/AT1846S.c (soft SPI, the
 * tables, DCS, CTCSS) and hardware/radioHardwareInterface.c (frequency, receive, transmit), DM32 branches.
 *
 * The soft SPI runs from RAM with the bit timing of the stock firmware (its delay 0x2df70: 5 counts a half bit to
 * write, 10 to read). Each register access is a critical section, as the main firmware.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "codeplug.h"
#include "tones.h"
#include "fd6818.h"

#define PIN_SCL 0x07
#define PIN_SDA 0x08
#define PIN_SCN 0x18
#define PIN_GP0 0x3B
#define PIN_GP1 0x3A

#define REG_AF        0x47 // 0x6142: receive audio out, 0x6042: muted
#define REG_VOLUME    0x48
#define REG_BANDWIDTH 0x43
#define REG_CONTROL   0x30
#define REG_TONE      0x07
#define REG_DCS       0x08
#define REG_CSS_MODE  0x51
#define REG_FLAGS     0x0C
#define REG_SIGNAL    0x67
#define REG_NOISE     0x65

#define CONTROL_IDLE 0x0000
#define CONTROL_RX   0xBFF1
#define CONTROL_TX   0xC1FE
#define CONTROL_TONE 0xC3FA // transmitting a tone: the mic ADC off (bit 2), the AF DAC on (bit 9), as the stock firmware
#define REG_TONES    0x70   // bit 15 Tone1 on, 14:8 its gain; bit 7 Tone2 on, 6:0 its gain
#define REG_TONE1_HZ 0x71   // Hz x 10.32444
#define REG_TONE2_HZ 0x72
#define TONE1_ON     0x8000
#define TONE2_ON     0x0080
#define TONE_SCALE   103244 // x 10000
#define BANDWIDTH_25 0x3028
#define BANDWIDTH_12 0x4008
#define BANDWIDTH_DMR 0x4048 // DMR (always 12.5 kHz): with the FM value the repeater downlink wasn't decoded

#define CTCSS_SCALE 20649 // register = Hz * 20.64888 (the DCS bit rate 134.4 Hz is 0x0AD7)
#define DCS_TONE    0x0AD7
#define DCS_TX_GAIN 0x3D
#define DCS_CODES   83

typedef struct {
    uint8_t reg;
    uint16_t value;
} fd6818_setting_t;

// Power on (AT1846InitSettings)
static const fd6818_setting_t init_settings[] = {
    {0x00, 0x8000}, {0x00, 0x0000}, {0x37, 0x1D0F}, {0x13, 0x03BE}, {0x12, 0x037B}, {0x11, 0x027B}, {0x10, 0x007A},
    {0x14, 0x0019}, {0x49, 0x2A38}, {0x7B, 0x8420}, {0x48, 0x33EF}, {0x1F, 0x5454}, {0x25, 0xC1BA}, {0x77, 0x88EF},
    {0x3E, 0xA037}, {0x13, 0x037E}, {0x7D, 0xE35E}, {0xB3, 0xFF00}, {0x33, 0xFE00}, {0xB4, 0x0000}, {0x34, 0x0009},
    {0xB3, 0xFE00}, {0x33, 0xFC00}, {0xB4, 0x0009}, {0x34, 0x0019}, {0x10, 0x007A}, {0x11, 0x027B}, {0x12, 0x037B},
    {0x13, 0x03BE}, {0x14, 0x0019}, {0x1F, 0x5454}, {0x30, 0xBFF1}, {0x37, 0x1D1F}, {0x3E, 0xA037}, {0x47, 0x0100},
    {0x48, 0xBBBF}, {0x49, 0x2A38}, {0x4E, 0x4708}, {0x4F, 0x302F}, {0x52, 0x028F}, {0x7B, 0x8420}, {0x09, 0x006F},
    {0x09, 0x106B}, {0x09, 0x2067}, {0x09, 0x3062}, {0x09, 0x4050}, {0x09, 0x5047}, {0x09, 0x603A}, {0x09, 0x702C},
    {0x09, 0x8041}, {0x09, 0x9037}, {0x09, 0xA025}, {0x09, 0xB017}, {0x09, 0xC0E4}, {0x09, 0xD0CB}, {0x09, 0xE0B5},
    {0x09, 0xF09F},
};

// After the modem init (AT1846PostinitSettings)
static const fd6818_setting_t post_init_settings[] = {
    {0x47, 0x0000}, {0x13, 0x037E}, {0x47, 0x6042}, {0x7E, 0x302E}, {0x2B, 0xC000}, {0x43, 0x3028}, {0x78, 0x6968},
    {0x30, 0x0000}, {0x38, 0xBA00}, {0x39, 0x00DB}, {0x32, 0x0244}, {0x07, 0x0470}, {0x07, 0x21CE}, {0x51, 0x0000},
    {0x3F, 0x0000}, {0x37, 0x1F0F}, {0x30, 0x0000}, {0x30, 0xBFF1}, {0x47, 0x6042}, {0x48, 0xBBBF},
};

// FM, as the OpenDM32 releases (0x0304E8F0 in 20260711); 0x50 = 0x3B20 (the DMR value 0xBB20 gives a low deviation)
// DMR, as the OpenDM32 releases (0x0304E8B1 in 20260711): 0x50 = 0xBB20, the FD6818 gives the HR_C7000 its raw
// baseband (without bit 15 it keeps sending demodulated audio and no DMR sync is ever found)
static const fd6818_setting_t dmr_settings[] = {
    {0x31, 0x0000}, {0x42, 0x6F5A}, {0x2A, 0x7400}, {0x47, 0x0000}, {0x30, 0x0000}, {0x13, 0x037E}, {0x47, 0x0140},
    {0x48, 0xBBBF}, {0x7E, 0xB03F}, {0x2B, 0x0700}, {0x43, 0x4048}, {0x78, 0x6968}, {0x30, 0x0000}, {0x37, 0x1F1F},
    {0x50, 0xBB20}, {0x30, 0x0000}, {0x30, 0xBFF1}, {0x47, 0x0140}, {0x3F, 0x0000}, {0xC7, 0x0140}, {0x47, 0x0140},
};

static bool dmr = false; // the DMR settings are in: its bandwidth setting

static uint16_t bandwidth(bool wide)
{
    return dmr ? BANDWIDTH_DMR : (wide ? BANDWIDTH_25 : BANDWIDTH_12);
}

static const fd6818_setting_t fm_settings[] = {
    {0x31, 0x0000}, {0x42, 0x6F5A}, {0x2A, 0x7400}, {0x30, 0x0000}, {0x13, 0x037E}, {0x47, 0x6042},
    {0x7E, 0x302E}, {0x2B, 0xC000}, {0x43, 0x3028}, {0x07, 0x0470}, {0x07, 0x21CE}, {0x51, 0x0000},
    {0x3F, 0x0000}, {0x37, 0x1F0F}, {0x50, 0x3B20}, {0x30, 0xBFF1}, {0x47, 0x6042}, {0x48, 0xBBBF},
};

// The band registers 0x1A / 0x40 by frequency (FD6818_SelBand_Reg1A()), from the top
static const struct {
    uint32_t from;
    uint16_t reg_1a;
    uint16_t reg_40;
} bands[] = {
    {73999999, 0x1F80, 0x3AF0}, {36999999, 0x2F50, 0x39F0}, {26000000, 0x9F30, 0x385C}, {18499999, 0x3F48, 0x38F0},
    {12399999, 0xAF28, 0x375C}, {9299999, 0x4F44, 0x37F0},  {6199999, 0xBF24, 0x365C},  {4699999, 0x5F42, 0x36F0},
    {3099999, 0xCF22, 0x355C},  {2399999, 0x6F41, 0x35F0},  {1599999, 0xDF21, 0x345C},
};

// 83 DCS codes (9 bits) with their Golay parity (11 bits), 20 bits packed in 3 bytes, by code
static const uint8_t dcs_packed[DCS_CODES * 3] = {
    0x63, 0x87, 0x09, 0xB7, 0x86, 0x0A, 0x5D, 0x06, 0x0B, 0x1F, 0x85, 0x0C, 0xF5, 0x05, 0x0D, 0xB6, 0x85, 0x11,
    0xFD, 0x80, 0x13, 0xCA, 0x87, 0x14, 0xF4, 0x06, 0x16, 0xD1, 0x85, 0x1A, 0x79, 0x86, 0x1C, 0x93, 0x06, 0x1D,
    0xE6, 0x82, 0x1D, 0x47, 0x07, 0x1E, 0x5E, 0x03, 0x26, 0x2B, 0x87, 0x26, 0xC1, 0x07, 0x27, 0x7B, 0x80, 0x2A,
    0xD3, 0x83, 0x2C, 0x39, 0x03, 0x2D, 0xED, 0x02, 0x2E, 0x7A, 0x83, 0x31, 0xEC, 0x01, 0x35, 0x4D, 0x84, 0x36,
    0xA7, 0x04, 0x37, 0xBC, 0x06, 0x39, 0x1D, 0x83, 0x3A, 0x5F, 0x00, 0x3D, 0x8B, 0x01, 0x3E, 0xE9, 0x86, 0x42,
    0x8E, 0x86, 0x49, 0xB0, 0x07, 0x4B, 0x5B, 0x84, 0x51, 0xFA, 0x01, 0x52, 0x8F, 0x85, 0x52, 0x27, 0x86, 0x54,
    0x77, 0x81, 0x58, 0xE8, 0x85, 0x59, 0x3C, 0x84, 0x5A, 0x94, 0x87, 0x5C, 0xCF, 0x00, 0x63, 0x8D, 0x83, 0x64,
    0xC6, 0x86, 0x66, 0x3E, 0x82, 0x6C, 0x97, 0x82, 0x71, 0xA9, 0x03, 0x73, 0xEB, 0x80, 0x74, 0x85, 0x06, 0x7A,
    0xF0, 0x82, 0x7A, 0x58, 0x81, 0x7C, 0x76, 0x87, 0x84, 0x9C, 0x07, 0x85, 0xE9, 0x83, 0x85, 0xB9, 0x84, 0x89,
    0xC5, 0x86, 0x8C, 0x2F, 0x06, 0x8D, 0xB8, 0x87, 0x92, 0x7E, 0x02, 0x9A, 0x0B, 0x86, 0x9A, 0xE1, 0x06, 0x9B,
    0xC6, 0x83, 0xA1, 0xF8, 0x02, 0xA3, 0x1B, 0x04, 0xA7, 0xE3, 0x00, 0xAD, 0x9E, 0x01, 0xB3, 0xC7, 0x80, 0xBA,
    0xD9, 0x05, 0xC3, 0x71, 0x06, 0xC5, 0xF5, 0x00, 0xCA, 0x1F, 0x80, 0xCB, 0x28, 0x87, 0xCC, 0xC2, 0x07, 0xCD,
    0xC3, 0x04, 0xD6, 0x47, 0x02, 0xD9, 0x93, 0x03, 0xDA, 0x2B, 0x82, 0xE1, 0xBD, 0x00, 0xE5, 0x98, 0x83, 0xE9,
    0xE4, 0x81, 0xEC, 0x0E, 0x01, 0xED, 0xDA, 0x00, 0xEE, 0x4D, 0x81, 0xF1, 0x0F, 0x02, 0xF6,
};

/* The soft SPI, in RAM */

static void __attribute__((noinline)) RAM_CODE spi_delay(uint32_t counts)
{
    for (volatile uint32_t i = counts; i != 0; i--) {
    }
}

static void RAM_CODE spi_send_byte(uint8_t byte)
{
    gpio_output(PIN_SDA, true);
    gpio_reset(PIN_SCL);
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
        if (byte & mask) {
            gpio_set(PIN_SDA);
        } else {
            gpio_reset(PIN_SDA);
        }
        spi_delay(5);
        gpio_set(PIN_SCL);
        spi_delay(5);
        gpio_reset(PIN_SCL);
    }
    gpio_set(PIN_SDA);
}

static uint16_t RAM_CODE spi_read_word(void)
{
    uint16_t word = 0;

    gpio_input(PIN_SDA);
    gpio_reset(PIN_SCL);
    for (int i = 0; i < 16; i++) {
        gpio_set(PIN_SCL);
        spi_delay(10);
        word = (uint16_t)((word << 1) | (((GPIO_EXT(PIN_SDA) & GPIO_BIT(PIN_SDA)) != 0) ? 1 : 0));
        gpio_reset(PIN_SCL);
        spi_delay(10);
    }
    return word;
}

// From a task or an interrupt (the DMR engine switches the transmitter per burst): interrupts off, not
// taskENTER_CRITICAL()
void RAM_CODE fd6818_write(uint8_t reg, uint16_t value)
{
    uint32_t psr = irq_save();
    gpio_reset(PIN_SCN);
    spi_send_byte(reg);
    spi_send_byte((uint8_t)(value >> 8));
    spi_send_byte((uint8_t)value);
    gpio_set(PIN_SCN);
    irq_restore(psr);
}

uint16_t RAM_CODE fd6818_read(uint8_t reg)
{
    uint16_t value;
    uint32_t psr = irq_save();
    gpio_reset(PIN_SCN);
    spi_send_byte(reg | 0x80);
    spi_delay(10);
    value = spi_read_word();
    gpio_set(PIN_SCN);
    irq_restore(psr);
    return value;
}

void RAM_CODE fd6818_read_signal(uint8_t *signal, uint8_t *noise)
{
    *signal = (uint8_t)((fd6818_read(REG_SIGNAL) & 0x1FF) >> 1);
    *noise = (uint8_t)fd6818_read(REG_NOISE);
}

static void write_table(const fd6818_setting_t *settings, int count)
{
    for (int i = 0; i < count; i++) {
        fd6818_write(settings[i].reg, settings[i].value);
    }
}

#define WRITE_TABLE(table) write_table((table), (int)(sizeof(table) / sizeof((table)[0])))

/* Set up */

void fd6818_init(void)
{
    gpio_input(PIN_GP0);
    gpio_input(PIN_GP1);
    gpio_output(PIN_SDA, false);
    gpio_output(PIN_SCL, false);
    gpio_output(PIN_SCN, true);
    WRITE_TABLE(init_settings);
}

void fd6818_post_init(void)
{
    WRITE_TABLE(post_init_settings);
}

// The FM table, then the receive level as the stock firmware (0x300f148: 0x31E0 | level)
void fd6818_set_fm(uint8_t rx_volume)
{
    dmr = false;
    WRITE_TABLE(fm_settings);
    fd6818_write(REG_VOLUME, 0x31E0 | (rx_volume & 0x0F));
}

// AT1846sSetMode(RADIO_MODE_DIGITAL)
void fd6818_set_dmr(void)
{
    dmr = true;
    WRITE_TABLE(dmr_settings);
}

/* Frequency, receive, transmit */

void fd6818_set_frequency(uint32_t frequency, bool wide)
{
    fd6818_write(REG_CONTROL, CONTROL_IDLE); // AF muted
    for (unsigned i = 0; i < (sizeof(bands) / sizeof(bands[0])); i++) {
        if (frequency >= bands[i].from) {
            fd6818_write(0x1A, bands[i].reg_1a);
            fd6818_write(0x40, bands[i].reg_40);
            break;
        }
    }
    fd6818_write(0x38, (uint16_t)frequency);
    fd6818_write(0x39, (uint16_t)(frequency >> 16));
    fd6818_write(REG_BANDWIDTH, bandwidth(wide));
    fd6818_write(REG_CONTROL, CONTROL_RX);
}

void fd6818_receive(bool wide)
{
    fd6818_write(REG_CONTROL, CONTROL_IDLE);
    fd6818_write(REG_BANDWIDTH, bandwidth(wide));
    fd6818_write(REG_CONTROL, CONTROL_RX);
}

// The FM mic sensitivity (0x7D bits 4:0, BK4819 REG_7D), as the stock firmware (0x300e618: 0xE340 | 22 .. 30, 26 by
// default; the power on table had 30)
void fd6818_set_mic_gain(uint8_t gain)
{
    fd6818_write(0x7D, (uint16_t)(0xE340 | ((gain > FD6818_MIC_GAIN_MAX) ? FD6818_MIC_GAIN_MAX : gain)));
}

void fd6818_transmit_prepare(void)
{
    fd6818_write(0x3F, 0x0000);
    fd6818_write(0x40, 0x34EA);
    fd6818_write(0x37, 0x1D0F);
}

void fd6818_transmit(bool wide)
{
    fd6818_write(REG_CONTROL, CONTROL_IDLE);
    fd6818_write(REG_BANDWIDTH, bandwidth(wide));
    fd6818_write(REG_CONTROL, CONTROL_TX);
}

/* DMR, each Tx burst, as the stock firmware's RF task (DM32.01.L01.048): only the frequency words change, the audio
   out and the raw baseband (0x50 bit 15) are off while transmitting */

#define DMR_37_TX    0x1D0F
#define DMR_37_RX    0x1F1F
#define DMR_50_TX    0x3B18 // stock 0x2b5a8
#define DMR_50_RX    0xBB20 // the raw baseband to the HR_C7000 (dmr_settings[])
#define DMR_AF_RX    0x0140
#define AF_OUT_MASK  0x0F00

static void frequency_words(uint32_t frequency)
{
    fd6818_write(0x38, (uint16_t)frequency);
    fd6818_write(0x39, (uint16_t)(frequency >> 16));
}

// 0x2b674: the frequency, idle, the audio out off, transmit, the baseband path for transmitting
void fd6818_dmr_burst_tx(uint32_t frequency)
{
    frequency_words(frequency);
    fd6818_write(REG_CONTROL, CONTROL_IDLE);
    fd6818_write(REG_AF, fd6818_read(REG_AF) & (uint16_t)~AF_OUT_MASK);
    fd6818_write(0x37, DMR_37_TX);
    fd6818_write(0x52, 0x028F);
    fd6818_write(REG_CONTROL, CONTROL_IDLE);
    fd6818_write(REG_CONTROL, CONTROL_TX);
    fd6818_write(0x50, DMR_50_TX);
}

// 0x2b6a0: idle, the frequency, receive, the audio out; 0x50 back to the raw baseband (stock elsewhere)
void fd6818_dmr_burst_rx(uint32_t frequency)
{
    fd6818_write(REG_CONTROL, CONTROL_IDLE);
    frequency_words(frequency);
    fd6818_write(0x32, 0x0244);
    fd6818_write(0x37, DMR_37_RX);
    fd6818_write(REG_CONTROL, CONTROL_IDLE);
    fd6818_write(REG_CONTROL, CONTROL_RX);
    fd6818_write(REG_AF, DMR_AF_RX);
    fd6818_write(0x3F, 0x0000);
    fd6818_write(0x50, DMR_50_RX);
}

// docs/fd6818-registers.md; BK4819_TransmitTone() of the UV-K5 firmware does the same
static uint16_t tone_word(uint16_t hz)
{
    return (uint16_t)((((uint32_t)hz * TONE_SCALE) + 5000) / 10000);
}

void fd6818_tx_tone(uint16_t hz1, uint16_t hz2, uint8_t gain)
{
    uint16_t tones = 0;

    if (hz1 != 0) {
        fd6818_write(REG_TONE1_HZ, tone_word(hz1));
        tones |= (uint16_t)(TONE1_ON | ((gain & 0x7F) << 8));
    }
    if (hz2 != 0) {
        fd6818_write(REG_TONE2_HZ, tone_word(hz2));
        tones |= (uint16_t)(TONE2_ON | (gain & 0x7F));
    }
    fd6818_write(REG_TONES, tones);
    fd6818_write(REG_CONTROL, CONTROL_TONE);
}

void fd6818_tx_tone_hz(uint16_t hz)
{
    fd6818_write(REG_TONE1_HZ, tone_word(hz));
}

void fd6818_tx_tone_end(void)
{
    fd6818_write(REG_TONES, 0x0000);
    fd6818_write(REG_CONTROL, CONTROL_TX);
}

// The stock firmware's tail (0x300E84C, as BK4819_GenTail() of the UV-K5 firmware), then 300 ms before the carrier
// goes: 0x52 bit 15 the shift or the 134.4 Hz of DCS, 14:13 its kind; or the CTCSS generator to 55 Hz
#define REG_TAIL        0x52
#define TAIL_NONE       0x028F // the init value
#define TAIL_SHIFT_240  0xE28F
#define TAIL_DCS_134    0x828F
#define TAIL_55HZ_TONE  0x046F

void fd6818_tx_tail(bool dcs, bool phase_shift)
{
    if (dcs) {
        fd6818_write(REG_TAIL, TAIL_DCS_134);
    } else if (phase_shift) {
        fd6818_write(REG_TAIL, TAIL_SHIFT_240);
    } else {
        fd6818_write(REG_TONE, TAIL_55HZ_TONE);
    }
}

void fd6818_tx_tail_end(void)
{
    fd6818_write(REG_TAIL, TAIL_NONE);
}

/* FFSK (MDC1200): the FSK modem as BK4819_send_MDC1200() of the UV-K5 firmware (OneOfEleven) */

#define REG_DEVIATION  0x40 // 11:0 the FM deviation
#define REG_FILTERS    0x2B
#define REG_FSK_MODE   0x58
#define REG_FSK        0x59
#define REG_FSK_SYNC01 0x5A
#define REG_FSK_SYNC23 0x5B
#define REG_FSK_CRC    0x5C
#define REG_FSK_LENGTH 0x5D
#define REG_FSK_FIFO   0x5F
#define REG_IRQ_ENABLE 0x3F
#define REG_IRQ        0x02
#define FSK_MODE_FFSK_1200_1800 0x3C03 // Tx FFSK 1200/1800, Rx FFSK 1200/1800, its Rx bandwidth, on
#define FSK_TX_FILTERS  0x0005 // the Tx 300 Hz high pass and the pre-emphasis off
#define FSK_CLEAR_FIFOS 0xC000
#define FSK_TX_ON       0x0800
#define FSK_PREAMBLE_4  (3 << 4) // 4 bytes of the chip's own preamble
#define FSK_SYNC_4      (1 << 3) // 4 sync bytes (0 here: MDC1200's sync is in the data)
#define FSK_CRC_OFF     0x5625
#define FSK_TONE2_GAIN  96
#define IRQ_FSK_TX_DONE 0x8000
#define IRQ_PENDING     0x0001 // REG_FLAGS bit 0
#define FSK_DEVIATION_NARROW 850
#define FSK_DEVIATION_WIDE   1050
#define FSK_BAUD        1200
#define FSK_OVERHEAD    8 // bytes the chip sends before the data: its preamble and sync

void fd6818_tx_fsk(const uint8_t *data, uint32_t length, bool wide)
{
    uint16_t css = fd6818_read(REG_CSS_MODE);
    uint16_t deviation = fd6818_read(REG_DEVIATION);
    uint16_t filters = fd6818_read(REG_FILTERS);
    uint16_t fsk = FSK_PREAMBLE_4 | FSK_SYNC_4;
    // The packet's time on the air; the chip says when it's done, this is twice that
    TickType_t limit = xTaskGetTickCount() +
                       pdMS_TO_TICKS(((2 * 8 * 1000) * (length + FSK_OVERHEAD)) / FSK_BAUD);

    fd6818_write(REG_CONTROL, CONTROL_TONE); // the mic off
    fd6818_write(REG_CSS_MODE, 0x0000);      // no CTCSS / DCS under it
    fd6818_write(REG_DEVIATION, (uint16_t)((deviation & 0xF000) | (wide ? FSK_DEVIATION_WIDE : FSK_DEVIATION_NARROW)));
    fd6818_write(REG_FILTERS, FSK_TX_FILTERS);
    fd6818_write(REG_FSK_MODE, FSK_MODE_FFSK_1200_1800);
    fd6818_write(REG_TONE2_HZ, tone_word(1200));
    fd6818_write(REG_TONES, TONE2_ON | FSK_TONE2_GAIN);
    fd6818_write(REG_FSK_LENGTH, (uint16_t)((length - 1) << 8));
    fd6818_write(REG_FSK_SYNC01, 0x0000);
    fd6818_write(REG_FSK_SYNC23, 0x0000);
    fd6818_write(REG_FSK_CRC, FSK_CRC_OFF);
    fd6818_write(REG_FSK, FSK_CLEAR_FIFOS | fsk);
    fd6818_write(REG_FSK, fsk);
    for (uint32_t i = 0; (i + 1) < length; i += 2) { // 16 bits at a time, the first byte low
        fd6818_write(REG_FSK_FIFO, (uint16_t)(data[i] | (data[i + 1] << 8)));
    }
    fd6818_write(REG_IRQ_ENABLE, IRQ_FSK_TX_DONE);
    fd6818_write(REG_FSK, FSK_TX_ON | fsk);

    while ((int32_t)(xTaskGetTickCount() - limit) < 0) {
        vTaskDelay(pdMS_TO_TICKS(4));
        if (fd6818_read(REG_FLAGS) & IRQ_PENDING) {
            fd6818_write(REG_IRQ, 0);
            if (fd6818_read(REG_IRQ) & IRQ_FSK_TX_DONE) {
                break;
            }
        }
    }

    fd6818_write(REG_FSK, fsk);
    fd6818_write(REG_IRQ_ENABLE, 0x0000);
    fd6818_write(REG_TONES, 0x0000);
    fd6818_write(REG_FSK_MODE, 0x0000);
    fd6818_write(REG_DEVIATION, deviation);
    fd6818_write(REG_FILTERS, filters);
    fd6818_write(REG_CSS_MODE, css);
    fd6818_write(REG_CONTROL, CONTROL_TX);
}

void RAM_CODE fd6818_af_out(bool on)
{
    fd6818_write(REG_AF, on ? 0x6142 : 0x6042);
}

/* CTCSS and DCS */

static uint16_t ctcss_register(uint16_t tone)
{
    return (uint16_t)((((uint32_t)tone * CTCSS_SCALE) + 5000) / 10000); // tone in 0.1 Hz: Hz x 20.649
}

// The octal digits held as hex (0x023) to the code (023 octal)
static uint16_t dcs_code(uint16_t tone)
{
    uint16_t digits = tone & TONES_DCS_CODE;

    return (uint16_t)((((digits >> 8) & 0x7) << 6) | (((digits >> 4) & 0x7) << 3) | (digits & 0x7));
}

// The 23 bit word to send: the Golay parity, the 0x800 marker and the code (dcsGetBitPatternFromCode())
static uint32_t dcs_word(uint16_t code)
{
    int low = 0;
    int high = DCS_CODES - 1;

    while (low <= high) {
        int middle = (low + high) / 2;
        const uint8_t *e = &dcs_packed[middle * 3];
        uint32_t entry = e[0] | (e[1] << 8) | ((uint32_t)e[2] << 16);
        uint16_t found = (entry >> 15) & 0x1FF;

        if (found < code) {
            low = middle + 1;
        } else if (found > code) {
            high = middle - 1;
        } else {
            return ((entry & 0x7FF) << 12) | 0x800 | code;
        }
    }
    return 0;
}

// The CTCSS generator at the DCS bit rate, the word in 0x08, then the mode (fd6818SetDCS())
static void set_dcs(uint16_t tone, bool transmit)
{
    uint32_t word = dcs_word(dcs_code(tone));
    bool inverted = (tone & TONES_DCS_INVERTED) != 0;

    fd6818_write(REG_TONE, DCS_TONE | (transmit ? 0 : 0x4000));
    fd6818_write(REG_DCS, (uint16_t)(word & 0x0FFF));
    fd6818_write(REG_DCS, (uint16_t)(0x8000 | (((word >> 20) & 0x07) << 8) | ((word >> 12) & 0xFF)));
    if (transmit) {
        fd6818_write(REG_CSS_MODE, (uint16_t)((inverted ? 0xA000 : 0x8000) | DCS_TX_GAIN));
    } else {
        fd6818_write(REG_CSS_MODE, inverted ? 0x3000 : 0x1000);
    }
}

void fd6818_set_rx_tone(uint16_t tone)
{
    if (tone == CODEPLUG_CSS_NONE) {
        fd6818_write(REG_CSS_MODE, 0x0000);
    } else if (tone & TONES_DCS) {
        set_dcs(tone, false);
    } else {
        fd6818_write(REG_CSS_MODE, 0x1000); // CTCSS, gain 0
        fd6818_write(REG_TONE, 0x0000);
        fd6818_write(REG_TONE, ctcss_register(tone));
        fd6818_write(0x52, 0x028F); // the detection thresholds
    }
}

void fd6818_set_tx_tone(uint16_t tone)
{
    if (tone == CODEPLUG_CSS_NONE) { // as the stock firmware (0x300E0D6)
        fd6818_write(REG_CSS_MODE, 0x0000);
        fd6818_write(REG_TONE, 0x0470);
        fd6818_write(REG_TONE, 0x21CE);
    } else if (tone & TONES_DCS) {
        set_dcs(tone, true);
    } else {
        fd6818_write(REG_CSS_MODE, 0x9045); // CTCSS, its gain
        fd6818_write(REG_TONE, ctcss_register(tone));
    }
}

// 0x0C: bit 10 CTCSS, bit 14 DCS, bit 15 inverted DCS (the stock firmware, 0x300EE06 / 0x300EE10)
bool RAM_CODE fd6818_rx_tone_detected(uint16_t tone)
{
    uint16_t flags;

    if (tone == CODEPLUG_CSS_NONE) {
        return false;
    }
    flags = fd6818_read(REG_FLAGS);
    if (!(tone & TONES_DCS)) {
        return (flags & 0x0400) != 0;
    }
    return (flags & ((tone & TONES_DCS_INVERTED) ? 0x8000 : 0x4000)) != 0;
}
