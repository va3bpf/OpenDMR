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
 * OpenDMR, screen: the radio information (menuRadioInfos.c, DM-32 list and pages; docs/manual radio infos).
 * The list: battery, time, date, location (Maidenhead), battery graph, up time, once a second again; the left key opens
 * the page of the item:
 *   - time, date, location: typed with the digits (the knob or LEFT deletes one), the left key sets it when complete
 *     (the clock and the real time clock, in the time zone shown; the location of the settings), the right key clears
 *     what was typed, else goes back to the list. On the location page UP / DOWN choose N / S, with SK2 E / W.
 *   - the battery graph: the average voltage every 40 s (battery.c), 6.4 V at the bottom to 8.2 V at the top, the
 *     7 V and 8 V lines dotted; LEFT / RIGHT bars or dots.
 */

#include <stdio.h>
#include <string.h>
#include "util.h"
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "battery.h"
#include "datetime.h"
#include "settings.h"
#include "systime.h"
#include "sound.h"
#include "menus.h"

#define REFRESH_MS      1000
#define LOCATION_UNSET  0x7F000000UL // settings.location_lat never set
#define LOCATION_SOUTH  0x80000000UL
#define ENTRY_Y         50
#define GRAPH_POINTS    94 // 2 pixels each
#define GRAPH_LEFT      40
#define GRAPH_TOP       96
#define GRAPH_BOTTOM    260
#define GRAPH_TICK      22 // samples, about a quarter of an hour
#define GRAPH_MIN_X10   64
#define GRAPH_MAX_X10   82
#define DIGITS_MAX      13 // the location: DDdddd DDDdddd

enum {
    ITEM_BATTERY,
    ITEM_TIME,
    ITEM_DATE,
    ITEM_LOCATION,
    ITEM_GRAPH,
    ITEM_UPTIME,
    ITEMS,
    PAGE_LIST = -1
};

static int page = PAGE_LIST;
static int selected = 0;
static char digits[DIGITS_MAX + 1];
static int digit_count = 0;
static bool south = false;
static bool west = false;
static bool graph_dots = false;

/* Values */

static int32_t offset_s(void) // of the time shown (the time zone when local)
{
    return (int32_t)(datetime_shown() - datetime_now());
}

// The settings' fixed point: sign bit 31, degrees 30:23, 1/100000 below
static int32_t location_e5(uint32_t fixed)
{
    int32_t value = (int32_t)(((fixed & 0x7FFFFFFF) >> 23) * 100000UL) + (int32_t)(fixed & 0x7FFFFF);

    return (fixed & LOCATION_SOUTH) ? -value : value;
}

// The 6 character Maidenhead locator (buildLocationAndMaidenheadStrings())
static void maidenhead(char *out)
{
    int32_t lon = location_e5(settings.location_lon) + 18000000;
    int32_t lat = location_e5(settings.location_lat) + 9000000;

    out[0] = (char)('A' + (lon / 2000000));
    out[1] = (char)('A' + (lat / 1000000));
    out[2] = (char)('0' + ((lon % 2000000) / 200000));
    out[3] = (char)('0' + ((lat % 1000000) / 100000));
    out[4] = (char)('a' + (((lon % 200000) * 24) / 200000));
    out[5] = (char)('a' + (((lat % 100000) * 24) / 100000));
    out[6] = '\0';
}

static void format_degrees(char *out, size_t size, uint32_t fixed, char positive, char negative)
{
    snprintf(out, size, "%lu.%04lu%c", (unsigned long)((fixed & 0x7FFFFFFF) >> 23),
             (unsigned long)((fixed & 0x7FFFFF) / 10), (fixed & LOCATION_SOUTH) ? negative : positive);
}

static const char *item_name(int item)
{
    static const text_id_t names[ITEMS] = {TEXT_BATTERY, TEXT_TIME,             TEXT_DATE,
                                           TEXT_LOCATION, TEXT_UI_BATTERY_GRAPH, TEXT_UPTIME};

    return text(names[item]);
}

// The value of an item in the list (and the "now" line of the pages); NULL: none
static const char *item_value(int item, char *buffer, size_t size)
{
    datetime_fields_t now;
    int volts_x10 = battery_voltage_x10();
    uint32_t minutes = systime_get_ms() / 60000UL;

    datetime_to_fields(datetime_shown(), &now);
    switch (item) {
        case ITEM_BATTERY:
            snprintf(buffer, size, "%d%%  %d.%dV", battery_percent(), volts_x10 / 10, volts_x10 % 10);
            return buffer;
        case ITEM_TIME:
            if (!datetime_is_valid()) {
                return text(TEXT_NOT_SET);
            }
            snprintf(buffer, size, "%02u:%02u:%02u%s", now.hour, now.minute, now.second,
                     (datetime_shown_is_utc() ? " UTC" : ""));
            return buffer;
        case ITEM_DATE:
            if (!datetime_is_valid()) {
                return text(TEXT_NOT_SET);
            }
            snprintf(buffer, size, "%04u-%02u-%02u", now.year, now.month, now.day);
            return buffer;
        case ITEM_LOCATION:
            if (settings.location_lat == LOCATION_UNSET) {
                return text(TEXT_NOT_SET);
            }
            maidenhead(buffer);
            return buffer;
        case ITEM_UPTIME:
            snprintf(buffer, size, "%lu:%02lu", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
            return buffer;
        default:
            return NULL;
    }
}

/* Entry */

static int digits_needed(void)
{
    return (page == ITEM_LOCATION) ? 13 : ((page == ITEM_DATE) ? 8 : 6);
}

static int value_of(int first, int count)
{
    int value = 0;

    for (int i = 0; i < count; i++) {
        value = (value * 10) + (digits[first + i] - '0');
    }
    return value;
}

// The left key with all the digits: true when it was set
static bool set_entry(void)
{
    datetime_fields_t f;
    int32_t offset = offset_s();

    if (digit_count != digits_needed()) {
        return false;
    }
    if (page == ITEM_LOCATION) {
        int lat_deg = value_of(0, 2);
        int lon_deg = value_of(6, 3);

        if ((lat_deg > 90) || (lon_deg > 180)) {
            return false;
        }
        settings.location_lat = ((uint32_t)lat_deg << 23) + ((uint32_t)value_of(2, 4) * 10) + (south ? LOCATION_SOUTH : 0);
        settings.location_lon = ((uint32_t)lon_deg << 23) + ((uint32_t)value_of(9, 4) * 10) + (west ? LOCATION_SOUTH : 0);
        settings_save_if_changed();
        return true;
    }
    datetime_to_fields(datetime_now() + offset, &f); // the other half from the current date and time
    if (page == ITEM_DATE) {
        f.year = (uint16_t)value_of(0, 4);
        f.month = (uint8_t)value_of(4, 2);
        f.day = (uint8_t)value_of(6, 2);
        if ((f.year < 2025) || (f.month < 1) || (f.month > 12) || (f.day < 1) || (f.day > 31)) {
            return false;
        }
    } else {
        f.hour = (uint8_t)value_of(0, 2);
        f.minute = (uint8_t)value_of(2, 2);
        f.second = (uint8_t)value_of(4, 2);
        if ((f.hour > 23) || (f.minute > 59) || (f.second > 59)) {
            return false;
        }
        if (!datetime_is_valid()) {
            f.year = 2026; // no date yet: one that makes the clock valid, set the date after
            f.month = 1;
            f.day = 1;
        }
    }
    return datetime_set(datetime_from_fields(&f) - offset);
}

static void open_page(int item)
{
    page = item;
    digit_count = 0;
    digits[0] = '\0';
    if ((item == ITEM_LOCATION) && (settings.location_lat != LOCATION_UNSET)) {
        south = (settings.location_lat & LOCATION_SOUTH) != 0;
        west = (settings.location_lon & LOCATION_SOUTH) != 0;
    }
}

/* Screen */

static void enter(void *arg)
{
    int32_t *started = ui_state();

    (void)arg;
    if (*started == 0) { // pushed: the list
        *started = 1;
        page = PAGE_LIST;
        selected = 0;
    }
    ui_timer_start(REFRESH_MS);
}

static bool handle_entry(const ui_event_t *ev)
{
    bool press = (ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT);
    int digit = ui_key_digit(ev->key);

    if ((ev->type == UI_EVENT_KEY_PRESS) && (digit >= 0) && (digit_count < digits_needed())) {
        digits[digit_count++] = (char)('0' + digit);
        digits[digit_count] = '\0';
        return true;
    }
    if ((press && (ev->key == UI_KEY_LEFT)) || ((ev->type == UI_EVENT_ROTARY) && (ev->steps < 0))) {
        if (digit_count > 0) {
            digits[--digit_count] = '\0';
        }
        return true;
    }
    if ((page == ITEM_LOCATION) && press && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
        if (ev->modifiers & UI_MODIFIER_SK2) {
            west = (ev->key == UI_KEY_DOWN);
        } else {
            south = (ev->key == UI_KEY_DOWN);
        }
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        if (digit_count == 0) {
            page = PAGE_LIST;
        } else if (set_entry()) {
            ui_key_beep(SOUND_ACK);
            page = PAGE_LIST;
        } else {
            ui_key_beep(SOUND_NACK); // incomplete or not a date / time / place
        }
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && !ev->long_press) {
        if (digit_count > 0) {
            digit_count = 0;
            digits[0] = '\0';
        } else {
            page = PAGE_LIST;
        }
        return true;
    }
    return false;
}

static bool handle(const ui_event_t *ev)
{
    bool press = (ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT);

    if (ev->type == UI_EVENT_TIMER) {
        ui_timer_start(REFRESH_MS);
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_LONG) && (ev->key == UI_KEY_RED)) {
        ui_home();
        return false;
    }
    if (page == PAGE_LIST) {
        if (press && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
            selected = (selected + ((ev->key == UI_KEY_DOWN) ? 1 : (ITEMS - 1))) % ITEMS;
            return true;
        }
        if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN) && (selected != ITEM_BATTERY) &&
            (selected != ITEM_UPTIME)) {
            open_page(selected);
            return true;
        }
        list_handle_back(ev);
        return false;
    }
    if (page == ITEM_GRAPH) {
        if ((press && ((ev->key == UI_KEY_LEFT) || (ev->key == UI_KEY_RIGHT))) || (ev->type == UI_EVENT_ROTARY)) {
            graph_dots = !graph_dots;
            return true;
        }
        if (((ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && !ev->long_press) ||
            ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN))) {
            page = PAGE_LIST;
            return true;
        }
        return false;
    }
    return handle_entry(ev);
}

static void draw_list(void)
{
    char values[ITEMS][24];

    list_view_draw_title(text(TEXT_RADIO_INFO), NULL);
    for (int i = 0; i < ITEMS; i++) {
        list_view_row_t row = {.icon = ICONS_COUNT, .name = item_name(i)};

        row.value = item_value(i, values[i], sizeof(values[i]));
        row.selected = (i == selected);
        list_view_draw_row(i, &row);
    }
    bottom_bar_draw_soft_keys(((selected == ITEM_BATTERY) || (selected == ITEM_UPTIME))
                                  ? NULL
                                  : text((selected == ITEM_GRAPH) ? TEXT_UI_SELECT : TEXT_UI_EDIT),
                              text(TEXT_UI_BACK));
}

// A field from a template ('0' where a digit goes): the digits typed for it, the rest of the template dim
static int32_t draw_field(int32_t y, const char *label, const char *hint, const char *template, int first, int count)
{
    char typed[24];
    int entered = CLAMP(digit_count - first, 0, count);
    int slot = 0;
    int split = 0;
    int length = (int)strlen(template);

    snprintf(typed, sizeof(typed), "%s", template);
    for (int i = 0; i < length; i++) {
        if (template[i] == '0') {
            if (slot < entered) {
                typed[i] = digits[first + slot];
                split = i + 1;
            }
            slot++;
        }
    }
    if (entered == count) {
        split = length; // complete: the hemisphere letter too
    }
    typed[split] = '\0';

    draw_text(WIDGET_MARGIN, y, label, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    draw_text(FB_WIDTH - WIDGET_MARGIN, y, hint, &font_roboto14, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_DIM);
    draw_text(WIDGET_MARGIN, y + 24, template, &font_roboto32_bold, FONT_ALIGN_LEFT, THEME_COLOUR_OFF);
    draw_text(WIDGET_MARGIN, y + 24, typed, &font_roboto32_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    return y + 70;
}

static void draw_entry(void)
{
    char hint[48];
    char value[48];
    char line[64];
    int32_t y = ENTRY_Y;

    list_view_draw_title(item_name(page), NULL);
    if (page == ITEM_LOCATION) {
        char template[16];

        snprintf(hint, sizeof(hint), "%s N/S", text(TEXT_UI_UP_DOWN));
        snprintf(template, sizeof(template), "00.0000%c", south ? 'S' : 'N');
        y = draw_field(y, text(TEXT_UI_LATITUDE), hint, template, 0, 6);
        snprintf(hint, sizeof(hint), "SK2+%s E/W", text(TEXT_UI_UP_DOWN));
        snprintf(template, sizeof(template), "000.0000%c", west ? 'W' : 'E');
        y = draw_field(y, text(TEXT_UI_LONGITUDE), hint, template, 6, 7);
    } else {
        int32_t quarters = offset_s() / (15 * 60);
        int32_t q = (quarters < 0) ? -quarters : quarters;

        if (datetime_shown_is_utc()) {
            snprintf(hint, sizeof(hint), "UTC");
        } else if (q % 4) {
            snprintf(hint, sizeof(hint), "%s UTC%c%d:%02d", text(TEXT_LOCAL), (quarters < 0) ? '-' : '+', (int)(q / 4),
                     (int)((q % 4) * 15));
        } else {
            snprintf(hint, sizeof(hint), "%s UTC%c%d", text(TEXT_LOCAL), (quarters < 0) ? '-' : '+', (int)(q / 4));
        }
        y = draw_field(y, item_name(page), hint, (page == ITEM_DATE) ? "0000-00-00" : "00:00:00", 0, digits_needed());
    }

    draw_hline(WIDGET_MARGIN, y, FB_WIDTH - (2 * WIDGET_MARGIN), THEME_COLOUR_LINE);
    snprintf(line, sizeof(line), "%s  %s", text(TEXT_UI_NOW), item_value(page, value, sizeof(value)));
    draw_text(WIDGET_MARGIN, y + 8, line, &font_roboto20, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_SECOND);
    if ((page == ITEM_LOCATION) && (settings.location_lat != LOCATION_UNSET)) {
        char lat[16];
        char lon[16];

        format_degrees(lat, sizeof(lat), settings.location_lat, 'N', 'S');
        format_degrees(lon, sizeof(lon), settings.location_lon, 'E', 'W');
        snprintf(line, sizeof(line), "%s  %s", lat, lon);
        draw_text(WIDGET_MARGIN, y + 34, line, &font_roboto17, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT_LABEL);
    }
    bottom_bar_draw_soft_keys("OK", text((digit_count != 0) ? TEXT_UI_CANCEL : TEXT_UI_BACK));
}

static int32_t graph_y(int volts_x10)
{
    int v = CLAMP(volts_x10, GRAPH_MIN_X10, GRAPH_MAX_X10) - GRAPH_MIN_X10;

    return GRAPH_BOTTOM - ((v * (GRAPH_BOTTOM - GRAPH_TOP)) / (GRAPH_MAX_X10 - GRAPH_MIN_X10));
}

static void draw_graph(void)
{
    static const int references[2] = {70, 80};
    static const char *const labels[2] = {"7V", "8V"};
    const int32_t right = GRAPH_LEFT + (2 * GRAPH_POINTS);
    uint8_t samples[BATTERY_HISTORY_MAX];
    int count = battery_history(samples, BATTERY_HISTORY_MAX);
    int first = (count > GRAPH_POINTS) ? (count - GRAPH_POINTS) : 0;
    int volts_x10 = battery_voltage_x10();
    char buffer[32];

    list_view_draw_title(text(TEXT_UI_BATTERY_GRAPH), NULL);
    snprintf(buffer, sizeof(buffer), "%d.%dV  %d%%", volts_x10 / 10, volts_x10 % 10, battery_percent());
    draw_text(WIDGET_MARGIN, ENTRY_Y, buffer, &font_roboto22, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);

    draw_fill_rect(GRAPH_LEFT - 3, GRAPH_TOP, 1, (GRAPH_BOTTOM - GRAPH_TOP) + 3, THEME_COLOUR_TEXT_LABEL);
    draw_fill_rect(GRAPH_LEFT - 3, GRAPH_BOTTOM + 2, (right - GRAPH_LEFT) + 3, 1, THEME_COLOUR_TEXT_LABEL);
    for (int i = 0; i <= GRAPH_POINTS; i += GRAPH_TICK) {
        draw_fill_rect(GRAPH_LEFT + (2 * i), GRAPH_BOTTOM + 3, 1, 4, THEME_COLOUR_TEXT_LABEL);
    }
    for (int i = first; i < count; i++) {
        int32_t x = GRAPH_LEFT + (2 * (i - first));
        int32_t y = graph_y(samples[i]);

        if (graph_dots) {
            draw_fill_rect(x, y - 1, 2, 3, THEME_COLOUR_TEXT);
        } else {
            draw_fill_rect(x, y, 2, (GRAPH_BOTTOM - y) + 1, THEME_COLOUR_ACCENT);
        }
    }
    for (int r = 0; r < 2; r++) {
        int32_t y = graph_y(references[r]);

        draw_text(GRAPH_LEFT - 8, y - 7, labels[r], &font_roboto12, FONT_ALIGN_RIGHT, THEME_COLOUR_TEXT_LABEL);
        for (int32_t x = GRAPH_LEFT; x < right; x += 6) {
            draw_fill_rect(x, y, 3, 1, THEME_COLOUR_TEXT_SECOND);
        }
    }
    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

static void draw(void)
{
    if (page == PAGE_LIST) {
        draw_list();
    } else if (page == ITEM_GRAPH) {
        draw_graph();
    } else {
        draw_entry();
    }
}

const ui_screen_t radio_info_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
};
