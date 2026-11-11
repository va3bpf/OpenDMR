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
 * OpenDMR, screen: the factory calibration, read only (menuCalibration.c; the table as functions/calibration.c
 * dm32ReadFactory() reads it). UP / DOWN scroll, the right key goes back.
 *
 * The table and where it is: services/factory_cal.h. NOTHING IS WRITTEN. TODO: editing (power, oscillator) needs
 * the transmitter to measure against, and a copy in the window of this firmware rather than the factory sector.
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "factory_cal.h"
#include "list_screen.h"
#include "calibration.h"

#define POINTS FACTORY_CAL_POINTS

enum {
    BAND_VHF = FACTORY_CAL_VHF,
    BAND_UHF = FACTORY_CAL_UHF,
    BANDS = FACTORY_CAL_BANDS
};

#define ROWS_HEADER   3
#define ROWS_PER_BAND (1 + POINTS)
#define ROW_COUNT     (ROWS_HEADER + (BANDS * ROWS_PER_BAND))

#define cal (*factory_cal_get()) // read at start (factory_cal_init())

static void enter(void *arg)
{
    (void)arg;
    if (*ui_state() == 0) {
        *ui_state() = 1;
    }
}

static int top_row(void)
{
    return (int)*ui_state() - 1; // the state: 1 + the top row (0: just pushed, not read yet)
}

static bool handle(const ui_event_t *ev)
{
    int32_t *state = ui_state();
    int top = top_row();
    int last = cal.valid ? (ROW_COUNT - LIST_VIEW_ROWS) : 0;

    if (((ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT)) &&
        ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
        int next = top + ((ev->key == UI_KEY_DOWN) ? 1 : -1);

        if ((next >= 0) && (next <= last)) {
            *state = next + 1;
            return true;
        }
        return false;
    }
    list_handle_back(ev);
    return false;
}

static void band_point(int band, int point, char *name, size_t name_size, char *value, size_t value_size)
{
    const uint8_t *power = cal.power[band];
    uint32_t f = cal.freqs[band][point];

    snprintf(name, name_size, "%lu.%04lu", (unsigned long)(f / 100000), (unsigned long)((f % 100000) / 10));
    snprintf(value, value_size, "%3u %3u %3u   %3u", power[point], power[POINTS + point], power[(2 * POINTS) + point],
             cal.bias[band][point]);
}

// TODO: the literal labels into the text tables
static void row_text(int index, char *name, size_t name_size, char *value, size_t value_size)
{
    value[0] = '\0';
    switch (index) {
        case 0:
            snprintf(name, name_size, "%s", text(TEXT_FACTORY_TABLE));
            snprintf(value, value_size, "0x%06lX", (unsigned long)cal.sector);
            return;
        case 1:
            snprintf(name, name_size, "%s", text(TEXT_OSC_TUNE));
            snprintf(value, value_size, "TX %u  RX %u", cal.osc_tune, cal.rx_adjust);
            return;
        case 2:
            snprintf(name, name_size, "%s", text(TEXT_RX_LEVEL));
            snprintf(value, value_size, "FM %u  DMR %u", cal.rx_levels[0], cal.rx_levels[1]);
            return;
        default:
            break;
    }

    int band = (index - ROWS_HEADER) / ROWS_PER_BAND;
    int point = ((index - ROWS_HEADER) % ROWS_PER_BAND) - 1;

    if (point < 0) {
        snprintf(name, name_size, "%s", (band == BAND_VHF) ? "VHF" : "UHF");
        snprintf(value, value_size, "%s L/M/H  Bias", text(TEXT_PWR));
    } else {
        band_point(band, point, name, name_size, value, value_size);
    }
}

static void draw(void)
{
    char position[16];
    int top = top_row();

    snprintf(position, sizeof(position), "%d/%d", top + 1, ROW_COUNT);
    list_view_draw_title(text(TEXT_CALIBRATION), cal.valid ? position : NULL);

    if (!cal.valid) {
        list_view_draw_text(text(TEXT_FACTORY_TABLE), text(TEXT_N_A), THEME_COLOUR_TEXT_LABEL);
    } else {
        for (int row = 0; row < LIST_VIEW_ROWS; row++) {
            char name[24];
            char value[32];
            list_view_row_t data = {.icon = ICONS_COUNT, .name = name, .value = value};

            row_text(top + row, name, sizeof(name), value, sizeof(value));
            data.dimmed = ((top + row) >= ROWS_HEADER) && (((top + row - ROWS_HEADER) % ROWS_PER_BAND) == 0);
            list_view_draw_row(row, &data);
        }
    }
    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

const ui_screen_t calibration_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
};
