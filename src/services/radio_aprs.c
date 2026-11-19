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
 * OpenDMR, service: the radio, the APRS beacon (radio.h; radio_internal.h). aprs.c builds the frame and decides where
 * it goes; here the request is checked and recorded, and the state machine (radio_fsm.c) sends it: on the channel, or
 * on the beacon's own frequency (FM, tuned for that frame only, then back to the user's channel), after listening for
 * a free channel (LISTEN), with the PTT and a channel change first.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "fd6818.h"
#include "systime.h"
#include "hotspot.h"
#include "radio_internal.h"

/* The frame as AFSK, as aprs.c aprsSendPacket() / aprsBitStreamSender() of the main firmware: the FD6818 Tone1
   switched between 1200 and 2200 Hz (300 baud: 1600 / 1800 Hz) at each bit, timed on the microsecond clock */

#define APRS_TX_DELAY_MS   300  // carrier before the frame: with 50 ms the first beacon after a start or a channel
                                // change was cut before the tones settled (Pablog5395, #35)
#define APRS_TONE_GAIN     66   // the UV-K5 firmware's tone level
#define APRS_LISTEN_MS     2000 // a busy channel: given up after this long (aprsBeaconingSendBeacon())
#define APRS_LISTEN_DIGITAL_MS 400 // tuned away from a digital channel: it is deaf meanwhile, wait briefly
#define APRS_SETTLE_MS     300  // tuned away: the noise reading settles before the busy check

static const uint8_t *aprs_bits; // the frame asked for (radio_aprs_send())
static uint16_t aprs_bit_count;
static bool aprs_baud300;
static uint32_t aprs_frequency;          // 0: on the channel; else the beacon's own frequency
static volatile bool aprs_active = false; // asked, not done yet: radio_aprs_busy()
static volatile bool aprs_asked = false;  // for the machine (RFSM_BEACON_REQ)
static volatile bool aprs_sent;
static volatile uint16_t aprs_id;

// Tuned away for the beacon: the user's channel kept here meanwhile
static bool tuned;
static bool tuned_from_digital; // the user's channel was DMR / P25 / NXDN: a short busy wait
static uint32_t tuned_ms;
static codeplug_channel_t saved;

/* The API (radio.h) */

bool radio_aprs_send(const uint8_t *bits, uint16_t bit_count, bool baud300, uint32_t frequency,
                     const codeplug_channel_t *channel)
{
    codeplug_channel_t c;
    bool accepted = false;

    // Not now: the radio busy, the channel not the one aprs.c checked, receive only
    if (rs.transmitting || rs.tx_wanted || rs.channel_pending || rs.scanning || hotspot_active() || !channel ||
        !radio_get_channel(&c) || (memcmp(channel, &c, sizeof(c)) != 0) || (c.flag4 & FLAG4_RX_ONLY)) {
        return false;
    }
    // Where it goes must be able to transmit: the beacon's frequency, or the channel itself (FM only)
    if (frequency != 0) {
        if (!tx_frequency_ok(frequency)) {
            return false;
        }
    } else if ((c.mode != CODEPLUG_MODE_FM) || !tx_frequency_ok(c.tx_freq)) {
        return false;
    }
    taskENTER_CRITICAL();
    if (!aprs_active) {
        aprs_bits = bits;
        aprs_bit_count = bit_count;
        aprs_baud300 = baud300;
        aprs_frequency = frequency;
        aprs_id = (uint16_t)(((aprs_id + 1) & RADIO_ID_MASK) | RADIO_ID_APRS);
        aprs_active = true;
        aprs_asked = true;
        accepted = true;
    }
    taskEXIT_CRITICAL();
    if (accepted) {
        radio_wake();
    }
    return accepted;
}

bool radio_aprs_busy(void)
{
    return aprs_active;
}

bool radio_aprs_sent(void)
{
    return aprs_sent;
}

/* For the radio task (radio.c) */

// A frame asked for since the last call: its request id for the machine (RFSM_BEACON_REQ)
bool aprs_take_request(uint16_t *id)
{
    if (!aprs_asked) {
        return false;
    }
    aprs_asked = false;
    *id = aprs_id;
    return true;
}

// The machine's outcome of the request id (an older one's is left)
void aprs_finished(uint16_t id, bool sent)
{
    if (id == aprs_id) {
        aprs_sent = sent;
        aprs_active = false;
    }
}

// The beacon may start: on its own frequency from a channel in any mode (checked again once tuned there: the FM
// channel then), else on an FM channel
bool aprs_can_start(void)
{
    return (aprs_frequency != 0) || (is_fm() && !rs.dmr_on);
}

// LISTEN begins: on the beacon's own frequency, the radio tuned there (FM, the channel's width, no tones, no VOX); the
// user's channel kept, the UI still showing it
void aprs_tune(void)
{
    static codeplug_channel_t beacon; // not on the radio task's stack

    if ((aprs_frequency == 0) || !rs.has_channel) {
        return;
    }
    tuned_from_digital = !is_analog();
    memcpy(&saved, &rs.channel, sizeof(saved));
    memcpy(&beacon, &rs.channel, sizeof(beacon));
    beacon.rx_freq = aprs_frequency;
    beacon.tx_freq = aprs_frequency;
    beacon.mode = CODEPLUG_MODE_FM;
    beacon.flag4 &= (uint8_t)~(FLAG4_VOX | FLAG4_RX_ONLY);
    beacon.rx_tone = CODEPLUG_CSS_NONE;
    beacon.tx_tone = CODEPLUG_CSS_NONE;
    squelch_close(); // a digital channel leaves the engine's state: the busy check starts clean
    tune_channel(&beacon, true);
    tuned = true;
    tuned_ms = systime_get_ms();
}

// Back on the user's channel (after the beacon, or given up); a channel asked meanwhile is applied after this
void aprs_restore(void)
{
    if (!tuned) {
        return;
    }
    tuned = false;
    tune_channel(&saved, true);
}

bool aprs_tuned_away(void)
{
    return tuned;
}

// Someone on the beacon's own frequency (tuned away): busy until the noise reading has settled, then the noise
// against the squelch level, without the squelch opening the speaker there (a digital user would hear the packet
// noise)
bool aprs_carrier(void)
{
    if ((systime_get_ms() - tuned_ms) < APRS_SETTLE_MS) {
        return true;
    }
    return rx_noise_carrier();
}

// How long the beacon listens for a free channel
uint32_t aprs_listen_ms(void)
{
    if (!tuned) {
        return APRS_LISTEN_MS;
    }
    return APRS_SETTLE_MS + (tuned_from_digital ? APRS_LISTEN_DIGITAL_MS : APRS_LISTEN_MS);
}

// The frame, the carrier on already (the machine's transmit entry): APRS_TX_DELAY_MS of carrier, then the bits
void aprs_frame(void)
{
    uint32_t baud = aprs_baud300 ? 300 : 1200;
    uint16_t mark = aprs_baud300 ? 1600 : 1200;
    uint16_t space = aprs_baud300 ? 1800 : 2200;
    uint16_t last;
    uint32_t start;

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
}
