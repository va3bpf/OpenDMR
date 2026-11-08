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
 * OpenDMR, driver: the RDA5807M FM broadcast receiver. After OpenDM32 hardware/RDA5807.c.
 *
 * I2C by software on SDIO GPIO 0x19 and SCLK GPIO 0x1A (the DM-32 pin map of the OpenDM32 forum thread): SCLK driven, SDIO
 * driven low or released (input) for a high, about 100 kHz. The random access address 0x11 reads and writes one
 * 16-bit register.
 */

#include "hw.h"
#include "hal.h"
#include "rda5807.h"

#define PIN_SDIO       0x19
#define PIN_SCLK       0x1A
#define ADDRESS_RANDOM 0x11
#define HALF_BIT_US    5

static void sda_high(void)
{
    gpio_input(PIN_SDIO);
}

static void sda_low(void)
{
    gpio_output(PIN_SDIO, false);
}

static bool sda_read(void)
{
    return (GPIO_EXT(PIN_SDIO) & GPIO_BIT(PIN_SDIO)) != 0;
}

static void scl_set(bool level)
{
    gpio_output(PIN_SCLK, level);
    delay_us(HALF_BIT_US);
}

static void i2c_start(void)
{
    sda_high();
    scl_set(true);
    sda_low();
    delay_us(HALF_BIT_US);
    scl_set(false);
}

static void i2c_stop(void)
{
    sda_low();
    scl_set(true);
    sda_high();
    delay_us(HALF_BIT_US);
}

// true: the device acknowledged
static bool i2c_write_byte(uint8_t byte)
{
    bool ack;

    for (int i = 7; i >= 0; i--) {
        if ((byte >> i) & 1) {
            sda_high();
        } else {
            sda_low();
        }
        scl_set(true);
        scl_set(false);
    }

    sda_high();
    scl_set(true);
    ack = !sda_read();
    scl_set(false);
    return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
    uint8_t byte = 0;

    sda_high();
    for (int i = 0; i < 8; i++) {
        scl_set(true);
        byte = (uint8_t)((byte << 1) | (sda_read() ? 1 : 0));
        scl_set(false);
    }

    if (ack) {
        sda_low();
    }
    scl_set(true);
    scl_set(false);
    sda_high();
    return byte;
}

bool rda5807_read(uint8_t reg, uint16_t *value)
{
    bool ok;

    i2c_start();
    ok = i2c_write_byte(ADDRESS_RANDOM << 1) && i2c_write_byte(reg);
    if (ok) {
        i2c_start(); // repeated start
        ok = i2c_write_byte((ADDRESS_RANDOM << 1) | 1);
        if (ok) {
            uint8_t high = i2c_read_byte(true);
            uint8_t low = i2c_read_byte(false);

            *value = (uint16_t)((high << 8) | low);
        }
    }
    i2c_stop();
    return ok;
}

bool rda5807_write(uint8_t reg, uint16_t value)
{
    bool ok;

    i2c_start();
    ok = i2c_write_byte(ADDRESS_RANDOM << 1) && i2c_write_byte(reg) && i2c_write_byte((uint8_t)(value >> 8)) &&
         i2c_write_byte((uint8_t)(value & 0xFF));
    i2c_stop();
    return ok;
}
