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

// The radio (FM for now): the channel of the home screen received with its squelch and tones, transmitted with the
// PTT. A task of its own owns the FD6818 and the baseband; the UI tells it what to do and gets UI_DATA_SIGNAL events
// (the squelch opened or closed, the shown signal changed). From OpenDM32 functions/trx.c and
// hardware/radioHardwareInterface.c.
//
// The squelch as trx.c trxCheckAnalogSquelch(): every 25 ms the FD6818 noise (0x65) against 70 - 3 x (level - 1),
// level from the channel or the band's default; it opens below and closes 3 over. With a receive tone the speaker
// opens only with the tone, and closes after it is missing 6 times. While it is open the radio task runs from RAM
// only (no flash or LCD bus access), as the main firmware's quiet wait: such accesses are heard.

typedef enum {
    RADIO_TX_OK,
    RADIO_TX_NO_CHANNEL,
    RADIO_TX_RX_ONLY,
    RADIO_TX_OUT_OF_BAND, // not a frequency of the radio, or out of the band limits of the settings
} radio_tx_check_t;

void radio_init(void);  // before the scheduler: the chips set up, receiver off
void radio_start(void); // the radio task

// The channel to use (copied); the same channel again changes nothing. NULL: none (receiver muted)
void radio_set_channel(const codeplug_channel_t *channel);

radio_tx_check_t radio_tx_check(void);
void radio_transmit(bool on); // only after RADIO_TX_OK; the task switches over
void radio_transmit_plain(void); // FM on without the start / end tones and MDC1200 (DTMF); radio_transmit(false) ends
bool radio_is_transmitting(void);
// APRS (aprs.c): the AX.25 frame as NRZI bits (bit 0 of byte 0 first), sent as AFSK on the FM channel by the radio
// task after waiting up to 2 s for a busy channel. false: not now (busy, not FM, transmitting). The bits stay in use
// until radio_aprs_busy() is false; radio_aprs_sent() then tells if it went out.
bool radio_aprs_send(const uint8_t *bits, uint16_t bit_count, bool baud300);
bool radio_aprs_busy(void);
bool radio_aprs_sent(void);
// The VOX (an FM channel with VOX, settings.vox_threshold set) posts PTT presses and releases; a key (from the input
// task) stops it and starts it again later
void radio_vox_key(bool ptt_held);
void radio_eco_activity(void); // a key or the knob (the input task): the receive power saving ends, starts over later
void radio_dmr_audio(bool on); // the DMR engine's speaker (from its task or its interrupts)
// The DMR engine's transmitter (from its timeslot interrupts): on at the start, each burst on / off, receive at the end
void radio_dmr_tx_on(void);
void radio_dmr_burst(bool on);
void radio_dmr_rx_on(void);
void radio_tx_signal(uint16_t hz1, uint16_t hz2); // FM transmit: a tone (1750 Hz) or DTMF instead of the mic; 0, 0: off
#define RADIO_TX_SILENT 0xFFFF // hz1: no tone and no mic (the gaps of the hotspot's CW ID)
int radio_band_of(uint32_t frequency); // the band of the settings' squelch defaults: 0 VHF, 1 220 MHz, 2 UHF
uint32_t radio_tx_frequency(void);
uint32_t radio_rx_frequency(void); // 0: no channel
bool radio_channel_is_dmr(void);
bool radio_get_channel(codeplug_channel_t *channel); // the channel radio_set_channel() was given; false: none

bool radio_has_carrier(void); // FM: a carrier (the noise squelch open)
void radio_set_scanning(bool on); // no receive beeps while scanning (scan.c)
bool radio_is_scanning(void);
bool radio_audio_heard(void); // a received signal plays (FM squelch and tone, or DMR audio)
bool radio_is_receiving(void);  // the squelch is open (the speaker on)
void radio_broadcast_amp(bool on); // the speaker amplifier for the FM broadcast receiver (fm_broadcast.c)
// A beep (sound.c): the audio switch to the baseband first, then the amplifier; off the other way round, each
// switched while the amplifier is off (a switch heard as a click otherwise, mostly while transmitting)
void radio_beep_path(bool on);
void radio_beep_amp(bool on);
uint8_t radio_signal(void);     // RSSI, raw (dBm = raw - 177 on VHF, raw - 167 on UHF), averaged for showing
int radio_signal_dbm(void);
uint8_t radio_signal_raw(void); // the last reading
uint8_t radio_noise(void);      // raw

#endif
