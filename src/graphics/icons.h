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

#ifndef DM32_ICONS_H
#define DM32_ICONS_H

#include <stdint.h>
#include "theme.h"

// Icons in LCD pixels; x, y is the top left corner
typedef enum {
    ICONS_TALKGROUP = 0, // two people
    ICONS_CONTACT,       // one person
    ICONS_TONE,          // a note
    ICONS_SCAN,          // circular arrow
    ICONS_TX,            // antenna with waves
    ICONS_ZONE,          // folder
    ICONS_CHANNEL,       // list
    ICONS_SIGNAL,        // signal bars
    ICONS_INFO,          // i in a circle
    ICONS_SETTINGS,      // gear
    ICONS_HISTORY,       // clock
    ICONS_RADIO_INFO,    // pulse
    ICONS_SATELLITE,     // satellite
    ICONS_LOCATION,      // map pin
    ICONS_KEYPAD,        // keypad
    ICONS_CONTACT_ADD,   // person and plus
    ICONS_SLIDERS,       // sliders
    ICONS_DISPLAY,       // screen
    ICONS_SOUND,         // speaker
    ICONS_LANGUAGE,      // globe
    ICONS_WRENCH,        // wrench
    ICONS_THEME,         // palette
    ICONS_FM_RADIO,      // broadcast radio
    ICONS_STEREO,        // two rings
    ICONS_LOCK,          // padlock
    ICONS_COUNT
} icons_id_t;

#define ICONS_SIZE 18 // all icons are 20 x 18

int32_t icons_draw(int32_t x, int32_t y, icons_id_t icon, theme_colour_t colour); // returns the icon width

// Battery: 30 x 14, filled to percent (0 to 100), in colour (the outline in the text colour)
void icons_draw_battery(int32_t x, int32_t y, int32_t percent, theme_colour_t colour);

// Signal: 7 bars of rising height, 27 x 16; the first 'level' (0 to ICONS_SIGNAL_BARS) in colour, the others unlit
#define ICONS_SIGNAL_BARS  7
#define ICONS_SIGNAL_WIDTH 27
void icons_draw_signal(int32_t x, int32_t y, int32_t level, theme_colour_t colour);

#endif /* DM32_ICONS_H */
