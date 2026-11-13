/*
 * Copyright (C) 2019      Kai Ludwig, DG4KLU
 * Copyright (C) 2019-2024 Roger Clark, VK3KYY / G4KYF
 * Copyright (C) 2026      Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, DMR: the audio of the calls. From firmware/dh4570_IAP functions/sound.c (the wav buffers,
 * soundRefillData(), soundSendData(), soundTickRXBuffer(), soundReceiveData(), soundReceiveMicSamples()),
 * interfaces/i2s.c (I2STerminateTransfers()) and user/main.c (the PCM interrupts 59 and 60), DM-32 branches.
 *
 * The codec decodes an AMBE frame into 6 wav buffers of 80 samples (8 kHz, 16 bits). Once 12 are there (one frame of
 * buffering) the playback starts: a buffer into SOUNDBUFRX, AUDIO_CONTROL bit 5 on; the modem asks for the next one with
 * the PCM read interrupt (vector 59). While transmitting, the PCM write interrupt (vector 60) gives the mic's samples.
 * The receive AGC (settings.dmr_rx_agc) is the one of the MD-UV380 code of the main firmware (its DM-32 branch computed
 * the gain but no longer applied it), in integers: it runs in the PCM interrupt.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "settings.h"
#include "dmr_compat.h"
#include "p25_phy.h"
#include "p25_radio.h"

#define WAV_BUFFER_AMBE_PREBUFFERING_COUNT 12 // 2 DMR frames, 6 buffers each
// the buffers before the playback starts: DMR's above; P25 sets its own while it receives (p25_radio.c)
volatile int sound_rx_prebuffer = WAV_BUFFER_AMBE_PREBUFFERING_COUNT;
#define IRQ_PCM_READ                       (59 - 32)
#define IRQ_PCM_WRITE                      (60 - 32)

audio_and_hotspot_buffer_t audioAndHotspotDataBuffer;
volatile int16_t wavbuffer_read_idx = 0;
volatile int16_t wavbuffer_write_idx = 0;
volatile int16_t wavbuffer_count = 0;
volatile bool isReceiving = false; // capturing the mic
uint8_t *currentWaveBuffer;        // the codec's input or output (codec_interface.c)

// The receive AGC (sound.c soundRefillData()): the gain makes the average peak of the samples the default one, times
// the setting's factor, at most 32; the average of the peaks over 100 buffers, of the buffers louder than a kerchunk
#define AGC_DEFAULT_PEAK  2000
#define AGC_WINDOW        100
#define AGC_KERCHUNK      200
#define AGC_EVERY         16  // buffers between two gain updates
#define AGC_GAIN_MAX      32
#define AGC_GAIN_ONE      256 // the gain in 1 / 256
#define AGC_SETTINGS      8
static const uint8_t agc_factors[AGC_SETTINGS] = {1, 2, 4, 8, 16, 32, 64, 128};
static int32_t agc_peak_average = AGC_DEFAULT_PEAK * AGC_WINDOW; // x AGC_WINDOW
static int32_t agc_gain = AGC_GAIN_ONE;

// The mic's level while transmitting, read over the cable afterwards (tools/debug/miclevel.py: the cable's plug
// switches the radio's own mic off): per 10 ms buffer the smallest and largest sample and the variance around the
// buffer's mean (the mic has a DC of about 2470) / 16, the last MIC_LEVELS buffers of the last transmission
#define MIC_LEVELS 1024
typedef struct {
    int16_t min;
    int16_t max;
    uint32_t variance;
} mic_level_t;
mic_level_t mic_levels[MIC_LEVELS];
volatile uint32_t mic_level_count; // buffers of the last transmission, the next one at mic_level_count % MIC_LEVELS

static volatile bool playing = false; // g_TX_SAI_in_use
static volatile bool sending = false; // isSending

void soundInit(void)
{
    wavbuffer_read_idx = 0;
    wavbuffer_write_idx = 0;
    wavbuffer_count = 0;
}

// I2STerminateTransfers()
static void RAM_CODE terminate(void)
{
    *AUDIO_CONTROL &= BITCLR(5);
    *INTERP_CLEAR |= BITSET(5);
    sending = false;
    isReceiving = false;
    playing = false;
    soundInit();
}

// Transmitting: the receive paths (a terminator from the repeater, RX timeouts) call this too; stopping the mic
// capture there made a whole repeater transmission silent. Only the playback stops.
void RAM_CODE soundTerminateSound(void)
{
    if (trxTransmissionEnabled) {
        *AUDIO_CONTROL &= BITCLR(5);
        *INTERP_CLEAR |= BITSET(5);
        playing = false;
        sending = false;
        return;
    }
    terminate();
}

void soundSetupBuffer(void)
{
    currentWaveBuffer = (uint8_t *)audioAndHotspotDataBuffer.wavbuffer[wavbuffer_write_idx];
}

void soundStoreBuffer(void)
{
    CPU_SR_ALLOC();

    CPU_CRITICAL_ENTER();
    if (wavbuffer_count < WAV_BUFFER_COUNT) {
        wavbuffer_write_idx = (int16_t)((wavbuffer_write_idx + 1) % WAV_BUFFER_COUNT);
        wavbuffer_count++;
    }
    CPU_CRITICAL_EXIT();
}

void soundRetrieveBuffer(void)
{
    CPU_SR_ALLOC();

    CPU_CRITICAL_ENTER();
    if (wavbuffer_count > 0) {
        currentWaveBuffer = (uint8_t *)audioAndHotspotDataBuffer.wavbuffer[wavbuffer_read_idx];
        wavbuffer_read_idx = (int16_t)((wavbuffer_read_idx + 1) % WAV_BUFFER_COUNT);
        wavbuffer_count--;
    }
    CPU_CRITICAL_EXIT();
}

// The next buffer to the modem's playback (with the AGC's gain when it is on); false: none left
static bool RAM_CODE refill(void)
{
    volatile uint8_t *buffer;
    int32_t peak = 0;
    uint8_t agc = settings.dmr_rx_agc;

    if (wavbuffer_count <= 0) {
        agc_peak_average = AGC_DEFAULT_PEAK * AGC_WINDOW;
        return false;
    }
    if ((agc != 0) && (agc <= AGC_SETTINGS) && ((wavbuffer_read_idx % AGC_EVERY) == 0) && (agc_peak_average > 0)) {
        agc_gain = ((AGC_DEFAULT_PEAK * AGC_WINDOW * AGC_GAIN_ONE) / agc_peak_average) * agc_factors[agc - 1];
        if (agc_gain > (AGC_GAIN_MAX * AGC_GAIN_ONE)) {
            agc_gain = AGC_GAIN_MAX * AGC_GAIN_ONE;
        }
    }
    buffer = audioAndHotspotDataBuffer.wavbuffer[wavbuffer_read_idx];
    for (int i = 0; i < WAV_BUFFER_SIZE; i += 2) {
        int32_t sample = (int16_t)(buffer[i] | (buffer[i + 1] << 8));
        int32_t magnitude = (sample < 0) ? -sample : sample;

        if (magnitude > peak) {
            peak = magnitude;
        }
        if (agc != 0) {
            sample = (sample * agc_gain) / AGC_GAIN_ONE;
            sample = (sample > 32767) ? 32767 : ((sample < -32768) ? -32768 : sample);
        }
        *(SOUNDBUFRX + i) = (uint8_t)sample;
        *(SOUNDBUFRX + i + 1) = (uint8_t)((uint16_t)sample >> 8);
    }
    if (peak > AGC_KERCHUNK) {
        agc_peak_average += peak - (agc_peak_average / AGC_WINDOW);
    }
    wavbuffer_read_idx = (int16_t)((wavbuffer_read_idx + 1) % WAV_BUFFER_COUNT);
    wavbuffer_count--;
    return (wavbuffer_count > 0);
}

// Receiving: the playback started (radioSetAudioPath(false): the FM audio path off, the modem in DMR)
static void send(void)
{
    if (!playing) {
        *AUDIO_CONTROL &= BITCLR(6);
        if (!p25_phy_active()) {
            *WORK_MODE = 0x6E;
        } else if (*WORK_MODE != p25_phy_work_mode) {
            *WORK_MODE = p25_phy_work_mode; // P25: the physical layer receive goes on (p25_phy.c)
        }
        playing = refill();
        if (playing) {
            p25_ev_log(20, 0); // the delay log (p25_radio.c)
            *INTERP_CLEAR |= BITSET(5);
            *AUDIO_CONTROL |= BITSET(5);
        }
    }
}

// From the engine's task: one frame of buffering, then the playback
void soundTickRXBuffer(void)
{
    if ((wavbuffer_count >= sound_rx_prebuffer) && !trxTransmissionEnabled) {
        send();
    }
}

// The start of a transmission: the mic capture (PCM interrupt 60), as the releases (0x3046C68). The playback bit (5),
// set for a received call, stays set after it ends: still set at the next transmission, the samples read at
// SOUNDBUFTX were the playback's and every transmission after the first was silent. The playback is stopped first.
bool soundReceiveData(void)
{
    if (!trxTransmissionEnabled) {
        return false;
    }
    sending = false;
    isReceiving = true;
    *AUDIO_CONTROL &= BITCLR(5);
    *INTERP_CLEAR |= BITSET(5);
    playing = false;
    mic_level_count = 0;
    *AUDIO_CONTROL |= BITSET(4);
    return true;
}

// The end of a transmission (DMR_STATE_TX_END_1, the voice sent): the mic capture off, as at power on. Left on, its
// buffers counted as received audio once there were enough (soundTickRXBuffer()): the mic played on the speaker,
// with the step it has at the PTT release, a buzz (the mic stays on the modem in DMR, radio.c apply_channel())
void RAM_CODE soundStopMicCapture(void)
{
    *AUDIO_CONTROL &= ~BITSET(4);
    *INTERP_CLEAR |= BITSET(4);
    terminate();
}

// P25 (p25_radio.c): the mic capture on its own, without the DMR engine: the playback off, the buffers emptied, the
// PCM write interrupt filling them; sound_mic_take() takes them out (80 samples each, 10 ms)
void sound_mic_start(void)
{
    terminate();
    mic_level_count = 0;
    isReceiving = true;
    *INTERP_CLEAR |= BITSET(4);
    *AUDIO_CONTROL |= BITSET(4);
}

bool sound_mic_take(int16_t out[WAV_BUFFER_SIZE / 2])
{
    CPU_SR_ALLOC();
    bool have = false;

    CPU_CRITICAL_ENTER();
    if (wavbuffer_count > 0) {
        volatile uint8_t *b = audioAndHotspotDataBuffer.wavbuffer[wavbuffer_read_idx];
        for (int i = 0; i < WAV_BUFFER_SIZE / 2; i++) {
            out[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
        }
        wavbuffer_read_idx = (int16_t)((wavbuffer_read_idx + 1) % WAV_BUFFER_COUNT);
        wavbuffer_count--;
        have = true;
    }
    CPU_CRITICAL_EXIT();
    return have;
}

// PCM read (vector 59): the modem wants the next buffer to play
static void RAM_CODE pcm_read_isr(void)
{
    playing = refill();
    *INTERP_CLEAR |= BITSET(5);
}

// PCM write (vector 60): while transmitting, the 80 mic samples the modem left at SOUNDBUFTX into the next wav buffer
// for the AMBE encoder (as the releases, 0x135C4)
static void RAM_CODE pcm_write_isr(void)
{
    if (isReceiving && (wavbuffer_count < (WAV_BUFFER_COUNT - 1))) {
        volatile uint8_t *source = (volatile uint8_t *)SOUNDBUFTX;
        volatile uint8_t *destination = audioAndHotspotDataBuffer.wavbuffer[wavbuffer_write_idx];
        mic_level_t *level = &mic_levels[mic_level_count % MIC_LEVELS];
        int16_t first = (int16_t)(source[0] | (source[1] << 8)); // the samples from the first: no DC in the sums
        int32_t sum = 0;
        uint32_t squares = 0; // / 16, held at the largest value (only an RMS near full scale gets there)

        level->min = 32767;
        level->max = -32768;
        for (int i = 0; i < WAV_BUFFER_SIZE; i += 2) {
            int16_t sample = (int16_t)(source[i] | (source[i + 1] << 8));
            int32_t d = sample - first;
            uint32_t square = ((uint32_t)d * (uint32_t)d) >> 4;

            destination[i] = source[i];
            destination[i + 1] = source[i + 1];
            sum += d;
            squares = (squares > UINT32_MAX - square) ? UINT32_MAX : (squares + square);
            level->min = (sample < level->min) ? sample : level->min;
            level->max = (sample > level->max) ? sample : level->max;
        }
        int32_t mean = sum / (WAV_BUFFER_SIZE / 2);
        uint32_t mean_square = ((uint32_t)mean * (uint32_t)mean) >> 4;
        uint32_t square = squares / (WAV_BUFFER_SIZE / 2);
        level->variance = (square > mean_square) ? (square - mean_square) : 0;
        mic_level_count++;
        wavbuffer_write_idx = (int16_t)((wavbuffer_write_idx + 1) % WAV_BUFFER_COUNT);
        wavbuffer_count++;
    }
    *INTERP_CLEAR |= BITSET(4);
}

// The capture and the playback off as at power on: a restart through the bootloader (fw_update.py, the CPS write)
// leaves the modem as it was, AUDIO_CONTROL bit 4 still set by the last transmission, and soundReceiveData() setting
// it again then started no capture (not one PCM write interrupt: transmissions without voice until a power cycle)
void dmr_sound_init(void)
{
    *AUDIO_CONTROL &= ~(BITSET(4) | BITSET(5));
    *INTERP_CLEAR |= BITSET(4) | BITSET(5);
    irq_register(IRQ_PCM_READ, pcm_read_isr);
    irq_register(IRQ_PCM_WRITE, pcm_write_isr);
}
