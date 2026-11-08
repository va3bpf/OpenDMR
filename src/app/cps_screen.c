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
 * OpenDMR, screen: the CPS link (uiCPS.c), over every other screen while the CPS reads or writes the codeplug:
 *   the status bar, the title (as the hotspot screen)
 *   "Reading" / "Writing" (the receive / transmit colour)
 *   the part of the codeplug its address is in, with its icon (channels, zones, contacts...)
 *   how far it is in that part (the end of the last request in the part's addresses; the CPS may begin within it),
 *   a bar and the percentage; none for an address in no known part
 *   the bottom bar: keep the cable in
 *
 * A codeplug read, as the CPS sends it (logged on the radio): twice open, clear, 4 lines, render, red LED, the radio
 * info read, close; then, without an open, clear, 3 lines ("CPS", "Reading", "Codeplug"), render, green LED, 125
 * reads of up to 1 KB, close. Opened by its 'C' 0 the screen came and went at each step: it opens with the first
 * codeplug data instead (not the radio info), and goes CLOSE_DELAY_MS after a close if nothing came since. The CPS's
 * lines are not shown: they say less than this screen does.
 */

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "icons.h"
#include "text.h"
#include "cps_screen.h"

#define PERCENT_STEP   2 // a redraw for each 2 % more
#define CLOSE_DELAY_MS 2000

#define TITLE_Y    66
#define DOING_Y    126
#define PART_Y     152 // the icon and the name
#define BAR_Y      192
#define BAR_W      160
#define BAR_H      6
#define PERCENT_Y  206

// cable_link.c CPS_ACCESS_x
#define AREA_FLASH  1
#define AREA_EEPROM 2

// The parts of the codeplug in the window of the data flash (codeplug.c: FLASH_PART_OFFSET 0x20000 and the ADDR_x
// from it; cable_link.c: the DMR ID database, the OpenGD77 calibration), in the order they are looked for
typedef struct {
    uint32_t from;
    uint32_t to;
    text_id_t name;
} cps_range_t;

static const cps_range_t ranges[] = {
    {0x00000, 0x03780, TEXT_CPS_SETTINGS},  // the user's call and ID, DTMF, APRS
    {0x03780, 0x05390, TEXT_CPS_CHANNELS},  // channels 1 - 128
    {0x06000, 0x08000, TEXT_CPS_SETTINGS},  // the last used channels, the quick keys, the VFOs
    {0x08000, 0x0AF00, TEXT_ZONES},
    {0x10000, 0x11000, TEXT_CALIBRATION},
    {0x30000, 0x70000, TEXT_CPS_DMR_IDS},
    {0x9B1B0, 0xA7620, TEXT_CPS_CHANNELS},  // channels 129 - 1024
    {0xA7620, 0xAD620, TEXT_CONTACTS},
    {0xAD620, 0xB0000, TEXT_CPS_RX_GROUPS},
    {0x8F000, 0x100000, TEXT_CPS_DMR_IDS},  // the rest of the database
};

typedef struct {
    bool open;
    bool writing;
    int range;   // in ranges[]; -1: an address in none
    int percent; // of that range
} cps_state_t;

static cps_state_t state; // the cable link task writes it, the UI task copies it (critical sections)
static int posted;        // the percentage at the last redraw asked
static bool shown = false;

static void post(void)
{
    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_CPS};

    ui_post(&ev);
}

void cps_screen_close(void)
{
    taskENTER_CRITICAL();
    state.open = false;
    taskEXIT_CRITICAL();
    post();
}

static int range_of(uint32_t address)
{
    for (unsigned i = 0; i < (sizeof(ranges) / sizeof(ranges[0])); i++) {
        if ((address >= ranges[i].from) && (address < ranges[i].to)) {
            return (int)i;
        }
    }
    return -1;
}

void cps_screen_transfer(int area, uint32_t address, uint32_t bytes, bool writing)
{
    int range = range_of(address);
    int percent = 0;
    bool redraw;

    if ((area != AREA_FLASH) && (area != AREA_EEPROM)) {
        return; // the radio info, the firmware: not the codeplug
    }
    if (range >= 0) {
        uint32_t done = (address + bytes) - ranges[range].from;
        uint32_t size = ranges[range].to - ranges[range].from;
        percent = (done >= size) ? 100 : (int)((done * 100) / size);
    }
    taskENTER_CRITICAL();
    redraw = !state.open || (range != state.range) || (writing != state.writing) ||
             ((percent - posted) >= PERCENT_STEP) || ((percent == 100) && (posted != 100));
    state.open = true;
    state.range = range;
    state.writing = writing;
    state.percent = percent;
    if (redraw) {
        posted = percent;
    }
    taskEXIT_CRITICAL();
    if (redraw) {
        post();
    }
}

bool cps_screen_hook(const ui_event_t *ev)
{
    if ((ev->type == UI_EVENT_DATA) && (ev->what & UI_DATA_CPS) && state.open && !shown) {
        ui_push(&cps_screen, NULL);
    }
    return false;
}

static void enter(void *arg)
{
    (void)arg;
    shown = true;
}

static void leave(void)
{
    shown = false;
}

static bool handle(const ui_event_t *ev)
{
    if ((ev->type == UI_EVENT_DATA) && (ev->what & UI_DATA_CPS)) {
        if (!state.open) {
            ui_timer_start(CLOSE_DELAY_MS); // gone only if nothing came again by then
            return false;
        }
        return true;
    }
    if ((ev->type == UI_EVENT_TIMER) && !state.open) {
        ui_pop();
    }
    return false; // keys: nothing while the CPS has the radio
}

static icons_id_t icon_of(text_id_t name)
{
    switch (name) {
        case TEXT_CPS_SETTINGS:
            return ICONS_SETTINGS;
        case TEXT_CPS_CHANNELS:
            return ICONS_CHANNEL;
        case TEXT_ZONES:
            return ICONS_ZONE;
        case TEXT_CONTACTS:
            return ICONS_CONTACT;
        case TEXT_CPS_RX_GROUPS:
        case TEXT_CPS_DMR_IDS:
            return ICONS_TALKGROUP;
        case TEXT_CALIBRATION:
            return ICONS_WRENCH;
        default:
            return ICONS_INFO;
    }
}

static void draw(void)
{
    cps_state_t s;
    char percent[8];
    const font_t *name_font = &font_roboto20;
    const font_t *small = &font_roboto17;

    taskENTER_CRITICAL();
    s = state;
    taskEXIT_CRITICAL();

    status_bar_data_t bar = {0};
    status_bar_draw(&bar);
    draw_text(FB_WIDTH / 2, TITLE_Y, text(TEXT_CPS_TITLE), &font_roboto32_bold, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT);
    theme_colour_t colour = s.writing ? THEME_COLOUR_TX : THEME_COLOUR_RX;
    draw_text(FB_WIDTH / 2, DOING_Y, text(s.writing ? TEXT_CPS_WRITING : TEXT_CPS_READING), small, FONT_ALIGN_CENTER,
              colour);

    // the icon and the name, centred together
    text_id_t part = (s.range >= 0) ? ranges[s.range].name : TEXT_CPS_CODEPLUG;
    const char *name = text(part);
    int32_t gap = 8;
    int32_t width = ICONS_SIZE + 2 + gap + font_text_width(name_font, name);
    int32_t x = (FB_WIDTH - width) / 2;
    icons_draw(x, PART_Y + ((name_font->line_height - ICONS_SIZE) / 2), icon_of(part), THEME_COLOUR_TEXT_SECOND);
    draw_text(x + ICONS_SIZE + 2 + gap, PART_Y, name, name_font, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);

    // how far in that part
    if (s.range >= 0) {
        int32_t bar_x = (FB_WIDTH - BAR_W) / 2;
        draw_fill_rect(bar_x, BAR_Y, BAR_W, BAR_H, THEME_COLOUR_LINE);
        draw_fill_rect(bar_x, BAR_Y, (BAR_W * s.percent) / 100, BAR_H, colour);
        snprintf(percent, sizeof(percent), "%d%%", s.percent);
        draw_text(FB_WIDTH / 2, PERCENT_Y, percent, small, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_LABEL);
    }

    int32_t bar_top = WIDGET_BOTTOM_BAR_TOP + 2;
    bottom_bar_draw_soft_keys(NULL, NULL);
    draw_text(FB_WIDTH / 2, bar_top + ((FB_HEIGHT - bar_top - small->line_height) / 2), text(TEXT_CPS_KEEP_CABLE),
              small, FONT_ALIGN_CENTER, THEME_COLOUR_WARNING);
}

const ui_screen_t cps_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
};
