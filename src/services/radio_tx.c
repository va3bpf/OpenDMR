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
 * OpenDMR, service: the radio, transmit (radio_internal.h). After OpenDM32 functions/trx.c (trxActivateTx(),
 * trxUpdate_PA_DAC_Drive(), trxCheckFrequencyInAmateurBand()), hardware/radioHardwareInterface.c (radioSetTx()) and
 * user_interface/uiTxScreen.c (tones around a transmission), DM32 branches: the power, FM on and off with its tones,
 * the tail and MDC1200, the four backends the state machine starts and stops, the transmit checks.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "util.h"
#include "leds.h"
#include "fd6818.h"
#include "modem.h"
#include "settings.h"
#include "factory_cal.h"
#include "sound.h"
#include "tones.h"
#include "mdc1200.h"
#include "codeplug.h"
#include "dmr.h"
#include "hotspot.h"
#include "p25_radio.h"
#include "nxdn_radio.h"
#include "radio_internal.h"

#define BEEP_DMR_TX_STOP  0x02 // settings.beep_options
#define TX_TONE_GAIN_STEP 12   // the air level 1 .. 10 to the Tone1 gain (the UV-K5 firmware sends its tones at 66)
#define TAIL_MS           300  // the stock firmware sends the tail this long (0x300DD44)

#define POWER_LEVELS 10 // 50 mW ... 10 W, then the user power
#define POWER_USER   9
#define PA_DRIVE_MAX 4095

/* The transmit power. The factory table has the stock Low, Middle and High (rated 1 W, 4 W, 10 W): the stock firmware
 * (v1.01.048, 0x0301A7C4) takes the drive of the level straight from it. The levels between go by the square root of
 * the power (the RF amplitude follows the drive), 50 and 500 mW below 1 W on the slope from 1 W to 4 W: estimates,
 * unmeasured. Levels 2, 5 and 8 are the factory points (before, 1, 4 and 8, named 250 mW, 1 W and 5 W: wrongly). */

typedef struct {
    uint8_t from;      // factory point: 0 Low (1 W), 1 Middle (4 W), 2 High (10 W)
    int16_t per_mille; // of the way to the next point, (sqrt(P) - sqrt(P from)) / (sqrt(P next) - sqrt(P from))
} power_step_t;

static const power_step_t power_steps[POWER_USER] = { // finer at the low end, coarser up high
    {0, -776}, // 50 mW
    {0, -293}, // 500 mW
    {0, 0},    // 1 W
    {0, 414},  // 2 W
    {0, 732},  // 3 W
    {1, 0},    // 4 W
    {1, 203},  // 5 W
    {1, 556},  // 7 W
    {2, 0},    // 10 W
};

uint16_t level_drive(uint32_t frequency, int level)
{
    uint16_t points[FACTORY_CAL_LEVELS];
    int32_t drive;

    level = CLAMP(level, 0, POWER_LEVELS - 1);
    if (level == POWER_USER) {
        drive = settings.user_power;
    } else {
        const power_step_t *step = &power_steps[level];
        int32_t from;
        int32_t next;

        factory_cal_pa_drive(frequency, points);
        from = points[step->from];
        next = (step->from < (FACTORY_CAL_LEVELS - 1)) ? points[step->from + 1] : from;
        drive = from + (((next - from) * step->per_mille) / 1000);
    }
    return (uint16_t)CLAMP(drive, 0, PA_DRIVE_MAX);
}

uint16_t pa_drive(uint32_t frequency)
{
    return level_drive(frequency,
                       (rs.channel.libre_dmr_power != 0) ? (rs.channel.libre_dmr_power - 1) : settings.tx_power_level);
}

/* Transmit (radioSetFrequency(tx), radioSetTx(); the tone first, as uiTxScreen.c) */

void transmit_on(void)
{
    bool wide = is_wide();
    uint32_t tx = rs.channel.tx_freq;

    squelch_close();
    rx_beep_cancel();
    leds_red(true);
    fd6818_set_tx_tone(rs.channel.tx_tone);
    fd6818_set_frequency(tx, wide);
    lna(false, 0);
    fd6818_transmit_prepare();
    fd6818_set_mic_gain(settings.mic_gain_fm); // the FM mic goes to the FD6818 (the codec gain is the DMR one)
    modem_fm_transmit(wide, settings.mic_gain_fm);
    gpio_output(PIN_MIC_MUT, true); // the mic to the FD6818 (low: to the modem, DMR)
    fd6818_transmit(wide);
    gpio_output((tx >= UHF_FROM) ? PIN_PA_UHF : PIN_PA_VHF, true);
    modem_pa_drive(pa_drive(tx));
    rs.transmitting = true;
    post_signal();
}

void transmit_off(void)
{
    modem_pa_drive(0);
    gpio_output(PIN_PA_VHF, false);
    gpio_output(PIN_PA_UHF, false);
    leds_red(false);
    rs.transmitting = false;
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

void tx_signal_update(void)
{
    uint16_t hz1 = signal_wanted[0];
    uint16_t hz2 = signal_wanted[1];

    if (!rs.transmitting || ((hz1 == signal_sent[0]) && (hz2 == signal_sent[1]))) {
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
        (rs.channel.tx_tone == CODEPLUG_CSS_NONE)) {
        return;
    }
    fd6818_tx_tail(((rs.channel.tx_tone & TONES_DCS) != 0), (sound_tx_tones.tail == SOUND_TAIL_PHASE));
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

/* The transmissions' backends (radio_fsm.h: the transmit entry and exit in one place). A backend starts the
   transmission, is asked once to stop it (the PTT released), tells while it is still sending (a digital one sends its
   last frames), then finishes. The PTT's is the channel's (set by apply_channel(), which waits while
   transmitting); the beacon has its own (rs.tx[] by kind). */

static bool always(void)
{
    return true;
}

static bool never(void)
{
    return false;
}

static void nothing(void)
{
}

// FM: the start and end tones and MDC1200 (not for a plain transmission: a DTMF sequence); the end, the tail and off
// all within stop()
static void tx_fm_start(void)
{
    transmit_on();
    if (!rs.tx_plain) {
        local_tone(sound_tx_tones.local_start);
        mdc1200(true);
        air_tone(sound_tx_tones.air_start);
    }
}

static void tx_fm_stop(void)
{
    signal_wanted[0] = 0;
    signal_wanted[1] = 0;
    tx_signal_update();
    if (!rs.tx_plain) {
        air_tone(sound_tx_tones.air_end);
        mdc1200(false);
    }
    tail();
    transmit_off();
    if (!rs.tx_plain) {
        local_tone(sound_tx_tones.local_end);
    }
}

// DMR: the engine asked to transmit; released, it ends its transmission, then the radio is free again
static void tx_dmr_start(void)
{
    p25_ev_log(21, 0); // the delay log (p25_radio.c)
    rf_set_drive(pa_drive(rs.channel.tx_freq));
    rs.transmitting = true;
    leds_red(true);
    dmr_transmit(true);
    post_signal();
}

static void tx_dmr_stop(void)
{
    dmr_transmit(false);
}

static void tx_dmr_finished(void)
{
    rs.transmitting = false;
    leds_red(false);
    // the DMR Tx stop beep once the engine has finished (uiTxScreen.c), not after a repeater that didn't answer
    if ((settings.beep_options & BEEP_DMR_TX_STOP) && (settings.audio_prompt_mode != SOUND_PROMPT_SILENT) &&
        !hotspot_active() && !dmr_repeater_failed()) {
        sound_play(SOUND_DMR_TX_STOP);
    }
    post_signal();
}

// P25 (p25_radio.c) and NXDN (nxdn_radio.c): transmitting until the last data unit / frame is sent
static void tx_p25_start(void)
{
    rs.transmitting = true;
    p25_radio_transmit(true);
    post_signal();
}

static void tx_p25_stop(void)
{
    p25_radio_transmit(false);
}

static void tx_nxdn_start(void)
{
    rs.transmitting = true;
    nxdn_radio_transmit(true);
    post_signal();
}

static void tx_nxdn_stop(void)
{
    nxdn_radio_transmit(false);
}

static void tx_digital_finished(void)
{
    rs.transmitting = false;
    post_signal();
}

// The APRS beacon: the FM carrier, the frame sent in it (radio_aprs.c, the machine's beacon_send()); on an FM channel
static bool beacon_can_start(void)
{
    return is_fm() && !rs.dmr_on;
}

static const tx_backend_t TX_FM = {is_fm, tx_fm_start, tx_fm_stop, never, nothing};
static const tx_backend_t TX_DMR = {always, tx_dmr_start, tx_dmr_stop, dmr_is_transmitting, tx_dmr_finished};
static const tx_backend_t TX_P25 = {always, tx_p25_start, tx_p25_stop, p25_radio_transmitting, tx_digital_finished};
static const tx_backend_t TX_NXDN = {always, tx_nxdn_start, tx_nxdn_stop, nxdn_radio_transmitting, tx_digital_finished};
static const tx_backend_t TX_BEACON = {beacon_can_start, transmit_on, nothing, never, transmit_off};

const tx_backend_t *tx_backend_beacon(void)
{
    return &TX_BEACON;
}

// The backend of a channel (FM's for none: its checks refuse)
const tx_backend_t *tx_backend_of(const codeplug_channel_t *c, bool valid)
{
    if (!valid) {
        return &TX_FM;
    }
    switch (c->mode) {
        case CODEPLUG_MODE_DMR: return &TX_DMR;
        case CODEPLUG_MODE_P25: return &TX_P25;
        case CODEPLUG_MODE_NXDN: return &TX_NXDN;
        default: return &TX_FM; // FM; AM and SSB too, its can_start() refuses them
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

radio_tx_check_t tx_check(const codeplug_channel_t *c, bool valid)
{
    if (!valid) {
        return RADIO_TX_NO_CHANNEL;
    }
    if (c->flag4 & FLAG4_RX_ONLY) {
        return RADIO_TX_RX_ONLY;
    }
    if (!codeplug_mode_can_transmit(c->mode)) {
        return RADIO_TX_RX_MODE;
    }
    if (!codeplug_frequency_can_transmit(c->tx_freq) || !in_band_limits(c->tx_freq)) {
        return RADIO_TX_OUT_OF_BAND;
    }
    return RADIO_TX_OK;
}

// For the UI: the channel asked for (the radio task checks the one it tunes when the PTT comes)
radio_tx_check_t radio_tx_check(void)
{
    return tx_check(&rs.requested, rs.requested_valid);
}

void radio_tx_signal(uint16_t hz1, uint16_t hz2)
{
    signal_wanted[0] = hz1;
    signal_wanted[1] = hz2;
    radio_wake();
}
