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
 * OpenDMR, menu: the radio options (menuRadioOptions.c, the DM-32 / MD-UV380 items in its order). All of them
 * are for the radio (band limits, DMR filter, scan, squelch, PTT, power...): stored now, used once it receives and
 * transmits.
 */

#include <stdio.h>
#include "util.h"
#include "settings.h"
#include "text.h"
#include "options.h"

#define BAND_LIMITS_MODES     3 // off, on (the legacy bands), from the CPS
#define CAPTURE_TIMEOUT_MIN   2 // s
#define CAPTURE_TIMEOUT_MAX   90
#define SCAN_DELAY_MIN        1 // s
#define SCAN_DELAY_MAX        30
#define SCAN_STEP_MAX         15 // dwell: (1 + step) timeslots of 30 ms
#define TIMESLOT_MS           30
#define SCAN_MODES            3 // hold, pause, stop
#define SQUELCH_MIN           1 // 1 .. 21: 0 % .. 100 % in 5 % steps
#define SQUELCH_MAX           21
#define SQUELCH_PERCENT_STEP  5
#define SQUELCH_VHF           0 // settings.squelch_defaults[]
#define SQUELCH_220           1
#define SQUELCH_UHF           2
#define PRIVATE_CALL_MODES    3    // off, on, PTT (the main firmware's "auto" isn't offered)
#define USER_POWER_MAX        4100 // the PA DAC limits it to 4096 anyway (trxUpdate_PA_DAC_Drive())
#define USER_POWER_STEP       100
#define USER_POWER_SMALL_STEP 10 // SK2 held

/* Band limits: off, on, CPS */

static void band_limits_format(char *buffer, size_t size)
{
    switch (settings.tx_freq_limited) {
        case 0:
            snprintf(buffer, size, "%s", text(TEXT_OFF));
            break;
        case 1:
            snprintf(buffer, size, "%s", text(TEXT_ON));
            break;
        default:
            snprintf(buffer, size, "CPS");
            break;
    }
}

static void band_limits_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.tx_freq_limited = (uint8_t)CLAMP(settings.tx_freq_limited + direction, 0, BAND_LIMITS_MODES - 1);
}

static const ui_custom_t band_limits = {
    .format = band_limits_format,
    .change = band_limits_change,
};

static const ui_number_t capture_timeout = {
    .value = &settings.dmr_capture_timeout,
    .min = CAPTURE_TIMEOUT_MIN,
    .max = CAPTURE_TIMEOUT_MAX,
    .step = 1,
    .unit = "s",
};

static const ui_number_t scan_delay = {
    .value = &settings.scan_delay,
    .min = SCAN_DELAY_MIN,
    .max = SCAN_DELAY_MAX,
    .step = 1,
    .unit = "s",
};

/* Scan dwell: in timeslots, shown in ms */

static void scan_dwell_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%dms", TIMESLOT_MS + (settings.scan_step_time * TIMESLOT_MS));
}

static void scan_dwell_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.scan_step_time = (uint8_t)CLAMP(settings.scan_step_time + direction, 0, SCAN_STEP_MAX);
}

static const ui_custom_t scan_dwell = {
    .format = scan_dwell_format,
    .change = scan_dwell_change,
};

static const text_id_t scan_modes[SCAN_MODES] = {TEXT_HOLD, TEXT_PAUSE, TEXT_STOP};

static const ui_choice_t scan_mode = {
    .value = &settings.scan_mode_pause,
    .options = scan_modes,
    .count = SCAN_MODES,
};

static const ui_bit_t scan_on_boot = {
    .word = &settings.bitfield_options,
    .mask = SETTINGS_BIT_SCAN_ON_BOOT,
};

static const ui_bit_t boot_screen = {
    .word = &settings.bitfield_options,
    .mask = SETTINGS_BIT_BOOT_SCREEN,
};

/* Default squelch of the three bands, in percent */

static void squelch_format(int band, char *buffer, size_t size)
{
    snprintf(buffer, size, "%d%%", (settings.squelch_defaults[band] - 1) * SQUELCH_PERCENT_STEP);
}

static void squelch_change(int band, int direction)
{
    settings.squelch_defaults[band] =
        (uint8_t)CLAMP(settings.squelch_defaults[band] + direction, SQUELCH_MIN, SQUELCH_MAX);
}

static void squelch_vhf_format(char *buffer, size_t size)
{
    squelch_format(SQUELCH_VHF, buffer, size);
}

static void squelch_vhf_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    squelch_change(SQUELCH_VHF, direction);
}

static void squelch_220_format(char *buffer, size_t size)
{
    squelch_format(SQUELCH_220, buffer, size);
}

static void squelch_220_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    squelch_change(SQUELCH_220, direction);
}

static void squelch_uhf_format(char *buffer, size_t size)
{
    squelch_format(SQUELCH_UHF, buffer, size);
}

static void squelch_uhf_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    squelch_change(SQUELCH_UHF, direction);
}

static const ui_custom_t squelch_vhf = {
    .format = squelch_vhf_format,
    .change = squelch_vhf_change,
};

static const ui_custom_t squelch_220 = {
    .format = squelch_220_format,
    .change = squelch_220_change,
};

static const ui_custom_t squelch_uhf = {
    .format = squelch_uhf_format,
    .change = squelch_uhf_change,
};

static const ui_bit_t ptt_toggle = {
    .word = &settings.bitfield_options,
    .mask = SETTINGS_BIT_PTT_LATCH,
};

static const text_id_t private_call_modes[PRIVATE_CALL_MODES] = {TEXT_OFF, TEXT_ON, TEXT_PTT};

static const ui_choice_t private_calls = {
    .value = &settings.private_calls,
    .options = private_call_modes,
    .count = PRIVATE_CALL_MODES,
};

/* User power: the PA DAC value of the "user" power level, 16 bits; steps of 100, of 10 with SK2 held */

static void user_power_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%u", (unsigned)settings.user_power);
}

// The PA drive of the user power level, taken at each transmission (radio.c pa_drive())
static void user_power_change(int direction, uint8_t modifiers)
{
    int step = (modifiers & UI_MODIFIER_SK2) ? USER_POWER_SMALL_STEP : USER_POWER_STEP;

    settings.user_power = (uint16_t)CLAMP(settings.user_power + (direction * step), 0, USER_POWER_MAX);
}

static const ui_custom_t user_power = {
    .format = user_power_format,
    .change = user_power_change,
};

static const ui_bit_t dmr_crc = {
    .word = &settings.bitfield_options,
    .mask = SETTINGS_BIT_DMR_CRC_IGNORED,
    .inverted = true,
};

/* Mode: 5 W or 10 W radio */

static void power_mode_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%sW", ((settings.bitfield_options & SETTINGS_BIT_FORCE_10W) ? "10" : "5"));
}

static void power_mode_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings_set_option(SETTINGS_BIT_FORCE_10W, (direction > 0));
}

static void power_mode_select(void)
{
    settings.bitfield_options ^= SETTINGS_BIT_FORCE_10W;
}

static const ui_custom_t power_mode = {
    .format = power_mode_format,
    .change = power_mode_change,
    .select = power_mode_select,
};

static const ui_item_t radio_options_items[] = {
    {TEXT_BAND_LIMITS, ICONS_COUNT, UI_ITEM_CUSTOM, &band_limits, NULL},
    {TEXT_DMR_FILTER_TIMEOUT, ICONS_COUNT, UI_ITEM_NUMBER, &capture_timeout, NULL},
    {TEXT_SCAN_DELAY, ICONS_COUNT, UI_ITEM_NUMBER, &scan_delay, NULL},
    {TEXT_SCAN_DWELL_TIME, ICONS_COUNT, UI_ITEM_CUSTOM, &scan_dwell, NULL},
    {TEXT_SCAN_MODE, ICONS_COUNT, UI_ITEM_CHOICE, &scan_mode, NULL},
    {TEXT_SCAN_ON_BOOT, ICONS_COUNT, UI_ITEM_BIT, &scan_on_boot, NULL},
    {TEXT_BOOT_SCREEN, ICONS_COUNT, UI_ITEM_BIT, &boot_screen, NULL},
    {TEXT_SQUELCH_VHF, ICONS_COUNT, UI_ITEM_CUSTOM, &squelch_vhf, NULL},
    {TEXT_SQUELCH_220, ICONS_COUNT, UI_ITEM_CUSTOM, &squelch_220, NULL},
    {TEXT_SQUELCH_UHF, ICONS_COUNT, UI_ITEM_CUSTOM, &squelch_uhf, NULL},
    {TEXT_PTT_TOGGLE, ICONS_COUNT, UI_ITEM_BIT, &ptt_toggle, NULL},
    {TEXT_PRIVATE_CALL_HANDLING, ICONS_COUNT, UI_ITEM_CHOICE, &private_calls, NULL},
    {TEXT_USER_POWER, ICONS_COUNT, UI_ITEM_CUSTOM, &user_power, NULL},
    {TEXT_DMR_CRC, ICONS_COUNT, UI_ITEM_BIT, &dmr_crc, NULL},
    {TEXT_MODE, ICONS_COUNT, UI_ITEM_CUSTOM, &power_mode, NULL},
};

const ui_list_t radio_options = {
    .title = TEXT_RADIO_OPTIONS,
    .items = radio_options_items,
    .count = sizeof(radio_options_items) / sizeof(radio_options_items[0]),
};
