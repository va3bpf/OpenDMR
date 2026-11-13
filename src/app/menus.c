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
 * OpenDMR: the menus as tables. Items and icons as the main firmware's menus (menuSystem.c mainMenuItems[],
 * menuSetEntryIconForMenu(), menuOptions.c, menuDisplayOptions.c); the screens not written yet open the "not yet" page.
 */

#include <stdio.h>
#include "util.h"
#include "settings.h"
#include "language.h"
#include "display.h"
#include "text.h"
#include "options.h"
#include "contacts.h"
#include "channel_details.h"
#include "rssi_screen.h"
#include "fm_radio.h"
#include "gps_screen.h"
#include "satellite_screen.h"
#include "calibration.h"
#include "lock_screen.h"
#include "last_heard_screen.h"
#include "key_settings.h"
#include "menus.h"

#define TIMEZONE_UTC      64   // timezone: lower 7 bits, quarters of an hour from UTC - 64
#define TIMEZONE_LOCAL    0x80 // timezone: show the local time
#define TIMEZONE_MIN      (TIMEZONE_UTC - (12 * 4))
#define TIMEZONE_MAX      (TIMEZONE_UTC + (14 * 4))
#define BACKLIGHT_STEP    10
#define BACKLIGHT_MAX_OFF 100

/* Display options */

// The "off" brightness stays under the day and night ones (menuDisplayOptions.c checkMinBacklightValue())
static void brightness_changed(void)
{
    int8_t *off = &settings.display_backlight_percentage_off;
    int8_t lowest = MIN(settings.display_backlight_percentage[0], settings.display_backlight_percentage[1]);

    if (*off >= lowest) {
        *off = (int8_t)((lowest > 0) ? (lowest - 1) : 0);
    }
    display_apply_brightness();
}

static const ui_number_t brightness_day = {
    .value = &settings.display_backlight_percentage[0],
    .is_signed = true,
    .min = 1,
    .max = 100,
    .step = BACKLIGHT_STEP,
    .small_below = BACKLIGHT_STEP,
    .unit = "%",
    .changed = brightness_changed,
};

static const ui_number_t brightness_night = {
    .value = &settings.display_backlight_percentage[1],
    .is_signed = true,
    .min = 1,
    .max = 100,
    .step = BACKLIGHT_STEP,
    .small_below = BACKLIGHT_STEP,
    .unit = "%",
    .changed = brightness_changed,
};

static const ui_number_t brightness_off = {
    .value = &settings.display_backlight_percentage_off,
    .is_signed = true,
    .min = 0,
    .max = BACKLIGHT_MAX_OFF,
    .step = BACKLIGHT_STEP,
    .small_below = BACKLIGHT_STEP,
    .unit = "%",
    .changed = brightness_changed,
};

static const text_id_t backlight_modes[SETTINGS_BACKLIGHT_MODES] = {TEXT_AUTO, TEXT_SQUELCH, TEXT_MANUAL, TEXT_BUTTONS};

static const ui_choice_t backlight_mode = {
    .value = &settings.backlight_mode,
    .options = backlight_modes,
    .count = SETTINGS_BACKLIGHT_MODES,
};

static const ui_number_t backlight_timeout = {
    .value = &settings.backlight_timeout,
    .min = 0,
    .max = 30,
    .step = 5,
    .unit = "s",
    .zero_is_off = true,
};

static const ui_bit_t auto_night = {
    .word = &settings.bitfield_options,
    .mask = SETTINGS_BIT_AUTO_NIGHT,
};

static const ui_bit_t leds = {
    .word = &settings.bitfield_options,
    .mask = SETTINGS_BIT_ALL_LEDS_DISABLED,
    .inverted = true,
};

// "+hh:mm" from UTC (menuDisplayOptions.c)
static void timezone_format(char *buffer, size_t size)
{
    int quarters = (settings.timezone & ~TIMEZONE_LOCAL) - TIMEZONE_UTC;
    int minutes = ((quarters < 0) ? -quarters : quarters) * 15;

    snprintf(buffer, size, "%c%u:%02u", ((quarters < 0) ? '-' : '+'), minutes / 60, minutes % 60);
}

// An hour a step, a quarter with SK2 held
static void timezone_change(int direction, uint8_t modifiers)
{
    int value = (settings.timezone & ~TIMEZONE_LOCAL) + (direction * ((modifiers & UI_MODIFIER_SK2) ? 1 : 4));

    value = CLAMP(value, TIMEZONE_MIN, TIMEZONE_MAX);
    settings.timezone = (uint8_t)((settings.timezone & TIMEZONE_LOCAL) | value);
}

static const ui_custom_t timezone = {
    .format = timezone_format,
    .change = timezone_change,
};

static void time_shown_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text((settings.timezone & TIMEZONE_LOCAL) ? TEXT_LOCAL : TEXT_UTC));
}

static void time_shown_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.timezone = (direction > 0) ? (settings.timezone | TIMEZONE_LOCAL) : (settings.timezone & ~TIMEZONE_LOCAL);
}

static void time_shown_select(void)
{
    settings.timezone ^= TIMEZONE_LOCAL;
}

static const ui_custom_t time_shown = {
    .format = time_shown_format,
    .change = time_shown_change,
    .select = time_shown_select,
};

static const ui_item_t display_options_items[] = {
    {TEXT_BRIGHTNESS, ICONS_COUNT, UI_ITEM_NUMBER, &brightness_day, NULL},
    {TEXT_BRIGHTNESS_NIGHT, ICONS_COUNT, UI_ITEM_NUMBER, &brightness_night, NULL},
    {TEXT_BRIGHTNESS_OFF, ICONS_COUNT, UI_ITEM_NUMBER, &brightness_off, NULL},
    {TEXT_MODE, ICONS_COUNT, UI_ITEM_CHOICE, &backlight_mode, NULL},
    {TEXT_BACKLIGHT_TIMEOUT, ICONS_COUNT, UI_ITEM_NUMBER, &backlight_timeout, NULL},
    {TEXT_AUTO_NIGHT, ICONS_COUNT, UI_ITEM_BIT, &auto_night, NULL},
    {TEXT_LEDS, ICONS_COUNT, UI_ITEM_BIT, &leds, NULL},
    {TEXT_TIMEZONE, ICONS_COUNT, UI_ITEM_CUSTOM, &timezone, NULL},
    {TEXT_TIME, ICONS_COUNT, UI_ITEM_CUSTOM, &time_shown, NULL},
};

static const ui_list_t display_options = {
    .title = TEXT_DISPLAY_OPTIONS,
    .items = display_options_items,
    .count = sizeof(display_options_items) / sizeof(display_options_items[0]),
};

/* Language: English / Chinese / Spanish (language.c), each named in its own language */

static void language_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text(TEXT_LANGUAGE_NAME));
}

static void language_change(int direction, uint8_t modifiers)
{
    uint8_t language = language_get();

    (void)modifiers;
    if (direction > 0) {
        language = (language + 1) % LANGUAGE_COUNT;
    } else {
        language = (language == 0) ? (LANGUAGE_COUNT - 1) : (language - 1);
    }
    language_set(language);
}

static void language_select(void)
{
    language_set((language_get() + 1) % LANGUAGE_COUNT);
}

static const ui_custom_t language = {
    .format = language_format,
    .change = language_change,
    .select = language_select,
};

/* Options */

static const ui_item_t options_items[] = {
    {TEXT_GENERAL_OPTIONS, ICONS_SLIDERS, UI_ITEM_LIST, &general_options, NULL},
    {TEXT_RADIO_OPTIONS, ICONS_TX, UI_ITEM_LIST, &radio_options, NULL},
    {TEXT_DISPLAY_OPTIONS, ICONS_DISPLAY, UI_ITEM_LIST, &display_options, NULL},
    {TEXT_SOUND_OPTIONS, ICONS_SOUND, UI_ITEM_LIST, &sound_options, NULL},
    {TEXT_KEY_SETTINGS, ICONS_KEYPAD, UI_ITEM_SCREEN, &key_settings_screen, (void *)1},
    {TEXT_LANGUAGE, ICONS_LANGUAGE, UI_ITEM_CUSTOM, &language, NULL},
    {TEXT_CALIBRATION, ICONS_WRENCH, UI_ITEM_SCREEN, &calibration_screen, NULL},
    {TEXT_APRS_OPTIONS, ICONS_LOCATION, UI_ITEM_LIST, &aprs_options, NULL},
};

static const ui_list_t options = {
    .title = TEXT_OPTIONS,
    .items = options_items,
    .count = sizeof(options_items) / sizeof(options_items[0]),
};

/* Main menu */

// * locks the keypad, # switches the PTT lock (menuDisplayMenuList.c)
static bool main_menu_keys(const ui_event_t *ev)
{
    if ((ev->type != UI_EVENT_KEY_RELEASE) || ev->long_press) {
        return false;
    }
    if (ev->key == UI_KEY_STAR) {
        lock_screen_lock(true, lock_screen_is_ptt_locked());
        return true;
    }
    if (ev->key == UI_KEY_HASH) {
        lock_screen_lock(lock_screen_is_keypad_locked(), !lock_screen_is_ptt_locked());
        return true;
    }
    return false;
}

static const ui_item_t main_menu_items[] = {
    {TEXT_ZONE, ICONS_ZONE, UI_ITEM_SCREEN, &pick_list_screen, (void *)&zone_list},
    {TEXT_CONTACTS, ICONS_TALKGROUP, UI_ITEM_LIST, &contacts_menu, NULL},
    {TEXT_RSSI, ICONS_SIGNAL, UI_ITEM_SCREEN, &rssi_screen, NULL},
    {TEXT_FM_RADIO, ICONS_FM_RADIO, UI_ITEM_SCREEN, &fm_radio_screen, NULL},
    {TEXT_FIRMWARE_INFO, ICONS_INFO, UI_ITEM_SCREEN, &firmware_info_screen, NULL},
    {TEXT_OPTIONS, ICONS_SETTINGS, UI_ITEM_LIST, &options, NULL},
    {TEXT_LAST_HEARD, ICONS_HISTORY, UI_ITEM_SCREEN, &last_heard_screen, NULL},
    {TEXT_RADIO_INFO, ICONS_RADIO_INFO, UI_ITEM_SCREEN, &radio_info_screen, NULL},
    {TEXT_SATELLITE, ICONS_SATELLITE, UI_ITEM_SCREEN, &satellite_screen, NULL},
    {TEXT_GPS, ICONS_LOCATION, UI_ITEM_SCREEN, &gps_screen, NULL},
};

const ui_list_t main_menu = {
    .title = TEXT_MENU,
    .items = main_menu_items,
    .count = sizeof(main_menu_items) / sizeof(main_menu_items[0]),
    .keys = main_menu_keys,
};

// The quick keys store the menus with the main firmware's numbers (menuSystem.h MENU_SCREENS), as the CPS knows them
// The channel details are in the menus of the channel and VFO screens (quick_menu.c), not the main menu
static const ui_item_t channel_details_item = {TEXT_CHANNEL_DETAILS, ICONS_CHANNEL, UI_ITEM_SCREEN,
                                               &channel_details_screen, NULL};

static const struct {
    uint8_t menu;
    const ui_item_t *item;
} quick_menus[] = {
    {1, &main_menu_items[1]},  {2, &main_menu_items[0]},  {3, &main_menu_items[7]},  {4, &main_menu_items[2]},
    {5, &main_menu_items[6]},  {6, &main_menu_items[5]},  {7, &options_items[0]},    {8, &options_items[1]},
    {9, &options_items[2]},    {10, &options_items[3]},   {11, &main_menu_items[8]}, {12, &main_menu_items[9]},
    {19, &options_items[6]},   {23, &channel_details_item}, {24, &main_menu_items[4]},
    {25, &options_items[7]},   {26, &main_menu_items[3]}, // 20, the theme: not here (the design's colours)
};

const ui_item_t *menus_quick_item(int menu)
{
    for (size_t i = 0; i < (sizeof(quick_menus) / sizeof(quick_menus[0])); i++) {
        if (quick_menus[i].menu == menu) {
            return quick_menus[i].item;
        }
    }
    return NULL;
}

int menus_quick_id(const ui_item_t *item)
{
    for (size_t i = 0; i < (sizeof(quick_menus) / sizeof(quick_menus[0])); i++) {
        if (quick_menus[i].item == item) {
            return quick_menus[i].menu;
        }
    }
    return -1;
}
