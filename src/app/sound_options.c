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
 * OpenDMR, menu: the sound options (menuSoundOptions.c, the DM-32 / MD-UV380 items in its order), and our FM
 * transmit tones (sound.h). The VOX works on FM and DMR channels with VOX on (radio.c, vox.c). The beep items are "N/A" while the
 * prompt is silent.
 */

#include <stdio.h>
#include "util.h"
#include "vox.h"
#include "settings.h"
#include "text.h"
#include "options.h"
#include "sound.h"

#define TIMEOUT_BEEP_MAX     4 // 5 s units
#define TIMEOUT_BEEP_UNIT_S  5
#define BEEP_VOLUME_ZERO     2 // divider: (2 - divider) x 3 dB, from +6 dB (0) to -24 dB (10)
#define BEEP_VOLUME_MAX      10
#define BEEP_VOLUME_DB_STEP  3
#define BEEP_TX_MASK         0x03 // beep_options: none, start, stop, both
#define BEEP_RX_SHIFT        2    // then none, carrier, talker, both
#define BEEP_RX_MASK         0x0C
#define BEEP_RX_TALKER       0x08
#define BEEP_RX_TALKER_BEGIN 0x10 // talker beep at the beginning too, not only at the end
#define BEEP_CHOICES         4
#define DMR_MIC_GAIN_MAX     20 // DM-32: GCR_ADCL, after the 20 dB analog boost (modem.c); 16 gives a normal level
#define FM_MIC_GAIN_MAX      31 // DM-32: the FD6818 mic sensitivity (fd6818_set_mic_gain()), 26 the stock default
#define VOX_THRESHOLD_MIN    1 // the VOX levels (vox.h): 1 the most sensitive; 0 (off) is only the default
#define VOX_THRESHOLD_MAX    VOX_LEVELS
#define VOX_TAIL_MIN         1 // 500 ms units: 0.5 s .. 5 s
#define VOX_TAIL_MAX         10
#define DMR_RX_AGC_MAX       8 // 0: off, then (value - 1) x 3 dB
#define DMR_RX_AGC_DB_STEP   3

// Audio prompt modes (audio_prompt_mode). The voice prompt levels need the voice prompt data, which this firmware
// doesn't load (yet): only silent, beep and "no keys" are offered, as the main firmware does without the data.
enum {
    PROMPT_SILENT = 0,
    PROMPT_BEEP,
    PROMPT_NO_KEY_BEEP,
    PROMPT_VOICE_LEVEL_1,
    PROMPT_VOICE_LEVEL_2,
    PROMPT_VOICE_LEVEL_3,
    PROMPT_MODES,
    PROMPT_MODES_WITHOUT_VOICE = PROMPT_VOICE_LEVEL_1
};

static bool silent(void)
{
    return (settings.audio_prompt_mode == PROMPT_SILENT);
}

static void format_text(text_id_t id, char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", text(id));
}

/* Timeout beep: 5 s units before the end of the transmit timeout; 0: none */

static void timeout_beep_format(char *buffer, size_t size)
{
    if (silent() || (settings.tx_timeout_beep_x5_secs == 0)) {
        format_text(TEXT_N_A, buffer, size);
    } else {
        snprintf(buffer, size, "%ds", settings.tx_timeout_beep_x5_secs * TIMEOUT_BEEP_UNIT_S);
    }
}

static void timeout_beep_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    if (!silent()) {
        settings.tx_timeout_beep_x5_secs =
            (uint8_t)CLAMP(settings.tx_timeout_beep_x5_secs + direction, 0, TIMEOUT_BEEP_MAX);
    }
}

static const ui_custom_t timeout_beep = {
    .format = timeout_beep_format,
    .change = timeout_beep_change,
};

/* Beep volume: a divider, so up is a smaller one */

static void beep_volume_format(char *buffer, size_t size)
{
    if (silent()) {
        format_text(TEXT_N_A, buffer, size);
    } else {
        snprintf(buffer, size, "%ddB", (BEEP_VOLUME_ZERO - settings.beep_volume_divider) * BEEP_VOLUME_DB_STEP);
    }
}

static void beep_volume_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    if (!silent()) {
        settings.beep_volume_divider = (uint8_t)CLAMP(settings.beep_volume_divider - direction, 0, BEEP_VOLUME_MAX);
    }
}

static const ui_custom_t beep_volume = {
    .format = beep_volume_format,
    .change = beep_volume_change,
};

/* DMR beep (transmit): none, start, stop, both */

static void dmr_beep_format(char *buffer, size_t size)
{
    static const text_id_t beeps[BEEP_CHOICES] = {TEXT_NONE, TEXT_START, TEXT_STOP, TEXT_BOTH};

    format_text(silent() ? TEXT_N_A : beeps[settings.beep_options & BEEP_TX_MASK], buffer, size);
}

static void dmr_beep_change(int direction, uint8_t modifiers)
{
    int value = CLAMP((settings.beep_options & BEEP_TX_MASK) + direction, 0, BEEP_CHOICES - 1);

    (void)modifiers;
    if (!silent()) {
        settings.beep_options = (uint8_t)((settings.beep_options & ~BEEP_TX_MASK) | value);
    }
}

static const ui_custom_t dmr_beep = {
    .format = dmr_beep_format,
    .change = dmr_beep_change,
};

/* Receive beep: none, carrier, talker, both */

static int rx_beep(void)
{
    return (settings.beep_options & BEEP_RX_MASK) >> BEEP_RX_SHIFT;
}

static void rx_beep_format(char *buffer, size_t size)
{
    static const text_id_t beeps[BEEP_CHOICES] = {TEXT_NONE, TEXT_CARRIER, TEXT_TALKER, TEXT_BOTH};

    format_text(silent() ? TEXT_N_A : beeps[rx_beep()], buffer, size);
}

// The talker begin bit is kept as it is (the main firmware did the same)
static void rx_beep_change(int direction, uint8_t modifiers)
{
    int value = CLAMP(rx_beep() + direction, 0, BEEP_CHOICES - 1);

    (void)modifiers;
    if (!silent()) {
        settings.beep_options = (uint8_t)((settings.beep_options & ~BEEP_RX_MASK) | (value << BEEP_RX_SHIFT));
    }
}

static const ui_custom_t rx_beep_item = {
    .format = rx_beep_format,
    .change = rx_beep_change,
};

/* Talker beep: at the end only, or at the beginning too; only with the talker receive beep */

static bool talker_beep_available(void)
{
    return !silent() && ((settings.beep_options & BEEP_RX_TALKER) != 0);
}

static void talker_beep_format(char *buffer, size_t size)
{
    if (!talker_beep_available()) {
        format_text(TEXT_N_A, buffer, size);
    } else {
        format_text((settings.beep_options & BEEP_RX_TALKER_BEGIN) ? TEXT_BOTH : TEXT_END_ONLY, buffer, size);
    }
}

static void talker_beep_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    if (talker_beep_available()) {
        settings.beep_options = (direction > 0) ? (settings.beep_options | BEEP_RX_TALKER_BEGIN)
                                                : (settings.beep_options & ~BEEP_RX_TALKER_BEGIN);
    }
}

static void talker_beep_select(void)
{
    if (talker_beep_available()) {
        settings.beep_options ^= BEEP_RX_TALKER_BEGIN;
    }
}

static const ui_custom_t talker_beep = {
    .format = talker_beep_format,
    .change = talker_beep_change,
    .select = talker_beep_select,
};

// Taken at each transmission: DMR into the codec (HRC7000SetMicGainDMR()), FM into the FD6818
static const ui_number_t mic_gain_dmr = {
    .value = &settings.mic_gain_dmr,
    .min = 0,
    .max = DMR_MIC_GAIN_MAX,
    .step = 1,
    .unit = "dB",
};

static const ui_number_t mic_gain_fm = {
    .value = &settings.mic_gain_fm,
    .min = 0,
    .max = FM_MIC_GAIN_MAX,
    .step = 1,
    .unit = "",
};

/* VOX threshold: up from anything (0 is off), down to 2 at the lowest */

static void vox_threshold_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%u", (unsigned)settings.vox_threshold);
}

// Taken at each VOX sample (radio.c); from 0 it switches the VOX on
static void vox_threshold_change(int direction, uint8_t modifiers)
{
    int value = settings.vox_threshold;

    (void)modifiers;
    if ((direction > 0) && (value < VOX_THRESHOLD_MAX)) {
        value++;
    } else if ((direction < 0) && (value > VOX_THRESHOLD_MIN)) {
        value--;
    }
    settings.vox_threshold = (uint8_t)value;
}

static const ui_custom_t vox_threshold = {
    .format = vox_threshold_format,
    .change = vox_threshold_change,
};

/* VOX tail: 500 ms units, "N/A" while the VOX is off */

static void vox_tail_format(char *buffer, size_t size)
{
    if (settings.vox_threshold == 0) {
        format_text(TEXT_N_A, buffer, size);
    } else {
        snprintf(buffer, size, "%u.%us", (unsigned)(settings.vox_tail_units / 2),
                 (unsigned)((settings.vox_tail_units & 1) ? 5 : 0));
    }
}

static void vox_tail_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.vox_tail_units = (uint8_t)CLAMP(settings.vox_tail_units + direction, VOX_TAIL_MIN, VOX_TAIL_MAX);
}

static const ui_custom_t vox_tail = {
    .format = vox_tail_format,
    .change = vox_tail_change,
};

/* Audio prompt */

static void prompt_format(char *buffer, size_t size)
{
    static const text_id_t modes[PROMPT_MODES] = {TEXT_SILENT,
                                                  TEXT_BEEP,
                                                  TEXT_NO_KEYS,
                                                  TEXT_VOICE_PROMPT_LEVEL_1,
                                                  TEXT_VOICE_PROMPT_LEVEL_2,
                                                  TEXT_VOICE_PROMPT_LEVEL_3};

    if (settings.audio_prompt_mode < PROMPT_MODES) {
        format_text(modes[settings.audio_prompt_mode], buffer, size);
    } else {
        snprintf(buffer, size, "?");
    }
}

// A voice level stored by the main firmware stays until it is changed (down from it is always possible)
static void prompt_change(int direction, uint8_t modifiers)
{
    int value = settings.audio_prompt_mode;

    (void)modifiers;
    if ((direction > 0) && (value < (PROMPT_MODES_WITHOUT_VOICE - 1))) {
        value++;
    } else if ((direction < 0) && (value > PROMPT_SILENT)) {
        value = MIN(value - 1, PROMPT_MODES - 1);
    }
    settings.audio_prompt_mode = (uint8_t)value;
}

static const ui_custom_t prompt = {
    .format = prompt_format,
    .change = prompt_change,
};

/* DMR receive AGC: off, then 0 dB .. 21 dB */

static void dmr_rx_agc_format(char *buffer, size_t size)
{
    if (settings.dmr_rx_agc == 0) {
        format_text(TEXT_OFF, buffer, size);
    } else {
        snprintf(buffer, size, "%ddB", (settings.dmr_rx_agc - 1) * DMR_RX_AGC_DB_STEP);
    }
}

// Read by the DMR playback at each buffer (dmr_sound.c): 0 plays the audio as decoded
static void dmr_rx_agc_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    settings.dmr_rx_agc = (uint8_t)CLAMP(settings.dmr_rx_agc + direction, 0, DMR_RX_AGC_MAX);
}

static const ui_custom_t dmr_rx_agc = {
    .format = dmr_rx_agc_format,
    .change = dmr_rx_agc_change,
};

/* FM transmit tones (our own): a pattern for each, the air level, the local volume; the CTCSS / DCS tail (sound.h) */

static void tx_tone_format(uint8_t pattern, char *buffer, size_t size)
{
    static const text_id_t patterns[SOUND_TX_TONE_PATTERNS] = {
        TEXT_OFF, TEXT_TONE_BEEP, TEXT_TONE_DOUBLE, TEXT_TONE_RISING, TEXT_TONE_FALLING, TEXT_TONE_MORSE_K};

    format_text((pattern < SOUND_TX_TONE_PATTERNS) ? patterns[pattern] : TEXT_OFF, buffer, size);
}

static void tx_tone_change(uint8_t *pattern, int direction)
{
    *pattern = (uint8_t)CLAMP(*pattern + direction, SOUND_TX_TONE_OFF, SOUND_TX_TONE_PATTERNS - 1);
    sound_tx_tones_changed();
}

// The new pattern is heard at once, on our speaker
static void tx_tone_try(uint8_t pattern, uint8_t volume)
{
    if (pattern != SOUND_TX_TONE_OFF) {
        ui_key_beep(NULL);
        sound_play_volume(sound_tx_tone(pattern), volume);
    }
}

static void air_start_format(char *buffer, size_t size)
{
    tx_tone_format(sound_tx_tones.air_start, buffer, size);
}

static void air_start_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    tx_tone_change(&sound_tx_tones.air_start, direction);
    tx_tone_try(sound_tx_tones.air_start, settings.beep_volume_divider);
}

static void air_end_format(char *buffer, size_t size)
{
    tx_tone_format(sound_tx_tones.air_end, buffer, size);
}

static void air_end_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    tx_tone_change(&sound_tx_tones.air_end, direction);
    tx_tone_try(sound_tx_tones.air_end, settings.beep_volume_divider);
}

static void air_level_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%u", (unsigned)sound_tx_tones.air_level);
}

static void air_level_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    sound_tx_tones.air_level = (uint8_t)CLAMP(sound_tx_tones.air_level + direction, 1, SOUND_TX_TONE_LEVEL_MAX);
    sound_tx_tones_changed();
}

static void local_start_format(char *buffer, size_t size)
{
    tx_tone_format(sound_tx_tones.local_start, buffer, size);
}

static void local_start_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    tx_tone_change(&sound_tx_tones.local_start, direction);
    tx_tone_try(sound_tx_tones.local_start, sound_tx_tones.local_volume);
}

static void local_end_format(char *buffer, size_t size)
{
    tx_tone_format(sound_tx_tones.local_end, buffer, size);
}

static void local_end_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    tx_tone_change(&sound_tx_tones.local_end, direction);
    tx_tone_try(sound_tx_tones.local_end, sound_tx_tones.local_volume);
}

// As the beep volume: (2 - divider) x 3 dB
static void local_volume_format(char *buffer, size_t size)
{
    snprintf(buffer, size, "%ddB", (BEEP_VOLUME_ZERO - sound_tx_tones.local_volume) * BEEP_VOLUME_DB_STEP);
}

static void local_volume_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    sound_tx_tones.local_volume = (uint8_t)CLAMP(sound_tx_tones.local_volume - direction, 0, BEEP_VOLUME_MAX);
    sound_tx_tones_changed();
    tx_tone_try(SOUND_TX_TONE_BEEP, sound_tx_tones.local_volume);
}

static void tail_format(char *buffer, size_t size)
{
    static const text_id_t tails[SOUND_TAILS] = {TEXT_OFF, TEXT_TAIL_PHASE, TEXT_TAIL_55HZ};

    format_text((sound_tx_tones.tail < SOUND_TAILS) ? tails[sound_tx_tones.tail] : TEXT_OFF, buffer, size);
}

static void tail_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    sound_tx_tones.tail = (uint8_t)CLAMP(sound_tx_tones.tail + direction, SOUND_TAIL_OFF, SOUND_TAILS - 1);
    sound_tx_tones_changed();
}

static const ui_custom_t tail = {.format = tail_format, .change = tail_change};

// The MDC1200 PTT ID: off, at the start, at the end, both (the DMR beep's texts)
static void mdc1200_format(char *buffer, size_t size)
{
    static const text_id_t modes[SOUND_MDC1200_MODES] = {TEXT_OFF, TEXT_START, TEXT_STOP, TEXT_BOTH};

    format_text((sound_tx_tones.mdc1200 < SOUND_MDC1200_MODES) ? modes[sound_tx_tones.mdc1200] : TEXT_OFF, buffer,
                size);
}

static void mdc1200_change(int direction, uint8_t modifiers)
{
    (void)modifiers;
    sound_tx_tones.mdc1200 =
        (uint8_t)CLAMP(sound_tx_tones.mdc1200 + direction, SOUND_MDC1200_OFF, SOUND_MDC1200_MODES - 1);
    sound_tx_tones_changed();
}

static const ui_custom_t mdc1200 = {.format = mdc1200_format, .change = mdc1200_change};
static const ui_custom_t air_start = {.format = air_start_format, .change = air_start_change};
static const ui_custom_t air_end = {.format = air_end_format, .change = air_end_change};
static const ui_custom_t air_level = {.format = air_level_format, .change = air_level_change};
static const ui_custom_t local_start = {.format = local_start_format, .change = local_start_change};
static const ui_custom_t local_end = {.format = local_end_format, .change = local_end_change};
static const ui_custom_t local_volume = {.format = local_volume_format, .change = local_volume_change};

static const ui_item_t tx_tones_items[] = {
    {TEXT_AIR_START, ICONS_COUNT, UI_ITEM_CUSTOM, &air_start, NULL},
    {TEXT_AIR_END, ICONS_COUNT, UI_ITEM_CUSTOM, &air_end, NULL},
    {TEXT_AIR_LEVEL, ICONS_COUNT, UI_ITEM_CUSTOM, &air_level, NULL},
    {TEXT_LOCAL_START, ICONS_COUNT, UI_ITEM_CUSTOM, &local_start, NULL},
    {TEXT_LOCAL_END, ICONS_COUNT, UI_ITEM_CUSTOM, &local_end, NULL},
    {TEXT_LOCAL_VOLUME, ICONS_COUNT, UI_ITEM_CUSTOM, &local_volume, NULL},
    {TEXT_MDC1200, ICONS_COUNT, UI_ITEM_CUSTOM, &mdc1200, NULL},
    {TEXT_TAIL, ICONS_COUNT, UI_ITEM_CUSTOM, &tail, NULL},
};

static const ui_list_t tx_tones = {
    .title = TEXT_FM_TX_TONES,
    .items = tx_tones_items,
    .count = sizeof(tx_tones_items) / sizeof(tx_tones_items[0]),
    .leave = sound_tx_tones_save,
};

static const ui_item_t sound_options_items[] = {
    {TEXT_TIMEOUT_BEEP, ICONS_COUNT, UI_ITEM_CUSTOM, &timeout_beep, NULL},
    {TEXT_BEEP_VOLUME, ICONS_COUNT, UI_ITEM_CUSTOM, &beep_volume, NULL},
    {TEXT_DMR_BEEP, ICONS_COUNT, UI_ITEM_CUSTOM, &dmr_beep, NULL},
    {TEXT_RX_BEEP, ICONS_COUNT, UI_ITEM_CUSTOM, &rx_beep_item, NULL},
    {TEXT_TALKER_BEEP, ICONS_COUNT, UI_ITEM_CUSTOM, &talker_beep, NULL},
    {TEXT_FM_TX_TONES, ICONS_COUNT, UI_ITEM_LIST, &tx_tones, NULL},
    {TEXT_DMR_MIC_GAIN, ICONS_COUNT, UI_ITEM_NUMBER, &mic_gain_dmr, NULL},
    {TEXT_FM_MIC_GAIN, ICONS_COUNT, UI_ITEM_NUMBER, &mic_gain_fm, NULL},
    {TEXT_VOX_THRESHOLD, ICONS_COUNT, UI_ITEM_CUSTOM, &vox_threshold, NULL},
    {TEXT_VOX_TAIL, ICONS_COUNT, UI_ITEM_CUSTOM, &vox_tail, NULL},
    {TEXT_AUDIO_PROMPT, ICONS_COUNT, UI_ITEM_CUSTOM, &prompt, NULL},
    {TEXT_DMR_RX_AGC, ICONS_COUNT, UI_ITEM_CUSTOM, &dmr_rx_agc, NULL},
};

const ui_list_t sound_options = {
    .title = TEXT_SOUND_OPTIONS,
    .items = sound_options_items,
    .count = sizeof(sound_options_items) / sizeof(sound_options_items[0]),
};
