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
 * OpenDMR, menu: the APRS beaconing options (menuAPRSOptions.c, without RATE_MESSAGE_FEATURE as the DM-32
 * build). The settings are packed into settings.aprs_beaconing_settings_part1[] / part2 as aprs.c
 * aprsBeaconingUpdateSystemSettingsFromConfiguration() does; each item unpacks, changes and packs its field; the
 * beaconing itself is services/aprs.c. SK2 held: bigger steps for the smart beaconing values.
 */

#include <stdio.h>
#include "util.h"
#include "settings.h"
#include "text.h"
#include "options.h"
#include "aprs.h"

// Fields: part1[0]: mode 31..29, low speed 28..24, fast rate 23..16, turn slope 15..8, turn time 7..0
//         part1[1]: message interval 25..21, slow rate 20..14, high speed 13..7, turn angle 6..0
//         part2: state 11..4, initial interval 3..0
typedef struct {
    uint8_t word; // 0, 1: part1[]; 2: part2
    uint8_t shift;
    uint8_t bits;
} field_t;

#define PART2 2

static const field_t FIELD_MODE = {0, 29, 3};
static const field_t FIELD_LOW_SPEED = {0, 24, 5};
static const field_t FIELD_FAST_RATE = {0, 16, 8};
static const field_t FIELD_TURN_SLOPE = {0, 8, 8};
static const field_t FIELD_TURN_TIME = {0, 0, 8};
static const field_t FIELD_SLOW_RATE = {1, 14, 7};
static const field_t FIELD_HIGH_SPEED = {1, 7, 7};
static const field_t FIELD_TURN_ANGLE = {1, 0, 7};
static const field_t FIELD_STATE = {PART2, 4, 8};
static const field_t FIELD_INITIAL_INTERVAL = {PART2, 0, 4};

// Modes (aprsBeaconingMode_t)
#define MODES 5 // off, manual, PTT, auto, smart

// Bits of the state (aprsBeaconingStates_t)
#define STATE_LOCATION_FROM_CHANNEL 0x02
#define STATE_LOCATION_FROM_GPS     0x04
#define STATE_LOCATION_MASK         0x06
#define STATE_DECAY                 0x08
#define STATE_COMPRESSED            0x10

#define INITIAL_INTERVAL_MAX 9
#define SECONDS_A_MINUTE     60

// The smart beaconing ranges (aprs.h APRS_SMART_BEACON_x_MIN / _MAX) and the steps with SK2 held
#define SLOW_RATE_MIN   1 // min
#define SLOW_RATE_MAX   100
#define FAST_RATE_MIN   10 // s
#define FAST_RATE_MAX   180
#define LOW_SPEED_MIN   2 // km/h
#define LOW_SPEED_MAX   30
#define HIGH_SPEED_MIN  2
#define HIGH_SPEED_MAX  90
#define TURN_ANGLE_MIN  5 // degrees
#define TURN_ANGLE_MAX  90
#define TURN_SLOPE_MIN  1 // 10 degrees / speed
#define TURN_SLOPE_MAX  255
#define TURN_TIME_MIN   5 // s
#define TURN_TIME_MAX   180
#define BIG_STEP        10
#define LOW_SPEED_STEP  4
#define TURN_SLOPE_UNIT 10

#define DEGREE "\xC2\xB0" // in the CJK font (the Roboto fonts are ASCII only)

// The initial interval choices (aprs.c initialIntervalsInSecs[])
static const uint16_t initial_intervals_s[INITIAL_INTERVAL_MAX + 1] = {12,  30,  60,   120,  180,
                                                                       300, 600, 1200, 1800, 3600};

static uint32_t *field_word(const field_t *field)
{
    static uint32_t part2;

    if (field->word == PART2) {
        part2 = settings.aprs_beaconing_settings_part2;
        return &part2;
    }
    return &settings.aprs_beaconing_settings_part1[field->word];
}

static unsigned field_get(const field_t *field)
{
    return (*field_word(field) >> field->shift) & ((1UL << field->bits) - 1);
}

static void field_set(const field_t *field, unsigned value)
{
    uint32_t mask = ((1UL << field->bits) - 1) << field->shift;
    uint32_t *word = field_word(field);

    *word = (*word & ~mask) | ((value << field->shift) & mask);
    if (field->word == PART2) {
        settings.aprs_beaconing_settings_part2 = (uint16_t)*word;
    }
}

// A smart beaconing value: 1 a step, big_step with SK2 held, within min .. max
static void field_change(const field_t *field, int direction, uint8_t modifiers, int big_step, int min, int max)
{
    int step = (modifiers & UI_MODIFIER_SK2) ? big_step : 1;

    field_set(field, (unsigned)CLAMP((int)field_get(field) + (direction * step), min, max));
}

static void field_format(const field_t *field, const char *unit, char *buffer, size_t size)
{
    snprintf(buffer, size, "%u%s", field_get(field), unit);
}

static void state_set(uint32_t mask, bool on)
{
    unsigned state = field_get(&FIELD_STATE);

    field_set(&FIELD_STATE, on ? (state | mask) : (state & ~mask));
}

/* Mode */

static void mode_format(char *buffer, size_t size)
{
    static const text_id_t modes[MODES] = {TEXT_OFF, TEXT_MANUAL, TEXT_PTT, TEXT_AUTO, TEXT_APRS_SMART};
    unsigned mode = field_get(&FIELD_MODE);

    snprintf(buffer, size, "%s", ((mode < MODES) ? text(modes[mode]) : "?"));
}

static void mode_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    field_set(&FIELD_MODE, (unsigned)CLAMP((int)field_get(&FIELD_MODE) + direction, 0, MODES - 1));
}

static const ui_custom_t mode = {
    .format = mode_format,
    .change = mode_change,
};

/* Location: from the channel or the GPS */

static void location_format(char *buffer, size_t size)
{
    bool gps = (field_get(&FIELD_STATE) & STATE_LOCATION_FROM_GPS) != 0;

    snprintf(buffer, size, "%s", text(gps ? TEXT_GPS : TEXT_APRS_CHANNEL));
}

static void location_change(int direction, uint8_t modifiers)
{
    unsigned state = field_get(&FIELD_STATE) & ~STATE_LOCATION_MASK;

    (void)modifiers;
    field_set(&FIELD_STATE, state | ((direction > 0) ? STATE_LOCATION_FROM_GPS : STATE_LOCATION_FROM_CHANNEL));
}

static void location_select(void)
{
    location_change(((field_get(&FIELD_STATE) & STATE_LOCATION_FROM_GPS) ? -1 : 1), 0);
}

static const ui_custom_t location = {
    .format = location_format,
    .change = location_change,
    .select = location_select,
};

/* Initial interval, in minutes with one decimal */

static void interval_format(char *buffer, size_t size)
{
    unsigned index = MIN(field_get(&FIELD_INITIAL_INTERVAL), INITIAL_INTERVAL_MAX);
    unsigned tenths = (initial_intervals_s[index] * 10) / SECONDS_A_MINUTE;

    if (tenths % 10) {
        snprintf(buffer, size, "%u.%umin", tenths / 10, tenths % 10);
    } else {
        snprintf(buffer, size, "%umin", tenths / 10);
    }
}

static void interval_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    field_set(&FIELD_INITIAL_INTERVAL,
              (unsigned)CLAMP((int)field_get(&FIELD_INITIAL_INTERVAL) + direction, 0, INITIAL_INTERVAL_MAX));
}

static const ui_custom_t interval = {
    .format = interval_format,
    .change = interval_change,
};

/* Decay, compressed: on / off bits of the state */

static void decay_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text((field_get(&FIELD_STATE) & STATE_DECAY) ? TEXT_ON : TEXT_OFF));
}

static void decay_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    state_set(STATE_DECAY, (direction > 0));
}

static void decay_select(void)
{
    state_set(STATE_DECAY, !(field_get(&FIELD_STATE) & STATE_DECAY));
}

static const ui_custom_t decay = {
    .format = decay_format,
    .change = decay_change,
    .select = decay_select,
};

static void compressed_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text((field_get(&FIELD_STATE) & STATE_COMPRESSED) ? TEXT_ON : TEXT_OFF));
}

static void compressed_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    state_set(STATE_COMPRESSED, (direction > 0));
}

static void compressed_select(void)
{
    state_set(STATE_COMPRESSED, !(field_get(&FIELD_STATE) & STATE_COMPRESSED));
}

static const ui_custom_t compressed = {
    .format = compressed_format,
    .change = compressed_change,
    .select = compressed_select,
};

/* Smart beaconing */

static void slow_rate_format(char *buffer, size_t size)
{
    field_format(&FIELD_SLOW_RATE, "min", buffer, size);
}

static void slow_rate_change(int direction, uint8_t modifiers)
{
    field_change(&FIELD_SLOW_RATE, direction, modifiers, BIG_STEP, SLOW_RATE_MIN, SLOW_RATE_MAX);
}

static const ui_custom_t slow_rate = {
    .format = slow_rate_format,
    .change = slow_rate_change,
};

static void fast_rate_format(char *buffer, size_t size)
{
    field_format(&FIELD_FAST_RATE, "s", buffer, size);
}

static void fast_rate_change(int direction, uint8_t modifiers)
{
    field_change(&FIELD_FAST_RATE, direction, modifiers, BIG_STEP, FAST_RATE_MIN, FAST_RATE_MAX);
}

static const ui_custom_t fast_rate = {
    .format = fast_rate_format,
    .change = fast_rate_change,
};

static void low_speed_format(char *buffer, size_t size)
{
    field_format(&FIELD_LOW_SPEED, "km/h", buffer, size);
}

static void low_speed_change(int direction, uint8_t modifiers)
{
    field_change(&FIELD_LOW_SPEED, direction, modifiers, LOW_SPEED_STEP, LOW_SPEED_MIN, LOW_SPEED_MAX);
}

static const ui_custom_t low_speed = {
    .format = low_speed_format,
    .change = low_speed_change,
};

static void high_speed_format(char *buffer, size_t size)
{
    field_format(&FIELD_HIGH_SPEED, "km/h", buffer, size);
}

static void high_speed_change(int direction, uint8_t modifiers)
{
    field_change(&FIELD_HIGH_SPEED, direction, modifiers, BIG_STEP, HIGH_SPEED_MIN, HIGH_SPEED_MAX);
}

static const ui_custom_t high_speed = {
    .format = high_speed_format,
    .change = high_speed_change,
};

static void turn_angle_format(char *buffer, size_t size)
{
    field_format(&FIELD_TURN_ANGLE, DEGREE, buffer, size);
}

static void turn_angle_change(int direction, uint8_t modifiers)
{
    field_change(&FIELD_TURN_ANGLE, direction, modifiers, BIG_STEP, TURN_ANGLE_MIN, TURN_ANGLE_MAX);
}

static const ui_custom_t turn_angle = {
    .format = turn_angle_format,
    .change = turn_angle_change,
};

static void turn_slope_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%u" DEGREE "/v", field_get(&FIELD_TURN_SLOPE) * TURN_SLOPE_UNIT);
}

static void turn_slope_change(int direction, uint8_t modifiers)
{
    field_change(&FIELD_TURN_SLOPE, direction, modifiers, BIG_STEP, TURN_SLOPE_MIN, TURN_SLOPE_MAX);
}

static const ui_custom_t turn_slope = {
    .format = turn_slope_format,
    .change = turn_slope_change,
};

static void turn_time_format(char *buffer, size_t size)
{
    field_format(&FIELD_TURN_TIME, "s", buffer, size);
}

static void turn_time_change(int direction, uint8_t modifiers)
{
    field_change(&FIELD_TURN_TIME, direction, modifiers, BIG_STEP, TURN_TIME_MIN, TURN_TIME_MAX);
}

static const ui_custom_t turn_time = {
    .format = turn_time_format,
    .change = turn_time_change,
};

// "Send beacon now": not a setting, an action; the row shows OK and the select sends (aprs_send() sets the request, the
// input task does the rest: in any mode but Off, and it beeps when it cannot)
static void send_now_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text(TEXT_UI_OK));
}

static void send_now_select(void)
{
    aprs_send();
}

static const ui_custom_t send_now = {
    .format = send_now_format,
    .select = send_now_select,
};

static const ui_item_t aprs_options_items[] = {
    {TEXT_MODE, ICONS_COUNT, UI_ITEM_CUSTOM, &mode, NULL},
    {TEXT_LOCATION, ICONS_COUNT, UI_ITEM_CUSTOM, &location, NULL},
    {TEXT_APRS_INTERVAL, ICONS_COUNT, UI_ITEM_CUSTOM, &interval, NULL},
    {TEXT_APRS_DECAY, ICONS_COUNT, UI_ITEM_CUSTOM, &decay, NULL},
    {TEXT_APRS_COMPRESS, ICONS_COUNT, UI_ITEM_CUSTOM, &compressed, NULL},
    {TEXT_APRS_SLOW_RATE, ICONS_COUNT, UI_ITEM_CUSTOM, &slow_rate, NULL},
    {TEXT_APRS_FAST_RATE, ICONS_COUNT, UI_ITEM_CUSTOM, &fast_rate, NULL},
    {TEXT_APRS_LOW_SPEED, ICONS_COUNT, UI_ITEM_CUSTOM, &low_speed, NULL},
    {TEXT_APRS_HIGH_SPEED, ICONS_COUNT, UI_ITEM_CUSTOM, &high_speed, NULL},
    {TEXT_APRS_TURN_ANGLE, ICONS_COUNT, UI_ITEM_CUSTOM, &turn_angle, NULL},
    {TEXT_APRS_TURN_SLOPE, ICONS_COUNT, UI_ITEM_CUSTOM, &turn_slope, NULL},
    {TEXT_APRS_TURN_TIME, ICONS_COUNT, UI_ITEM_CUSTOM, &turn_time, NULL},
    {TEXT_APRS_SEND_NOW, ICONS_COUNT, UI_ITEM_CUSTOM, &send_now, NULL},
};

const ui_list_t aprs_options = {
    .title = TEXT_APRS_OPTIONS,
    .items = aprs_options_items,
    .count = sizeof(aprs_options_items) / sizeof(aprs_options_items[0]),
};
