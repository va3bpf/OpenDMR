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
 * OpenDMR, service: the prompt FIFO's one writer (voice_play.h): the software receivers' decoded voice (NXDN, M17, DMR)
 * and the beeps (sound.c) mixed into it; the audio path and the amplifier switched here only.
 *
 * The task, every tick (1 ms): off until a source wants the speaker: a call begun (voice_play_prepare()) or enough
 * frames queued (start frames, start_ms after the call's first: they come with a jitter of the recording's 16 ms
 * quarters), or a beep (voice_play_beep_begin()). Then the speaker on as a beep always turned it on: the audio switch
 * to the baseband and the prompt path, silence for PATH_SETTLE_MS, the amplifier (the voice's with the green LED, a
 * beep's without), silence for AMP_START_MS while it starts. Then one block (16 samples: twice real time) a tick while
 * the FIFO has room: the voice's next 16 samples (silence before they are enough, or when they ran out) plus the
 * beep's; one source alone goes as it is. One block only: the FIFO's full flag comes late, several at once overran it
 * and samples were lost (heard as the voice shaking, as pops; NXDN and M17 both, 2026-11). No source left (the call over
 * and played out, the beep done): the amplifier off first, AMP_STOP_MS, then the path back. All at once (path and
 * amplifier together) the amplifier caught the switching: a pop at each call's start (2026-11-20).
 *
 * The beeps wrote into the FIFO themselves before and switched the path on and off on their own: while a DMR, NXDN or
 * M17 call played, a key beep broke the voice up and switched the path off at its end, the rest of the call silent
 * (#107, 2026-11-21).
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "leds.h"
#include "modem.h"
#include "p25_radio.h"
#include "radio_hooks.h"
#include "voice_play.h"

#define PATH_SETTLE_MS 10 // the audio path switched, silence into the FIFO, before the amplifier goes on
#define AMP_START_MS   75 // the amplifier on before the sound (DELAYED_BEEP_DURATION_MS)
#define AMP_STOP_MS    20 // the amplifier off, before the audio path switches back
#define BLOCK          (MODEM_BEEP_BLOCK / 2) // samples a block

enum { OFF, PATH, AMP, PLAY, STOP }; // the task's states

static StaticTask_t feed_tcb;
static StackType_t feed_stack[192];
static TaskHandle_t feeder;

// the voice: a receiver's queue
static voice_play_cfg_t cfg;
static volatile bool enabled;             // a receiver's playback allowed
static volatile bool wanted;              // enough queued: play (until the call's end)
static volatile bool call_on;             // a call begun (voice_play_prepare()): the speaker on, waiting for its voice
static volatile bool have_first;          // the call's first frame queued, at first_at
static volatile TickType_t first_at;
static volatile uint32_t pcm_in, pcm_out; // frames put in / taken out (the task's)
static uint32_t pcm_pos;                  // samples of frames[pcm_out] already in the FIFO (the task's)
static bool voice_out;                    // the voice playing: its queue played out after the call's end
static bool in_underrun;                  // the delay log: an underrun going on
static bool first_block;                  // the delay log (p25_radio.c): the first voice block not yet written

// a beep (sound.c): on from voice_play_beep_begin() to _end(), one block of it at a time in the slot
static volatile bool beep_on, beep_full;
static int16_t beep[BLOCK];

static volatile uint32_t loops; // the task's rounds (voice_play_stop() waits for them)
static volatile int state;      // OFF ... STOP
static TickType_t since;
static bool voice_amp, beep_amp;

// the audio switch and the prompt path
static void path(bool on)
{
    if (on) {
        radio_beep_path(true);
        modem_beep(true);
    } else {
        modem_beep(false);
        radio_beep_path(false);
    }
}

// the amplifier: the voice's (with the LED) and a beep's, either keeps it on
static void amps(bool voice, bool b)
{
    if (voice != voice_amp) {
        voice_amp = voice;
        radio_dmr_audio(voice);
        leds_green(voice);
    }
    if (b != beep_amp) {
        beep_amp = b;
        radio_beep_amp(b);
    }
}

// a new state from now
static void go(int next)
{
    state = next;
    since = xTaskGetTickCount();
}

static bool waited(uint32_t ms)
{
    return (xTaskGetTickCount() - since) >= pdMS_TO_TICKS(ms);
}

static void count(volatile uint32_t *counter)
{
    if (counter) {
        (*counter)++;
    }
}

// the voice's next 16 samples into x; false: none now (silence)
static bool voice_block(int16_t *x)
{
    if (!enabled || !voice_out) {
        return false;
    }
    if (pcm_in == pcm_out) {
        if (wanted) { // out of voice, the call going on
            if (!in_underrun) {
                in_underrun = true;
                p25_ev_log(53, (uint16_t)pcm_out); // the delay log (p25_radio.c): out of voice (arg: frames played)
            }
            count(cfg.underruns);
        }
        return false;
    }
    if (first_block) {
        first_block = false;
        p25_ev_log(52, (uint16_t)(pcm_in - pcm_out)); // the first voice to the speaker (arg: queued)
    }
    in_underrun = false;
    const int16_t *frame = &cfg.frames[pcm_out % cfg.count][pcm_pos];
    memcpy(x, frame, sizeof(int16_t) * BLOCK);
    if (cfg.tap) {
        cfg.tap(frame, BLOCK);
    }
    count(cfg.writes);
    pcm_pos += BLOCK;
    if (pcm_pos >= VOICE_PLAY_SAMPLES) {
        pcm_pos = 0;
        pcm_out++;
    }
    return true;
}

// a block into the FIFO if it has room: silence, or (playing) the voice plus the beep
static void write_block(bool play)
{
    if (modem_beep_fifo_full()) {
        return;
    }
    int16_t x[BLOCK];
    bool voice = play && voice_block(x);
    bool b = play && beep_full;
    uint8_t block[MODEM_BEEP_BLOCK];
    for (int i = 0; i < BLOCK; i++) {
        int32_t v = (voice ? x[i] : 0) + (b ? beep[i] : 0);
        int16_t s = (int16_t)((v > 32767) ? 32767 : (v < -32768) ? -32768 : v);
        block[2 * i] = (uint8_t)s; // low byte first
        block[2 * i + 1] = (uint8_t)((uint16_t)s >> 8);
    }
    if (b) {
        beep_full = false;
    }
    modem_beep_write(block);
}

static void feed_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(1);
        loops++;
        if (enabled && !wanted && have_first && ((pcm_in - pcm_out) >= cfg.start) &&
            ((xTaskGetTickCount() - first_at) >= pdMS_TO_TICKS(cfg.start_ms))) {
            p25_ev_log(50, (uint16_t)(pcm_in - pcm_out)); // the delay log (p25_radio.c): enough queued
            wanted = true;
        }
        if (!enabled) {
            voice_out = false;
        }
        // the sources wanting the speaker: the voice (a call on, its queue to play out), a beep
        bool want_voice = enabled && (wanted || call_on || (voice_out && (pcm_in != pcm_out)));
        bool want_beep = beep_on || beep_full;
        switch (state) {
        case OFF:
            if (want_voice || want_beep) {
                path(true);
                go(PATH);
            }
            break;
        case PATH: // the path settling
            write_block(false);
            if (waited(PATH_SETTLE_MS)) {
                p25_ev_log(51, (uint16_t)(pcm_in - pcm_out)); // the amplifier on (arg: frames queued)
                amps(want_voice, want_beep);
                go(AMP);
            }
            break;
        case AMP: // the amplifier starting
            write_block(false);
            if (waited(AMP_START_MS)) {
                go(PLAY);
            }
            break;
        case PLAY:
            if (!want_voice && !want_beep) { // all done: the amplifier off, then the path back
                amps(false, false);
                go(STOP);
                break;
            }
            if (!want_voice) { // played out (a beep goes on): the next call's voice waits for enough again
                voice_out = false;
            } else if (wanted && !voice_out) { // the voice begins (the amplifier on already)
                voice_out = true;
                first_block = true;
            }
            amps(want_voice, want_beep);
            write_block(true);
            break;
        default: // STOP: the amplifier off, then the path back
            if (waited(AMP_STOP_MS)) {
                path(false);
                pcm_pos = 0;
                voice_out = false;
                if (!enabled) {
                    pcm_out = pcm_in;
                }
                state = OFF;
            }
            break;
        }
    }
}

static void task_start(void)
{
    if (!feeder) {
        feeder = xTaskCreateStatic(feed_task, "voice", sizeof(feed_stack) / sizeof(feed_stack[0]), NULL,
                                   tskIDLE_PRIORITY + 4, feed_stack, &feed_tcb);
    }
}

void voice_play_start(const voice_play_cfg_t *c)
{
    task_start();
    voice_play_stop();
    cfg = *c;
    pcm_in = pcm_out = 0;
    pcm_pos = 0;
    wanted = false;
    call_on = false;
    have_first = false;
    enabled = true;
}

// The voice off, the task done with the receiver's frames (two of its rounds) before their RAM is another's; a beep
// going on keeps the speaker (the path and the amplifier are the task's)
void voice_play_stop(void)
{
    enabled = false;
    wanted = false;
    call_on = false;
    have_first = false;
    if (feeder) {
        uint32_t n = loops;
        while ((uint32_t)(loops - n) < 2) {
            vTaskDelay(1);
        }
    }
    pcm_out = pcm_in;
}

int16_t *voice_play_frame(void)
{
    if (!enabled || ((pcm_in - pcm_out) >= cfg.count)) {
        return NULL;
    }
    return cfg.frames[pcm_in % cfg.count];
}

void voice_play_prepare(void)
{
    call_on = true;
}

void voice_play_put(void)
{
    pcm_in++;
    if (!have_first) { // the call's first frame: the task starts the playback once enough came (start, start_ms)
        first_at = xTaskGetTickCount();
        have_first = true;
    }
}

void voice_play_end(void)
{
    wanted = false;
    call_on = false;
    have_first = false;
}

/* ---- the beeps (sound.c) ---- */

void voice_play_beep_begin(void)
{
    task_start();
    beep_on = true;
}

bool voice_play_beep_ready(void)
{
    return beep_on && (state == PLAY);
}

bool voice_play_beep_put(const int16_t x[16])
{
    if (beep_full || (state != PLAY)) {
        return false;
    }
    memcpy(beep, x, sizeof(beep));
    beep_full = true;
    return true;
}

void voice_play_beep_end(void)
{
    beep_on = false;
}
