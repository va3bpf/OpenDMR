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
 * OpenDMR, screen: the firmware information (menuFirmwareInfoScreen.c, docs/ui-design.md): the name and git
 * version, when it was built, the chip, the model, the credits in short.
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "menus.h"

#define STRINGIFY(x)  #x
#define XSTRINGIFY(x) STRINGIFY(x)

// "2026-11-03 21:45" from __DATE__ ("Nov  3 2026") and __TIME__ ("21:45:12"); this file is built every time
static void format_built(char *buffer, size_t size)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    const char *found = strstr(months, (char[]){date[0], date[1], date[2], '\0'});
    int month = found ? (int)((found - months) / 3) + 1 : 0;
    int day = ((date[4] == ' ') ? 0 : (date[4] - '0')) * 10 + (date[5] - '0');

    snprintf(buffer, size, "%.4s-%02d-%02d %.5s", &date[7], month, day, __TIME__);
}

static bool handle(const ui_event_t *ev)
{
    list_handle_back(ev);
    return false;
}

static void draw(void)
{
    char built[32];
    const font_t *font = &font_roboto17;
    int32_t y = LIST_VIEW_TOP + (4 * LIST_VIEW_ROW_HEIGHT) + 12;
    list_view_row_t row = {.icon = ICONS_COUNT};

    list_view_draw_title(text(TEXT_FIRMWARE_INFO), NULL);

    row.name = "OpenDMR";
#if defined(GITVERSION)
    row.value = XSTRINGIFY(GITVERSION);
#endif
    list_view_draw_row(0, &row);

    format_built(built, sizeof(built));
    row.name = text(TEXT_BUILT);
    row.value = built;
    list_view_draw_row(1, &row);

    row.name = text(TEXT_UI_CHIP);
    row.value = "HR_C7000";
    list_view_draw_row(2, &row);

    row.name = text(TEXT_UI_MODEL);
    row.value = "DM-32UV";
    list_view_draw_row(3, &row);

    draw_text(WIDGET_MARGIN, y, text(TEXT_CREDITS), font, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    y += font->line_height + 4;
    draw_text(WIDGET_MARGIN, y, "OpenGD77  OpenUV008", font, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    y += font->line_height;
    draw_text(WIDGET_MARGIN, y, "OpenDM32  FreeRTOS", font, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);

    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

const ui_screen_t firmware_info_screen = {
    .handle = handle,
    .draw = draw,
};
