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
 * OpenDMR, service: power off with the power switch. After OpenDM32 user/SOURCE/interfaces/
 * batteryAndPowerManagement.c: PWR_SWCHECK reads 1 when the switch is off, PWR_SW low cuts the power. The main
 * firmware saves the settings first: so does the input task here, the interrupt only asks for it.
 */

#include <stddef.h>
#include "hw.h"
#include "hal.h"
#include "power.h"

#define POWER_OFF_DEBOUNCE_MS 50
#define POWER_OFF_FORCE_MS    2000 // after the debounce: the tasks didn't do it

static volatile bool pending = false;
static void (*savers[POWER_OFF_SAVERS])(void);

void power_off_save_register(void (*save)(void))
{
    for (int i = 0; i < POWER_OFF_SAVERS; i++) {
        if ((savers[i] == save) || (savers[i] == NULL)) {
            savers[i] = save;
            return;
        }
    }
}

void power_off_save_all(void)
{
    for (int i = 0; (i < POWER_OFF_SAVERS) && savers[i]; i++) {
        savers[i]();
    }
}

bool power_off_pending(void)
{
    return pending;
}

void RAM_CODE power_off(void)
{
    watchdog_stop(); // the radio no longer fed it; the power stays on while the knob is on (auto power off)
    gpio_output(PIN_LCD_BKLIGHT, false);
    gpio_output(PIN_PWR_SW, false);
    for (;;) {
    }
}

void RAM_CODE power_check_1ms(void)
{
    static uint32_t off_ms = 0;

    if ((GPIO_EXT(PIN_PWR_SWCHECK) & GPIO_BIT(PIN_PWR_SWCHECK)) == 0) {
        off_ms = 0;
        pending = false;
        return;
    }

    off_ms++;
    if (off_ms == POWER_OFF_DEBOUNCE_MS) {
        gpio_output(PIN_LCD_BKLIGHT, false); // looks off at once
        pending = true;
    } else if (off_ms == (POWER_OFF_DEBOUNCE_MS + POWER_OFF_FORCE_MS)) {
        gpio_output(PIN_PWR_SW, false);
    }
}
