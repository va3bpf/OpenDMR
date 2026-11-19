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
 * requests in their order; handle() never drains (no recursion), rfsm_ptt() / rfsm_scan() / rfsm_event() do after.
 * The scan is the UI's (scan.c stops it on the PTT, the dual watch goes on after its transmission): its level makes
 * receiving SCAN or RX, as the PTT's makes a press, and a transmission goes back to whichever it says then.
 */

#include <stddef.h>
#include "radio_fsm.h"

static const char *const NAMES[RFSM_STATES] = {"rx", "scan", "tx_voice", "tx_ending", "listen", "beacon_send",
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

// Receiving: SCAN while the UI scans, else RX
static rfsm_state_t receiving(const rfsm_t *m)
{
    return m->scan_wanted ? RFSM_SCAN : RFSM_RX;
}

static bool elapsed(const rfsm_t *m, uint32_t ms)
{
    return (uint32_t)(m->ops->now_ms() - m->since_ms) >= ms;
}

// RX: a beacon starts by listening on its frequency; a burst only on a DMR channel; neither while scanning
static void start_beacon(rfsm_t *m, uint16_t id)
{
    if (m->state == RFSM_SCAN) {
        m->ops->notify(id, RFSM_SCANNING);
        return;
    }
    if (!m->ops->tx_allowed(RFSM_TX_KIND_BEACON, id)) {
        m->ops->notify(id, RFSM_REFUSED);
        return;
    }
    m->kind = RFSM_TX_KIND_BEACON;
    m->current = id;
    m->ops->beacon_tune(id);
    m->since_ms = m->ops->now_ms();
    m->state = RFSM_LISTEN;
}

static void start_data(rfsm_t *m, uint16_t id)
{
    if (m->state == RFSM_SCAN) {
        m->ops->notify(id, RFSM_SCANNING);
        return;
    }
    if (!m->ops->dmr_channel() || !m->ops->tx_allowed(RFSM_TX_KIND_DATA, id)) {
        m->ops->notify(id, RFSM_REFUSED);
        return;
    }
    m->kind = RFSM_TX_KIND_DATA;
    m->current = id;
    m->since_ms = m->ops->now_ms();
    m->state = RFSM_LISTEN;
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

// The beacon given up while listening (the PTT, another channel, its result)
static void listen_off(rfsm_t *m, rfsm_result_t result)
{
    m->ops->notify(m->current, result);
    m->ops->restore();
    m->state = receiving(m);
}

// Waiting in the transmitting states: the requests queued, the channel kept for later
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

    switch (m->state) {
        case RFSM_RX:
        case RFSM_SCAN:
            switch (event) {
                case RFSM_PTT_DOWN: ptt_press(m); break;
                case RFSM_CHANNEL: ops->apply_channel(); break;
                case RFSM_BEACON_REQ: start_beacon(m, id); break;
                case RFSM_DATA_REQ: start_data(m, id); break;
                case RFSM_TICK:
                    if (m->state == RFSM_SCAN) {
                        ops->scan_work();
                    } else {
                        ops->rx_work();
                    }
                    break;
                case RFSM_TX_DONE:
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT: m->faults++; break;
                default: break; // PTT_UP
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
                case RFSM_TICK:
                    if (m->state == RFSM_TX_VOICE) {
                        ops->tx_work();
                    } else {
                        ops->ending_work();
                    }
                    break;
                case RFSM_TX_DONE: // the tail over, or the backend ended by itself (a repeater that didn't answer)
                    ops->tx_end();
                    m->state = receiving(m);
                    break;
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT:
                    m->faults++;
                    ops->tx_end();
                    m->state = receiving(m);
                    break;
                default: break; // PTT_DOWN (TX_VOICE: sending already; TX_ENDING: its level waits)
            }
            break;

        case RFSM_LISTEN:
            switch (event) {
                case RFSM_PTT_DOWN: listen_off(m, RFSM_ABANDONED); break; // the PTT then, from RX
                case RFSM_CHANNEL:
                    m->channel_deferred = true;
                    listen_off(m, RFSM_ABANDONED);
                    break;
                case RFSM_BEACON_REQ: enqueue(m, RFSM_TX_KIND_BEACON, id); break;
                case RFSM_DATA_REQ: enqueue(m, RFSM_TX_KIND_DATA, id); break;
                case RFSM_TICK:
                    ops->rx_work(); // listening is receiving: the squelch reads the carrier first
                    if (!ops->carrier()) {
                        if (!ops->tx_allowed(m->kind, m->current)) {
                            listen_off(m, RFSM_REFUSED);
                        } else if (m->kind == RFSM_TX_KIND_BEACON) {
                            ops->tx_begin(RFSM_TX_KIND_BEACON);
                            ops->beacon_send(m->current);
                            m->state = RFSM_BEACON_SEND;
                        } else {
                            ops->tx_begin(RFSM_TX_KIND_DATA);
                            ops->data_send(m->current);
                            m->since_ms = ops->now_ms();
                            m->state = RFSM_DATA_SEND;
                        }
                    } else if (elapsed(m, ops->listen_ms(m->kind))) {
                        listen_off(m, RFSM_BUSY);
                    }
                    break;
                case RFSM_TX_DONE:
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT:
                    m->faults++;
                    listen_off(m, RFSM_FAILED);
                    break;
                default: break; // PTT_UP
            }
            break;

        case RFSM_BEACON_SEND:
            if (deferred(m, event, id)) {
                break;
            }
            switch (event) {
                case RFSM_TX_DONE:
                    ops->tx_end();
                    listen_off(m, RFSM_SENT);
                    break;
                case RFSM_DATA_DONE: m->ignored_late++; break;
                case RFSM_FAULT:
                    m->faults++;
                    ops->tx_end();
                    listen_off(m, RFSM_FAILED);
                    break;
                default: break; // PTT_DOWN (its level waits), PTT_UP, TICK (none while sending)
            }
            break;

        case RFSM_DATA_SEND:
            if (deferred(m, event, id)) {
                break;
            }
            switch (event) {
                case RFSM_TICK:
                    ops->tx_work();
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
                    m->state = receiving(m);
                    break;
                case RFSM_FAULT:
                    m->faults++;
                    ops->data_clear();
                    ops->tx_end();
                    ops->notify(m->current, RFSM_FAILED);
                    m->state = receiving(m);
                    break;
                default: break; // PTT_DOWN (its level waits), PTT_UP
            }
            break;

        default:
            m->faults++;
            m->state = receiving(m);
            break;
    }
}

// Back to receiving: what waited, in this order, until something leaves it again
static void drain(rfsm_t *m)
{
    while ((m->state == RFSM_RX) || (m->state == RFSM_SCAN)) {
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

void rfsm_scan(rfsm_t *m, bool on)
{
    m->scan_wanted = on;
    if ((m->state == RFSM_RX) || (m->state == RFSM_SCAN)) {
        m->state = receiving(m);
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
