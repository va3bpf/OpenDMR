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

// The radio task's state machine (radio_task.html of the OpenDM32 notes, section 6): who has the transceiver now. No
// hardware and no OS here: the radio task gives it events and answers its questions, and does what it asks, through
// rfsm_ops_t; built on the PC too (tools/radio_fsm), every state against every event.
#ifndef RADIO_FSM_H
#define RADIO_FSM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RFSM_RX,            // receiving (the backends and the squelch, VOX, power saving in the ticks)
    RFSM_SCAN,          // scanning
    RFSM_TX_VOICE,      // the PTT: transmitting
    RFSM_TX_ENDING,     // the PTT released: end tones, tail, the digital backend finishing
    RFSM_BEACON_LISTEN, // on the beacon's frequency, listening for a free channel
    RFSM_BEACON_SEND,   // sending the beacon
    RFSM_DATA_SEND,     // a DMR data burst
    RFSM_STATES
} rfsm_state_t;

typedef enum {
    RFSM_PTT_DOWN, // made by rfsm_ptt() from the PTT's level
    RFSM_PTT_UP,
    RFSM_CHANNEL,  // another channel wanted (the radio task keeps which: the latest)
    RFSM_SCAN_ON,
    RFSM_SCAN_OFF,
    RFSM_BEACON_REQ, // with the request's id
    RFSM_DATA_REQ,   // with the request's id
    RFSM_TICK,       // the heartbeat
    RFSM_TX_DONE,    // the transmission finished: the tail, the digital backend, the beacon's frame
    RFSM_DATA_DONE,  // the data burst finished (with its id)
    RFSM_FAULT,      // a repeater that didn't wake, a backend error, a timeout
    RFSM_EVENTS
} rfsm_event_t;

// What a request came to (rfsm_ops_t.notify)
typedef enum {
    RFSM_SENT,
    RFSM_REFUSED,    // the transmit check failed (Rx only, mode, band), or not a DMR channel for data
    RFSM_SCANNING,   // refused while scanning
    RFSM_QUEUE_FULL, // refused: too many waiting
    RFSM_ABANDONED,  // given up for the PTT, another channel or the scan
    RFSM_BUSY,       // the channel stayed busy
    RFSM_FAILED      // a fault while sending
} rfsm_result_t;

typedef enum { RFSM_TX_KIND_VOICE, RFSM_TX_KIND_BEACON, RFSM_TX_KIND_DATA } rfsm_tx_kind_t;

#define RFSM_LISTEN_MS  2000 // the beacon gives up on a busy channel after this long
#define RFSM_DATA_MS    2000 // a data burst taking longer is a fault
#define RFSM_QUEUE      4    // requests waiting (beacons and data bursts)

// The radio task's side: the questions (asked when deciding, so always current), then the actions (in the order
// the machine calls them; none may call rfsm_*() back)
typedef struct {
    bool (*tx_allowed)(rfsm_tx_kind_t kind, uint16_t id); // the check on the channel tuned (a beacon: its frequency)
    bool (*dmr_channel)(void);
    bool (*carrier)(void); // a signal on the frequency tuned
    uint32_t (*now_ms)(void);

    void (*tx_begin)(rfsm_tx_kind_t kind); // the transmit entry: the drive, the LED, transmitting, the UI told
    void (*tx_ending)(void);               // the PTT released: the end tones, the tail, the backend asked to stop
    void (*tx_end)(void);                  // the transmit exit: off, the LED, the UI told
    void (*ptt_refused)(void);             // the PTT pressed on a channel that can't transmit
    void (*apply_channel)(void);           // the channel wanted now
    void (*scan)(bool on);
    void (*beacon_tune)(uint16_t id); // the user's channel kept, the beacon's frequency tuned
    void (*beacon_send)(uint16_t id); // the frame (its end: RFSM_TX_DONE)
    void (*restore)(void);            // back on the user's channel
    void (*data_send)(uint16_t id);   // the burst (its end: RFSM_DATA_DONE)
    void (*data_clear)(void);         // the DMR engine's data state cleared after a fault
    void (*rx_work)(void);            // a receiving tick: squelch, S meter, VOX, power saving
    void (*scan_work)(void);          // a scanning tick
    void (*tx_work)(void);            // a transmitting tick: DTMF / tones
    void (*ending_work)(void);        // a tick while ending: the tail's time, the backend asked
    void (*notify)(uint16_t id, rfsm_result_t result);
} rfsm_ops_t;

typedef struct {
    rfsm_tx_kind_t kind;
    uint16_t id;
} rfsm_request_t;

typedef struct {
    const rfsm_ops_t *ops;
    rfsm_state_t state;
    bool ptt_wanted;       // the PTT's level
    bool ptt_handled;      // its press acted on (sent, refused): no event again until it is released
    bool channel_deferred; // a channel wanted while transmitting
    bool scan_deferred;    // the scan asked for while sending
    rfsm_request_t queue[RFSM_QUEUE];
    uint8_t queued;
    uint16_t current; // the id of the beacon or burst in progress
    uint32_t since_ms; // when listening or the burst started
    // seen only in the counters: events that came late or never should (none in normal use)
    uint32_t ignored_late, faults;
} rfsm_t;

void rfsm_init(rfsm_t *m, const rfsm_ops_t *ops);
void rfsm_ptt(rfsm_t *m, bool down);                     // the PTT's level: its press or release when it changes
void rfsm_event(rfsm_t *m, rfsm_event_t event, uint16_t id); // id: of a request, a DATA_DONE; else 0
const char *rfsm_state_name(rfsm_state_t state);

#endif
