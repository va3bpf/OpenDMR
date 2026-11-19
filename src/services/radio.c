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

/* The state machine (radio_fsm.h, the OpenDM32 notes' radio_task.html): it runs the PTT, the channel changes and the
   beacon; what it asks and does, here. The checks are on the channel tuned (applied before a PTT, a deferred one
   first). */

static rfsm_t fsm;
static rfsm_tx_kind_t tx_kind; // of the transmission on
static bool beacon_done;       // the frame sent: the machine told (RFSM_TX_DONE) after its call

static bool fsm_tx_allowed(rfsm_tx_kind_t kind, uint16_t id)
{
    (void)id;
    if (tx_check(&rs.channel, rs.has_channel) != RADIO_TX_OK) {
        return false;
    }
    if (kind == RFSM_TX_KIND_VOICE) {
        return tx_backend()->can_start();
    }
    return (kind == RFSM_TX_KIND_BEACON) && is_fm() && !rs.dmr_on; // AFSK on an FM channel; no data bursts yet
}

static bool fsm_dmr_channel(void)
{
    return rs.dmr_on;
}

static bool fsm_carrier(void)
{
    return rs.carrier || rs.speaker;
}

static void fsm_tx_begin(rfsm_tx_kind_t kind)
{
    tx_kind = kind;
    if (kind == RFSM_TX_KIND_VOICE) {
        tx_backend()->start();
    } else if (kind == RFSM_TX_KIND_BEACON) {
        transmit_on();
    }
}

static void fsm_tx_ending(void)
{
    tx_backend()->stop();
}

static void fsm_tx_end(void)
{
    if (tx_kind == RFSM_TX_KIND_VOICE) {
        tx_backend()->finished();
    } else if (tx_kind == RFSM_TX_KIND_BEACON) {
        transmit_off();
    }
}

static void fsm_apply_channel(void)
{
    apply_channel();
}

static void fsm_beacon_send(uint16_t id)
{
    (void)id;
    aprs_frame();
    beacon_done = true;
}

static void fsm_id_nothing(uint16_t id)
{
    (void)id; // the beacon on the channel itself (its own frequency: #35); no data bursts yet
}

static void fsm_nothing(void)
{
    // the PTT refused: the transmit screen shows why (radio_tx_check()); nothing to restore; the ticks' work still
    // in the loop
}

static void fsm_notify(uint16_t id, rfsm_result_t result)
{
    aprs_finished(id, result == RFSM_SENT);
}

static const rfsm_ops_t FSM_OPS = {
    .tx_allowed = fsm_tx_allowed,
    .dmr_channel = fsm_dmr_channel,
    .carrier = fsm_carrier,
    .now_ms = systime_get_ms,
    .tx_begin = fsm_tx_begin,
    .tx_ending = fsm_tx_ending,
    .tx_end = fsm_tx_end,
    .ptt_refused = fsm_nothing,
    .apply_channel = fsm_apply_channel,
    .beacon_tune = fsm_id_nothing,
    .beacon_send = fsm_beacon_send,
    .restore = fsm_nothing,
    .data_send = fsm_id_nothing,
    .data_clear = fsm_nothing,
    .rx_work = fsm_nothing,
    .scan_work = fsm_nothing,
    .tx_work = fsm_nothing,
    .ending_work = fsm_nothing,
    .notify = fsm_notify,
};

// A pass of the machine: the scan's level (the UI's), the requests recorded, the PTT's level, the tick, then what
// came of them
static void fsm_pass(void)
{
    uint16_t id;

    rfsm_scan(&fsm, rs.scanning);
    if (aprs_take_request(&id)) {
        rfsm_event(&fsm, RFSM_BEACON_REQ, id);
    }
    rfsm_ptt(&fsm, rs.tx_wanted);
    rfsm_event(&fsm, RFSM_TICK, 0);
    if (beacon_done) {
        beacon_done = false;
        rfsm_event(&fsm, RFSM_TX_DONE, 0);
    }
    if ((fsm.state == RFSM_TX_ENDING) && !tx_backend()->sending()) {
        rfsm_event(&fsm, RFSM_TX_DONE, 0);
    }
}

static void RAM_CODE radio_task(void *arg)
{
    TickType_t full_pass = 0;

    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(scan_sampling() ? SCAN_SAMPLE_MS : SAMPLE_MS));

        if (rs.channel_pending) {
            rfsm_event(&fsm, RFSM_CHANNEL, 0); // now, or after a transmission (the machine)
        }
        if (scan_sampling()) {
            scan_sample();
            // the rest every SAMPLE_MS as when not scanning (the S meter takes the peak of its last readings)
            if ((xTaskGetTickCount() - full_pass) < pdMS_TO_TICKS(SAMPLE_MS)) {
                continue;
            }
        }
        full_pass = xTaskGetTickCount();
        fsm_pass();
        tx_signal_update();
        vox_pass();
        if (eco_rx_off()) {
            // the receiver off (power saving): nothing to read
        } else if (is_analog() && !rs.transmitting) {
            squelch_check();
        } else if (rs.dmr_on) {
            dmr_signal();
        }
        rx_beep_end_check();
        eco_tick();
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
