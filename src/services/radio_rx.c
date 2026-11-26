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
 * OpenDMR, service: the radio, receive (radio_internal.h). After OpenDM32 functions/trx.c (trxSetModeAndBandwidth(),
 * trxSetFrequency(), trxActivateRx(), trxCheckAnalogSquelch()) and hardware/radioHardwareInterface.c (radioSetRx(),
 * radioAudioAmp(), radioReadRSSIAndNoiseFromRam()), DM32 branches: the channel set up, the FM / AM / SSB squelch, the
 * DMR signal, the S meter, the receive beeps, the audio switch and amplifier, the scan's readings. The squelch runs
 * from RAM, from when the FM audio went through the baseband (audio switch low) and bus activity was heard in it.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "leds.h"
#include "trx.h"
#include "modem.h"
#include "settings.h"
#include "factory_cal.h"
#include "ui.h"
#include "sound.h"
#include "codeplug.h"
#include "dmr.h"
#include "p25_radio.h"
#include "nxdn_radio.h"
#include "ysf/ysf_radio.h"
#include "m17_radio.h"
#include "dmr_soft.h"
#include "radio_internal.h"

#define TUNE_SETTLE_MS 20 // after a channel change the FD6818 noise reading is low a moment: no squelch opening
#define SCAN_SETTLE_MS 10 // scanning a digital channel: the signal read only from this long after the retune

#define SQUELCH_MAX        70
#define SQUELCH_STEP       3
#define SQUELCH_HYSTERESIS 3
#define SQUELCH_OPEN       256 // over any noise reading: always open
#define RSSI_SQUELCH_MIN_DBM (-128)
#define RSSI_SQUELCH_STEP    3
#define CSS_HOLD           6 // checks a receive tone may be missing before the speaker closes
// AM and SSB: how long the signal stays under the closing level before the squelch closes. The RSSI follows the
// modulation: AM's envelope dips on deep modulation and fades, SSB has no carrier and drops to the noise between words
#define SQUELCH_HOLD_AM_MS  200
#define SQUELCH_HOLD_SSB_MS 500
#define SIGNAL_SAMPLES     10
#define SIGNAL_MOVE        2 // the shown signal follows the average when it moved this much
#define RX_END_BEEP_MS     150 // the carrier end beep waits this long, a new carrier in it cancels both
#define BEEP_RX_CARRIER    0x04 // settings.beep_options

// Receive
// Scanning a digital channel: the strongest reading since the retune (SCAN_SETTLE_MS on), and how many were taken
static volatile uint8_t scan_peak;
static volatile uint16_t scan_samples;
static TickType_t scan_settled_at;
static TickType_t settled_at;           // after a channel change, the squelch may open from then on
static uint8_t css_missing = 0;
static uint8_t signal_low = 0; // AM, SSB: checks in a row the signal was under the closing level
static bool rx_end_beep = false; // the carrier end beep is waiting
static TickType_t rx_end_beep_at;
static volatile uint8_t signal_raw = 0;
static volatile uint8_t noise_raw = 255;
static uint8_t samples[SIGNAL_SAMPLES];
static uint8_t sample_index = 0;
static volatile uint8_t signal_shown = 0;

/* Channel facts */

// FM: transmits, has the noise squelch and the tones
bool is_fm(void)
{
    return rs.has_channel && (rs.channel.mode == CODEPLUG_MODE_FM);
}

// FM, AM, USB, LSB: the FD6818 demodulates, the speaker from its audio output
bool is_analog(void)
{
    return rs.has_channel && codeplug_mode_is_analog(rs.channel.mode);
}

static trx_demod_t demod_of(uint8_t mode)
{
    return (mode == CODEPLUG_MODE_AM) ? TRX_DEMOD_AM
                                      : (codeplug_mode_is_ssb(mode) ? TRX_DEMOD_SSB : TRX_DEMOD_FM);
}

bool is_wide(void)
{
    return is_fm() && ((rs.channel.flag4 & FLAG4_WIDE) != 0);
}

int radio_band_of(uint32_t frequency)
{
    return (frequency >= UHF_FROM) ? 2 : ((frequency >= B220_FROM) ? 1 : 0);
}

// The noise level the squelch opens under (trxCheckAnalogSquelch()); level 1 (0 %): open whatever the noise, as on
// most radios (the noise reading is 8 bits, so it never reaches the level and the hysteresis over it)
static int squelch_level(void)
{
    int level = (rs.channel.sql != 0) ? rs.channel.sql : settings.squelch_defaults[radio_band_of(rs.channel.rx_freq)];

    if (level <= 1) {
        return SQUELCH_OPEN;
    }
    return SQUELCH_MAX - ((level - 1) * SQUELCH_STEP);
}

// A carrier by the noise against the squelch level, without the squelch's side effects (the speaker, the LED, the
// beeps): the beacon's busy check on its own frequency. A fully open squelch (level 1) can't tell: free
bool rx_noise_busy(void)
{
    uint8_t signal;
    uint8_t noise;
    int level = squelch_level();

    trx_read_signal(&signal, &noise);
    return (level < SQUELCH_OPEN) && (noise < level);
}

// AM and SSB: the FM noise reading means nothing there, the squelch is on the signal (raw RSSI, 1 dB a step): level 1
// (0 %) always open, then from -128 dBm up by 3 dB a level (-71 dBm at 100 %). 0: open.
static int RAM_CODE rssi_squelch_level(void)
{
    int level = (rs.channel.sql != 0) ? rs.channel.sql : settings.squelch_defaults[radio_band_of(rs.channel.rx_freq)];

    if (level <= 1) {
        return 0;
    }
    return RSSI_SQUELCH_MIN_DBM + ((level - 2) * RSSI_SQUELCH_STEP) + ((rs.channel.rx_freq >= UHF_FROM) ? 167 : 177);
}

/* Pins */

void lna(bool on, uint32_t frequency)
{
    gpio_output(PIN_LNA_VHF, on && (frequency < UHF_FROM));
    gpio_output(PIN_LNA_UHF, on && (frequency >= UHF_FROM));
}

// The speaker amplifier: on for the received audio or for a beep (sound.c), off when neither needs it. The audio
// switch: high in FM (the speaker from the FD6818), low during a beep (the beeps come from the baseband's DAC; with the
// switch high they don't reach the speaker). The FM broadcast receiver is on the switch's FD6818 side too: on a
// digital channel the switch is high for it (radio_broadcast_amp())
static volatile bool beep_path = false; // the audio switch to the baseband for a beep
static volatile bool audio_sw_fm = false;
static volatile bool audio_sw_broadcast = false;

static void audio_switch(void)
{
    gpio_output(PIN_AUDIO_SW, (audio_sw_fm || audio_sw_broadcast) && !beep_path);
}

static void RAM_CODE audio_amp(bool on)
{
    taskENTER_CRITICAL();
    rs.audio_needs_amp = on;
    gpio_output(PIN_AUDIO_AMP, rs.audio_needs_amp || rs.beep_needs_amp || rs.broadcast_needs_amp);
    taskEXIT_CRITICAL();
}

/* Receive */

void RAM_CODE post_signal(void)
{
    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_SIGNAL};

    ui_post(&ev);
}

void squelch_close(void)
{
    audio_amp(false);
    trx_af_out(false);
    leds_green(false);
    if (rs.has_carrier || rs.speaker_open) {
        rs.has_carrier = false;
        rs.speaker_open = false;
        post_signal();
    }
    css_missing = 0;
    signal_low = 0;
}

// The squelch may open from ms on (the FD6818's noise reading is low a moment after it is set up)
void rx_settle(uint32_t ms)
{
    settled_at = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
}

// Receive on the channel: radioSetRx(), radioSetFrequency(), radioSetIF()
void receive_on(void)
{
    bool wide = is_wide();

    modem_pa_drive(0);
    gpio_output(PIN_PA_VHF, false);
    gpio_output(PIN_PA_UHF, false);
    lna(true, rs.channel.rx_freq);
    trx_receive(wide);
    if (is_analog()) {
        modem_fm_receive(wide, factory_cal_rx_dac_gain());
    }
    trx_set_frequency(rs.channel.rx_freq, wide);
    trx_receive(wide);
    trx_set_rx_tone(is_fm() ? rs.channel.rx_tone : CODEPLUG_CSS_NONE);
}

// The channel asked for (radio_set_channel())
void apply_channel(void)
{
    static codeplug_channel_t next; // not on the radio task's stack
    bool valid;

    taskENTER_CRITICAL();
    memcpy(&next, &rs.requested, sizeof(next));
    valid = rs.has_requested;
    rs.has_new_channel = false;
    taskEXIT_CRITICAL();
    tune_channel(&next, valid);
}

#define RETUNE_ONLY_MAX 5000 // 50 kHz (10 Hz units): farther, the whole set up (another band's LNA, calibration)

// The same channel but its frequencies, a little apart (the satellites' Doppler correction, the VFO tuned)
static bool only_retuned(const codeplug_channel_t *next)
{
    static codeplug_channel_t moved; // not on the radio task's stack
    int32_t apart = (int32_t)(next->rx_freq - rs.channel.rx_freq);

    if ((apart > RETUNE_ONLY_MAX) || (apart < -RETUNE_ONLY_MAX)) {
        return false;
    }
    memcpy(&moved, &rs.channel, sizeof(moved));
    moved.rx_freq = next->rx_freq;
    moved.tx_freq = next->tx_freq;
    return memcmp(&moved, next, sizeof(moved)) == 0;
}

// The radio set up on a channel: the one asked for, or the beacon's frequency for a frame (radio_aprs.c)
void tune_channel(const codeplug_channel_t *next, bool valid)
{
    const factory_cal_t *cal = factory_cal_get();
    bool was_analog = is_analog();

    eco_reset(); // the channel is set up receiving: the power saving starts over

    // An analog channel only retuned: the receiver moved as it is, the squelch and the speaker left as they are (the
    // whole set up below closed the squelch at each step: a gap and a click in a satellite's downlink every few
    // seconds, #104)
    if (valid && rs.has_channel && was_analog && only_retuned(next)) {
        rs.channel.rx_freq = next->rx_freq;
        rs.channel.tx_freq = next->tx_freq;
        lna(true, rs.channel.rx_freq);
        trx_tune(rs.channel.rx_freq);
        return;
    }

    // P25: the modem back to DMR while the channel changes (p25_radio.c), started again below on a P25 channel
    if (rs.p25_running) {
        p25_radio_stop();
        rs.p25_running = false;
    }
    if (rs.nxdn_running) {
        nxdn_radio_stop();
        rs.nxdn_running = false;
    }
    if (rs.ysf_running) {
        ysf_radio_stop();
        rs.ysf_running = false;
    }
    if (rs.m17_running) {
        m17_radio_stop();
        rs.m17_running = false;
    }
    if (rs.dmrs_running) {
        dmr_soft_stop();
        rs.dmrs_running = false;
    }

    if (valid) {
        memcpy(&rs.channel, next, sizeof(rs.channel));
    }
    rs.has_channel = valid;
    rs.tx[RFSM_TX_KIND_VOICE] = tx_backend_of(&rs.channel, rs.has_channel);

    // The FM squelch of the channel left; in DMR the engine has the speaker and the LED, and the FD6818 audio register
    // its DMR setting (the FM value there stops the baseband to the HR_C7000: no DMR sync after a retune)
    if (was_analog) {
        squelch_close();
    }
    settled_at = xTaskGetTickCount() + pdMS_TO_TICKS(TUNE_SETTLE_MS);
    scan_settled_at = xTaskGetTickCount() + pdMS_TO_TICKS(SCAN_SETTLE_MS);
    scan_peak = 0;
    scan_samples = 0;
    // FM as the stock firmware: the speaker from the FD6818's audio output; through the baseband (switch low, as the
    // DMR receive needs it) the CPU, flash and display activity was heard in the FM audio
    taskENTER_CRITICAL();
    audio_sw_fm = is_analog();
    audio_switch();
    taskEXIT_CRITICAL();
    // The mic to the FD6818 for FM (high), to the modem's codec for DMR (low), set with the mode as the stock firmware
    // does: switched at each DMR transmission, it put a DC step and a click (to -32000) into the first 100 ms of the
    // capture, sent as a pop at the start of every transmission
    gpio_output(PIN_MIC_MUT, is_analog());
    memset(samples, 0, sizeof(samples));
    signal_shown = 0;
    if (rs.dmr_running && (!rs.has_channel || is_analog())) {
        dmr_leave();
        rs.dmr_running = false;
    }
    if (!rs.has_channel) {
        lna(false, 0);
        trx_idle();
        vox_setup();
        post_signal();
        return;
    }

    if (is_analog()) {
        // FM set up (trxSetModeAndBandwidth()): the baseband, the FD6818 FM table and level, then the channel; AM and
        // SSB the same, then their demodulator
        modem_fm_receive(is_wide(), factory_cal_rx_dac_gain());
        trx_set_fm(factory_cal_analog_rx_volume());
        if (!is_fm()) {
            trx_set_demod(demod_of(rs.channel.mode));
        }
        modem_set_frequency_calibration(cal->valid, cal->osc_tune, cal->rx_adjust, factory_cal_mod_bias(rs.channel.tx_freq));
        receive_on();
    } else {
        // DMR (trxSetModeAndBandwidth(), trxSetFrequency()): the FD6818 DMR table the first time, the engine stopped
        // while the frequency changes, then started on the channel
        if (rs.dmr_running) {
            dmr_retune();
        } else {
            trx_set_dmr();
        }
        modem_set_frequency_calibration(cal->valid, cal->osc_tune, cal->rx_adjust, factory_cal_mod_bias(rs.channel.tx_freq));
        receive_on();
        if (rs.dmr_running) {
            dmr_channel(&rs.channel);
        } else {
            dmr_enter(&rs.channel);
            rs.dmr_running = true;
        }
        // P25: the DMR engine set the modem up for 4FSK; the P25 receiver takes it over (physical layer receive)
        if (rs.channel.mode == CODEPLUG_MODE_P25) {
            p25_radio_start();
            rs.p25_running = true;
        }
        // NXDN: the DMR engine stands aside too; the modem records the discriminator (FM) for the NXDN receiver
        if (rs.channel.mode == CODEPLUG_MODE_NXDN) {
            nxdn_radio_start();
            rs.nxdn_running = true;
        }
        // YSF Fusion: the discriminator recorded and the C4FM frames found in software, as NXDN (ysf_radio.c)
        if (rs.channel.mode == CODEPLUG_MODE_YSF) {
            ysf_radio_start();
            rs.ysf_running = true;
        }
        // M17: the discriminator recorded and demodulated in software, as NXDN (m17_radio.c)
        if (rs.channel.mode == CODEPLUG_MODE_M17) {
            m17_radio_start();
            rs.m17_running = true;
        }
        // DMR: received in software the same way (dmr_soft.c); the engine stands aside but transmits
        if ((rs.channel.mode == CODEPLUG_MODE_DMR) && dmr_soft_wanted()) {
            dmr_soft_start();
            rs.dmrs_running = true;
        }
    }
    vox_setup();
    post_signal();
}

// The shown signal: the average of the last readings, moved only by SIGNAL_MOVE or more (the stock firmware averages
// too; the raw reading jumps a step or two all the time)
static void RAM_CODE add_sample(uint8_t signal)
{
    uint32_t sum = 0;

    samples[sample_index] = signal;
    sample_index = (uint8_t)((sample_index + 1) % SIGNAL_SAMPLES);
    for (int i = 0; i < SIGNAL_SAMPLES; i++) {
        sum += samples[i];
    }

    int32_t average = (int32_t)(sum / SIGNAL_SAMPLES);
    int32_t moved = average - signal_shown;

    if ((moved >= SIGNAL_MOVE) || (moved <= -SIGNAL_MOVE)) {
        signal_shown = (uint8_t)average;
        post_signal();
    }
}

/* The carrier beeps (applicationMain.c rxBeepsHandler()): the speaker on and off, the end beep a moment later; none
   while scanning (as the main firmware: beeps aren't allowed while scanning) */


void radio_set_scanning(bool on)
{
    rs.is_scanning = on;
}

bool radio_is_scanning(void)
{
    return rs.is_scanning;
}

static bool rx_beeps(void)
{
    return !rs.is_scanning && (settings.beep_options & BEEP_RX_CARRIER) && (settings.audio_prompt_mode != SOUND_PROMPT_SILENT);
}

static void rx_beep_begin(void)
{
    if (rx_end_beep) {
        rx_end_beep = false; // the same over, interrupted a moment: no beeps
    } else if (rx_beeps()) {
        sound_play(SOUND_RX_BEGIN);
    }
}

static void rx_beep_end(void)
{
    if (rx_beeps()) {
        rx_end_beep = true;
        rx_end_beep_at = xTaskGetTickCount() + pdMS_TO_TICKS(RX_END_BEEP_MS);
    }
}

// The receive beeps off (a transmission starts)
void rx_beep_cancel(void)
{
    rx_end_beep = false;
}

void rx_beep_end_check(void)
{
    if (rx_end_beep && ((int32_t)(xTaskGetTickCount() - rx_end_beep_at) >= 0)) {
        rx_end_beep = false;
        sound_play(SOUND_RX_END);
    }
}

// A digital channel with a signal on it (dmr_signal()): any, whatever its colour code or timeslot (the FM broadcast
// radio makes way for it: the channel is not received while the radio plays, broadcast_check())
#define DIGITAL_BUSY_DBM (-105)
#define DIGITAL_FREE_DBM (-110)
static volatile bool digital_carrier = false;

bool radio_channel_busy(void)
{
    return rs.audio_needs_amp || (digital_carrier && !is_analog());
}

// The channel quiet: not busy for RADIO_QUIET_MS (radio_channel_quiet()); a UI_DATA_SIGNAL when that begins (the
// digital receivers switch their audio without one)
static bool quiet = true;
static TickType_t heard_at;

void quiet_check(void)
{
    bool busy = radio_channel_busy();
    bool now;

    if (busy) {
        heard_at = xTaskGetTickCount();
    }
    now = !busy && ((xTaskGetTickCount() - heard_at) >= pdMS_TO_TICKS(RADIO_QUIET_MS));
    if (now != quiet) {
        quiet = now;
        if (quiet) {
            post_signal();
        }
    }
}

bool radio_channel_quiet(void)
{
    return quiet && !radio_channel_busy(); // busy just now: before quiet_check() sees it
}

// The end of a transmission (a beacon) sets the FD6818's audio out again (trx_dmr_burst_rx()): muted again while
// the FM broadcast receiver has the audio switch (radio_broadcast_amp())
void broadcast_check(void)
{
    static bool transmitted = false;

    if (rs.is_transmitting) {
        transmitted = true;
    } else if (transmitted) {
        transmitted = false;
        if (audio_sw_broadcast) {
            trx_dmr_af(false);
        }
    }
}

// DMR: the signal and the noise for the S meter and the engine's SNR check (trxGetSNRMargindBm()); the engine does
// the rest (the green LED, the speaker). The receiver follows the timeslots: a reading in the other slot is the noise
// floor, even under a steady carrier (measured: 120 and 85 in turn), so the S meter takes the highest of the last
// DMR_PEAK_SAMPLES readings (100 ms at SAMPLE_MS: always one in the 30 ms slot of every 60 ms), not their average
#define DMR_PEAK_SAMPLES 4

void dmr_signal(void)
{
    static uint8_t recent[DMR_PEAK_SAMPLES];
    static uint8_t recent_index;
    uint8_t signal;
    uint8_t noise;
    uint8_t peak = 0;

    trx_read_signal(&signal, &noise);
    signal_raw = signal;
    noise_raw = noise;
    recent[recent_index] = signal;
    recent_index = (uint8_t)((recent_index + 1) % DMR_PEAK_SAMPLES);
    for (int i = 0; i < DMR_PEAK_SAMPLES; i++) {
        peak = (recent[i] > peak) ? recent[i] : peak;
    }
    add_sample(peak);

    int dbm = (int)peak - ((rs.channel.rx_freq >= UHF_FROM) ? 167 : 177);
    bool carrier = (dbm >= DIGITAL_BUSY_DBM) || (digital_carrier && (dbm >= DIGITAL_FREE_DBM));

    if (carrier != digital_carrier) {
        digital_carrier = carrier;
        post_signal();
    }
}

void RAM_CODE squelch_check(void)
{
    uint8_t signal;
    uint8_t noise;
    bool fm = is_fm();
    int level = fm ? squelch_level() : rssi_squelch_level();
    // the receive tone of the channel, unless the analog filter of the quick menu is "none" (trxAnalogFilterLevel)
    bool tone = fm && (rs.channel.rx_tone != CODEPLUG_CSS_NONE) && (settings.analog_filter_level != 0);
    bool open;
    bool close;

    trx_read_signal(&signal, &noise);
    signal_raw = signal;
    noise_raw = noise;
    add_sample(signal);

    if (fm) {
        open = (noise < level);
        close = (noise > (level + SQUELCH_HYSTERESIS));
    } else {
        // 3 dB under the opening level, and for the hold time in a row: a moment under it (a dip of the modulation, a
        // pause between words) doesn't close it
        int hold = (codeplug_mode_is_ssb(rs.channel.mode) ? SQUELCH_HOLD_SSB_MS : SQUELCH_HOLD_AM_MS) / SAMPLE_MS;

        open = (signal >= level);
        if (signal >= (level - SQUELCH_HYSTERESIS)) {
            signal_low = 0;
        } else if (signal_low < hold) {
            signal_low++;
        }
        close = (signal_low >= hold);
    }
    if (!rs.has_carrier && open && ((int32_t)(xTaskGetTickCount() - settled_at) >= 0)) {
        rs.has_carrier = true;
        css_missing = 0;
        trx_af_out(true);
        leds_green(true);
        post_signal();
    } else if (rs.has_carrier && close) {
        if (rs.speaker_open) {
            rx_beep_end();
        }
        squelch_close();
        return;
    }
    if (!rs.has_carrier) {
        return;
    }

    bool tone_ok = !tone || trx_rx_tone_detected(rs.channel.rx_tone);

    if (!rs.speaker_open && tone_ok) {
        rs.speaker_open = true;
        css_missing = 0;
        modem_fm_audio(true);
        audio_amp(true);
        rx_beep_begin();
        post_signal();
    } else if (rs.speaker_open && !tone_ok) {
        if (++css_missing >= CSS_HOLD) { // a tone is often lost a moment on a strong deviation
            rs.speaker_open = false;
            css_missing = 0;
            audio_amp(false);
            rx_beep_end();
            post_signal();
        }
    } else {
        css_missing = 0;
    }
}

// Scanning a digital channel: a DMR transmission without a repeater is on the air 27.5 ms of every 60, so a step of
// 50 ms sees it only with readings every few ms (SAMPLE_MS would give one or two, maybe in the gap)
bool scan_sampling(void)
{
    return rs.is_scanning && rs.dmr_running && !rs.is_transmitting && !eco_rx_off();
}

void scan_sample(void)
{
    uint8_t signal;
    uint8_t noise;

    if ((int32_t)(xTaskGetTickCount() - scan_settled_at) < 0) {
        return;
    }
    trx_read_signal(&signal, &noise);
    if (signal > scan_peak) {
        scan_peak = signal;
    }
    if (scan_samples < UINT16_MAX) {
        scan_samples++;
    }
}

// The DMR engine's audio amplifier (enableAudioAmp(), disableAudioAmp()): from its task or its interrupts
void RAM_CODE radio_dmr_audio(bool on)
{
    uint32_t psr = irq_save();

    rs.audio_needs_amp = on;
    gpio_output(PIN_AUDIO_AMP, rs.audio_needs_amp || rs.beep_needs_amp || rs.broadcast_needs_amp);
    irq_restore(psr);
}

void radio_beep_path(bool on)
{
    taskENTER_CRITICAL();
    beep_path = on;
    audio_switch();
    taskEXIT_CRITICAL();
}

void radio_beep_amp(bool on)
{
    taskENTER_CRITICAL();
    rs.beep_needs_amp = on;
    gpio_output(PIN_AUDIO_AMP, rs.audio_needs_amp || rs.beep_needs_amp || rs.broadcast_needs_amp);
    taskEXIT_CRITICAL();
}

// The FM broadcast receiver's amplifier, from its task (this waits). As the voice (voice_play.c): the path first, then
// the amplifier; off, the amplifier first, then the path back (switched under a running amplifier: a click). On a
// digital channel the path is the audio switch high, the FD6818's raw audio out muted before: the digital receivers
// take it through the switch low (to the baseband); high, it would be heard (no squelch). The channel is not received
// meanwhile (measured 2026-11-23: the switch high, no DMR call heard); radio_channel_busy() still sees a signal.
#define BROADCAST_PATH_SETTLE_MS 10
#define BROADCAST_AMP_STOP_MS    20

void radio_broadcast_amp(bool on)
{
    if (on && rs.has_channel && !is_analog() && !audio_sw_broadcast) {
        trx_dmr_af(false);
        taskENTER_CRITICAL();
        audio_sw_broadcast = true;
        audio_switch();
        taskEXIT_CRITICAL();
        vTaskDelay(pdMS_TO_TICKS(BROADCAST_PATH_SETTLE_MS));
    }
    taskENTER_CRITICAL();
    rs.broadcast_needs_amp = on;
    gpio_output(PIN_AUDIO_AMP, rs.audio_needs_amp || rs.beep_needs_amp || rs.broadcast_needs_amp);
    taskEXIT_CRITICAL();
    if (!on && audio_sw_broadcast) {
        vTaskDelay(pdMS_TO_TICKS(BROADCAST_AMP_STOP_MS));
        taskENTER_CRITICAL();
        audio_sw_broadcast = false;
        audio_switch();
        taskEXIT_CRITICAL();
        if (rs.has_channel && !is_analog()) {
            trx_dmr_af(true);
        }
    }
}

// FM: the noise under the squelch level (before the tone check)
bool radio_has_carrier(void)
{
    return rs.has_carrier;
}

// The speaker plays a received signal (FM squelch and tone open, or DMR audio)
bool radio_audio_heard(void)
{
    return rs.audio_needs_amp;
}

// Something heard: the FM squelch (and tone) open, or DMR / P25 audio playing (the screens' green line and S meter;
// speaker is the FM squelch's only)
bool radio_is_receiving(void)
{
    return rs.speaker_open || rs.audio_needs_amp;
}

uint8_t radio_signal(void)
{
    return signal_shown;
}

int radio_signal_dbm(void)
{
    return (int)signal_shown - ((rs.channel.rx_freq >= UHF_FROM) ? 167 : 177);
}

uint8_t radio_signal_raw(void)
{
    return signal_raw;
}

uint8_t radio_scan_peak(uint16_t *samples)
{
    taskENTER_CRITICAL();
    uint8_t peak = scan_peak;
    uint16_t n = scan_samples;
    taskEXIT_CRITICAL();

    if (samples != NULL) {
        *samples = n;
    }
    return peak;
}

uint8_t radio_noise(void)
{
    return noise_raw;
}
