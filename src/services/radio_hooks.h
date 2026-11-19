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

#ifndef DM32_RADIO_HOOKS_H
#define DM32_RADIO_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

/* The radio's hooks for the firmware's lower services, not for the application (radio.h): the DMR engine, P25 and
   NXDN switch the transmitter and the speaker through them, the sounds and the FM broadcast receiver the audio path,
   the input tells of keys. */

// The DMR engine (its task or its interrupts): its speaker; the transmitter on at the start, each burst on / off,
// receiving at the end
void radio_dmr_audio(bool on);
void radio_dmr_tx_on(void);
void radio_dmr_burst(bool on);
void radio_dmr_rx_on(void);

// The physical layer transmit (p25_phy.c: P25, NXDN): the transmitter on (low_power: 50 mW, else the channel's power)
// and kept on, or back to receiving
void radio_phy_tx(bool on, bool low_power);

// A beep (sound.c): the audio switch to the baseband first, then the amplifier; off the other way round, each
// switched while the amplifier is off (a click otherwise, mostly while transmitting)
void radio_beep_path(bool on);
void radio_beep_amp(bool on);
void radio_broadcast_amp(bool on); // the amplifier for the FM broadcast receiver (fm_broadcast.c)

// The input (input.c) and P25 (a sync): a key, the knob or a call: the receive power saving starts over; a key stops
// the VOX and starts it again later
void radio_eco_activity(void);
void radio_vox_key(bool ptt_held);

const char *radio_state_name(void); // the state machine's state (radio_fsm.h), for the test interface

#endif
