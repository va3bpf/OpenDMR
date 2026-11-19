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
 * OpenDMR, service: the display backlight. After OpenDM32 menuSystem.c displayLightTrigger(),
 * applicationMain.c (the light timer, the manual toggle) and display.c displayEnableBacklight().
 *
 * Auto: a key, the knob, a signal heard or a transmission light it, backlight_timeout seconds after dims it.
 * Squelch: the same, the time does not run while a signal is heard. Buttons: only the keys and the knob light it.
 * Manual: SK1 alone toggles it. Dim is display_backlight_percentage_off; a timeout of 0 keeps it lit. Only the input
 * task calls these, but the menu (display_apply_brightness()).
 *
 * Lit, it is the day brightness, or the night one when "Auto night" is on and it is night by the clock (the hours of
 * the auto theme's dark one; no clock: day).
 */

#include "backlight.h"
#include "settings.h"
#include "radio.h"
#include "display.h"
#include "datetime.h"
#include "extra_options.h"
#include "theme.h"

#define DEFAULT_PERCENT 100
#define NIGHT_CHECK_MS  1000 // day or night looked at again while lit

#define AUTO_LIGHT_FROM 7  // local hours of the day: the light theme in auto, the day brightness
#define AUTO_LIGHT_TO   19

static volatile bool lit = true;
static volatile uint32_t light_left_ms; // 0: no timeout running
static uint32_t night_check_ms;

static int local_hour(void);

static bool is_night(void)
{
    int hour = local_hour();

    return (hour >= 0) && ((hour < AUTO_LIGHT_FROM) || (hour >= AUTO_LIGHT_TO));
}

static int lit_percent(void)
{
    bool night = ((settings.bitfield_options & SETTINGS_BIT_AUTO_NIGHT) != 0) && is_night();
    int percent = settings.display_backlight_percentage[night ? 1 : 0];

    if (!settings_valid() || (percent < 1) || (percent > 100)) {
        percent = DEFAULT_PERCENT;
    }
    return percent;
}

static void light(bool on)
{
    lit = on;
    backlight_set_percent(on ? lit_percent() : settings.display_backlight_percentage_off);
}

// displayLightTrigger(): lit, the timeout from now
static void trigger(bool from_key)
{
    uint8_t mode = settings.backlight_mode;

    if ((mode == SETTINGS_BACKLIGHT_AUTO) || (mode == SETTINGS_BACKLIGHT_SQUELCH) ||
        ((mode == SETTINGS_BACKLIGHT_BUTTONS) && from_key)) {
        light_left_ms = settings.backlight_timeout * 1000UL;
        light(true);
    }
}

void display_apply_brightness(void)
{
    if (!settings_valid() || (settings.backlight_mode == SETTINGS_BACKLIGHT_MANUAL)) {
        light(true);
        return;
    }
    trigger(true);
}

void display_light_start(void)
{
    if (settings_valid() && (settings.backlight_mode != SETTINGS_BACKLIGHT_AUTO) &&
        (settings.display_backlight_percentage_off > 0)) {
        light(false);
    } else {
        display_apply_brightness();
    }
}

void display_light_key(bool sk1_alone)
{
    if (!settings_valid()) {
        return;
    }
    if (settings.backlight_mode == SETTINGS_BACKLIGHT_MANUAL) {
        if (sk1_alone) {
            light(!lit);
        }
        return;
    }
    trigger(true);
}

void display_light_tick(uint32_t elapsed_ms)
{
    uint8_t mode = settings.backlight_mode;
    bool heard;

    if (!settings_valid()) {
        return;
    }
    night_check_ms += elapsed_ms;
    if (lit && (night_check_ms >= NIGHT_CHECK_MS)) { // day to night or back while lit
        night_check_ms = 0;
        backlight_set_percent(lit_percent());
    }
    if (mode == SETTINGS_BACKLIGHT_MANUAL) {
        return;
    }
    heard = radio_audio_heard(); // FM or DMR audio
    if (((mode == SETTINGS_BACKLIGHT_AUTO) && heard) || radio_is_transmitting()) {
        trigger(true); // lit while it lasts, the timeout runs after
        return;
    }
    if ((mode == SETTINGS_BACKLIGHT_SQUELCH) && heard) {
        if (!lit) {
            trigger(false);
        }
        return; // the time stands still while heard
    }
    if (light_left_ms == 0) {
        return;
    }
    light_left_ms = (light_left_ms > elapsed_ms) ? (light_left_ms - elapsed_ms) : 0;
    if (light_left_ms == 0) {
        light(false);
    }
}

// The choice is kept as a theme id (theme.h) or THEME_ID_AUTO. None yet (0xFF), a theme no longer in the menu (hidden
// or deleted by the CPS) and 6 (auto before the user's themes, then the place after the 6 built-in ones): auto
uint8_t display_theme_choice(void)
{
    uint8_t choice = extra_options_theme();

    return ((choice == THEME_ID_AUTO) || (theme_index(choice) < 0)) ? THEME_ID_AUTO : choice;
}

void display_theme_choose(uint8_t choice)
{
    extra_options_set_theme(choice);
    display_theme_apply();
}

void display_theme_step(int direction)
{
    uint8_t count = theme_count();
    uint8_t choice = display_theme_choice();
    int at = (choice == THEME_ID_AUTO) ? count : theme_index(choice); // auto after the themes

    at = (at + count + 1 + ((direction > 0) ? 1 : -1)) % (count + 1);
    display_theme_choose((at == count) ? THEME_ID_AUTO : theme_id((uint8_t)at));
}

// The local hour (settings.timezone: quarters from UTC, 64 = UTC, in the lower 7 bits); -1: no clock
static int local_hour(void)
{
    datetime_fields_t fields;
    uint32_t utc = datetime_now();

    if (!datetime_is_valid() || (utc == 0)) {
        return -1;
    }
    datetime_to_fields(utc + (((int32_t)(settings.timezone & 0x7F) - 64) * 900), &fields);
    return fields.hour;
}

void display_theme_apply(void)
{
    uint8_t choice = display_theme_choice();
    uint8_t wanted = choice;

    if (choice == THEME_ID_AUTO) {
        int hour = local_hour();

        wanted = theme_first(!((hour >= AUTO_LIGHT_FROM) && (hour < AUTO_LIGHT_TO)));
    }
    if (wanted != theme_current()) {
        theme_set(wanted);
    }
}
