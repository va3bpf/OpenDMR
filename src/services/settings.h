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

#ifndef DM32_SETTINGS_H
#define DM32_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

// The settings as stored in the data flash window at SETTINGS_ADDRESS. The layout is the one of the main firmware
// (OpenDM32 settingsStruct_t, DM-32 build: HAS_GPS, no LOG_GPS_DATA), so both read the same settings.
// Only the names follow docs/coding-style.md.
#define SETTINGS_MAGIC_NUMBER  0x477B
#define SETTINGS_SQUELCH_BANDS 3 // VHF, 220 MHz, UHF

typedef struct {
    uint32_t magic_number;
    uint32_t location_lat; // fixed point: 1 sign bit, 8 bits integer, 23 bits decimals
    uint32_t location_lon;
    uint8_t timezone; // lower 7 bits: 64 = UTC, below negative; bit 7: TZ (1) / UTC (0)
    uint8_t beep_options;
    uint16_t vfo_sweep_settings; // 3 bits channel step, 5 bits RSSI noise floor, 7 bits gain
    uint32_t override_tg;
    uint32_t vfo_scan_low[2];
    uint32_t vfo_scan_high[2];
    uint32_t bitfield_options;
    uint32_t aprs_beaconing_settings_part1[2];
    int16_t current_index_in_trx_group_list[3]; // current channel, VFO A, VFO B
    int16_t current_zone;
    uint16_t user_power;
    uint16_t ts_manual_override;
    int16_t dual_watch_path[2]; // channel number (1 ..), -1 VFO A, -2 VFO B, 0 none
    uint16_t aprs_beaconing_settings_part2;
    uint8_t tx_power_level;
    uint8_t tx_timeout_beep_x5_secs;
    uint8_t beep_volume_divider;
    uint8_t mic_gain_dmr;
    uint8_t mic_gain_fm;
    uint8_t backlight_mode;
    uint8_t backlight_timeout; // seconds, 0 = never
    int8_t display_contrast;
    int8_t display_backlight_percentage[2]; // day, night
    int8_t display_backlight_percentage_off;
    uint8_t initial_menu_number;
    uint8_t extended_infos_on_screen;
    uint8_t tx_freq_limited;
    uint8_t scan_mode_pause;
    uint8_t scan_delay;
    uint8_t dmr_rx_agc;
    uint8_t hotspot_type;
    uint8_t scan_step_time;
    uint8_t current_vfo_number;
    uint8_t dmr_destination_filter;
    uint8_t dmr_capture_timeout;
    uint8_t dmr_cc_ts_filter;
    uint8_t analog_filter_level;
    uint8_t private_calls;
    uint8_t contact_display_priority;
    uint8_t split_contact;
    uint8_t vox_threshold;  // 0: off
    uint8_t vox_tail_units; // 500 ms units
    uint8_t audio_prompt_mode;
    int8_t temperature_calibration; // 0.5 C units
    uint8_t battery_calibration;    // 0.01 V units, lower 4 bits
    uint8_t squelch_defaults[SETTINGS_SQUELCH_BANDS];
    uint8_t eco_level;
    uint8_t apo; // 30 minute units
    uint8_t keypad_timer_long;
    uint8_t keypad_timer_repeat;
    uint8_t autolock_timer; // minutes
    uint8_t gps;
} settings_t;

extern settings_t settings;

bool settings_load(void);  // none in the flash: the defaults; out of range: those defaults; false: not readable
bool settings_save(void);  // only what changed is written (flash.c)
bool settings_valid(void); // the magic number is right

// Bits of bitfield_options (the main firmware's BIT_x)
#define SETTINGS_BIT_PTT_LATCH          (1UL << 1)
#define SETTINGS_BIT_BOOT_SCREEN        (1UL << 2) // DM-32: boot screen and melody
#define SETTINGS_BIT_ALL_LEDS_DISABLED  (1UL << 6)
#define SETTINGS_BIT_SCAN_ON_BOOT       (1UL << 7)
#define SETTINGS_BIT_SATELLITE_AUTO     (1UL << 9) // BIT_SATELLITE_MANUAL_AUTO: set is auto
#define SETTINGS_BIT_DMR_CRC_IGNORED    (1UL << 12)
#define SETTINGS_BIT_APO_WITH_RF        (1UL << 13)
#define SETTINGS_BIT_SAFE_POWER_ON      (1UL << 14)
#define SETTINGS_BIT_AUTO_NIGHT         (1UL << 15)
#define SETTINGS_BIT_SECONDARY_LANGUAGE (1UL << 19) // Chinese
#define SETTINGS_BIT_FORCE_10W          (1UL << 24) // BIT_FORCE_10W_RADIO: unused, the levels go to 10 W anyway

// The backlight modes (backlight_mode)
enum {
    SETTINGS_BACKLIGHT_AUTO,
    SETTINGS_BACKLIGHT_SQUELCH,
    SETTINGS_BACKLIGHT_MANUAL,
    SETTINGS_BACKLIGHT_BUTTONS,
    SETTINGS_BACKLIGHT_MODES
};

void settings_set_option(uint32_t bit, bool on); // a SETTINGS_BIT_x of bitfield_options
void settings_changed(void);         // the settings in RAM changed: settings_save_if_changed() writes them
bool settings_save_if_changed(void); // from the UI when a settings list is left, and at power off

#endif
