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
 * OpenDMR, graphics: the themes (theme.h). The built-in ones are themes.c's; the user's come from the codeplug, as
 * the CPS writes them in OpenDMR's custom data block "DT" 7 (0x44540007):
 *
 *   header (4 bytes): version 1, the number of entries, 2 bytes of 0
 *   entries (68 bytes each, THEME_USER_ENTRIES of room): an id (a built-in theme's: its colours replaced or it hidden;
 *   THEME_ID_USER + n: a theme of the user's), flags (USER_DARK, USER_HIDDEN, USER_COLOURS), 2 bytes of 0, its name
 *   (UTF-8, 16 bytes, NUL padded when shorter; the user's themes only), its 16 colours as R, G, B in theme_colour_t order
 *
 * Only the ids and the flags are kept in RAM (the menu goes through them); a name and the colours are read from the
 * flash when wanted. The colours of the theme in use are kept, for theme_contrast().
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "theme.h"
#include "framebuffer.h"
#include "panel.h"
#include "flash.h"
#include "custom_data.h"
#include "text.h"
#include "themes/themes.h"

// Text on these is drawn without anti-aliasing (its edges would take the greys made for the background)
#define SOLID_BACKGROUNDS ((1U << THEME_COLOUR_ACCENT) | (1U << THEME_COLOUR_RX) | (1U << THEME_COLOUR_TX) |                            (1U << THEME_COLOUR_WARNING))

#define USER_DATA_TYPE 0x44540007 // a custom data block of this firmware ("DT", 7)
#define USER_VERSION   1
#define USER_HEADER    4
#define USER_ENTRIES   14 // the 6 built-in ones and THEME_ID_USERS
#define USER_NAME      16
#define USER_DARK      0x01
#define USER_HIDDEN    0x02
#define USER_COLOURS   0x04 // a built-in theme recoloured (the user's always have their colours)

typedef struct {
    uint8_t id;
    uint8_t flags;
    uint8_t reserved[2];
    char name[USER_NAME];
    uint8_t rgb[THEME_COLOUR_COUNT * 3];
} user_entry_t; // as in the flash: 68 bytes

static uint16_t palette[16];
static uint32_t colours[THEME_COLOUR_COUNT]; // of the theme in use (RGB888)
static uint8_t current = 0;
static uint32_t user_address; // of the first entry; 0: no block
static uint8_t user_count;
static uint8_t user_ids[USER_ENTRIES];
static uint8_t user_flags[USER_ENTRIES];

static void user_load(void)
{
    uint32_t length = 0;
    uint32_t address = custom_data_find(USER_DATA_TYPE, &length);
    uint8_t header[USER_HEADER];

    user_address = 0;
    user_count = 0;
    if ((address == 0) || (length < USER_HEADER) || !flash_read(address, header, sizeof(header)) ||
        (header[0] != USER_VERSION)) {
        return;
    }
    user_address = address + USER_HEADER;
    for (uint8_t i = 0; (i < header[1]) && (i < USER_ENTRIES) &&
                        ((USER_HEADER + ((i + 1U) * sizeof(user_entry_t))) <= length); i++) {
        uint8_t entry[2];

        if (!flash_read(user_address + (i * sizeof(user_entry_t)), entry, sizeof(entry))) {
            break;
        }
        user_ids[user_count] = entry[0];
        user_flags[user_count] = entry[1];
        user_count++;
    }
}

static int user_find(uint8_t id)
{
    for (int i = 0; i < user_count; i++) {
        if (user_ids[i] == id) {
            return i;
        }
    }
    return -1;
}

static bool user_read(int i, user_entry_t *entry)
{
    return flash_read(user_address + ((uint32_t)i * sizeof(user_entry_t)), (uint8_t *)entry, sizeof(*entry));
}

static bool is_user_id(uint8_t id)
{
    return (id >= THEME_ID_USER) && (id < (THEME_ID_USER + THEME_ID_USERS));
}

// In the menu: a built-in theme not hidden, a user's theme of the block not hidden
static bool shown(uint8_t id)
{
    int u = user_find(id);

    if (id < THEMES_COUNT) {
        return (u < 0) || !(user_flags[u] & USER_HIDDEN);
    }
    return is_user_id(id) && (u >= 0) && !(user_flags[u] & USER_HIDDEN);
}

static bool is_dark(uint8_t id)
{
    int u = user_find(id);

    if ((u >= 0) && (is_user_id(id) || (user_flags[u] & USER_COLOURS))) {
        return (user_flags[u] & USER_DARK) != 0;
    }
    return (id < THEMES_COUNT) && THEMES[id]->dark;
}

// The menu: the built-in themes shown, then the user's shown, in the block's order. None at all (all hidden): the
// first built-in one
uint8_t theme_count(void)
{
    uint8_t n = 0;

    for (uint8_t id = 0; id < THEMES_COUNT; id++) {
        n += shown(id);
    }
    for (int i = 0; i < user_count; i++) {
        n += (is_user_id(user_ids[i]) && shown(user_ids[i]));
    }
    return n ? n : 1;
}

uint8_t theme_id(uint8_t index)
{
    for (uint8_t id = 0; id < THEMES_COUNT; id++) {
        if (shown(id) && (index-- == 0)) {
            return id;
        }
    }
    for (int i = 0; i < user_count; i++) {
        if (is_user_id(user_ids[i]) && shown(user_ids[i]) && (index-- == 0)) {
            return user_ids[i];
        }
    }
    return 0;
}

int theme_index(uint8_t id)
{
    for (uint8_t i = 0, n = theme_count(); i < n; i++) {
        if (theme_id(i) == id) {
            return i;
        }
    }
    return -1;
}

void theme_name(uint8_t id, char *buffer, uint32_t size)
{
    int u = user_find(id);
    user_entry_t entry;

    if (id < THEMES_COUNT) {
        snprintf(buffer, size, "%s", text((text_id_t)THEMES[id]->name));
    } else if ((u >= 0) && user_read(u, &entry) && ((uint8_t)entry.name[0] != 0xFF) && (entry.name[0] != '\0')) {
        snprintf(buffer, size, "%.*s", USER_NAME, entry.name);
    } else {
        snprintf(buffer, size, "%u", (unsigned)(id - THEME_ID_USER + 1));
    }
}

uint8_t theme_first(bool dark)
{
    for (uint8_t i = 0, n = theme_count(); i < n; i++) {
        if (is_dark(theme_id(i)) == dark) {
            return theme_id(i);
        }
    }
    return theme_id(0);
}

void theme_set(uint8_t id)
{
    int u;
    user_entry_t entry;

    if (theme_index(id) < 0) {
        id = theme_id(0);
    }
    u = user_find(id);
    if ((u >= 0) && (is_user_id(id) || (user_flags[u] & USER_COLOURS)) && user_read(u, &entry)) {
        for (int i = 0; i < THEME_COLOUR_COUNT; i++) {
            colours[i] = ((uint32_t)entry.rgb[3 * i] << 16) | ((uint32_t)entry.rgb[(3 * i) + 1] << 8) |
                         entry.rgb[(3 * i) + 2];
        }
    } else {
        memcpy(colours, THEMES[(id < THEMES_COUNT) ? id : 0]->colours, sizeof(colours));
    }
    current = id;
    for (int i = 0; i < THEME_COLOUR_COUNT; i++) {
        palette[i] = panel_rgb888_to_native(colours[i]);
    }
    fb_set_solid_backgrounds(SOLID_BACKGROUNDS);
    fb_set_palette(palette); // the buffer holds palette entries: the whole screen takes the new colours
}

uint8_t theme_current(void)
{
    return current;
}

void theme_init(void)
{
    user_load();
    theme_set(current);
}

uint16_t theme_colour(theme_colour_t colour)
{
    return palette[(colour < THEME_COLOUR_COUNT) ? colour : THEME_COLOUR_TEXT];
}


// WCAG relative luminance of an RGB888 colour, 0 - 1 (the sRGB curve as a square: close enough to compare)
static float luminance(uint32_t rgb)
{
    float r = (float)((rgb >> 16) & 0xFF) / 255.0f;
    float g = (float)((rgb >> 8) & 0xFF) / 255.0f;
    float b = (float)(rgb & 0xFF) / 255.0f;

    return (0.2126f * r * r) + (0.7152f * g * g) + (0.0722f * b * b);
}

float theme_contrast(theme_colour_t a, theme_colour_t b)
{
    float la = luminance(colours[a]);
    float lb = luminance(colours[b]);

    return (la > lb) ? ((la + 0.05f) / (lb + 0.05f)) : ((lb + 0.05f) / (la + 0.05f));
}
