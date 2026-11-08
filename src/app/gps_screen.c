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
 * OpenDMR, screen: GPS (uiGPS.c, the DM-32 GPS screen; docs/manual gps.png): a sky plot of the
 * satellites (GPS in the accent colour, BeiDou in the warning colour, filled when received), their signal, the
 * position, the satellites, the height, the UTC and local times. The left key turns the GPS on / off (settings.gps,
 * as the general options), the right key goes back. Redrawn once a second (the seconds of the times).
 *
 * The data come from services/gps.c, whose driver is not written yet: no fix, no satellites.
 */

#include <stdio.h>
#include "util.h"
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "battery.h"
#include "datetime.h"
#include "settings.h"
#include "gps.h"
#include "list_screen.h"
#include "gps_screen.h"

#define SKY_CX       120
#define SKY_CY       100
#define SKY_R        58
#define BARS_Y       166
#define BARS_H       32
#define BAR_LABELS_Y 200
#define BAR_MAX_W    24
#define BAR_GAP      2
#define BAR_SNR_MAX  50 // dB-Hz at the top of a bar
#define INFO_Y       220
#define INFO_LINE    20
#define COL2_X       132
#define VALUE_DX     40
#define REFRESH_MS   1000
#define TZ_UTC       64 // settings.timezone: lower 7 bits, quarters of an hour from UTC - 64
#define TZ_MASK      0x7F

static gps_data_t gps; // about 210 bytes: not on the UI task stack

// sin(degrees) * 1024 (Bhaskara's approximation, within 0.2 %: enough for pixels)
static int32_t sin_1024(int32_t degrees)
{
    int32_t sign = 1;
    int32_t a;

    degrees %= 360;
    if (degrees < 0) {
        degrees += 360;
    }
    if (degrees >= 180) {
        degrees -= 180;
        sign = -1;
    }
    a = degrees * (180 - degrees);
    return sign * ((4096 * a) / (40500 - a));
}

void gps_screen_sky_point(int32_t cx, int32_t cy, int32_t r, int32_t azimuth, int32_t elevation, int32_t *x, int32_t *y)
{
    int32_t distance = (r * (90 - CLAMP(elevation, 0, 90))) / 90;

    *x = cx + ((distance * sin_1024(azimuth)) / 1024);
    *y = cy - ((distance * sin_1024(azimuth + 90)) / 1024);
}

void gps_screen_draw_sky(int32_t cx, int32_t cy, int32_t r, bool labels)
{
    const char *symbols = text(TEXT_SYMBOLS); // N S E W
    char c[2] = {0, 0};

    for (int i = 1; i <= 3; i++) {
        draw_ring(cx, cy, (r * i * 4) / 3, 4, THEME_COLOUR_LINE);
    }
    draw_fill_rect(cx, cy - r, 1, 2 * r, THEME_COLOUR_LINE);
    draw_hline(cx - r, cy, 2 * r, THEME_COLOUR_LINE);

    c[0] = symbols[0];
    draw_text(cx, cy - r + 2, c, &font_roboto12, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_DIM);
    if (!labels) {
        return;
    }
    c[0] = symbols[1];
    draw_text(cx, cy + r - 15, c, &font_roboto12, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_DIM);
    c[0] = symbols[2];
    draw_text(cx + r - 8, cy - 16, c, &font_roboto12, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_DIM);
    c[0] = symbols[3];
    draw_text(cx - r + 8, cy - 16, c, &font_roboto12, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_DIM);
}

static bool gps_is_on(void)
{
    return settings.gps >= GPS_MODE_ON;
}

static void enter(void *arg)
{
    (void)arg;
    ui_timer_start(REFRESH_MS);
}

static bool handle(const ui_event_t *ev)
{
    if (ev->type == UI_EVENT_TIMER) {
        ui_timer_start(REFRESH_MS);
        return true;
    }
    if (ev->type == UI_EVENT_DATA) {
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        settings.gps = (gps_is_on() ? GPS_MODE_OFF : GPS_MODE_ON);
        settings_changed();
        settings_save_if_changed();
        gps_set_enabled(gps_is_on());
        return true;
    }
    list_handle_back(ev);
    return false;
}

static void draw_satellites(const gps_satellite_t *satellites, int count, theme_colour_t colour)
{
    for (int i = 0; i < MIN(count, GPS_SATELLITES_MAX); i++) {
        const gps_satellite_t *s = &satellites[i];
        int32_t x;
        int32_t y;
        char label[6];

        if ((s->elevation < 0) || (s->elevation > 90) || (s->azimuth < 0) || (s->azimuth > 360)) {
            continue; // no position yet
        }
        gps_screen_sky_point(SKY_CX, SKY_CY, SKY_R, s->azimuth, s->elevation, &x, &y);
        if (s->snr > 0) {
            draw_fill_circle(x, y, 4 * 4, colour);
        } else {
            draw_ring(x, y, 3 * 4, 6, colour);
        }
        snprintf(label, sizeof(label), "%u", s->number);
        draw_text(x + 6, y - 14, label, &font_roboto12, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    }
}

// One bar per satellite in view, GPS then BeiDou; the SNR is the height
static void draw_bars(void)
{
    int gp = MIN(gps.gps_count, GPS_SATELLITES_MAX);
    int count = gp + MIN(gps.beidou_count, GPS_SATELLITES_MAX);
    const int32_t width = FB_WIDTH - (2 * WIDGET_MARGIN);

    if (count == 0) {
        return;
    }

    int32_t bar_w = MIN(BAR_MAX_W, (width - ((count - 1) * BAR_GAP)) / count);

    for (int i = 0; i < count; i++) {
        const gps_satellite_t *s = ((i < gp) ? &gps.gps[i] : &gps.beidou[i - gp]);
        int32_t x = WIDGET_MARGIN + (i * (bar_w + BAR_GAP));
        int32_t h = CLAMP((s->snr * BARS_H) / BAR_SNR_MAX, 0, BARS_H);

        draw_fill_rect(x, BARS_Y, bar_w, BARS_H, THEME_COLOUR_LINE);
        if (h > 0) {
            draw_fill_rect(x, BARS_Y + BARS_H - h, bar_w, h, ((i < gp) ? THEME_COLOUR_ACCENT : THEME_COLOUR_WARNING));
        }
        if (bar_w >= 14) {
            char label[6];

            snprintf(label, sizeof(label), "%u", s->number);
            draw_text(x + (bar_w / 2), BAR_LABELS_Y, label, &font_roboto12, FONT_ALIGN_CENTER, THEME_COLOUR_TEXT_DIM);
        }
    }
}

static int satellites_received(void)
{
    int n = 0;

    for (int i = 0; i < MIN(gps.gps_count, GPS_SATELLITES_MAX); i++) {
        n += (gps.gps[i].snr > 0) ? 1 : 0;
    }
    for (int i = 0; i < MIN(gps.beidou_count, GPS_SATELLITES_MAX); i++) {
        n += (gps.beidou[i].snr > 0) ? 1 : 0;
    }
    return n;
}

// Degrees with 4 decimals and the hemisphere letter
static void format_degrees(char *buffer, size_t size, int32_t value, char positive, char negative)
{
    uint32_t magnitude = (uint32_t)((value < 0) ? -value : value);

    snprintf(buffer, size, "%lu.%04lu°%c", (unsigned long)(magnitude / 10000), (unsigned long)(magnitude % 10000),
             ((value < 0) ? negative : positive));
}

// HH:MM:SS of the clock with an offset in seconds, "--" when it was never set
static void format_time(char *buffer, size_t size, int32_t offset)
{
    datetime_fields_t t;

    if (!datetime_is_valid()) {
        snprintf(buffer, size, "--");
        return;
    }
    datetime_to_fields(datetime_now() + offset, &t);
    snprintf(buffer, size, "%02u:%02u:%02u", t.hour, t.minute, t.second);
}

static void draw_info(int32_t x, int32_t y, const char *label, const char *value)
{
    draw_text(x, y, label, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    draw_text(x + VALUE_DX, y, value, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
}

static void draw(void)
{
    char buffer[32];
    status_bar_data_t bar = {0};
    bool on = gps_is_on();
    bool position;
    int received;

    gps_read(&gps);
    received = satellites_received();

    bar.state = (gps.fix ? STATUS_BAR_RECEIVING : STATUS_BAR_IDLE);
    bar.signal_bars = (int8_t)MIN(received, ICONS_SIGNAL_BARS);
    bar.item1 = "GPS";
    bar.item1_bold = true;
    bar.item2 = (!on ? text(TEXT_OFF) : (gps.fix ? (gps.fix_3d ? "3D" : "2D") : text(TEXT_UI_NO_FIX)));
    status_bar_draw(&bar);

    gps_screen_draw_sky(SKY_CX, SKY_CY, SKY_R, true);
    draw_satellites(gps.gps, gps.gps_count, THEME_COLOUR_ACCENT);
    draw_satellites(gps.beidou, gps.beidou_count, THEME_COLOUR_WARNING);
    draw_bars();

    position = gps.fix && gps.has_position;
    if (position) {
        format_degrees(buffer, sizeof(buffer), gps.latitude, 'N', 'S');
    } else {
        snprintf(buffer, sizeof(buffer), "--");
    }
    draw_info(WIDGET_MARGIN, INFO_Y, text(TEXT_UI_LATITUDE), buffer);

    snprintf(buffer, sizeof(buffer), "%d / %d", received, gps.gps_count + gps.beidou_count);
    draw_info(COL2_X, INFO_Y, text(TEXT_UI_SATS), buffer);

    if (position) {
        format_degrees(buffer, sizeof(buffer), gps.longitude, 'E', 'W');
    } else {
        snprintf(buffer, sizeof(buffer), "--");
    }
    draw_info(WIDGET_MARGIN, INFO_Y + INFO_LINE, text(TEXT_UI_LONGITUDE), buffer);

    if (gps.fix && gps.has_height) {
        snprintf(buffer, sizeof(buffer), "%d m", gps.height_m);
    } else {
        snprintf(buffer, sizeof(buffer), "--");
    }
    draw_info(COL2_X, INFO_Y + INFO_LINE, text(TEXT_ALTITUDE), buffer);

    // UTC and the local time of the timezone setting
    format_time(buffer, sizeof(buffer), 0);
    draw_info(WIDGET_MARGIN, INFO_Y + (2 * INFO_LINE), text(TEXT_UTC), buffer);
    format_time(buffer, sizeof(buffer), ((settings.timezone & TZ_MASK) - TZ_UTC) * (15 * 60));
    draw_info(COL2_X, INFO_Y + (2 * INFO_LINE), text(TEXT_LOCAL), buffer);

    snprintf(buffer, sizeof(buffer), "GPS %s", text(on ? TEXT_ON : TEXT_OFF));
    bottom_bar_draw_soft_keys(buffer, text(TEXT_UI_BACK));
}

const ui_screen_t gps_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
};
