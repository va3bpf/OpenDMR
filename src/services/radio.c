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
 * OpenDMR, service: the radio. The task and the API here; its parts in radio_rx.c, radio_tx.c, radio_rf.c,
 * radio_vox.c, radio_eco.c and radio_aprs.c (radio_internal.h).
 *
 * The API records what is asked (a channel, the PTT, a beacon) and wakes the task. The task waits for that or 25 ms;
 * each pass hands the requests to the state machine (radio_fsm.h: it decides, the calls here act), then the receive
 * work: the squelch or the DMR signal, the VOX, the receive beeps, the power saving.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "fd6818.h"
#include "systime.h"
#include "modem.h"
#include "factory_cal.h"
#include "codeplug.h"
#include "dmr.h"
#include "radio_fsm.h"
#include "radio_internal.h"

#define RADIO_TASK_PRIORITY 4
#define SCAN_SAMPLE_MS      5 // scanning a digital channel: the signal read this often (radio_scan_peak())

static StaticTask_t task_tcb;
static StackType_t task_stack[256];
static TaskHandle_t task;

radio_shared_t rs;

/* The state machine (radio_fsm.c; the OpenDM32 notes' radio_task.html).

   The machine decides; this file acts. Each pass of radio_task hands the machine what was asked, and the machine
   calls back the functions below (the table FSM_OPS) to start or stop a transmission, change the channel, send the
   beacon. All of it runs in radio_task only, so nothing here needs a lock. */

static rfsm_t fsm;

// The backend of the transmission on: chosen when it starts (fsm_tx_begin()), used to stop and end it
static const tx_backend_t *tx_on;

// Set when the beacon frame has gone out (fsm_beacon_send()); fsm_pass() then tells the machine RFSM_TX_DONE
static bool beacon_done;

/* The machine's questions */

// May this kind of transmission start? The checks on the channel tuned (receive only, mode, band), then its backend's
static bool fsm_tx_allowed(rfsm_tx_kind_t kind, uint16_t id)
{
    (void)id;
    if (tx_check(&rs.channel, rs.has_channel) != RADIO_TX_OK) {
        return false;
    }
    return (rs.tx[kind] != NULL) && rs.tx[kind]->can_start();
}

static bool fsm_dmr_channel(void)
{
    return rs.dmr_on;
}

// Someone on the channel: the beacon waits for it to be free
static bool fsm_carrier(void)
{
    return rs.carrier || rs.speaker;
}

/* The machine's actions */

// A transmission starts, ends (the PTT released: the backend finishes sending), is over
static void fsm_tx_begin(rfsm_tx_kind_t kind)
{
    tx_on = rs.tx[kind];
    tx_on->start();
}

static void fsm_tx_ending(void)
{
    tx_on->stop();
}

static void fsm_tx_end(void)
{
    tx_on->finished();
}

static void fsm_apply_channel(void)
{
    apply_channel();
}

// The beacon frame, sent at once (its timing is strict); the transmitter is already on
static void fsm_beacon_send(uint16_t id)
{
    (void)id;
    aprs_frame();
    beacon_done = true;
}

// The result of a beacon request (sent, busy, refused, ...) for radio_aprs_sent()
static void fsm_notify(uint16_t id, rfsm_result_t result)
{
    aprs_finished(id, result == RFSM_SENT);
}

// The PTT refused: nothing to do, the transmit screen shows why (radio_tx_check())
static void fsm_ptt_refused(void)
{
}

// Not done yet: the beacon on its own frequency and back (step 4, #35), data bursts (step 5)
static void not_yet_id(uint16_t id)
{
    (void)id;
}

static void not_yet(void)
{
}

/* The work of each tick (25 ms), by state: the machine calls receive_work() in RX, SCAN and BEACON_LISTEN,
   transmit_work() in TX_VOICE, TX_ENDING and DATA_SEND (none in BEACON_SEND: the frame goes out in one call) */

// The signal: the squelch in FM / AM / SSB (not while transmitting), the S meter in DMR / P25 / NXDN; nothing while
// the power saving has the receiver off
static void read_signal(void)
{
    if (eco_rx_off()) {
        return;
    }
    if (is_analog() && !rs.transmitting) {
        squelch_check();
    } else if (rs.dmr_on) {
        dmr_signal();
    }
}

// Receiving: the VOX, the signal, the receive end beep, the power saving
static void receive_work(void)
{
    vox_pass();
    read_signal();
    rx_beep_end_check();
    eco_tick();
}

// Transmitting: the tones asked (radio_tx_signal(): DTMF and the like), then as receiving (the VOX releases the PTT,
// the power saving stays off)
static void transmit_work(void)
{
    tx_signal_update();
    receive_work();
}

static const rfsm_ops_t FSM_OPS = {
    // questions
    .tx_allowed = fsm_tx_allowed,
    .dmr_channel = fsm_dmr_channel,
    .carrier = fsm_carrier,
    .now_ms = systime_get_ms,
    // a transmission
    .tx_begin = fsm_tx_begin,
    .tx_ending = fsm_tx_ending,
    .tx_end = fsm_tx_end,
    .ptt_refused = fsm_ptt_refused,
    // the channel
    .apply_channel = fsm_apply_channel,
    // the beacon
    .beacon_tune = not_yet_id,
    .beacon_send = fsm_beacon_send,
    .restore = not_yet,
    // data bursts
    .data_send = not_yet_id,
    .data_clear = not_yet,
    // each tick
    .rx_work = receive_work,
    .scan_work = receive_work,
    .tx_work = transmit_work,
    .ending_work = transmit_work,
    // a request's result
    .notify = fsm_notify,
};

/* One pass of the machine, in three parts, in this order:
   1. what was asked: the scan's level, a beacon request, the PTT's level;
   2. the tick: the state's work (receive_work() / transmit_work(), the beacon listening for a free channel, the data
      timeout);
   3. what came of it: a transmission over.
   The PTT before the tick: a PTT pressed while the beacon listens wins, the beacon is given up before the tick could
   start sending it. The results after the tick: the tick sends the beacon frame, the same pass ends it. */
static void fsm_pass(void)
{
    uint16_t id;

    // 1. what was asked
    rfsm_scan(&fsm, rs.scanning);
    if (aprs_take_request(&id)) {
        rfsm_event(&fsm, RFSM_BEACON_REQ, id);
    }
    rfsm_ptt(&fsm, rs.tx_wanted);

    // 2. the tick
    rfsm_event(&fsm, RFSM_TICK, 0);

    // 3. a transmission over: the beacon frame sent, or the PTT's backend done sending its last frames
    if (beacon_done) {
        beacon_done = false;
        rfsm_event(&fsm, RFSM_TX_DONE, 0);
    }
    if ((fsm.state == RFSM_TX_ENDING) && !tx_on->sending()) {
        rfsm_event(&fsm, RFSM_TX_DONE, 0);
    }
}

/* The task. It sleeps until radio_wake() or SAMPLE_MS (25 ms), then makes a pass, in this order:
   1. a new channel to the machine (applied now, or after a transmission);
   2. scanning a digital channel: a signal reading (see below);
   3. the machine's pass (fsm_pass(), above): the requests, the tick with the state's work (receive_work() /
      transmit_work(): the tones, the VOX, the signal, the receive end beep, the power saving), the results.
   Scanning a digital channel it wakes every SCAN_SAMPLE_MS (5 ms) to read the signal, a DMR burst being on the air
   only 27.5 ms of every 60; step 3 still runs every 25 ms only. */
static void RAM_CODE radio_task(void *arg)
{
    TickType_t last_full_pass = 0;

    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(scan_sampling() ? SCAN_SAMPLE_MS : SAMPLE_MS));

        // 1. a new channel
        if (rs.channel_pending) {
            rfsm_event(&fsm, RFSM_CHANNEL, 0);
        }

        // 2. scanning a digital channel: a reading at every wake, the rest only once SAMPLE_MS has gone by
        if (scan_sampling()) {
            scan_sample();
            if ((xTaskGetTickCount() - last_full_pass) < pdMS_TO_TICKS(SAMPLE_MS)) {
                continue;
            }
        }
        last_full_pass = xTaskGetTickCount();

        // 3. the state machine
        fsm_pass();
    }
}

/* The API */

void radio_wake(void)
{
    if (task) {
        xTaskNotifyGive(task);
    }
}

void radio_init(void)
{
    rs.tx[RFSM_TX_KIND_VOICE] = tx_backend_of(NULL, false);
    rs.tx[RFSM_TX_KIND_BEACON] = tx_backend_beacon();
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
    rfsm_init(&fsm, &FSM_OPS);
    task = xTaskCreateStatic(radio_task, "radio", sizeof(task_stack) / sizeof(task_stack[0]), NULL, RADIO_TASK_PRIORITY,
                             task_stack, &task_tcb);
    rf_start();
    dmr_start();
}

void radio_set_channel(const codeplug_channel_t *new_channel)
{
    bool valid = (new_channel != NULL);

    taskENTER_CRITICAL();
    if ((valid != rs.requested_valid) || (valid && (memcmp(&rs.requested, new_channel, sizeof(rs.requested)) != 0))) {
        if (valid) {
            memcpy(&rs.requested, new_channel, sizeof(rs.requested));
        }
        rs.requested_valid = valid;
        rs.channel_pending = true;
    }
    taskEXIT_CRITICAL();
    if (rs.channel_pending) {
        radio_wake();
    }
}

// The PTT's level, recorded: the machine decides
void radio_transmit(bool on)
{
    if (on) {
        rs.tx_plain = false;
    }
    rs.tx_wanted = on;
    radio_wake();
}

void radio_transmit_plain(void)
{
    rs.tx_plain = true;
    rs.tx_wanted = true;
    radio_wake();
}

// The state machine's state (radio_fsm.h): shown by the test interface
const char *radio_state_name(void)
{
    return rfsm_state_name(fsm.state);
}

bool radio_is_transmitting(void)
{
    return rs.transmitting;
}

uint32_t radio_tx_frequency(void)
{
    return rs.requested.tx_freq;
}

uint32_t radio_rx_frequency(void)
{
    return rs.requested_valid ? rs.requested.rx_freq : 0;
}

bool radio_get_channel(codeplug_channel_t *out)
{
    bool valid;

    taskENTER_CRITICAL();
    valid = rs.requested_valid;
    if (valid) {
        memcpy(out, &rs.requested, sizeof(*out));
    }
    taskEXIT_CRITICAL();
    return valid;
}

bool radio_channel_is_dmr(void)
{
    return rs.requested_valid && (rs.requested.mode == CODEPLUG_MODE_DMR);
}
