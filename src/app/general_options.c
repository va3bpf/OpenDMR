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
 * OpenDMR, menu: the general options (menuGeneralOptions.c, the DM-32 / MD-UV380 items in its order). Key long
 * and key repeat are used at once by the input service, the battery calibration by the battery service; the
 * others are stored for the services that will use them (auto lock, eco level, auto power off, GPS...).
 */

#include "number_entry.h"
#include <stdio.h>
#include "util.h"
#include "settings.h"
#include "codeplug.h"
#include "text.h"
#include "options.h"
#include "list_screen.h"
#include "gps.h"

#define KEY_TIMER_MIN      1  // 100 ms units; never 0 (the main firmware: else the watchdog may kick in)
#define KEY_TIMER_MAX      90 // 9 s
#define AUTOLOCK_MAX       30 // 30 s units: 15 min
#define BATTERY_CAL_MASK   0x0F
#define BATTERY_CAL_ZERO   5 // lower 4 bits: 0.1 V units from -0.5 V (0) to +0.5 V (10)
#define BATTERY_CAL_MAX    10
#define ECO_LEVEL_MAX      5
#define APO_MINUTES_A_UNIT 30
#define APO_SKIPPED        5 // 150 min isn't offered: 0, 30, 60, 90, 120, 180 min
#define APO_MAX            6
#define HOTSPOT_TYPES      3 // off, MMDVM, BlueDV
#define GPS_MODES          4 // settings.gps: gps.h GPS_MODE_x

/* Key long, key repeat: 100 ms units, shown as seconds */

static void tenths_format(uint8_t value, char *buffer, size_t size)
{
    snprintf(buffer, size, "%u.%us", (unsigned)(value / 10), (unsigned)(value % 10));
}

static void tenths_change(uint8_t *value, int direction)
{
    *value = (uint8_t)CLAMP(*value + direction, KEY_TIMER_MIN, KEY_TIMER_MAX);
}

static void key_long_format(char *buffer, size_t size)
{
    tenths_format(settings.keypad_timer_long, buffer, size);
}

static void key_long_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    tenths_change(&settings.keypad_timer_long, direction);
}

static const ui_custom_t key_long = {
    .format = key_long_format,
    .change = key_long_change,
};

static void key_repeat_format(char *buffer, size_t size)
{
    tenths_format(settings.keypad_timer_repeat, buffer, size);
}

static void key_repeat_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    tenths_change(&settings.keypad_timer_repeat, direction);
}

static const ui_custom_t key_repeat = {
    .format = key_repeat_format,
    .change = key_repeat_change,
};

/* Auto lock: 30 s units, shown in minutes */

static void autolock_format(char *buffer, size_t size)
{
    unsigned value = settings.autolock_timer;

    if (value == 0) {
        snprintf(buffer, size, "%s", text(TEXT_OFF));
    } else if (value & 1) {
        snprintf(buffer, size, "%u.5min", value / 2);
    } else {
        snprintf(buffer, size, "%umin", value / 2);
    }
}

static void autolock_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.autolock_timer = (uint8_t)CLAMP(settings.autolock_timer + direction, 0, AUTOLOCK_MAX);
}

static const ui_custom_t autolock = {
    .format = autolock_format,
    .change = autolock_change,
};

/* Hotspot: off, MMDVM, BlueDV (only through the cable, later) */

static void hotspot_format(char *buffer, size_t size)
{
    static const char *const types[HOTSPOT_TYPES] = {NULL, "MMDVM", "BlueDV"};
    uint8_t type = settings.hotspot_type;

    snprintf(buffer, size, "%s", ((type == 0) || (type >= HOTSPOT_TYPES)) ? text(TEXT_OFF) : types[type]);
}

static void hotspot_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.hotspot_type = (uint8_t)CLAMP(settings.hotspot_type + direction, 0, HOTSPOT_TYPES - 1);
}

static const ui_custom_t hotspot = {
    .format = hotspot_format,
    .change = hotspot_change,
};

/* Battery calibration: -0.5 V .. +0.5 V, the upper 4 bits kept */

static void battery_cal_format(char *buffer, size_t size)
{
    int cal = (settings.battery_calibration & BATTERY_CAL_MASK) - BATTERY_CAL_ZERO;

    snprintf(buffer, size, "%c0.%dV", ((cal == 0) ? ' ' : ((cal > 0) ? '+' : '-')), ((cal < 0) ? -cal : cal));
}

static void battery_cal_change(int direction, uint8_t modifiers)
{
    int value = CLAMP((settings.battery_calibration & BATTERY_CAL_MASK) + direction, 0, BATTERY_CAL_MAX);

    (void)modifiers;
    settings.battery_calibration = (uint8_t)((settings.battery_calibration & ~BATTERY_CAL_MASK) | value);
}

static const ui_custom_t battery_cal = {
    .format = battery_cal_format,
    .change = battery_cal_change,
};

static const ui_number_t eco_level = {
    .value = &settings.eco_level,
    .min = 0,
    .max = ECO_LEVEL_MAX,
    .step = 1,
};

static const ui_bit_t safe_power_on = {
    .word = &settings.bitfield_options,
    .mask = SETTINGS_BIT_SAFE_POWER_ON,
};

/* Auto power off: 30 min units, 150 min skipped */

static void apo_format(char *buffer, size_t size)
{
    if (settings.apo == 0) {
        snprintf(buffer, size, "%s", text(TEXT_NO));
    } else {
        snprintf(buffer, size, "%u", (unsigned)(settings.apo * APO_MINUTES_A_UNIT));
    }
}

static void apo_change(int direction, uint8_t modifiers)
{
    int value = settings.apo + direction;

    (void)modifiers;
    if (value == APO_SKIPPED) {
        value += direction;
    }
    settings.apo = (uint8_t)CLAMP(value, 0, APO_MAX);
}

static const ui_custom_t apo = {
    .format = apo_format,
    .change = apo_change,
};

/* Auto power off with RF: only when the auto power off is on */

static void apo_with_rf_format(char *buffer, size_t size)
{
    if (settings.apo == 0) {
        snprintf(buffer, size, "%s", text(TEXT_N_A));
    } else {
        snprintf(buffer, size, "%s", text((settings.bitfield_options & SETTINGS_BIT_APO_WITH_RF) ? TEXT_YES : TEXT_NO));
    }
}

static void apo_with_rf_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    if (settings.apo != 0) {
        settings.bitfield_options = (direction > 0) ? (settings.bitfield_options | SETTINGS_BIT_APO_WITH_RF)
                                                    : (settings.bitfield_options & ~SETTINGS_BIT_APO_WITH_RF);
    }
}

static void apo_with_rf_select(void)
{
    if (settings.apo != 0) {
        settings.bitfield_options ^= SETTINGS_BIT_APO_WITH_RF;
    }
}

static const ui_custom_t apo_with_rf = {
    .format = apo_with_rf_format,
    .change = apo_with_rf_change,
    .select = apo_with_rf_select,
};

/* Satellite: manual / auto */

static void satellite_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s",
             text((settings.bitfield_options & SETTINGS_BIT_SATELLITE_AUTO) ? TEXT_AUTO : TEXT_MANUAL));
}

static void satellite_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.bitfield_options = (direction > 0) ? (settings.bitfield_options | SETTINGS_BIT_SATELLITE_AUTO)
                                                : (settings.bitfield_options & ~SETTINGS_BIT_SATELLITE_AUTO);
}

static void satellite_select(void)
{
    settings.bitfield_options ^= SETTINGS_BIT_SATELLITE_AUTO;
}

static const ui_custom_t satellite = {
    .format = satellite_format,
    .change = satellite_change,
    .select = satellite_select,
};

/* GPS: none (not detected, can't be changed), off, on, NMEA (sentences out of the cable) */

static void gps_format(char *buffer, size_t size)
{
    switch (settings.gps) {
        case GPS_MODE_OFF:
            snprintf(buffer, size, "%s", text(TEXT_OFF));
            break;
        case GPS_MODE_ON:
            snprintf(buffer, size, "%s", text(TEXT_ON));
            break;
        case GPS_MODE_ON_NMEA:
            snprintf(buffer, size, "NMEA");
            break;
        default:
            snprintf(buffer, size, "%s", text(TEXT_NONE));
            break;
    }
}

// The DM-32 always has the module: "not detected" (the default settings) counts as off
static void gps_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    if (settings.gps >= GPS_MODES) {
        return;
    }
    settings.gps = (uint8_t)CLAMP(MAX(settings.gps, GPS_MODE_OFF) + direction, GPS_MODE_OFF, GPS_MODES - 1);
    gps_set_enabled(settings.gps >= GPS_MODE_ON);
}

static const ui_custom_t gps = {
    .format = gps_format,
    .change = gps_change,
};

/* User DMR ID: the one of the codeplug; the left key opens the number entry */

static void own_id_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%lu", (unsigned long)codeplug_get_dmr_id());
}

// menuNumericalEntryOpenOwnId()
static void own_id_select(void)
{
    ui_push(&number_entry_screen, NUMBER_ENTRY_OWN_ID);
}

static const ui_custom_t own_id = {
    .format = own_id_format,
    .select = own_id_select,
};

/* The menus open where they were left also after a restart (ui/list.c; while on, always) */

static void menu_places_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text(ui_list_places_kept() ? TEXT_ON : TEXT_OFF));
}

static void menu_places_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    ui_list_places_keep(direction > 0);
}

static const ui_custom_t menu_places = {
    .format = menu_places_format,
    .change = menu_places_change,
};

static const ui_item_t general_options_items[] = {
    {TEXT_KEY_LONG, ICONS_COUNT, UI_ITEM_CUSTOM, &key_long, NULL},
    {TEXT_KEY_REPEAT, ICONS_COUNT, UI_ITEM_CUSTOM, &key_repeat, NULL},
    {TEXT_AUTO_LOCK, ICONS_COUNT, UI_ITEM_CUSTOM, &autolock, NULL},
    {TEXT_HOTSPOT_MODE, ICONS_COUNT, UI_ITEM_CUSTOM, &hotspot, NULL},
    {TEXT_BATTERY_CALIBRATION, ICONS_COUNT, UI_ITEM_CUSTOM, &battery_cal, NULL},
    {TEXT_ECO_LEVEL, ICONS_COUNT, UI_ITEM_NUMBER, &eco_level, NULL},
    {TEXT_SAFE_POWER_ON, ICONS_COUNT, UI_ITEM_BIT, &safe_power_on, NULL},
    {TEXT_AUTO_POWER_OFF, ICONS_COUNT, UI_ITEM_CUSTOM, &apo, NULL},
    {TEXT_APO_WITH_RF, ICONS_COUNT, UI_ITEM_CUSTOM, &apo_with_rf, NULL},
    {TEXT_SATELLITE_SHORT, ICONS_COUNT, UI_ITEM_CUSTOM, &satellite, NULL},
    {TEXT_GPS, ICONS_COUNT, UI_ITEM_CUSTOM, &gps, NULL},
    {TEXT_MENU_PLACES, ICONS_COUNT, UI_ITEM_CUSTOM, &menu_places, NULL},
    {TEXT_USER_DMR_ID, ICONS_COUNT, UI_ITEM_CUSTOM, &own_id, NULL},
};

const ui_list_t general_options = {
    .title = TEXT_GENERAL_OPTIONS,
    .items = general_options_items,
    .count = sizeof(general_options_items) / sizeof(general_options_items[0]),
};
