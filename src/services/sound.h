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

#ifndef DM32_SOUND_H
#define DM32_SOUND_H

#include <stdbool.h>
#include <stdint.h>

// Beeps and melodies on the speaker (sound.c): (Hz, ms) pairs, 0 Hz a pause, ended by -1, -1
extern const int16_t SOUND_POWER_ON[];
extern const int16_t SOUND_KEY[];
extern const int16_t SOUND_KEY_LONG[];
extern const int16_t SOUND_KEY_FIRST_ITEM[];
extern const int16_t SOUND_ACK[];
extern const int16_t SOUND_NACK[];
extern const int16_t SOUND_ERROR[];
extern const int16_t SOUND_TX_TIMEOUT[];
extern const int16_t SOUND_LOW_BATTERY[];
extern const int16_t SOUND_APO_TRIGGERED[];
extern const int16_t SOUND_RX_BEGIN[];
extern const int16_t SOUND_RX_END[];
extern const int16_t SOUND_RX_TALKER_BEGIN[]; // DMR: a caller identified
extern const int16_t SOUND_RX_TALKER_END[];
extern const int16_t SOUND_DMR_TX_START[];
extern const int16_t SOUND_DMR_TX_STOP[];
extern const int16_t SOUND_1750[]; // heard while sending 1750 Hz (stopped with sound_play(NULL))

// settings.audio_prompt_mode (the main firmware's AUDIO_PROMPT_MODE_x)
enum {
    SOUND_PROMPT_SILENT,
    SOUND_PROMPT_BEEP,
    SOUND_PROMPT_NO_KEY_BEEP,
};

// The FM transmit start and end tones (our own, the main firmware has none in FM): on the air (the FD6818's tone
// generator, radio.c) and on our speaker only (the prompt path), each off or one of the patterns; the air level and the
// local volume their own. Stored as a custom data block of the codeplug (SOUND_TX_TONES_DATA_TYPE).
enum {
    SOUND_TX_TONE_OFF,
    SOUND_TX_TONE_BEEP,
    SOUND_TX_TONE_DOUBLE,
    SOUND_TX_TONE_RISING,
    SOUND_TX_TONE_FALLING,
    SOUND_TX_TONE_MORSE_K,
    SOUND_TX_TONE_PATTERNS
};

#define SOUND_TX_TONE_LEVEL_MAX 10 // air_level 1 .. 10

enum {
    SOUND_MDC1200_OFF,
    SOUND_MDC1200_START,
    SOUND_MDC1200_END,
    SOUND_MDC1200_BOTH,
    SOUND_MDC1200_MODES
};

enum {
    SOUND_TAIL_OFF,
    SOUND_TAIL_PHASE, // CTCSS shifted 240 degrees (DCS: 134.4 Hz)
    SOUND_TAIL_55HZ,  // CTCSS to 55 Hz (DCS: 134.4 Hz)
    SOUND_TAILS
};

typedef struct {
    uint8_t air_start; // SOUND_TX_TONE_x
    uint8_t air_end;
    uint8_t local_start;
    uint8_t local_end;
    uint8_t air_level;    // 1 .. SOUND_TX_TONE_LEVEL_MAX
    uint8_t local_volume; // as settings.beep_volume_divider
    uint8_t tail;         // SOUND_TAIL_x: the CTCSS / DCS tail at the end (the stock firmware's)
    uint8_t mdc1200;      // SOUND_MDC1200_x: the PTT ID (the unit ID: the last 4 digits of the DMR ID)
} sound_tx_tones_t;

extern sound_tx_tones_t sound_tx_tones;

const int16_t *sound_tx_tone(uint8_t pattern); // NULL: off
void sound_tx_tones_changed(void);             // sound_tx_tones changed: sound_tx_tones_save() writes them
void sound_tx_tones_save(void);                // when changed

void sound_start(void);
void sound_play(const int16_t *melody); // replaces what plays; NULL stops; key beeps not in the no key beep mode
void sound_play_volume(const int16_t *melody, uint8_t volume_divider); // at that volume
void sound_play_dtmf(uint16_t low, uint16_t high); // the DTMF pair, heard while sending it (sound_play(NULL) stops)
void sound_play_boot(void); // the boot melody, with the boot screen option
bool sound_is_playing(void);

#endif
