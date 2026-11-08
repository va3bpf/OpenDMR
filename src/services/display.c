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
 */

#include "backlight.h"
#include "settings.h"
#include "radio.h"
#include "display.h"

#define DEFAULT_PERCENT 100

static volatile bool lit = true;
static volatile uint32_t light_left_ms; // 0: no timeout running

static int lit_percent(void)
{
    int percent = settings.display_backlight_percentage[0];

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

    if (!settings_valid() || (mode == SETTINGS_BACKLIGHT_MANUAL)) {
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
