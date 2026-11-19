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

#ifndef DM32_RADIO_H
#define DM32_RADIO_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug.h"

/* The radio, for the application (the screens, scan and the dual watch, APRS, the hotspot): it receives on the
   channel it is given and transmits on it when asked. A task of its own owns the chips and decides (radio_fsm.h):
   every function here only records what is asked and returns at once; what came of a beacon or a data burst is asked
   with radio_result(). The UI hears of the signal's changes through UI_DATA_SIGNAL events. The firmware's lower
   services (the DMR engine, P25, NXDN, the sounds, the input) use radio_hooks.h. */

void radio_init(void);  // before the scheduler: the chips set up, the receiver off
void radio_start(void); // the radio task

/* The channel */

// The channel to receive and transmit on (copied); the same channel again changes nothing. NULL: none (muted)
void radio_set_channel(const codeplug_channel_t *channel);
bool radio_get_channel(codeplug_channel_t *channel); // the channel given; false: none
uint32_t radio_rx_frequency(void);                   // 0: no channel
bool radio_channel_is_dmr(void);
int radio_band_of(uint32_t frequency); // the band of the settings' squelch defaults: 0 VHF, 1 220 MHz, 2 UHF

/* Transmitting with the PTT */

typedef enum {
    RADIO_TX_OK,
    RADIO_TX_NO_CHANNEL,
    RADIO_TX_RX_ONLY,
    RADIO_TX_RX_MODE,     // AM, USB, LSB: receive only
    RADIO_TX_OUT_OF_BAND, // not a frequency of the radio, or out of the band limits of the settings
} radio_tx_check_t;

radio_tx_check_t radio_tx_check(void); // can the channel given transmit (the transmit screen shows why not)
// The PTT's level: transmitting while it is on, if the channel can (the radio checks it, and refuses otherwise)
void radio_transmit(bool on);
void radio_transmit_plain(void); // FM without the start / end tones and MDC1200 (DTMF); radio_transmit(false) ends
// FM transmitting: a tone (1750 Hz) or DTMF instead of the mic; 0, 0: the mic again
void radio_tx_signal(uint16_t hz1, uint16_t hz2);
#define RADIO_TX_SILENT 0xFFFF // hz1: no tone and no mic (the gaps of the hotspot's CW ID)
bool radio_is_transmitting(void);

/* Requests: an APRS beacon, a data burst. Each returns its id (0: refused at once); radio_result() then tells what
   came of it. One of each kind at a time; the PTT and a channel change win over them. */

typedef uint16_t radio_request_t;

typedef enum {
    RADIO_PENDING,   // not done yet
    RADIO_SENT,
    RADIO_REFUSED,   // the channel can't (receive only, the mode, the band), or it changed since the request
    RADIO_BUSY,      // the channel stayed busy
    RADIO_ABANDONED, // given up for the PTT or another channel
    RADIO_FAILED,    // a fault while sending; also the result of an old request (another one since)
} radio_result_t;

radio_result_t radio_result(radio_request_t request);

// An APRS frame (aprs.c: the AX.25 frame as NRZI bits, bit 0 of byte 0 first) as AFSK. frequency 0: on the channel
// (an FM channel); else on that frequency (10 Hz units) from a channel in any mode: FM there for the frame, then back
// (the channel shown unchanged). After a free channel (up to 2 s; 0.4 s when it left a DMR / P25 / NXDN channel).
// Refused at once while transmitting, scanning, in hotspot mode, on a receive only channel, on a frequency the radio
// can't transmit on, or with a frame still in progress. The bits stay in use until the result isn't RADIO_PENDING.
radio_request_t radio_aprs_send(const uint8_t *bits, uint16_t bit_count, bool baud300, uint32_t frequency);

// A data burst on the DMR channel (APRS over DMR: an LRRP report). The caller's functions drive the DMR engine; the
// radio calls them in its task, after a free channel (up to 2 s), with the power, the red LED and the transmitting
// state set. The PTT pressed during the burst waits for it.
typedef struct {
    bool (*start)(void);   // hand the burst to the engine; false: it couldn't (RADIO_FAILED)
    bool (*sending)(void); // still being sent
    bool (*failed)(void);  // the engine gave up (a repeater that didn't wake, an engine reset)
    void (*cancel)(void);  // abort it, the engine's data state cleared (after a failure, or 5 s without the end)
} radio_data_t;

// Refused at once: not a DMR channel, receive only, out of band, transmitting, scanning, hotspot, a burst in
// progress. burst stays in use until the result isn't RADIO_PENDING.
radio_request_t radio_data_send(const radio_data_t *burst);

/* Receiving */

bool radio_has_carrier(void);  // FM: a carrier (the noise squelch open)
bool radio_audio_heard(void);  // a received signal plays (FM squelch and tone, or DMR audio)
bool radio_is_receiving(void); // the squelch open or a digital call heard (the screens' green line, S meter)
uint8_t radio_signal(void);    // RSSI, averaged for showing (dBm = value - 177 on VHF, - 167 on UHF)
int radio_signal_dbm(void);
uint8_t radio_signal_raw(void); // the last reading (the RSSI screen)
uint8_t radio_noise(void);      // the last noise reading (the RSSI screen)

/* Scanning (scan.c, the dual watch) */

void radio_set_scanning(bool on); // scanning: no receive beeps, no beacons or bursts, fast readings on DMR
bool radio_is_scanning(void);
// Scanning a digital channel: the strongest reading since the last retune settled, and how many (0: none yet)
uint8_t radio_scan_peak(uint16_t *samples);

#endif
