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
 * OpenDMR, driver: the keypad, the side buttons and the PTT. After OpenDM32 user/SOURCE/io/
 * keyboard.c (keyboardRead(), DM32 fast path as the stock firmware 0x3012c58) and buttons.c (buttonsRead(),
 * pttDebounced()).
 *
 * The keypad rows are LCD_DB0 - DB4: switched to GPIOs for the scan (IO_DIPLEX2), with the LCD bus taken.
 * The side buttons pull the columns KEY_3 / KEY_4 down by themselves (rows idle high).
 */

#include "hw.h"
#include "hal.h"
#include "panel.h"
#include "keys.h"
#include "cable_link.h"

#define PIN_KEY_1          49 // columns
#define PIN_KEY_3          51
#define PIN_KEY_4          52
#define PIN_PTT            43
#define ROWS               5
#define COLUMNS            4
#define ROWS_AS_GPIO       0xFE00 // IO_DIPLEX2: LCD_DB0 - DB4 (and DB5 / DB6 bits of the same field) as GPIOs
#define PTT_STABLE_SAMPLES 16     // x 100 us: a UART byte is low for at most 9 bits (78 us at 115200 baud)

static void settle(void)
{
    for (volatile int x = 0; x < 100; x++) {
    }
}

static bool column_down(int column)
{
    uint32_t pin = PIN_KEY_1 + column;

    return ((GPIO_EXT(pin) & GPIO_BIT(pin)) == 0);
}

void keys_init(void)
{
    for (int column = 0; column < COLUMNS; column++) {
        gpio_input(PIN_KEY_1 + column);
    }
    gpio_input(PIN_PTT);
}

uint32_t keys_read_buttons(void)
{
    static bool ptt_down = false;
    uint32_t result = 0;

    settle();
    if (column_down(PIN_KEY_3 - PIN_KEY_1)) {
        result |= KEYS_BUTTON_SK1;
    }
    if (column_down(PIN_KEY_4 - PIN_KEY_1)) {
        result |= KEYS_BUTTON_SK2;
    }

    // The PTT line is down for real when it stays down: checked once when it goes down, then kept until it goes up
    if ((GPIO_EXT(PIN_PTT) & GPIO_BIT(PIN_PTT)) != 0) {
        ptt_down = false;
    } else if (ptt_down == false) {
        ptt_down = true;
        for (int i = 0; i < PTT_STABLE_SAMPLES; i++) {
            delay_us(100);
            if ((GPIO_EXT(PIN_PTT) & GPIO_BIT(PIN_PTT)) != 0) {
                ptt_down = false;
                break;
            }
        }
    }
    if (ptt_down && (cable_link_is_busy() == false)) {
        result |= KEYS_BUTTON_PTT;
    }

    return result;
}

uint32_t keys_read_keypad(void)
{
    uint32_t buttons = keys_read_buttons();
    uint32_t result = 0;
    bool any_down = false;

    panel_bus_take();
    uint32_t mux = IO_DIPLEX2;

    IO_DIPLEX2 = mux | ROWS_AS_GPIO;

    // All the rows down, the columns read once: no key down (most of the time), no row by row scan
    for (int row = 0; row < ROWS; row++) {
        gpio_reset(PIN_KEYPAD_ROW0 + row);
    }
    settle();
    for (int column = 0; column < COLUMNS; column++) {
        any_down |= column_down(column);
    }
    for (int row = 0; row < ROWS; row++) {
        gpio_set(PIN_KEYPAD_ROW0 + row);
    }

    if (any_down) {
        for (int row = 0; row < ROWS; row++) {
            gpio_reset(PIN_KEYPAD_ROW0 + row);
            settle();
            for (int column = 0; column < COLUMNS; column++) {
                // a side button holds its column down: no key of that column can be read
                if (((column == 2) && (buttons & KEYS_BUTTON_SK1)) || ((column == 3) && (buttons & KEYS_BUTTON_SK2))) {
                    continue;
                }
                if (((row * COLUMNS) + column < KEYS_COUNT) && column_down(column)) {
                    result |= (1UL << ((row * COLUMNS) + column));
                }
            }
            gpio_set(PIN_KEYPAD_ROW0 + row);
        }
    }

    IO_DIPLEX2 = mux;
    panel_bus_give();

    return result;
}
