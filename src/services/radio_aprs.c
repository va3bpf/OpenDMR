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
 * OpenDMR, service: the radio, APRS (radio_internal.h; aprs.c builds the frame): the request recorded, the frame sent
 * when the state machine has the transmitter on.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "fd6818.h"
#include "systime.h"
#include "radio_internal.h"

/* APRS (aprs.c): the AX.25 frame as AFSK on the FM channel, as aprs.c aprsSendPacket() / aprsBitStreamSender() of
   the main firmware: the FD6818 Tone1 switched between 1200 and 2200 Hz (300 baud: 1600 / 1800 Hz) at each bit,
   timed here on the microsecond clock; aprsBeaconingSendBeacon(): a busy channel is waited for up to 2 s (the state
   machine's RFSM_LISTEN_MS, listening without holding the task), then 50 ms of carrier before the frame */

#define APRS_TX_DELAY_MS  50
#define APRS_TONE_GAIN    66 // the UV-K5 firmware's tone level

static const uint8_t *volatile aprs_bits = NULL; // the frame asked for (radio_aprs_send())
static volatile uint16_t aprs_bit_count;
static volatile bool aprs_baud300;
static volatile bool aprs_active = false; // asked, not done yet
static volatile bool aprs_asked = false;  // for the machine (RFSM_BEACON_REQ)
static volatile bool aprs_sent;
static volatile uint16_t aprs_id;

// The frame, the carrier on already (the machine's transmit entry): 50 ms of carrier, then the bits
void aprs_frame(void)
{
    uint32_t baud = aprs_baud300 ? 300 : 1200;
    uint16_t mark = aprs_baud300 ? 1600 : 1200;
    uint16_t space = aprs_baud300 ? 1800 : 2200;
    uint16_t last = 0;
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

// Recorded only: the radio task's machine decides (an FM channel that can transmit, not scanning, the channel free;
// the PTT first), radio_aprs_busy() until then, radio_aprs_sent() its outcome
bool radio_aprs_send(const uint8_t *bits, uint16_t bit_count, bool baud300)
{
    if (aprs_active) {
        return false;
    }
    aprs_bit_count = bit_count;
    aprs_baud300 = baud300;
    aprs_bits = bits;
    aprs_id++;
    aprs_active = true;
    aprs_asked = true;
    radio_wake();
    return true;
}

bool radio_aprs_busy(void)
{
    return aprs_active;
}

bool radio_aprs_sent(void)
{
    return aprs_sent;
}

// A frame asked for since the last call: its request number for the machine (RFSM_BEACON_REQ)
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
