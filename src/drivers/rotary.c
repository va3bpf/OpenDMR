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
 * OpenDMR, driver: the rotary encoder. After OpenDM32 user/SOURCE/io/rotary_switch.c
 * (rotaryEncoderUpdate(), read every millisecond by the 1 ms interrupt).
 */

#include "hw.h"
#include "hal.h"
#include "rotary.h"

#define PIN_ROTARY_A 0x27
#define PIN_ROTARY_B 0x26

static volatile int32_t count = 0;
static bool last_b = false;
static bool initialised = false;

static bool RAM_CODE pin_high(uint32_t pin)
{
    return ((GPIO_EXT(pin) & GPIO_BIT(pin)) != 0);
}

void rotary_init(void)
{
    gpio_input(PIN_ROTARY_A);
    gpio_input(PIN_ROTARY_B);
    last_b = pin_high(PIN_ROTARY_B);
    count = 0;
    initialised = true;
}

void RAM_CODE rotary_update_1ms(void)
{
    if (!initialised) {
        return;
    }

    bool a = pin_high(PIN_ROTARY_A);
    bool b = pin_high(PIN_ROTARY_B);

    if (b != last_b) {
        last_b = b;
        count += ((a == b) ? -1 : 1);
    }
}

int32_t rotary_get_count(void)
{
    return count;
}
