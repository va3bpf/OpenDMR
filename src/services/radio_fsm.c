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
 * OpenDMR, service: the radio task's state machine (radio_fsm.h). Every state handles every event (the transition
 * table of radio_task.html 6.2): run it, defer it, refuse it (the requester told why), ignore it (counted when it
 * came late), or a fault (back to receiving, safe). Deferred work waits for RX: the channel, then the PTT, then the
 * requests in their order, then the scan; handle() never drains (no recursion), rfsm_ptt() / rfsm_event() do after.
 */

#include <stddef.h>
#include "radio_fsm.h"

static const char *const NAMES[RFSM_STATES] = {"rx", "scan", "tx_voice", "tx_ending", "beacon_listen", "beacon_send",
                                               "data_send"};

const char *rfsm_state_name(rfsm_state_t state)
{
    return (state < RFSM_STATES) ? NAMES[state] : "?";
}

void rfsm_init(rfsm_t *m, const rfsm_ops_t *ops)
{
    *m = (rfsm_t){0};
    m->ops = ops;
    m->state = RFSM_RX;
}

static void enqueue(rfsm_t *m, rfsm_tx_kind_t kind, uint16_t id)
{
    if (m->queued >= RFSM_QUEUE) {
        m->ops->notify(id, RFSM_QUEUE_FULL);
        return;
    }
    m->queue[m->queued].kind = kind;
    m->queue[m->queued].id = id;
    m->queued++;
}

static bool elapsed(const rfsm_t *m, uint32_t ms)
{
    return (uint32_t)(m->ops->now_ms() - m->since_ms) >= ms;
}

// RX: a beacon starts by listening on its frequency; a burst only on a DMR channel
static void start_beacon(rfsm_t *m, uint16_t id)
{
    if (!m->ops->tx_allowed(RFSM_TX_KIND_BEACON, id)) {
        m->ops->notify(id, RFSM_REFUSED);
        return;
    }
    m->current = id;
    m->ops->beacon_tune(id);
    m->since_ms = m->ops->now_ms();
    m->state = RFSM_BEACON_LISTEN;
}

static void start_data(rfsm_t *m, uint16_t id)
{
    if (!m->ops->dmr_channel() || !m->ops->tx_allowed(RFSM_TX_KIND_DATA, id)) {
        m->ops->notify(id, RFSM_REFUSED);
        return;
    }
    m->current = id;
    m->ops->tx_begin(RFSM_TX_KIND_DATA);
    m->ops->data_send(id);
    m->since_ms = m->ops->now_ms();
    m->state = RFSM_DATA_SEND;
}

static void ptt_press(rfsm_t *m)
{
    m->ptt_handled = true;
    if (m->ops->tx_allowed(RFSM_TX_KIND_VOICE, 0)) {
        m->ops->tx_begin(RFSM_TX_KIND_VOICE);
        m->state = RFSM_TX_VOICE;
    } else {
        m->ops->ptt_refused();
    }
}

// The beacon given up while listening (the PTT, another channel, the scan, its result)
static void beacon_off(rfsm_t *m, rfsm_result_t result)
{
    m->ops->notify(m->current, result);
    m->ops->restore();
    m->state = RFSM_RX;
}

// Waiting in the transmitting states: the requests queued, the channel and the scan kept for later
static bool deferred(rfsm_t *m, rfsm_event_t event, uint16_t id)
{
    switch (event) {
        case RFSM_CHANNEL:
            m->channel_deferred = true;
            return true;
        case RFSM_BEACON_REQ:
            enqueue(m, RFSM_TX_KIND_BEACON, id);
            return true;
        case RFSM_DATA_REQ:
            enqueue(m, RFSM_TX_KIND_DATA, id);
            return true;
        default:
            return false;
    }
}

static void handle(rfsm_t *m, rfsm_event_t event, uint16_t id)
{
    const rfsm_ops_t *ops = m->ops;

    if (event == RFSM_SCAN_OFF) {
        m->scan_deferred = false; // a scan asked for and given up before it began
    }
    switch (m->state) {
        case RFSM_RX:
            switch (event) {
                case RFSM_PTT_DOWN: ptt_press(m); break;
                case RFSM_CHANNEL: ops->apply_channel(); break;
                case RFSM_SCAN_ON:
                    ops->scan(true);
                    m->state = RFSM_SCAN;
                    break;
                case RFSM_BEACON_REQ: start_beacon(m, id); break;
                case RFSM_DATA_REQ: start_data(m, id); break;
                case RFSM_TICK: ops->rx_work(); break;
                case RFSM_TX_DONE:
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT: m->faults++; break;
                default: break; // PTT_UP, SCAN_OFF
            }
            break;

        case RFSM_SCAN:
            switch (event) {
                case RFSM_PTT_DOWN:
                    ops->scan(false);
                    m->state = RFSM_RX;
                    ptt_press(m);
                    break;
                case RFSM_CHANNEL: ops->apply_channel(); break;
                case RFSM_SCAN_OFF:
                    ops->scan(false);
                    m->state = RFSM_RX;
                    break;
                case RFSM_BEACON_REQ:
                case RFSM_DATA_REQ: ops->notify(id, RFSM_SCANNING); break;
                case RFSM_TICK: ops->scan_work(); break;
                case RFSM_TX_DONE:
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT:
                    m->faults++;
                    ops->scan(false);
                    m->state = RFSM_RX;
                    break;
                default: break; // PTT_UP, SCAN_ON
            }
            break;

        case RFSM_TX_VOICE:
        case RFSM_TX_ENDING:
            if (deferred(m, event, id)) {
                break;
            }
            switch (event) {
                case RFSM_PTT_UP:
                    if (m->state == RFSM_TX_VOICE) {
                        ops->tx_ending();
                        m->state = RFSM_TX_ENDING;
                    }
                    break;
                case RFSM_SCAN_ON: ops->scan(false); break; // refused: the scan told it is off
                case RFSM_TICK:
                    if (m->state == RFSM_TX_VOICE) {
                        ops->tx_work();
                    } else {
                        ops->ending_work();
                    }
                    break;
                case RFSM_TX_DONE: // the tail over, or the backend ended by itself (a repeater that didn't answer)
                    ops->tx_end();
                    m->state = RFSM_RX;
                    break;
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT:
                    m->faults++;
                    ops->tx_end();
                    m->state = RFSM_RX;
                    break;
                default: break; // PTT_DOWN (TX_VOICE: sending already; TX_ENDING: its level waits), SCAN_OFF
            }
            break;

        case RFSM_BEACON_LISTEN:
            switch (event) {
                case RFSM_PTT_DOWN: beacon_off(m, RFSM_ABANDONED); break; // the PTT then, from RX
                case RFSM_CHANNEL:
                    m->channel_deferred = true;
                    beacon_off(m, RFSM_ABANDONED);
                    break;
                case RFSM_SCAN_ON:
                    m->scan_deferred = true;
                    beacon_off(m, RFSM_ABANDONED);
                    break;
                case RFSM_BEACON_REQ: enqueue(m, RFSM_TX_KIND_BEACON, id); break;
                case RFSM_DATA_REQ: enqueue(m, RFSM_TX_KIND_DATA, id); break;
                case RFSM_TICK:
                    if (!ops->carrier()) {
                        if (ops->tx_allowed(RFSM_TX_KIND_BEACON, m->current)) {
                            ops->tx_begin(RFSM_TX_KIND_BEACON);
                            ops->beacon_send(m->current);
                            m->state = RFSM_BEACON_SEND;
                        } else {
                            beacon_off(m, RFSM_REFUSED);
                        }
                    } else if (elapsed(m, RFSM_LISTEN_MS)) {
                        beacon_off(m, RFSM_BUSY);
                    }
                    break;
                case RFSM_TX_DONE:
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT:
                    m->faults++;
                    beacon_off(m, RFSM_FAILED);
                    break;
                default: break; // PTT_UP, SCAN_OFF
            }
            break;

        case RFSM_BEACON_SEND:
            if (deferred(m, event, id)) {
                break;
            }
            switch (event) {
                case RFSM_SCAN_ON: m->scan_deferred = true; break;
                case RFSM_TX_DONE:
                    ops->tx_end();
                    beacon_off(m, RFSM_SENT);
                    break;
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT:
                    m->faults++;
                    ops->tx_end();
                    beacon_off(m, RFSM_FAILED);
                    break;
                default: break; // PTT_DOWN (its level waits), PTT_UP, SCAN_OFF, TICK (none while sending)
            }
            break;

        case RFSM_DATA_SEND:
            if (deferred(m, event, id)) {
                break;
            }
            switch (event) {
                case RFSM_SCAN_ON: m->scan_deferred = true; break;
                case RFSM_TICK:
                    if (elapsed(m, RFSM_DATA_MS)) {
                        handle(m, RFSM_FAULT, 0);
                    }
                    break;
                case RFSM_TX_DONE: m->ignored_late++; break;
                case RFSM_DATA_DONE:
                    if (id != m->current) {
                        m->ignored_late++; // another burst's
                        break;
                    }
                    ops->tx_end();
                    ops->notify(m->current, RFSM_SENT);
                    m->state = RFSM_RX;
                    break;
                case RFSM_FAULT:
                    m->faults++;
                    ops->data_clear();
                    ops->tx_end();
                    ops->notify(m->current, RFSM_FAILED);
                    m->state = RFSM_RX;
                    break;
                default: break; // PTT_DOWN (its level waits), PTT_UP, SCAN_OFF
            }
            break;

        default:
            m->faults++;
            m->state = RFSM_RX;
            break;
    }
}

// Back in RX: what waited, in this order, until something leaves RX again
static void drain(rfsm_t *m)
{
    while (m->state == RFSM_RX) {
        if (m->channel_deferred) {
            m->channel_deferred = false;
            m->ops->apply_channel();
        } else if (m->ptt_wanted && !m->ptt_handled) {
            ptt_press(m);
        } else if (m->queued > 0) {
            rfsm_request_t r = m->queue[0];

            for (uint8_t i = 1; i < m->queued; i++) {
                m->queue[i - 1] = m->queue[i];
            }
            m->queued--;
            handle(m, (r.kind == RFSM_TX_KIND_BEACON) ? RFSM_BEACON_REQ : RFSM_DATA_REQ, r.id);
        } else if (m->scan_deferred) {
            m->scan_deferred = false;
            handle(m, RFSM_SCAN_ON, 0);
        } else {
            break;
        }
    }
}

void rfsm_ptt(rfsm_t *m, bool down)
{
    m->ptt_wanted = down;
    if (down && !m->ptt_handled) {
        handle(m, RFSM_PTT_DOWN, 0);
    } else if (!down && m->ptt_handled) {
        m->ptt_handled = false;
        handle(m, RFSM_PTT_UP, 0);
    }
    drain(m);
}

void rfsm_event(rfsm_t *m, rfsm_event_t event, uint16_t id)
{
    if (event == RFSM_PTT_DOWN) {
        rfsm_ptt(m, true);
        return;
    }
    if (event == RFSM_PTT_UP) {
        rfsm_ptt(m, false);
        return;
    }
    handle(m, event, id);
    drain(m);
}
