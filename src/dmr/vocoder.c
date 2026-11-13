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

// Our own AMBE+2 vocoder (vocoder/c/ambe, built from there by the Makefile): its code, hot tables and variables run
// from the RAM at 0x44400 (ckcpu.ld .vocoder_overlay), copied from the flash at the HR_C7000 task's first codec call
// (vocoder_sync()). The stock codec is not supported.
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "vocoder.h"
#include "ambe.h"
#include "imbe.h"
#include "rx_capture.h"

extern uint8_t __vocoder_ram_start[], __vocoder_ram_end[], __vocoder_flash_start[];
extern uint8_t __p25_ram_start[], __p25_ram_end[], __p25_flash_start[];

static uint8_t overlay = OVERLAY_NONE; // which one is in the overlay RAM (ckcpu.ld)
#define loaded (overlay == OVERLAY_AMBE) // the vocoder is in the RAM

// The encoder's profile (vocoder/c/ambe/ambe_enc.c PROF): APB clocks (84 MHz) a step, summed; read over the cable
uint32_t ambe_prof[AMBE_PROF_N];

uint32_t ambe_now(void)
{
    TickType_t t;
    uint32_t c;
    do {
        t = xTaskGetTickCount();
        c = TIMER_CURRENT(0); // counts down from TIMER_LOADCOUNT(0) a tick
    } while (t != xTaskGetTickCount());
    return t * TIMER_LOADCOUNT(0) + (TIMER_LOADCOUNT(0) - c);
}

// Our decoder's and encoder's state: in the overlay, zeroed by every load
static ambe_decoder decoder __attribute__((section(".vocoder_bss")));
static ambe_encoder encoder __attribute__((section(".vocoder_bss")));
static uint8_t decoder_ready __attribute__((section(".vocoder_bss")));
static uint8_t encoder_ready __attribute__((section(".vocoder_bss")));

// The P25 encoder for the tests over the cable (imbe_enc_test, vocoder_bench 5): in the P25 overlay
static imbe_encoder imbe_encoder_test __attribute__((section(".p25_bss")));
uint8_t imbe_enc_ready __attribute__((section(".p25_bss"))); // 0: the next frame starts a fresh encoder

// one frame of 160 samples -> u88 (88 parameter bits); 1 a frame out, 0 not yet (the encoder's delay), 0xFFFFFFFF
// without the P25 overlay (the radio on a P25 channel)
uint32_t imbe_enc_test(const int16_t *pcm, uint8_t *u88)
{
    if (overlay != OVERLAY_P25) {
        return 0xFFFFFFFF;
    }
    if (!imbe_enc_ready) {
        imbe_enc_init(&imbe_encoder_test);
        imbe_enc_ready = 1;
    }
    return (uint32_t)imbe_enc_frame(&imbe_encoder_test, pcm, u88);
}

void vocoder_load(void)
{
    memcpy(__vocoder_ram_start, __vocoder_flash_start, (size_t)(__vocoder_ram_end - __vocoder_ram_start));
    overlay = OVERLAY_AMBE;
}

void vocoder_sync(void)
{
    if (!loaded) {
        vocoder_load();
    }
}

void overlay_use(int which)
{
    if (overlay == which) {
        return;
    }
    if (which == OVERLAY_P25) {
        memcpy(__p25_ram_start, __p25_flash_start, (size_t)(__p25_ram_end - __p25_ram_start));
        overlay = OVERLAY_P25;
    } else if (which == OVERLAY_AMBE) {
        vocoder_load();
    }
}

void vocoderDecodeFrame(const uint8_t *in9, int16_t *pcm)
{
    if (!decoder_ready) {
        ambe_dec_init(&decoder);
        decoder_ready = 1;
    }
    rx_capture_frame(in9, ambe_dec_frame(&decoder, in9, pcm)); // debug: the frames to the flash (rx_capture.h)
}

void vocoderEncodeFrame(const int16_t *pcm, uint8_t *out9)
{
    if (!encoder_ready) {
        ambe_enc_init(&encoder);
        encoder_ready = 1;
    }
    ambe_enc_frame(&encoder, pcm, out9);
}

void vocoderReset(void)
{
    if (loaded) { // else its variables are not there yet: the load starts it afresh
        decoder_ready = 0;
        encoder_ready = 0;
    }
    rx_capture_new_call();
}

uint32_t vocoder_bench(uint32_t what_n, const uint8_t *data)
{
    uint32_t what = what_n >> 16, n = what_n & 0xFFFF;
    TickType_t t0 = xTaskGetTickCount();
    if (what == 0 || what == 1) {
        volatile float a = 1.0001f, b = 0.9999f;
        float acc = 1.0f;
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 1000; k++) {
                acc = what == 0 ? acc * a : acc + b;
            }
        }
        a = acc;
    } else if (what == 2) {
        volatile int32_t a = 3, b = 5;
        int32_t acc = 0, x = a, y = b;
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 1000; k++) {
                acc += x * y;
                x += 1;
            }
        }
        a = acc;
    } else if ((what == 3 || what == 4) && loaded) { // our code is in the RAM (after the first DMR call)
        static int16_t pcm[AMBE_N];
        uint8_t out[9];
        for (uint32_t i = 0; i < n; i++) {
            if (what == 3) {
                vocoderDecodeFrame(data + 9 * i, pcm);
            } else { // data: 4 frames of PCM, used over and over
                vocoderEncodeFrame((const int16_t *)data + AMBE_N * (i % 4), out);
            }
        }
    } else if (what == 5 && overlay == OVERLAY_P25) { // the P25 encoder; data: 4 frames of PCM
        uint8_t u[88];
        for (uint32_t i = 0; i < n; i++) {
            imbe_enc_test((const int16_t *)data + IMBE_N * (i % 4), u);
        }
    } else {
        return 0xFFFFFFFF;
    }
    return (uint32_t)(xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
}

