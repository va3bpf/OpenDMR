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

#ifndef DM32_THEME_H
#define DM32_THEME_H

#include <stdint.h>

// Colours of the design (docs/ui-design.md, 4.1). They are also the LCD palette, with greys between them for anti-aliasing.
typedef enum {
    THEME_COLOUR_BACKGROUND = 0,
    THEME_COLOUR_LINE,        // separators, empty part of bars
    THEME_COLOUR_OFF,         // unlit (signal, digits not entered yet)
    THEME_COLOUR_TEXT_DIM,    // the other channel when dual watching
    THEME_COLOUR_TEXT_LABEL,  // zone, labels, units
    THEME_COLOUR_TEXT_SECOND, // frequency, talk group
    THEME_COLOUR_TEXT,        // channel name, important values
    THEME_COLOUR_ACCENT,      // selected / current
    THEME_COLOUR_RX,          // receiving
    THEME_COLOUR_TX,          // transmitting
    THEME_COLOUR_WARNING,     // low battery, scanning
    THEME_COLOUR_WHITE,       // text on the accent, receive and transmit colours
    THEME_COLOUR_COUNT
} theme_colour_t;

void theme_init(void);                        // sets the LCD palette; call once the LCD type is known
uint16_t theme_colour(theme_colour_t colour); // native colour format

#endif /* DM32_THEME_H */
