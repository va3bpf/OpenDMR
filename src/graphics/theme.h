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

#include <stdbool.h>
#include <stdint.h>

// The colours of the design by what they are for (docs/ui-design.md, 4.1): a theme gives each its value, and these 16
// are the whole LCD palette (the screen holds palette entries: another theme recolours it at once). Anti-aliased text
// takes the nearest of them (a theme's greys between its background and its text).
typedef enum {
    THEME_COLOUR_BACKGROUND = 0,
    THEME_COLOUR_LINE,        // the main area's separators, empty part of bars
    THEME_COLOUR_OFF,         // unlit (signal, digits not entered yet)
    THEME_COLOUR_TEXT_DIM,    // the other channel when dual watching; unlit signal bars
    THEME_COLOUR_TEXT_LABEL,  // zone, labels, units
    THEME_COLOUR_TEXT_SECOND, // frequency, talk group
    THEME_COLOUR_TEXT,        // channel name, important values
    THEME_COLOUR_ACCENT,      // selected / current
    THEME_COLOUR_RX,          // receiving
    THEME_COLOUR_TX,          // transmitting
    THEME_COLOUR_WARNING,     // low battery, scanning
    THEME_COLOUR_WHITE,       // text on the receive, transmit and warning colours
    THEME_COLOUR_ON_ACCENT,   // text on the accent (the selected row, the mode chip, PTT)
    THEME_COLOUR_PANEL,       // the band under the status bar, the list titles and the bottom bar (no lines on it)
    THEME_COLOUR_PANEL_TEXT,  // text on the panel
    THEME_COLOUR_PANEL_LABEL, // labels on the panel
    THEME_COLOUR_COUNT
} theme_colour_t;

// A theme (graphics/themes/: a file each, listed in themes.c): its name (a TEXT_x, in the current language), dark or
// light (the auto choice takes the first light one by day, the first dark one at night), its colours (RGB888)
typedef struct {
    uint16_t name;
    bool dark;
    uint32_t colours[THEME_COLOUR_COUNT];
} theme_t;

// The themes of the menu: the built-in ones (themes.c), each of them recoloured or hidden by the user, then the
// user's own, as the CPS writes them in the codeplug (OpenDMR's custom data block "DT" 7, theme.c). A theme is known by
// its id, kept when the list changes: a built-in one by its place in themes.c, the user's from THEME_ID_USER
#define THEME_ID_USER  0x40
#define THEME_ID_USERS 8
#define THEME_ID_AUTO  0xFE // not a theme: display.c's auto choice

void theme_init(void);                        // the user's themes read, the LCD palette set; once the LCD type is known
uint8_t theme_count(void);                    // the themes of the menu
uint8_t theme_id(uint8_t index);              // the id of the menu's theme at that place
int theme_index(uint8_t id);                  // the place of that theme in the menu; -1: not there (hidden, none)
void theme_name(uint8_t id, char *buffer, uint32_t size); // in the language shown
uint8_t theme_first(bool dark);               // the first dark (or light) theme of the menu
void theme_set(uint8_t id);                   // its palette: the whole screen changes at once; not there: the first one
uint8_t theme_current(void);                  // its id
uint16_t theme_colour(theme_colour_t colour); // native colour format
// The WCAG contrast of two colours of the theme in use, 1 - 21 (an icon on a fill shows from about 2.5)
float theme_contrast(theme_colour_t a, theme_colour_t b);

#endif /* DM32_THEME_H */
