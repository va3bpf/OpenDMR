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
 * OpenDMR, service: the settings in the data flash. After OpenDM32 user/SOURCE/functions/settings.c
 * and interfaces/settingsStorage.c: the same place and layout, so the settings of the main firmware are read as they
 * are. None there (a radio new to this firmware): the main firmware's defaults (restore_defaults()).
 */

#include <string.h>

#include "codeplug.h"
#include "flash.h"
#include "settings.h"

#define SETTINGS_ADDRESS (0x6000 + 0x4B) // in the window, after the last used channel in each zone (codeplug)

// The DMR timeslot capture (hrc7000GetTSTimeoutValue()): its menu goes from 2 to 90 s. A 0 stored there (never set
// by the menu) left the receive locked for good on the first timeslot heard on a repeater (check_ranges()).
#define CAPTURE_TIMEOUT_MIN     2
#define CAPTURE_TIMEOUT_MAX     90
#define CAPTURE_TIMEOUT_DEFAULT 10 // settingsRestoreDefaultSettings()

_Static_assert(sizeof(settings_t) == 108, "settings_t must keep the layout of the main firmware");

settings_t settings;

static volatile bool changed = false;

bool settings_valid(void)
{
    return (settings.magic_number == SETTINGS_MAGIC_NUMBER);
}

// settingsRestoreDefaultSettings() of the main firmware, DM-32 build, with the values its enums have there. Not taken
// over: the DMR mic gain is 16 (it was 11; with the 20 dB analog boost of modem.c 16 gives a normal level), the
// prompts are beeps (no voice prompts here), the VOX is off, APRS beaconing is off (all 0)
static void restore_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->magic_number = SETTINGS_MAGIC_NUMBER;
    s->location_lat = 0x7F000000; // SETTINGS_UNITIALISED_LOCATION_LAT: never set (satellite.c, gps.c)
    s->location_lon = 0;
    s->timezone = 64;                  // SETTINGS_TIMEZONE_UTC
    s->beep_options = 0x01 | 0x02;     // BEEP_TX_START | BEEP_TX_STOP
    s->vfo_sweep_settings = (6 << 12) | (14 << 7) | 86; // the last of the 7 steps, noise floor 14, gain 86
    s->bitfield_options = (1UL << 4);  // BIT_SETTINGS_UPDATED
    s->user_power = 4100;
    s->dual_watch_path[0] = -1;        // VFO A and VFO B
    s->dual_watch_path[1] = -2;
    s->tx_power_level = 4;             // 1 W
    s->tx_timeout_beep_x5_secs = 2;
    s->beep_volume_divider = 4;        // -6 dB
    s->mic_gain_dmr = 16;
    s->mic_gain_fm = 26;               // SETTINGS_FM_MIC_ZERO, the stock default
    s->backlight_mode = SETTINGS_BACKLIGHT_AUTO;
    s->backlight_timeout = 0;          // never
    s->display_contrast = 0x12;
    s->display_backlight_percentage[0] = 100;
    s->display_backlight_percentage[1] = 100;
    s->display_backlight_percentage_off = 0;
    s->initial_menu_number = 35;       // UI_VFO_MODE (VFO_SCREEN_MENU_VFO)
    s->scan_mode_pause = 0;            // SCAN_MODE_HOLD
    s->scan_delay = 5;                 // seconds
    s->hotspot_type = 0;               // HOTSPOT_TYPE_OFF
    s->current_vfo_number = 0;         // VFO A
    s->dmr_capture_timeout = CAPTURE_TIMEOUT_DEFAULT;
    s->dmr_cc_ts_filter = 0x01 | 0x02; // DMR_CCTS_FILTER_CC_TS
    s->analog_filter_level = 1;        // ANALOG_FILTER_CSS
    s->private_calls = 1;              // ALLOW_PRIVATE_CALLS_ON
    s->split_contact = 1;              // SPLIT_CONTACT_ON_TWO_LINES
    s->vox_threshold = 0;                   // off (the main firmware had 20: its scale, not vox.h's 1 .. 13)
    s->vox_tail_units = 4;             // 2 s
    s->audio_prompt_mode = 1;          // AUDIO_PROMPT_MODE_BEEP (SOUND_PROMPT_BEEP)
    s->battery_calibration = 0x05 | (0x07 << 4); // no correction (5 is 0)
    for (int band = 0; band < SETTINGS_SQUELCH_BANDS; band++) {
        s->squelch_defaults[band] = 10;
    }
    s->eco_level = 1;
    s->keypad_timer_long = 4;          // 400 ms
    s->keypad_timer_repeat = 2;        // 200 ms
    s->gps = 0;                        // GPS_NOT_DETECTED
}

// Each setting in the range its menu gives it, else its default. A radio whose settings were saved before there were
// defaults (all 0xFF, but the magic number right: the green radio, 2026-11-06) gets them all back here; the menus'
// limits are in the app/ files named. The main firmware did this for the DMR mic gain only.
#define IN(value, low, high) (((value) >= (low)) && ((value) <= (high)))
#define CHECK_IF(field, ok)                                                                                            \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            settings.field = defaults.field;                                                                           \
            fixed = true;                                                                                              \
        }                                                                                                              \
    } while (0)
#define CHECK(field, low, high) CHECK_IF(field, IN(settings.field, (low), (high)))

#define BITS_KNOWN       0x07FFFBFFUL // bitfield_options: the main firmware's BIT_x (not 10, nor above 26)
#define TIMEZONE_MASK    0x7F         // menus.c: quarters of an hour from UTC - 64, -12 h .. +14 h
#define TIMEZONE_MIN     (64 - (12 * 4))
#define TIMEZONE_MAX     (64 + (14 * 4))
#define PC_FLAG          0x03         // tx_target.h TX_TARGET_PC_FLAG: the top byte of a private call
#define TS_OVERRIDE_BITS 0x01FF       // tx_target.c: 3 x 2 bits of timeslot, 3 bits of contact overridden

// override_tg: 0 (none), or a number with the top byte of a TG (0) or of a private call. 0xFFFFFFFF overrode every
// channel's TG (the one the CPS gives it)
static bool override_tg_ok(void)
{
    uint32_t top = settings.override_tg >> 24;

    return (settings.override_tg == 0) ||
           (((top == 0) || (top == PC_FLAG)) && ((settings.override_tg & CODEPLUG_CONTACT_ID_MAX) != 0));
}

// A location as gps.c stores it: the sign in bit 31, the degrees in bits 30 - 23, the decimals (0 - 99999) below
static bool location_ok(uint32_t fixed, uint32_t max_degrees)
{
    uint32_t degrees = (fixed & 0x7FFFFFFFUL) >> 23;
    uint32_t decimals = fixed & ((1UL << 23) - 1);

    return (decimals < 100000) && ((degrees < max_degrees) || ((degrees == max_degrees) && (decimals == 0)));
}

// A VFO scan limit: 0 (none) or a frequency of a band
static bool scan_limit_ok(uint32_t frequency)
{
    return (frequency == 0) || codeplug_frequency_is_valid(frequency);
}

// The APRS beaconing fields, each in the range of its option (aprs_options.c)
static bool aprs_ok(void)
{
    uint32_t p0 = settings.aprs_beaconing_settings_part1[0];
    uint32_t p1 = settings.aprs_beaconing_settings_part1[1];
    uint16_t p2 = settings.aprs_beaconing_settings_part2;

    return IN((p0 >> 29) & 0x07, 0, 4) && IN((p0 >> 24) & 0x1F, 0, 30) && IN((p0 >> 16) & 0xFF, 0, 180) &&
           IN(p0 & 0xFF, 0, 180) && IN((p1 >> 14) & 0x7F, 0, 100) && IN((p1 >> 7) & 0x7F, 0, 90) &&
           IN(p1 & 0x7F, 0, 90) && IN(p2 & 0x0F, 0, 9);
}

static bool check_ranges(void)
{
    settings_t defaults;
    bool fixed = false;
    bool aprs = aprs_ok();

    restore_defaults(&defaults);
    CHECK_IF(location_lat, (settings.location_lat == defaults.location_lat) || location_ok(settings.location_lat, 90));
    CHECK_IF(location_lon, location_ok(settings.location_lon, 180));
    CHECK_IF(timezone, IN(settings.timezone & TIMEZONE_MASK, TIMEZONE_MIN, TIMEZONE_MAX));
    CHECK(beep_options, 0, 0x1F);
    CHECK_IF(vfo_sweep_settings, ((settings.vfo_sweep_settings >> 12) & 0x0F) <= 6);
    CHECK_IF(override_tg, override_tg_ok());
    for (int vfo = 0; vfo < 2; vfo++) {
        CHECK_IF(vfo_scan_low[vfo], scan_limit_ok(settings.vfo_scan_low[vfo]));
        CHECK_IF(vfo_scan_high[vfo], scan_limit_ok(settings.vfo_scan_high[vfo]));
    }
    CHECK_IF(bitfield_options, (settings.bitfield_options & ~BITS_KNOWN) == 0); // all set: the forced 10 W too
    CHECK_IF(aprs_beaconing_settings_part1[0], aprs);
    CHECK_IF(aprs_beaconing_settings_part1[1], aprs);
    CHECK_IF(aprs_beaconing_settings_part2, aprs);
    for (int i = 0; i < 3; i++) {
        CHECK(current_index_in_trx_group_list[i], 0, INT16_MAX);
    }
    CHECK(current_zone, 0, 255);
    CHECK(user_power, 0, 4100);           // radio_options.c USER_POWER_MAX
    CHECK(ts_manual_override, 0, TS_OVERRIDE_BITS);
    CHECK(dual_watch_path[0], -2, 1024);  // dual_watch.c: a channel number, -1 VFO A, -2 VFO B
    CHECK(dual_watch_path[1], -2, 1024);
    CHECK(tx_power_level, 0, 9);          // radio.c POWER_LEVELS
    CHECK(tx_timeout_beep_x5_secs, 0, 4); // sound_options.c
    CHECK(beep_volume_divider, 0, 10);
    CHECK(mic_gain_dmr, 0, 20);
    CHECK(mic_gain_fm, 0, 31);
    CHECK(backlight_mode, 0, SETTINGS_BACKLIGHT_MODES - 1);
    CHECK(backlight_timeout, 0, 30);      // menus.c
    CHECK(display_backlight_percentage[0], 0, 100);
    CHECK(display_backlight_percentage[1], 0, 100);
    CHECK(display_backlight_percentage_off, 0, 100);
    CHECK(initial_menu_number, 35, 36);   // vfo_screen.h: the VFO or the channel screen
    CHECK(extended_infos_on_screen, 0, 3);
    CHECK(tx_freq_limited, 0, 2);         // radio_options.c BAND_LIMITS_MODES
    CHECK(scan_mode_pause, 0, 2);
    CHECK(scan_delay, 0, 30);
    CHECK(dmr_rx_agc, 0, 8);
    CHECK(hotspot_type, 0, 2);
    CHECK(scan_step_time, 0, 15);
    CHECK(current_vfo_number, 0, 1);
    CHECK(dmr_destination_filter, 0, 3);  // dmr_compat.h
    CHECK(dmr_capture_timeout, CAPTURE_TIMEOUT_MIN, CAPTURE_TIMEOUT_MAX);
    CHECK(dmr_cc_ts_filter, 0, 3);
    CHECK(analog_filter_level, 0, 1);
    CHECK(private_calls, 0, 2);
    CHECK(contact_display_priority, 0, 3);
    CHECK(split_contact, 0, 2);
    CHECK(vox_threshold, 0, 13);          // vox.h VOX_LEVELS
    CHECK(vox_tail_units, 1, 10);         // sound_options.c
    CHECK(audio_prompt_mode, 0, 2);       // sound.h
    CHECK_IF(battery_calibration, (settings.battery_calibration & 0x0F) <= 10); // general_options.c: 5 is none
    for (int band = 0; band < SETTINGS_SQUELCH_BANDS; band++) {
        CHECK(squelch_defaults[band], 1, 21);
    }
    CHECK(eco_level, 0, 5);               // general_options.c
    CHECK(apo, 0, 6);
    CHECK(keypad_timer_long, 1, 90);      // 255: a long press took 25.5 s
    CHECK(keypad_timer_repeat, 1, 90);
    CHECK(autolock_timer, 0, 30);
    CHECK(gps, 0, 3);
    return fixed;
}

// settingsLoadSettings(): none in the flash (a radio new to this firmware: all 0xFF there) -> the defaults; settings
// out of range -> their defaults; saved either way. Not when the read itself failed: the settings there may be good.
bool settings_load(void)
{
    if (!flash_read(SETTINGS_ADDRESS, (uint8_t *)&settings, sizeof(settings))) {
        settings.magic_number = 0;
        return false;
    }
    if (!settings_valid()) {
        restore_defaults(&settings);
        settings_save();
    } else if (check_ranges()) {
        settings_save();
    }
    return true;
}

bool settings_save(void)
{
    settings.magic_number = SETTINGS_MAGIC_NUMBER;
    return flash_write(SETTINGS_ADDRESS, (const uint8_t *)&settings, sizeof(settings));
}

void settings_set_option(uint32_t bit, bool on)
{
    settings.bitfield_options = on ? (settings.bitfield_options | bit) : (settings.bitfield_options & ~bit);
}

void settings_changed(void)
{
    changed = true;
}

bool settings_save_if_changed(void)
{
    if (!changed) {
        return true;
    }
    changed = false;
    if (!settings_save()) {
        changed = true;
        return false;
    }
    return true;
}
