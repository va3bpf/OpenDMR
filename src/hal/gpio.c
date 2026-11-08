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
 * OpenDMR, hardware abstraction: GPIO and the pin setup at start.
 * After OpenDM32 (user/main.c, user/DRV/C7000_DRV.c, ucos3/bsp).
 */

#include "hw.h"
#include "hal.h"

void RAM_CODE gpio_output(uint32_t pin, bool level)
{
    GPIO_INTEN(pin) &= ~GPIO_BIT(pin);
    GPIO_DDR(pin) |= GPIO_BIT(pin);
    if (level) {
        GPIO_DR(pin) |= GPIO_BIT(pin);
    } else {
        GPIO_DR(pin) &= ~GPIO_BIT(pin);
    }
}

void RAM_CODE gpio_input(uint32_t pin)
{
    GPIO_INTEN(pin) &= ~GPIO_BIT(pin);
    GPIO_DDR(pin) &= ~GPIO_BIT(pin);
}

void RAM_CODE gpio_set(uint32_t pin)
{
    GPIO_DR(pin) |= GPIO_BIT(pin);
}

void RAM_CODE gpio_reset(uint32_t pin)
{
    GPIO_DR(pin) &= ~GPIO_BIT(pin);
}

// Pin mux and directions as GPIO_Init(); the power switch is held on, the RF switches off
void pins_init(void)
{
    IO_DIPLEX0 = 0xFFFDFD80;
    IO_DIPLEX1 = 0xFFFFFE07;
    IO_DIPLEX2 = 0x0FC80100;

    GPIOA_DDR = 0x070604E0;
    GPIOB_DDR = 0x00A0D01F;
    GPIOC_DDR = 0x00007FC4;

    GPIOA_DR |= 0x0060;
    GPIOC_DR |= 0x7044;
    GPIOB_DR = 0x0000C005;

    IOMGR_REN_REG0 = 0xFFFBFFEF;
    IOMGR_REN_REG1 = 0x00FFFFFF;
    IOMGR_REN_REG2 = 0x0FFFFF70;
    // As the end of GPIO_Init(): pad 24 without its pull resistor, pads of REG2 bits 26 and 27 with theirs
    IO_DIPLEX1 |= 0x38000000;
    IOMGR_REN_REG0 &= ~BITSET(24);
    IOMGR_REN_REG2 |= 0x0C000000;

    gpio_output(PIN_MIC_MUT, true);
    for (uint32_t pin = PIN_KEYPAD_ROW0; pin <= PIN_KEYPAD_ROW4; pin++) {
        gpio_output(pin, true);
    }
    gpio_input(PIN_PWR_SWCHECK);
    gpio_output(PIN_PWR_SW, true);
    gpio_output(0x2C, true); // GPS off (low = on): gps_set_enabled()
    gpio_output(PIN_SPI0_CS, true);
    gpio_output(0x22, false); // R5_V_SW, LNA VHF
    gpio_output(0x24, false); // R5_U_SW, LNA UHF
    gpio_output(0x37, false); // PA_SEL_VHF
    gpio_output(0x23, false); // PA_SEL_UHF
}
