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
 * OpenDMR, driver: the LCD and keypad backlights. After OpenDM32 user/SOURCE/interfaces/gpio.c
 * gpioSetDisplayBacklightIntensityPercentage(): 0 % and 100 % with the pin as a GPIO, between with PWM0.
 */

#include "hw.h"
#include "hal.h"
#include "backlight.h"

#define PWM0_CFG REG(0x140C0000 + 0x00)
#define PWM0_PER REG(0x140C0000 + 0x08)
#define PWM0_FP  REG(0x140C0000 + 0x0C)

static int current_percent = -1;

void backlight_set_percent(int percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent == current_percent) {
        return;
    }

    if ((percent == 100) || (percent == 0)) {
        IO_DIPLEX0 |= BITSET(17); // the pin as a GPIO
        gpio_output(PIN_LCD_BKLIGHT, (percent == 100));
        gpio_output(PIN_KEY_BKLIGHT, (percent == 100));
    } else {
        IO_DIPLEX0 &= BITCLR(17); // the pin to PWM0
        PWM0_CFG = 0x328;
        PWM0_PER = 0xC8;
        PWM0_FP = percent;
        PWM0_CFG = 0x32C;
        PWM0_CFG = 0x32D;
        gpio_output(PIN_KEY_BKLIGHT, true);
    }

    current_percent = percent;
}

int backlight_get_percent(void)
{
    return current_percent;
}
