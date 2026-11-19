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
 * OpenDMR, service: the radio's parts, what they share. radio.c: the task, the state machine's calls (radio_fsm.h),
 * the API; radio_rx.c: receive (the channel, the squelch, the S meter, the audio switch and amplifier, scanning);
 * radio_tx.c: transmit (the power, FM on and off, its tones, the four backends, the checks); radio_rf.c: the DMR
 * transmitter's task (bursts, PA ramp); radio_vox.c; radio_eco.c: receive power saving; radio_aprs.c: the AFSK frame.
 * For those files only; the rest of the firmware uses radio.h.
 */

#ifndef RADIO_INTERNAL_H
#define RADIO_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug.h"
#include "radio.h"
#include "radio_fsm.h"

#define SAMPLE_MS 25 // the task's pass

#define PIN_LNA_VHF   0x22 // R5_V_SW
#define PIN_LNA_UHF   0x24 // R5_U_SW
#define PIN_PA_VHF    0x37
#define PIN_PA_UHF    0x23
#define PIN_AUDIO_SW  0x2F // the audio switch ("PWM/FD6818"): high in FM, the speaker straight from the FD6818 (stock)
#define PIN_AUDIO_AMP 0x35

#define UHF_FROM  28000000 // 10 Hz: the UHF band of the radio (RADIO_HARDWARE_FREQUENCY_BANDS)
#define B220_FROM 20000000

#define FLAG4_VOX     0x40
#define FLAG4_RX_ONLY 0x04
#define FLAG4_WIDE    0x02

// A transmission's backend (radio_tx.c: the channel's, the beacon's; radio_fsm.h: the transmit entry and exit)
typedef struct {
    bool (*can_start)(void);
    void (*start)(void);    // transmitting from here, the UI told
    void (*stop)(void);     // the PTT released
    bool (*sending)(void);  // still sending after stop()
    void (*finished)(void); // not transmitting any more, the UI told
} tx_backend_t;

// The state the parts share (the rest is each part's own)
typedef struct {
    // What the UI asks (radio.c's API), read by the task
    codeplug_channel_t requested;
    volatile bool requested_valid;
    volatile bool channel_pending;
    volatile bool tx_wanted;
    volatile bool tx_plain; // no start / end tones nor MDC1200 (a DTMF sequence)
    // What the task uses
    codeplug_channel_t channel;
    bool has_channel;
    bool dmr_on;  // the modem and the FD6818 are in DMR
    bool p25_on;  // the P25 receiver runs (p25_radio.c)
    bool nxdn_on; // the NXDN receiver runs (nxdn_radio.c)
    volatile bool transmitting;
    // The backend of each kind of transmission: the PTT's the channel's (apply_channel(), never while transmitting),
    // the beacon's set once (radio_init()), none for data bursts yet
    const tx_backend_t *tx[RFSM_TX_KINDS];
    // Receive
    volatile bool carrier;  // the noise under the squelch level
    volatile bool speaker;  // the audio on (the carrier, and the tone if one is set)
    volatile bool scanning; // radio_set_scanning(): no receive beeps
    // The speaker amplifier: on for the received audio, a beep (sound.c) or the FM broadcast receiver (its audio goes
    // straight to the amplifier), off when none needs it
    volatile bool amp_for_audio;
    volatile bool amp_for_beep;
    volatile bool amp_for_broadcast;
} radio_shared_t;

extern radio_shared_t rs;

// radio.c
void radio_wake(void); // a request recorded: the task looks at once

// Request ids (the machine's results go back by id): the high bits tell whose
#define RADIO_ID_MASK 0x3FFF
#define RADIO_ID_APRS 0x4000
#define RADIO_ID_DATA 0x8000

// radio_rx.c
bool is_fm(void);
bool is_analog(void);
bool is_wide(void);
void post_signal(void);
void lna(bool on, uint32_t frequency);
void squelch_close(void);
void receive_on(void);
void apply_channel(void);
void tune_channel(const codeplug_channel_t *next, bool valid);
bool rx_noise_carrier(void);
void rx_settle(uint32_t ms);
void rx_beep_cancel(void);
void rx_beep_end_check(void);
void squelch_check(void);
void dmr_signal(void);
bool scan_sampling(void);
void scan_sample(void);

// radio_tx.c
const tx_backend_t *tx_backend_of(const codeplug_channel_t *c, bool valid);
const tx_backend_t *tx_backend_beacon(void);
const tx_backend_t *tx_backend_data(void);
bool tx_frequency_ok(uint32_t frequency);
uint16_t level_drive(uint32_t frequency, int level);
uint16_t pa_drive(uint32_t frequency);
void transmit_on(void);
void transmit_off(void);
void tx_signal_update(void);
radio_tx_check_t tx_check(const codeplug_channel_t *c, bool valid);

// radio_rf.c
void rf_start(void);
void rf_set_drive(uint16_t drive);

// radio_vox.c
void vox_setup(void);
void vox_pass(void);
bool vox_running(void);

// radio_eco.c
void eco_reset(void);
void eco_tick(void);
bool eco_rx_off(void);

// radio_aprs.c
void aprs_frame(void);
bool aprs_take_request(uint16_t *id);
void aprs_finished(uint16_t id, bool sent);
bool aprs_can_start(void);
void aprs_tune(void);
void aprs_restore(void);
bool aprs_tuned_away(void);
bool aprs_carrier(void);
uint32_t aprs_listen_ms(void);

// radio_data.c
bool data_take_request(uint16_t *id);
void data_finished(uint16_t id, bool sent);
void data_start(void);
bool data_ended(void);
bool data_failed(void);
void data_cancel(void);
uint32_t data_listen_ms(void);

#endif
