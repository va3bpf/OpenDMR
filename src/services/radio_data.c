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
 * OpenDMR, service: the radio, data bursts on a DMR channel (radio.h: radio_data_send(); radio_internal.h). The
 * caller's radio_data_t drives the DMR engine (an LRRP position report: aprs_dmr.c); here the request is checked and
 * recorded, and the state machine (radio_fsm.c) sends it: after listening for a free channel (LISTEN), with the PA
 * drive, the LED and the transmitting state of the transmit entry; the PTT waits for the burst, a fault or the
 * timeout (RFSM_DATA_MS) cancels it.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hotspot.h"
#include "radio_internal.h"

#define DATA_LISTEN_MS 2000 // a busy channel: given up after this long

static const radio_data_t *data;  // the burst asked for (radio_data_send())
static uint32_t data_gen;         // the channel's generation when asked: refused once another channel is given
static volatile bool data_asked = false;                 // for the machine (RFSM_DATA_REQ)
static volatile radio_result_t data_res = RADIO_FAILED;  // of the last request; RADIO_PENDING while it runs
static volatile uint16_t data_id;
static bool start_failed; // start() refused: a fault for the machine

/* The API (radio.h) */

radio_request_t radio_data_send(const radio_data_t *burst)
{
    uint32_t gen = rs.channel_gen; // before the channel is read: a change after it refuses the request
    codeplug_channel_t c;
    radio_request_t id = 0;

    // Not now: the radio busy, not a DMR channel that can transmit
    if (!burst || rs.is_transmitting || rs.ptt_down || rs.has_new_channel || rs.is_scanning || hotspot_active() ||
        !radio_get_channel(&c) || (c.mode != CODEPLUG_MODE_DMR) || (tx_check(&c, true) != RADIO_TX_OK)) {
        return 0;
    }
    taskENTER_CRITICAL();
    if (data_res != RADIO_PENDING) {
        data = burst;
        data_gen = gen;
        data_id = (uint16_t)(((data_id + 1) & RADIO_ID_MASK) | RADIO_ID_DATA);
        data_res = RADIO_PENDING;
        data_asked = true;
        id = data_id;
    }
    taskEXIT_CRITICAL();
    if (id != 0) {
        radio_wake();
    }
    return id;
}

/* For the radio task (radio.c) */

// A burst asked for since the last call: its request id for the machine (RFSM_DATA_REQ); refused here when the
// channel changed since it was asked
bool data_take_request(uint16_t *id)
{
    if (!data_asked) {
        return false;
    }
    data_asked = false;
    if (data_gen != rs.channel_gen) {
        data_res = RADIO_REFUSED;
        return false;
    }
    *id = data_id;
    return true;
}

void data_finished(uint16_t id, radio_result_t result)
{
    if (id == data_id) {
        data_res = result;
    }
}

radio_result_t data_result(uint16_t id)
{
    return (id == data_id) ? data_res : RADIO_FAILED;
}

bool data_pending(void)
{
    return data_res == RADIO_PENDING;
}

// DATA_SEND begins (the transmit entry done): the engine takes the burst
void data_start(void)
{
    start_failed = !data->start();
}

// While DATA_SEND: the burst's end (RFSM_DATA_DONE) or its fault (RFSM_FAULT)
bool data_ended(void)
{
    return !start_failed && !data->failed() && !data->sending();
}

bool data_failed(void)
{
    return start_failed || data->failed();
}

// After a fault or the timeout: the engine's data state cleared
void data_cancel(void)
{
    start_failed = false;
    data->cancel();
}

uint32_t data_listen_ms(void)
{
    return DATA_LISTEN_MS;
}
