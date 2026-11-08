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
 * OpenDMR, service: the radio, FM. After OpenDM32 functions/trx.c (trxSetModeAndBandwidth(),
 * trxSetFrequency(), trxActivateRx() / trxActivateTx(), trxCheckAnalogSquelch(), trxUpdate_PA_DAC_Drive(),
 * trxCheckFrequencyInAmateurBand()), hardware/radioHardwareInterface.c (radioSetRx(), radioSetTx(), radioAudioAmp(),
 * radioReadRSSIAndNoiseFromRam()) and user_interface/uiTxScreen.c (tones around a transmission), DM32 branches.
 *
 * The task waits for a request (a channel, the PTT) or 25 ms; in FM receive it reads the signal and noise and runs the
 * squelch, and touches the UI only when what is shown changes. That path still runs from RAM, from when the FM audio
 * went through the baseband (audio switch low) and bus activity was heard in it; probably not needed since.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "util.h"
#include "leds.h"
#include "fd6818.h"
#include "systime.h"
#include "modem.h"
#include "settings.h"
#include "factory_cal.h"
#include "ui.h"
#include "sound.h"
#include "tones.h"
#include "mdc1200.h"
#include "codeplug.h"
#include "dmr.h"
#include "hr_c7000.h"
#include "vox.h"
#include "hotspot.h"
#include "adc.h"
#include "radio.h"

#define RADIO_TASK_PRIORITY 4
#define SAMPLE_MS           25
#define TUNE_SETTLE_MS      20 // after a channel change the FD6818 noise reading is low a moment: no squelch opening

#define PIN_LNA_VHF   0x22 // R5_V_SW
#define PIN_LNA_UHF   0x24 // R5_U_SW
#define PIN_PA_VHF    0x37
#define PIN_PA_UHF    0x23
#define PIN_AUDIO_SW  0x2F // the audio switch ("PWM/FD6818"): high in FM, the speaker straight from the FD6818 (stock)
#define PIN_AUDIO_AMP 0x35

#define UHF_FROM  28000000 // 10 Hz: the UHF band of the radio (RADIO_HARDWARE_FREQUENCY_BANDS)
#define B220_FROM 20000000

#define SQUELCH_MAX        70
#define SQUELCH_STEP       3
#define SQUELCH_HYSTERESIS 3
#define CSS_HOLD           6 // checks a receive tone may be missing before the speaker closes
#define SIGNAL_SAMPLES     10
#define SIGNAL_MOVE        2 // the shown signal follows the average when it moved this much
#define RX_END_BEEP_MS     150 // the carrier end beep waits this long, a new carrier in it cancels both
#define BEEP_RX_CARRIER    0x04 // settings.beep_options
#define BEEP_DMR_TX_STOP   0x02

#define FLAG4_VOX     0x40
#define FLAG4_RX_ONLY 0x04
#define FLAG4_WIDE    0x02

#define TX_TONE_GAIN_STEP 12  // the air level 1 .. 10 to the Tone1 gain (the UV-K5 firmware sends its tones at 66)
#define TAIL_MS           300 // the stock firmware sends the tail this long (0x300DD44)

#define POWER_LEVELS 10 // 50 mW ... 5 W, then the user power
#define POWER_USER   9
#define PA_DRIVE_MAX 4095

static StaticTask_t task_tcb;
static StackType_t task_stack[256];
static TaskHandle_t task;

// What the UI asks (read by the task)
static codeplug_channel_t requested;
static volatile bool requested_valid = false;
static volatile bool channel_pending = false;
static volatile bool tx_wanted = false;
static volatile bool tx_plain = false; // no start / end tones nor MDC1200 (a DTMF sequence)

// What the task uses
static codeplug_channel_t channel;
static bool has_channel = false;
static bool dmr_on = false; // the modem and the FD6818 are in DMR
static bool dmr_stopping = false; // the PTT released, the engine ends its transmission
static uint16_t dmr_drive;        // the PA drive of the DMR transmission (pa_drive() of its frequency)
static volatile bool transmitting = false;

// Receive
static volatile bool carrier = false; // the noise under the squelch level
static volatile bool speaker = false; // the audio on (the carrier, and the tone if one is set)
static volatile bool scanning = false; // radio_set_scanning(): no receive beeps
static TickType_t settled_at;           // after a channel change, the squelch may open from then on
static uint8_t css_missing = 0;
static bool rx_end_beep = false; // the carrier end beep is waiting
static TickType_t rx_end_beep_at;
static volatile uint8_t signal_raw = 0;
static volatile uint8_t noise_raw = 255;
static uint8_t samples[SIGNAL_SAMPLES];
static uint8_t sample_index = 0;
static volatile uint8_t signal_shown = 0;

/* Channel facts */

static bool is_fm(void)
{
    return has_channel && (channel.mode == CODEPLUG_MODE_FM);
}

static bool is_wide(void)
{
    return is_fm() && ((channel.flag4 & FLAG4_WIDE) != 0);
}

int radio_band_of(uint32_t frequency)
{
    return (frequency >= UHF_FROM) ? 2 : ((frequency >= B220_FROM) ? 1 : 0);
}

// The noise level the squelch opens under (trxCheckAnalogSquelch())
static int squelch_level(void)
{
    int level = (channel.sql != 0) ? channel.sql : settings.squelch_defaults[radio_band_of(channel.rx_freq)];

    return SQUELCH_MAX - ((level - 1) * SQUELCH_STEP);
}

/* Pins */

static void lna(bool on, uint32_t frequency)
{
    gpio_output(PIN_LNA_VHF, on && (frequency < UHF_FROM));
    gpio_output(PIN_LNA_UHF, on && (frequency >= UHF_FROM));
}

// The speaker amplifier: on for the received audio or for a beep (sound.c), off when neither needs it. The audio
// switch: high in FM (the speaker from the FD6818), low during a beep (the beeps come from the baseband's DAC; with the
// switch high they don't reach the speaker)
static volatile bool amp_for_audio = false;

// The VOX (vox.h): on for a channel with VOX and a VOX level set, on the stock firmware's VOX ADC channel. As the
// stock firmware, 10 samples (1 s) are left out after the channel is set and after a received signal ends; also
// after a key (the main firmware) or a beep (the speaker into the mic). Not after its own transmission: it keys
// again at once when the speech goes on.
#define VOX_SETTLE_MS 1000
static bool vox_on = false;
static bool vox_keyed = false;         // the VOX posted a PTT press, its release is due
static volatile bool vox_key = false;  // the input task saw a key (radio_vox_key())
static volatile bool vox_ptt = false;  // ... and the PTT was held
static uint32_t vox_next_ms;
static void vox_setup(void);
static volatile bool vox_heard = false; // a received signal was heard: start again
static uint16_t vox_window_max = 0;    // the loudest ADC reading of this VOX_SAMPLE_MS window
static volatile bool amp_for_beep = false;
static volatile bool beep_path = false; // the audio switch to the baseband for a beep
static volatile bool amp_for_broadcast = false; // the FM broadcast receiver plays (its audio goes straight to the amplifier)
static volatile bool audio_sw_fm = false;

static void audio_switch(void)
{
    gpio_output(PIN_AUDIO_SW, audio_sw_fm && !beep_path);
}

static void RAM_CODE audio_amp(bool on)
{
    taskENTER_CRITICAL();
    amp_for_audio = on;
    gpio_output(PIN_AUDIO_AMP, amp_for_audio || amp_for_beep || amp_for_broadcast);
    taskEXIT_CRITICAL();
}

/* The transmit power: the factory points 250 mW, 1 W, 5 W and the fractions of the OpenDM32 releases between them */

static uint16_t pa_drive(uint32_t frequency)
{
    // [0] 50 mW of 250 mW, [1] [2] 500 / 750 mW between 250 mW and 1 W, [3] [4] [5] 2 / 3 / 4 W between 1 W and 5 W
    static const uint16_t fractions[3][6] = {
        {900, 410, 760, 340, 600, 810}, // VHF, per mille
        {900, 420, 750, 310, 550, 780}, // 220 MHz
        {900, 430, 740, 270, 510, 750}, // UHF
    };
    const uint16_t *f = fractions[radio_band_of(frequency)];
    uint16_t points[FACTORY_CAL_LEVELS];
    int level = (channel.libre_dmr_power != 0) ? (channel.libre_dmr_power - 1) : settings.tx_power_level;
    int32_t very_low;
    int32_t low;
    int32_t mid;
    int32_t drive;

    factory_cal_pa_drive(frequency, points);
    very_low = points[0];
    low = points[1];
    mid = points[2];

    switch (CLAMP(level, 0, POWER_LEVELS - 1)) {
        case 0:
            drive = (very_low * f[0]) / 1000;
            break;
        case 1:
            drive = very_low;
            break;
        case 2:
        case 3:
            drive = very_low + (((low - very_low) * f[level - 1]) / 1000);
            break;
        case 4:
            drive = low;
            break;
        case 5:
        case 6:
        case 7:
            drive = low + (((mid - low) * f[level - 2]) / 1000);
            break;
        case 8:
            drive = mid;
            break;
        default: // POWER_USER
            drive = settings.user_power;
            break;
    }
    return (uint16_t)CLAMP(drive, 0, PA_DRIVE_MAX);
}

/* Receive */

static void RAM_CODE post_signal(void)
{
    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_SIGNAL};

    ui_post(&ev);
}

static void squelch_close(void)
{
    audio_amp(false);
    fd6818_af_out(false);
    leds_green(false);
    if (carrier || speaker) {
        carrier = false;
        speaker = false;
        post_signal();
    }
    css_missing = 0;
}

// Receive on the channel: radioSetRx(), radioSetFrequency(), radioSetIF()
static void receive_on(void)
{
    bool wide = is_wide();

    modem_pa_drive(0);
    gpio_output(PIN_PA_VHF, false);
    gpio_output(PIN_PA_UHF, false);
    lna(true, channel.rx_freq);
    fd6818_receive(wide);
    if (is_fm()) {
        modem_fm_receive(wide, factory_cal_rx_dac_gain());
    }
    fd6818_set_frequency(channel.rx_freq, wide);
    fd6818_receive(wide);
    fd6818_set_rx_tone(is_fm() ? channel.rx_tone : CODEPLUG_CSS_NONE);
}

static void eco_reset(void);

static void apply_channel(void)
{
    const factory_cal_t *cal = factory_cal_get();
    bool was_fm = is_fm();

    eco_reset(); // the channel is set up receiving: the power saving starts over

    taskENTER_CRITICAL();
    memcpy(&channel, &requested, sizeof(channel));
    has_channel = requested_valid;
    channel_pending = false;
    taskEXIT_CRITICAL();

    // The FM squelch of the channel left; in DMR the engine has the speaker and the LED, and the FD6818 audio register
    // its DMR setting (the FM value there stops the baseband to the HR_C7000: no DMR sync after a retune)
    if (was_fm) {
        squelch_close();
    }
    settled_at = xTaskGetTickCount() + pdMS_TO_TICKS(TUNE_SETTLE_MS);
    // FM as the stock firmware: the speaker from the FD6818's audio output; through the baseband (switch low, as the
    // DMR receive needs it) the CPU, flash and display activity was heard in the FM audio
    taskENTER_CRITICAL();
    audio_sw_fm = is_fm();
    audio_switch();
    taskEXIT_CRITICAL();
    // The mic to the FD6818 for FM (high), to the modem's codec for DMR (low), set with the mode as the stock firmware
    // does: switched at each DMR transmission, it put a DC step and a click (to -32000) into the first 100 ms of the
    // capture, sent as a pop at the start of every transmission
    gpio_output(PIN_MIC_MUT, is_fm());
    memset(samples, 0, sizeof(samples));
    signal_shown = 0;
    if (dmr_on && (!has_channel || is_fm())) {
        dmr_leave();
        dmr_on = false;
    }
    if (!has_channel) {
        lna(false, 0);
        fd6818_write(0x30, 0x0000); // idle
        vox_setup();
        post_signal();
        return;
    }

    if (is_fm()) {
        // FM set up (trxSetModeAndBandwidth()): the baseband, the FD6818 FM table and level, then the channel
        modem_fm_receive(is_wide(), factory_cal_rx_dac_gain());
        fd6818_set_fm(factory_cal_analog_rx_volume());
        modem_set_frequency_calibration(cal->valid, cal->osc_tune, factory_cal_mod_bias(channel.tx_freq));
        receive_on();
    } else {
        // DMR (trxSetModeAndBandwidth(), trxSetFrequency()): the FD6818 DMR table the first time, the engine stopped
        // while the frequency changes, then started on the channel
        if (dmr_on) {
            dmr_retune();
        } else {
            fd6818_set_dmr();
        }
        modem_set_frequency_calibration(cal->valid, cal->osc_tune, factory_cal_mod_bias(channel.tx_freq));
        receive_on();
        if (dmr_on) {
            dmr_channel(&channel);
        } else {
            dmr_enter(&channel);
            dmr_on = true;
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
    scanning = on;
}

bool radio_is_scanning(void)
{
    return scanning;
}

static bool rx_beeps(void)
{
    return !scanning && (settings.beep_options & BEEP_RX_CARRIER) && (settings.audio_prompt_mode != SOUND_PROMPT_SILENT);
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

static void rx_beep_end_check(void)
{
    if (rx_end_beep && ((int32_t)(xTaskGetTickCount() - rx_end_beep_at) >= 0)) {
        rx_end_beep = false;
        sound_play(SOUND_RX_END);
    }
}

// DMR: the signal and the noise for the S meter and the engine's SNR check (trxGetSNRMargindBm()); the engine does
// the rest (the green LED, the speaker)
static void dmr_signal(void)
{
    uint8_t signal;
    uint8_t noise;

    fd6818_read_signal(&signal, &noise);
    signal_raw = signal;
    noise_raw = noise;
    add_sample(signal);
}

static void RAM_CODE squelch_check(void)
{
    uint8_t signal;
    uint8_t noise;
    int level = squelch_level();
    // the receive tone of the channel, unless the analog filter of the quick menu is "none" (trxAnalogFilterLevel)
    bool tone = (channel.rx_tone != CODEPLUG_CSS_NONE) && (settings.analog_filter_level != 0);

    fd6818_read_signal(&signal, &noise);
    signal_raw = signal;
    noise_raw = noise;
    add_sample(signal);

    if (!carrier && (noise < level) && ((int32_t)(xTaskGetTickCount() - settled_at) >= 0)) {
        carrier = true;
        css_missing = 0;
        fd6818_af_out(true);
        leds_green(true);
        post_signal();
    } else if (carrier && (noise > (level + SQUELCH_HYSTERESIS))) {
        if (speaker) {
            rx_beep_end();
        }
        squelch_close();
        return;
    }
    if (!carrier) {
        return;
    }

    bool tone_ok = !tone || fd6818_rx_tone_detected(channel.rx_tone);

    if (!speaker && tone_ok) {
        speaker = true;
        css_missing = 0;
        modem_fm_audio(true);
        audio_amp(true);
        rx_beep_begin();
        post_signal();
    } else if (speaker && !tone_ok) {
        if (++css_missing >= CSS_HOLD) { // a tone is often lost a moment on a strong deviation
            speaker = false;
            css_missing = 0;
            audio_amp(false);
            rx_beep_end();
            post_signal();
        }
    } else {
        css_missing = 0;
    }
}

/* Transmit (radioSetFrequency(tx), radioSetTx(); the tone first, as uiTxScreen.c) */

static void transmit_on(void)
{
    bool wide = is_wide();
    uint32_t tx = channel.tx_freq;

    squelch_close();
    rx_end_beep = false;
    leds_red(true);
    fd6818_set_tx_tone(channel.tx_tone);
    fd6818_set_frequency(tx, wide);
    lna(false, 0);
    fd6818_transmit_prepare();
    fd6818_set_mic_gain(settings.mic_gain_fm); // the FM mic goes to the FD6818 (the codec gain is the DMR one)
    modem_fm_transmit(wide, settings.mic_gain_fm);
    gpio_output(PIN_MIC_MUT, true); // the mic to the FD6818 (low: to the modem, DMR)
    fd6818_transmit(wide);
    gpio_output((tx >= UHF_FROM) ? PIN_PA_UHF : PIN_PA_VHF, true);
    modem_pa_drive(pa_drive(tx));
    transmitting = true;
    post_signal();
}

static void transmit_off(void)
{
    modem_pa_drive(0);
    gpio_output(PIN_PA_VHF, false);
    gpio_output(PIN_PA_UHF, false);
    leds_red(false);
    transmitting = false;
    receive_on();
    post_signal();
}

/* The FM transmit tones (sound.h): on the air while transmitting, the radio task waits for them; on our speaker by the
   sound service, at the start while the air one goes */

static void air_tone(uint8_t pattern)
{
    const int16_t *melody = sound_tx_tone(pattern);

    if (!melody) {
        return;
    }
    for (int i = 0; melody[i] >= 0; i += 2) {
        fd6818_tx_tone((uint16_t)melody[i], 0, (uint8_t)(sound_tx_tones.air_level * TX_TONE_GAIN_STEP));
        vTaskDelay(pdMS_TO_TICKS(melody[i + 1]));
    }
    fd6818_tx_tone_end();
}

// What the UI asks (radio_tx_signal()), and what is sent
static volatile uint16_t signal_wanted[2];
static uint16_t signal_sent[2];

static void tx_signal_update(void)
{
    uint16_t hz1 = signal_wanted[0];
    uint16_t hz2 = signal_wanted[1];

    if (!transmitting || ((hz1 == signal_sent[0]) && (hz2 == signal_sent[1]))) {
        return;
    }
    if ((hz1 == 0) && (hz2 == 0)) {
        fd6818_tx_tone_end();
    } else {
        fd6818_tx_tone((hz1 == RADIO_TX_SILENT) ? 0 : hz1, hz2, (uint8_t)(sound_tx_tones.air_level * TX_TONE_GAIN_STEP));
    }
    signal_sent[0] = hz1;
    signal_sent[1] = hz2;
}

// The CTCSS / DCS tail before the carrier goes, so the receivers close their squelch without the noise burst
static void tail(void)
{
    if ((sound_tx_tones.tail == SOUND_TAIL_OFF) || (sound_tx_tones.tail >= SOUND_TAILS) ||
        (channel.tx_tone == CODEPLUG_CSS_NONE)) {
        return;
    }
    fd6818_tx_tail(((channel.tx_tone & TONES_DCS) != 0), (sound_tx_tones.tail == SOUND_TAIL_PHASE));
    vTaskDelay(pdMS_TO_TICKS(TAIL_MS));
    fd6818_tx_tail_end();
}

// MDC1200 PTT ID: the unit ID the last 4 decimal digits of the DMR ID, as MDC IDs are shown (1234 is 0x1234)
static void mdc1200(bool start)
{
    uint8_t mode = sound_tx_tones.mdc1200;
    uint8_t packet[MDC1200_PACKET_SIZE];
    uint32_t id = codeplug_get_dmr_id();
    uint16_t unit = 0;

    if ((mode != SOUND_MDC1200_BOTH) && (mode != (start ? SOUND_MDC1200_START : SOUND_MDC1200_END))) {
        return;
    }
    for (int digit = 0; digit < 4; digit++) {
        unit |= (uint16_t)((id % 10) << (4 * digit));
        id /= 10;
    }
    mdc1200_encode(packet, MDC1200_OP_PTT_ID, (start ? MDC1200_ARG_PRE : MDC1200_ARG_POST), unit);
    fd6818_tx_fsk(packet, sizeof(packet), is_wide());
}

static void local_tone(uint8_t pattern)
{
    const int16_t *melody = sound_tx_tone(pattern);

    if (melody) {
        sound_play_volume(melody, sound_tx_tones.local_volume);
    }
}

/* DMR transmit: the engine switches the transmitter from its interrupts (trxActivateDMRTx(), trxFastDMRTx(),
   trxActivateRx() of the main firmware: radioSetTx(), radioFastTx(), radioSetRx() of the DM-32 releases). As the stock
   firmware (DM32.01.L01.048): the HR_C7000's RF transmit start (vector 64) and RF receive start (65) interrupts only
   ask its RF task (0x300dc44), which switches each burst with waits and a PA ramp (RAM 0x2b440 on, 0x2b4a0 off,
   0x2b364 ramp). Switched at once, without the ramp and back on the Rx frequency in between, the bursts spread over
   the band: a radio next to ours didn't decode the repeater's downlink of our call (a DM1801 next to another did). */

#define RF_TASK_PRIORITY (configMAX_PRIORITIES - 1) // the burst timing first
#define RF_SETUP         0x01 // the first burst of a transmission: the transmitter set up first
#define RF_ON            0x02
#define RF_OFF           0x04
#define RF_RX            0x08 // the end of the transmission

static StaticTask_t rf_task_tcb;
static StackType_t rf_task_stack[256];
static TaskHandle_t rf_task;
static bool rf_on_air = false; // the PA on (rf task)

// The stock PA ramp (0x3074714): the drive is the target x ramp[i] / 248, up through all 40, down from ramp[20]
static const uint8_t pa_ramp[40] = {1,   2,   2,   4,   8,   10,  12,  14,  16,  18,  20,  24,  28,  32,
                                    36,  40,  46,  52,  58,  64,  70,  76,  82,  88,  94,  100, 106, 112,
                                    118, 124, 133, 143, 158, 173, 188, 203, 218, 228, 238, 248};
#define PA_RAMP_FULL 248
#define PA_RAMP_DOWN 20

// The stock busy wait (RAM 0x2df80): 164 nops a unit, about 1 us
static void RAM_CODE rf_wait(uint32_t units)
{
    for (uint32_t i = 0; i < units; i++) {
        __asm__ volatile(".rept 164\n"
                         "nop\n"
                         ".endr\n");
    }
}

// A ramp step: the stock firmware's soft float maths between two writes, about 4 units (estimated)
static void rf_ramp_step(uint8_t level)
{
    modem_pa_drive((uint16_t)(((uint32_t)dmr_drive * level) / PA_RAMP_FULL));
    rf_wait(4);
}

// 0x2b440: the LNA off, a wait by band (stock: 1.6 / 1.0 / 1.2 ms picked from a channel field, not mapped yet; here VHF /
// 220 MHz / UHF, unverified), the Tx frequency, the PA switch, the ramp up
static void rf_on(void)
{
    static const uint16_t waits[3] = {1600, 1000, 1600}; // UHF 1600: the PA full 0.85 ms before the timeslot (1200: 1.25)

    lna(false, 0);
    rf_wait(waits[radio_band_of(channel.tx_freq)]);
    fd6818_dmr_burst_tx(channel.tx_freq);
    gpio_output((channel.tx_freq >= UHF_FROM) ? PIN_PA_UHF : PIN_PA_VHF, true);
    for (int i = 0; i < (int)sizeof(pa_ramp); i++) {
        rf_ramp_step(pa_ramp[i]);
    }
    modem_pa_drive(dmr_drive);
    rf_on_air = true;
    hrc7000TraceRf(5);
}

// 0x2b4a0: 1.6 ms more (the interrupt comes before the end of the burst), the ramp down, the PA switch off, the Rx
// frequency, the LNA on
static void rf_off(void)
{
    if (!rf_on_air) {
        return;
    }
    rf_wait(1600);
    for (int i = PA_RAMP_DOWN; i > 0; i--) {
        rf_ramp_step(pa_ramp[i]);
    }
    modem_pa_drive(0);
    rf_wait(300);
    gpio_output(PIN_PA_UHF, false);
    gpio_output(PIN_PA_VHF, false);
    rf_on_air = false;
    hrc7000TraceRf(6);
    rf_wait(700);
    fd6818_dmr_burst_rx(channel.rx_freq);
    lna(true, channel.rx_freq);
    rf_wait(50);
}

static void rf_task_main(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t asked = 0;

        xTaskNotifyWait(0, UINT32_MAX, &asked, portMAX_DELAY);
        if (asked & RF_OFF) {
            rf_off();
        }
        if (asked & RF_RX) {
            // radioSetRx(), radioSetFrequency(rx), trxUpdateC7000Calibration()
            const factory_cal_t *cal = factory_cal_get();

            rf_off();
            modem_set_frequency_calibration(cal->valid, cal->osc_tune, factory_cal_mod_bias(channel.tx_freq));
        }
        if (asked & RF_SETUP) {
            // radioSetTx(): the band of the Tx frequency, the modem's DMR calibration, the mic to the modem (vocoder)
            fd6818_set_frequency(channel.tx_freq, false);
            fd6818_transmit_prepare();
            dmr_modem_calibration();
            gpio_output(PIN_MIC_MUT, false);
        }
        if (asked & (RF_SETUP | RF_ON)) {
            rf_on();
        }
    }
}

static void RAM_CODE rf_ask(uint32_t what)
{
    if (xPortIsInsideInterrupt()) {
        BaseType_t woken = pdFALSE;

        xTaskNotifyFromISR(rf_task, what, eSetBits, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xTaskNotify(rf_task, what, eSetBits);
    }
}

// The transmitter set up on the Tx frequency, then the RF on
void RAM_CODE radio_dmr_tx_on(void)
{
    rf_ask(RF_SETUP);
}

// Each burst: the RF on, or off with the FD6818 back on the Rx frequency meanwhile
void RAM_CODE radio_dmr_burst(bool on)
{
    rf_ask(on ? RF_ON : RF_OFF);
}

// Back to receive at the end
void RAM_CODE radio_dmr_rx_on(void)
{
    rf_ask(RF_RX);
}

// The PTT: the engine asked to transmit; released: it ends its transmission, then the radio is free again
static void dmr_transmit_check(void)
{
    if (tx_wanted && !transmitting) {
        dmr_drive = pa_drive(channel.tx_freq);
        transmitting = true;
        dmr_stopping = false;
        leds_red(true);
        dmr_transmit(true);
        post_signal();
    } else if (!tx_wanted && transmitting && !dmr_stopping) {
        dmr_stopping = true;
        dmr_transmit(false);
    }
    if (dmr_stopping && !dmr_is_transmitting()) {
        dmr_stopping = false;
        transmitting = false;
        leds_red(false);
        // the DMR Tx stop beep once the engine has finished (uiTxScreen.c), not after a repeater that didn't answer
        if ((settings.beep_options & BEEP_DMR_TX_STOP) && (settings.audio_prompt_mode != SOUND_PROMPT_SILENT) &&
            !hotspot_active() &&
            !dmr_repeater_failed()) {
            sound_play(SOUND_DMR_TX_STOP);
        }
        post_signal();
    }
}

/* APRS (aprs.c): the AX.25 frame as AFSK on the FM channel, as aprs.c aprsSendPacket() / aprsBitStreamSender() of
   the main firmware: the FD6818 Tone1 switched between 1200 and 2200 Hz (300 baud: 1600 / 1800 Hz) at each bit,
   timed here on the microsecond clock; aprsBeaconingSendBeacon(): a busy channel is waited for up to 2 s, then
   50 ms of carrier before the frame */

#define APRS_COLLISION_MS 2000
#define APRS_TX_DELAY_MS  50
#define APRS_TONE_GAIN    66 // the UV-K5 firmware's tone level

static const uint8_t *volatile aprs_bits = NULL; // asked, the radio task sends it
static volatile uint16_t aprs_bit_count;
static volatile bool aprs_baud300;
static volatile bool aprs_sent;

static void aprs_transmit(void)
{
    uint32_t baud = aprs_baud300 ? 300 : 1200;
    uint16_t mark = aprs_baud300 ? 1600 : 1200;
    uint16_t space = aprs_baud300 ? 1800 : 2200;
    uint16_t last = 0;
    uint32_t start;

    for (uint32_t waited = 0; (carrier || speaker) && (waited < APRS_COLLISION_MS); waited += SAMPLE_MS) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        squelch_check();
    }
    if (carrier || speaker || tx_wanted || (radio_tx_check() != RADIO_TX_OK)) {
        aprs_sent = false;
        aprs_bits = NULL;
        return;
    }
    transmit_on();
    vTaskDelay(pdMS_TO_TICKS(APRS_TX_DELAY_MS));
    fd6818_tx_tone(mark, 0, APRS_TONE_GAIN);
    last = mark;
    start = systime_get_us();
    for (uint32_t i = 0; i < aprs_bit_count; i++) {
        uint16_t hz = ((aprs_bits[i / 8] >> (i % 8)) & 1) ? space : mark;

        if (hz != last) {
            fd6818_tx_tone_hz(hz);
            last = hz;
        }
        while ((int32_t)(systime_get_us() - (start + (uint32_t)(((uint64_t)(i + 1) * 1000000U) / baud))) < 0) {
        }
    }
    fd6818_tx_tone_end();
    transmit_off();
    aprs_sent = true;
    aprs_bits = NULL;
}

bool radio_aprs_send(const uint8_t *bits, uint16_t bit_count, bool baud300)
{
    if (aprs_bits || !requested_valid || (requested.mode != CODEPLUG_MODE_FM) || transmitting || tx_wanted) {
        return false;
    }
    aprs_bit_count = bit_count;
    aprs_baud300 = baud300;
    aprs_bits = bits;
    if (task) {
        xTaskNotifyGive(task);
    }
    return true;
}

bool radio_aprs_busy(void)
{
    return aprs_bits != NULL;
}

bool radio_aprs_sent(void)
{
    return aprs_sent;
}

/* VOX */

static bool vox_wanted(void)
{
    return has_channel && (channel.flag4 & FLAG4_VOX) && !(channel.flag4 & FLAG4_RX_ONLY) &&
           (settings.vox_threshold != 0);
}

static void vox_post(bool press)
{
    ui_event_t ev = {.type = press ? UI_EVENT_KEY_PRESS : UI_EVENT_KEY_RELEASE, .key = UI_KEY_PTT};

    ui_post(&ev);
}

// After the channel is set, or the VOX level changed: on or off
static void vox_setup(void)
{
    bool wanted = vox_wanted();

    if (wanted && !vox_on) {
        vox_reset(VOX_SETTLE_MS);
    }
    if (!wanted && vox_keyed) {
        vox_post(false);
        vox_keyed = false;
    }
    vox_on = wanted;
}

// Every VOX_SAMPLE_MS (voxTick() and the PTT of applicationMain.c): not while a sound plays (the speaker into the
// mic), a key gives the transmitter back to the keys
static void vox_check(void)
{
    uint32_t now = systime_get_ms();

    if (vox_key) {
        vox_key = false;
        if (vox_keyed) {
            if (!vox_ptt) {
                vox_post(false);
            }
            vox_keyed = false; // the PTT held: its release ends it
        }
        vox_reset(VOX_SETTLE_MS);
        return;
    }
    // the ADC each pass (25 ms), the loudest of the VOX_SAMPLE_MS window decides: one reading a window, as the stock
    // firmware, often fell between the syllables and cut the speech
    uint16_t level = adc_read_vox();

    if (level > vox_window_max) {
        vox_window_max = level;
    }
    if ((int32_t)(now - vox_next_ms) < 0) {
        return;
    }
    vox_next_ms = now + VOX_SAMPLE_MS;
    level = vox_window_max;
    vox_window_max = 0;
    // a sound into the mic starts it again, but not once it keyed the transmitter (the DMR start beep plays before
    // the transmission: that release cut every DMR VOX transmission at once)
    if (!vox_keyed && (vox_heard || (!transmitting && amp_for_beep))) {
        vox_heard = false;
        vox_reset(VOX_SETTLE_MS);
        return;
    }
    vox_heard = false;

    bool held = vox_sample(level, settings.vox_threshold, settings.vox_tail_units);

    if (held && !vox_keyed && !tx_wanted) {
        vox_post(true);
        vox_keyed = true;
    } else if (!held && vox_keyed) {
        vox_post(false);
        vox_keyed = false;
    }
}

void radio_vox_key(bool ptt_held)
{
    vox_ptt = ptt_held;
    vox_key = true;
}

/* Receive power saving (rxPowerSaving.c rxPowerSavingTick(), trxPowerUpDownRxAndC7000()): no signal, key, beep or
   transmission for 12 - 2 x level seconds (at least 4 s), then the receiver (the FD6818 and the LNA) goes off and on
   in turns: on for 130 - 10 x level ms, off for that x 2^(level - 1) (level 1: 120 ms / 120 ms ... level 5: 80 ms /
   1.28 s). A signal found while on, a key (radio_eco_activity()) or anything going on ends it. Not on a channel with
   "no eco", nor while scanning, with the VOX, in hotspot mode or while the FM broadcast radio plays (switching the
   receiver cut its audio). The main firmware's slower CPU clock (level 2 and
   over) isn't done here. */

#define ECO_FLAG1_NO_ECO  0x20 // libre_dmr_flag1
#define ECO_SETTLE_MS     25   // after the receiver comes on, before the squelch reads the noise

typedef enum {
    ECO_INACTIVE,
    ECO_WAIT_START,
    ECO_RX_ON,
    ECO_RX_OFF,
} eco_state_t;

static eco_state_t eco = ECO_INACTIVE;
static TickType_t eco_due;
static volatile bool eco_activity = false;
static bool rx_off = false; // the receiver is off for power saving

static void eco_wait(void)
{
    int level = MIN(settings.eco_level, 4);

    eco_due = xTaskGetTickCount() + pdMS_TO_TICKS((12 - (level * 2)) * 1000);
}

static void rx_power(bool on)
{
    if (on == !rx_off) {
        return;
    }
    rx_off = !on;
    if (on) {
        receive_on();
        settled_at = xTaskGetTickCount() + pdMS_TO_TICKS(ECO_SETTLE_MS);
    } else {
        lna(false, 0);
        fd6818_write(0x30, 0x0000); // radioPowerOff(): the FD6818 idle
    }
}

// The channel is set up again (apply_channel()): receiving, the wait starts over
static void eco_reset(void)
{
    rx_off = false;
    eco = ECO_INACTIVE;
    eco_wait();
}

// Back to receiving all the time (a channel set, something going on)
static void eco_stop(void)
{
    if (eco != ECO_INACTIVE) {
        rx_power(true);
        eco = ECO_INACTIVE;
    }
    eco_wait();
}

static void eco_tick(void)
{
    int level = settings.eco_level;
    bool active = eco_activity;

    eco_activity = false;
    if (!has_channel || (level == 0) || (channel.libre_dmr_flag1 & ECO_FLAG1_NO_ECO) || transmitting || tx_wanted ||
        carrier || speaker || amp_for_audio || amp_for_beep || amp_for_broadcast || scanning || vox_on || aprs_bits ||
        hotspot_active() ||
        (dmr_on && dmr_has_sync()) || active) {
        eco_stop();
        return;
    }
    if ((int32_t)(xTaskGetTickCount() - eco_due) < 0) {
        return;
    }

    uint32_t rx_ms = (uint32_t)(130 - (10 * level));

    switch (eco) {
        case ECO_INACTIVE: // wait before shutting down
            eco_wait();
            eco = ECO_WAIT_START;
            break;
        case ECO_WAIT_START:
        case ECO_RX_ON:
            rx_power(false);
            eco_due = xTaskGetTickCount() + pdMS_TO_TICKS(rx_ms * (1U << (level - 1)));
            eco = ECO_RX_OFF;
            break;
        case ECO_RX_OFF:
            rx_power(true);
            eco_due = xTaskGetTickCount() + pdMS_TO_TICKS(rx_ms);
            eco = ECO_RX_ON;
            break;
    }
}

void radio_eco_activity(void)
{
    eco_activity = true;
    if (rx_off && task) {
        xTaskNotifyGive(task);
    }
}

/* The task */

static void RAM_CODE radio_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAMPLE_MS));

        if (channel_pending && !transmitting) {
            apply_channel();
        }
        if (aprs_bits && !dmr_on && !transmitting) {
            aprs_transmit();
        }
        if (dmr_on) {
            dmr_transmit_check();
        } else if (tx_wanted != transmitting) {
            if (tx_wanted && is_fm()) {
                transmit_on();
                if (!tx_plain) {
                    local_tone(sound_tx_tones.local_start);
                    mdc1200(true);
                    air_tone(sound_tx_tones.air_start);
                }
            } else if (!tx_wanted) {
                signal_wanted[0] = 0;
                signal_wanted[1] = 0;
                tx_signal_update();
                if (!tx_plain) {
                    air_tone(sound_tx_tones.air_end);
                    mdc1200(false);
                }
                tail();
                transmit_off();
                if (!tx_plain) {
                    local_tone(sound_tx_tones.local_end);
                }
            }
        }
        tx_signal_update();
        if (!transmitting && (speaker || amp_for_audio)) {
            vox_heard = vox_on; // flash code waits: the quiet wait while heard (vox_check() starts it again)
        } else {
            if (!transmitting && (vox_wanted() != vox_on)) {
                vox_setup(); // the VOX threshold set or cleared in the menu
            }
            if (vox_on) {
                vox_check();
            }
        }
        if (rx_off) {
            // the receiver off (power saving): nothing to read
        } else if (is_fm() && !transmitting) {
            squelch_check();
        } else if (dmr_on) {
            dmr_signal();
        }
        if (rx_end_beep) {
            rx_beep_end_check();
        }
        eco_tick();
    }
}

/* The API */

void radio_init(void)
{
    gpio_output(PIN_AUDIO_SW, false);
    gpio_output(PIN_AUDIO_AMP, false);
    gpio_output(PIN_PA_VHF, false);
    gpio_output(PIN_PA_UHF, false);
    lna(false, 0);

    factory_cal_init();
    fd6818_init();
    modem_init();
    fd6818_post_init();
    modem_pa_drive(0);
    dmr_init();
}

void radio_start(void)
{
    task = xTaskCreateStatic(radio_task, "radio", sizeof(task_stack) / sizeof(task_stack[0]), NULL, RADIO_TASK_PRIORITY,
                             task_stack, &task_tcb);
    rf_task = xTaskCreateStatic(rf_task_main, "rf", sizeof(rf_task_stack) / sizeof(rf_task_stack[0]), NULL,
                                RF_TASK_PRIORITY, rf_task_stack, &rf_task_tcb);
    dmr_start();
}

void radio_set_channel(const codeplug_channel_t *new_channel)
{
    bool valid = (new_channel != NULL);

    taskENTER_CRITICAL();
    if ((valid != requested_valid) || (valid && (memcmp(&requested, new_channel, sizeof(requested)) != 0))) {
        if (valid) {
            memcpy(&requested, new_channel, sizeof(requested));
        }
        requested_valid = valid;
        channel_pending = true;
    }
    taskEXIT_CRITICAL();
    if (channel_pending && task) {
        xTaskNotifyGive(task);
    }
}

// trxCheckFrequencyInAmateurBand(): off, the legacy default bands (also 220 MHz), the CPS bands (VHF, UHF)
static bool in_band_limits(uint32_t frequency)
{
    static const uint32_t limits[3][2] = {{14400000, 14800000}, {22200000, 22500000}, {42000000, 45000000}};

    if (settings.tx_freq_limited == 0) {
        return true;
    }
    for (int band = 0; band < 3; band++) {
        if ((band == 1) && (settings.tx_freq_limited != 1)) {
            continue;
        }
        if ((frequency >= limits[band][0]) && (frequency <= limits[band][1])) {
            return true;
        }
    }
    return false;
}

radio_tx_check_t radio_tx_check(void)
{
    const codeplug_channel_t *c = &requested;

    if (!requested_valid) {
        return RADIO_TX_NO_CHANNEL;
    }
    if (c->flag4 & FLAG4_RX_ONLY) {
        return RADIO_TX_RX_ONLY;
    }
    if (!codeplug_frequency_is_valid(c->tx_freq) || !in_band_limits(c->tx_freq)) {
        return RADIO_TX_OUT_OF_BAND;
    }
    return RADIO_TX_OK;
}

void radio_transmit(bool on)
{
    if (on) {
        tx_plain = false;
    }
    tx_wanted = on && (radio_tx_check() == RADIO_TX_OK);
    if (task) {
        xTaskNotifyGive(task);
    }
}

void radio_transmit_plain(void)
{
    tx_plain = true;
    tx_wanted = (radio_tx_check() == RADIO_TX_OK);
    if (task) {
        xTaskNotifyGive(task);
    }
}

void radio_tx_signal(uint16_t hz1, uint16_t hz2)
{
    signal_wanted[0] = hz1;
    signal_wanted[1] = hz2;
    if (task) {
        xTaskNotifyGive(task);
    }
}

bool radio_is_transmitting(void)
{
    return transmitting;
}

uint32_t radio_tx_frequency(void)
{
    return requested.tx_freq;
}

uint32_t radio_rx_frequency(void)
{
    return requested_valid ? requested.rx_freq : 0;
}

bool radio_get_channel(codeplug_channel_t *channel)
{
    bool valid;

    taskENTER_CRITICAL();
    valid = requested_valid;
    if (valid) {
        memcpy(channel, &requested, sizeof(*channel));
    }
    taskEXIT_CRITICAL();
    return valid;
}

bool radio_channel_is_dmr(void)
{
    return requested_valid && (requested.mode == CODEPLUG_MODE_DMR);
}

// The DMR engine's audio amplifier (enableAudioAmp(), disableAudioAmp()): from its task or its interrupts
void RAM_CODE radio_dmr_audio(bool on)
{
    uint32_t psr = irq_save();

    amp_for_audio = on;
    gpio_output(PIN_AUDIO_AMP, amp_for_audio || amp_for_beep || amp_for_broadcast);
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
    amp_for_beep = on;
    gpio_output(PIN_AUDIO_AMP, amp_for_audio || amp_for_beep || amp_for_broadcast);
    taskEXIT_CRITICAL();
}

void radio_broadcast_amp(bool on)
{
    taskENTER_CRITICAL();
    amp_for_broadcast = on;
    gpio_output(PIN_AUDIO_AMP, amp_for_audio || amp_for_beep || amp_for_broadcast);
    taskEXIT_CRITICAL();
}

// FM: the noise under the squelch level (before the tone check)
bool radio_has_carrier(void)
{
    return carrier;
}

// The speaker plays a received signal (FM squelch and tone open, or DMR audio)
bool radio_audio_heard(void)
{
    return amp_for_audio;
}

bool radio_is_receiving(void)
{
    return speaker;
}

uint8_t radio_signal(void)
{
    return signal_shown;
}

int radio_signal_dbm(void)
{
    return (int)signal_shown - ((channel.rx_freq >= UHF_FROM) ? 167 : 177);
}

uint8_t radio_signal_raw(void)
{
    return signal_raw;
}

uint8_t radio_noise(void)
{
    return noise_raw;
}
