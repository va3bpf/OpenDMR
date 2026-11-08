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

#ifndef DM32_KEYS_H
#define DM32_KEYS_H

#include <stdint.h>

// Keypad keys, one bit each (matrix row * 4 + column, from OpenDM32 keyboard.c KeyboardMatrix[])
enum {
    KEYS_GREEN = 0,
    KEYS_3,
    KEYS_2,
    KEYS_1,
    KEYS_LEFT,
    KEYS_6,
    KEYS_5,
    KEYS_4,
    KEYS_RIGHT,
    KEYS_9,
    KEYS_8,
    KEYS_7,
    KEYS_RED,
    KEYS_HASH,
    KEYS_0,
    KEYS_STAR,
    KEYS_FRONT_UP,
    KEYS_FRONT_DOWN,
    KEYS_COUNT
};

// Side buttons and the PTT
#define KEYS_BUTTON_SK1 0x01
#define KEYS_BUTTON_SK2 0x02
#define KEYS_BUTTON_PTT 0x04

void keys_init(void);
uint32_t keys_read_keypad(void); // bit (1 << KEYS_x) per key down; takes the LCD bus (the rows are LCD_DB0 - DB4)
uint32_t
keys_read_buttons(void); // KEYS_BUTTON_x down; the PTT counts when it stays down, and not while the cable link is busy

#endif
