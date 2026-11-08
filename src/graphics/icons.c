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

#include <string.h>
#include "icons.h"
#include "framebuffer.h"
#include "hw.h"

// Each icon is drawn as text: '#' is the colour, anything else is left as it is. All rows of an icon have the same length.
typedef struct {
    uint8_t width;
    const char *rows[ICONS_SIZE];
} iconArt_t;

static const iconArt_t ICONS[ICONS_COUNT] FLASH_CONST = {
    {// ICONS_TALKGROUP: two people
     20,
     {"                    ", "                    ", "                    ", "    ###             ",
      "   #####    ###     ", "   #####   #####    ", "   #####   #####    ", "    ###     ###     ",
      "                    ", "  #######  ######   ", " ######### #######  ", " ######### ######## ",
      " ######### ######## ", " ######### ######## ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_CONTACT: one person
     20,
     {"                    ", "                    ", "                    ", "       #####        ",
      "      #######       ", "      #######       ", "      #######       ", "       #####        ",
      "                    ", "    ###########     ", "   #############    ", "   #############    ",
      "   #############    ", "   #############    ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_TONE: a note
     20,
     {"                    ", "                    ", "          ##        ", "          ####      ",
      "          ## ###    ", "          ##   ##   ", "          ##    #   ", "          ##        ",
      "          ##        ", "          ##        ", "          ##        ", "     #######        ",
      "    ########        ", "    ########        ", "     ######         ", "                    ",
      "                    ", "                    "}},
    {// ICONS_SCAN: circular arrow
     20,
     {"                    ", "                    ", "      ######  #     ", "    ##      ###     ",
      "   #       ####     ", "  #       #####     ", "  #                 ", " #                  ",
      " #                  ", " #                  ", " #              #   ", "  #             #   ",
      "  #            #    ", "   #          #     ", "    ##      ##      ", "      ######        ",
      "                    ", "                    "}},
    {// ICONS_TX: antenna with waves
     20,
     {"                    ", "                    ", "  #     ##     #    ", " #  #   ##   #  #   ",
      " # #    ##    # #   ", " # #    ##    # #   ", " #  #   ##   #  #   ", "  #     ##     #    ",
      "        ##          ", "        ##          ", "        ##          ", "        ##          ",
      "        ##          ", "        ##          ", "        ##          ", "                    ",
      "                    ", "                    "}},
    {// ICONS_ZONE: folder
     20,
     {"                    ", "                    ", "                    ", "  #####             ",
      " #     #            ", " #      #########   ", " #              #   ", " ################   ",
      " #              #   ", " #              #   ", " #              #   ", " #              #   ",
      " #              #   ", " #              #   ", " ################   ", "                    ",
      "                    ", "                    "}},
    {// ICONS_CHANNEL: list
     20,
     {"                    ", "                    ", "                    ", "  ##  ###########   ",
      "  ##  ###########   ", "                    ", "  ##  ###########   ", "  ##  ###########   ",
      "                    ", "  ##  ###########   ", "  ##  ###########   ", "                    ",
      "  ##  ###########   ", "  ##  ###########   ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_SIGNAL: signal bars
     20,
     {"                    ", "                    ", "                    ", "              ##    ",
      "              ##    ", "              ##    ", "          ##  ##    ", "          ##  ##    ",
      "          ##  ##    ", "      ##  ##  ##    ", "      ##  ##  ##    ", "      ##  ##  ##    ",
      "  ##  ##  ##  ##    ", "  ##  ##  ##  ##    ", "  ##  ##  ##  ##    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_INFO: i in a circle
     20,
     {"                    ", "                    ", "      ######        ", "    ##      ##      ",
      "   #    ##    #     ", "  #     ##     #    ", "  #            #    ", " #      ##      #   ",
      " #      ##      #   ", " #      ##      #   ", " #      ##      #   ", "  #     ##     #    ",
      "  #     ##     #    ", "   #          #     ", "    ##      ##      ", "      ######        ",
      "                    ", "                    "}},
    {// ICONS_SETTINGS: gear
     20,
     {"                    ", "                    ", "       ####         ", "   ##  #  #  ##     ",
      "   # ###  ### #     ", "   #          #     ", " ###   ####   ###   ", " #    #    #    #   ",
      " #   #      #   #   ", " #   #      #   #   ", " #    #    #    #   ", " ###   ####   ###   ",
      "   #          #     ", "   # ###  ### #     ", "   ##  #  #  ##     ", "       ####         ",
      "                    ", "                    "}},
    {// ICONS_HISTORY: clock
     20,
     {"                    ", "                    ", "      ######        ", "    ##      ##      ",
      "   #    ##    #     ", "  #     ##     #    ", "  #     ##     #    ", " #      ##      #   ",
      " #      ######  #   ", " #              #   ", " #              #   ", "  #            #    ",
      "  #            #    ", "   #          #     ", "    ##      ##      ", "      ######        ",
      "                    ", "                    "}},
    {// ICONS_RADIO_INFO: pulse
     20,
     {"                    ", "                    ", "                    ", "                    ",
      "       #            ", "      # #           ", "      # #           ", "     #   #     #    ",
      "     #   #    # #   ", " #####   #    #  ## ", "          #  #      ", "          #  #      ",
      "           ##       ", "                    ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_SATELLITE: satellite
     20,
     {"                    ", "                    ", " ###                ", " ####               ",
      " #####              ", "  #####             ", "   ###  ##          ", "       ####         ",
      "      ######        ", "       ######       ", "        ####        ", "         ##  ###    ",
      "            #####   ", "             #####  ", "              ####  ", "               ###  ",
      "                    ", "                    "}},
    {// ICONS_LOCATION: map pin
     20,
     {"                    ", "                    ", "     ######         ", "   ##      ##       ",
      "  #          #      ", "  #   ####   #      ", " #   #    #   #     ", " #   #    #   #     ",
      " #    ####    #     ", "  #          #      ", "  #          #      ", "   #        #       ",
      "    #      #        ", "     #    #         ", "      #  #          ", "       ##           ",
      "                    ", "                    "}},
    {// ICONS_KEYPAD: keypad
     20,
     {"                    ", "                    ", "                    ", "  ##   ##   ##      ",
      "  ##   ##   ##      ", "                    ", "  ##   ##   ##      ", "  ##   ##   ##      ",
      "                    ", "  ##   ##   ##      ", "  ##   ##   ##      ", "                    ",
      "       ##           ", "       ##           ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_CONTACT_ADD: person and plus
     20,
     {"                    ", "                    ", "                    ", "     #####          ",
      "    #######         ", "    #######      ## ", "    #######      ## ", "     #####     #####",
      "               #####", "  ###########    ## ", " #############   ## ", " #############      ",
      " #############      ", " #############      ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_SLIDERS: sliders
     20,
     {"                    ", "                    ", "                    ", "    ##              ",
      " ###  ###########   ", "    ##              ", "                    ", "          ##        ",
      " #########  #####   ", "          ##        ", "                    ", "      ##            ",
      " #####  #########   ", "      ##            ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_DISPLAY: screen
     20,
     {"                    ", "                    ", "                    ", "                    ",
      " ################   ", " #              #   ", " #              #   ", " #              #   ",
      " #              #   ", " #              #   ", " #              #   ", " ################   ",
      "       ####         ", "     ########       ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_SOUND: speaker
     20,
     {"                    ", "                    ", "                    ", "                    ",
      "       ##           ", "      ###      #    ", "     ####   #   #   ", " #######  #  #  #   ",
      " #######   # #  #   ", " #######   # #  #   ", " #######  #  #  #   ", "     ####   #   #   ",
      "      ###      #    ", "       ##           ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_LANGUAGE: globe
     20,
     {"                    ", "                    ", "      ######        ", "    ##  ##  ##      ",
      "   #   #  #   #     ", "  #   #    #   #    ", "  ##############    ", " #   #      #   #   ",
      " #   #      #   #   ", " #   #      #   #   ", " #   #      #   #   ", "  ##############    ",
      "  #   #    #   #    ", "   #   #  #   #     ", "    ##  ##  ##      ", "      ######        ",
      "                    ", "                    "}},
    {// ICONS_WRENCH: wrench
     20,
     {"                    ", "                    ", "            ####    ", "           #  ##    ",
      "          #   #     ", "          #  #  #   ", "         ##    ##   ", "        ##  #####   ",
      "       ##           ", "      ##            ", "     ##             ", "    ##              ",
      "   ##               ", "  ##                ", " ##                 ", "                    ",
      "                    ", "                    "}},
    {// ICONS_THEME: palette
     20,
     {"                    ", "                    ", "      ######        ", "    ##      ##      ",
      "   #  ##  ##  #     ", "  #   ##  ##   #    ", "  #            #    ", " # ##           #   ",
      " # ##       ##  #   ", " #          ##  #   ", "  #     ##     #    ", "  #    #  #   #     ",
      "   #    ##   #      ", "    ##     ##       ", "      #####         ", "                    ",
      "                    ", "                    "}},
    {// ICONS_FM_RADIO: broadcast radio
     20,
     {"                    ", "              ##    ", "            ##      ", "          ##        ",
      " ################## ", " #                # ", " #  ####    ##### # ", " # #    #   #   # # ",
      " # #    #   ##### # ", " # #    #         # ", " # #    #   ## ## # ", " #  ####          # ",
      " #                # ", " ################## ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_STEREO: two rings
     20,
     {"                    ", "                    ", "                    ", "   ####    ####     ",
      "  #    #  #    #    ", " #      ##      #   ", " #      ##      #   ", " #     #  #     #   ",
      " #     #  #     #   ", " #      ##      #   ", " #      ##      #   ", "  #    #  #    #    ",
      "   ####    ####     ", "                    ", "                    ", "                    ",
      "                    ", "                    "}},
    {// ICONS_LOCK: padlock
     20,
     {"                    ", "                    ", "       #####        ", "      ##   ##       ",
      "     ##     ##      ", "     ##     ##      ", "     ##     ##      ", "   #############    ",
      "   #############    ", "   ######  #####    ", "   #####    ####    ", "   ######  #####    ",
      "   ######  #####    ", "   #############    ", "   #############    ", "                    ",
      "                    ", "                    "}}};

int32_t icons_draw(int32_t x, int32_t y, icons_id_t icon, theme_colour_t colour)
{
    if (icon >= ICONS_COUNT) {
        return 0;
    }

    const iconArt_t *art = &ICONS[icon];
    uint16_t c = theme_colour(colour);

    for (int32_t row = 0; row < ICONS_SIZE; row++) {
        const char *p = art->rows[row];

        for (int32_t col = 0; (col < art->width) && p[col]; col++) {
            if (p[col] == '#') {
                fb_set_pixel(x + col, y + row, c);
            }
        }
    }

    return art->width;
}

void icons_draw_signal(int32_t x, int32_t y, int32_t level, theme_colour_t colour)
{
    for (int32_t bar = 0; bar < ICONS_SIGNAL_BARS; bar++) {
        int32_t h = 4 + (bar * 2);
        int32_t bx = x + (bar * 4);

        fb_fill(bx, y + 16 - h, bx + 3, y + 16, theme_colour((bar < level) ? colour : THEME_COLOUR_OFF));
    }
}

void icons_draw_battery(int32_t x, int32_t y, int32_t percent, theme_colour_t colour)
{
    uint16_t outline = theme_colour(THEME_COLOUR_TEXT);
    int32_t inner =
        ((21 * ((percent < 0) ? 0 : ((percent > 100) ? 100 : percent))) + 50) / 100; // 21 px inside the outline

    // body 27 x 14 with a 2 px outline, tip 3 x 6
    fb_fill(x, y, x + 27, y + 2, outline);
    fb_fill(x, y + 12, x + 27, y + 14, outline);
    fb_fill(x, y, x + 2, y + 14, outline);
    fb_fill(x + 25, y, x + 27, y + 14, outline);
    fb_fill(x + 27, y + 4, x + 30, y + 10, outline);

    if (inner > 0) {
        fb_fill(x + 3, y + 3, x + 3 + inner, y + 11, theme_colour(colour));
    }
}
