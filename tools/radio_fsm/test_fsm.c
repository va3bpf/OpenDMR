/*
 * The radio task's state machine (src/services/radio_fsm.c) on the PC: every state against every event, as the
 * transition table of the OpenDM32 notes' radio_task.html (6.2; SCAN set by the UI's level), then sequences.
 *
 *   python tools/radio_fsm/run.py
 */

#include <stdio.h>
#include <string.h>
#include "radio_fsm.h"

/* ---- the radio task, played: the answers set by the test, the actions written to a trace ---- */

static bool allowed = true, dmr = true, carrier_on = false;
static uint32_t now = 1000;
static char trace[512];

static void put(const char *s)
{
    if (trace[0]) {
        strcat(trace, ",");
    }
    strcat(trace, s);
}

static const char *kind_name(rfsm_tx_kind_t k)
{
    return (k == RFSM_TX_KIND_VOICE) ? "voice" : (k == RFSM_TX_KIND_BEACON) ? "beacon" : "data";
}

static const char *result_name(rfsm_result_t r)
{
    static const char *names[] = {"sent", "refused", "scanning", "queue_full", "abandoned", "busy", "failed"};
    return names[r];
}

static bool q_allowed(rfsm_tx_kind_t kind, uint16_t id) { (void)kind; (void)id; return allowed; }
static bool q_dmr(void) { return dmr; }
static bool q_carrier(void) { return carrier_on; }
static uint32_t q_now(void) { return now; }
static void a_tx_begin(rfsm_tx_kind_t k) { char b[32]; sprintf(b, "tx_begin(%s)", kind_name(k)); put(b); }
static void a_tx_ending(void) { put("ending"); }
static void a_tx_end(void) { put("end"); }
static void a_refused(void) { put("ptt_refused"); }
static void a_apply(void) { put("apply"); }
static void a_tune(uint16_t id) { char b[16]; sprintf(b, "tune(%u)", id); put(b); }
static void a_send(uint16_t id) { char b[16]; sprintf(b, "send(%u)", id); put(b); }
static void a_restore(void) { put("restore"); }
static void a_data(uint16_t id) { char b[16]; sprintf(b, "data(%u)", id); put(b); }
static void a_clear(void) { put("clear"); }
static void a_rx(void) { put("rx"); }
static void a_scanw(void) { put("scanw"); }
static void a_txw(void) { put("txw"); }
static void a_endw(void) { put("endw"); }
static void a_notify(uint16_t id, rfsm_result_t r) { char b[32]; sprintf(b, "notify(%u,%s)", id, result_name(r)); put(b); }

static const rfsm_ops_t OPS = {q_allowed, q_dmr, q_carrier, q_now, a_tx_begin, a_tx_ending, a_tx_end, a_refused,
                               a_apply, a_tune, a_send, a_restore, a_data, a_clear, a_rx, a_scanw, a_txw, a_endw,
                               a_notify};

static int failures, checks;

static void expect(const char *what, const rfsm_t *m, rfsm_state_t state, const char *actions)
{
    checks++;
    if ((m->state != state) || (strcmp(trace, actions) != 0)) {
        failures++;
        printf("FAIL %s: %s [%s], expected %s [%s]\n", what, rfsm_state_name(m->state), trace,
               rfsm_state_name(state), actions);
    }
}

static void expect_counts(const char *what, const rfsm_t *m, uint32_t late, uint32_t faults, uint8_t queued)
{
    checks++;
    if ((m->ignored_late != late) || (m->faults != faults) || (m->queued != queued)) {
        failures++;
        printf("FAIL %s: late %u faults %u queued %u, expected %u %u %u\n", what, m->ignored_late, m->faults,
               m->queued, late, faults, queued);
    }
}

/* ---- the table ---- */

static const char *const EVENT_NAMES[RFSM_EVENTS] = {"PTT_DOWN", "PTT_UP", "CHANNEL", "BEACON_REQ", "DATA_REQ",
                                                     "TICK", "TX_DONE", "DATA_DONE", "FAULT"};

typedef struct {
    rfsm_state_t next;
    const char *actions;
    uint32_t late, faults;
    uint8_t queued;
} cell_t;

#define RX RFSM_RX
#define SC RFSM_SCAN
#define TV RFSM_TX_VOICE
#define TE RFSM_TX_ENDING
#define BL RFSM_BEACON_LISTEN
#define BS RFSM_BEACON_SEND
#define DS RFSM_DATA_SEND

// [state][event]; requests carry id 5; in BEACON_LISTEN / BEACON_SEND the beacon is 7, in DATA_SEND the burst 9
// (DATA_DONE carries 9)
static const cell_t TABLE[RFSM_STATES][RFSM_EVENTS] = {
    //         PTT_DOWN, PTT_UP, CHANNEL, BEACON_REQ, DATA_REQ, TICK, TX_DONE, DATA_DONE, FAULT
    [RX] = {{TV, "tx_begin(voice)"}, {RX, ""}, {RX, "apply"}, {BL, "tune(5)"}, {DS, "tx_begin(data),data(5)"},
            {RX, "rx"}, {RX, "", 1}, {RX, "", 1}, {RX, "", 0, 1}},
    [SC] = {{TV, "tx_begin(voice)"}, {SC, ""}, {SC, "apply"}, {SC, "notify(5,scanning)"}, {SC, "notify(5,scanning)"},
            {SC, "scanw"}, {SC, "", 1}, {SC, "", 1}, {SC, "", 0, 1}},
    [TV] = {{TV, ""}, {TE, "ending"}, {TV, ""}, {TV, "", 0, 0, 1}, {TV, "", 0, 0, 1}, {TV, "txw"}, {RX, "end"},
            {TV, "", 1}, {RX, "end", 0, 1}},
    [TE] = {{TE, ""}, {TE, ""}, {TE, ""}, {TE, "", 0, 0, 1}, {TE, "", 0, 0, 1}, {TE, "endw"}, {RX, "end"},
            {TE, "", 1}, {RX, "end", 0, 1}},
    [BL] = {{TV, "notify(7,abandoned),restore,tx_begin(voice)"}, {BL, ""}, {RX, "notify(7,abandoned),restore,apply"},
            {BL, "", 0, 0, 1}, {BL, "", 0, 0, 1}, {BS, "rx,tx_begin(beacon),send(7)"}, {BL, "", 1}, {BL, "", 1},
            {RX, "notify(7,failed),restore", 0, 1}},
    [BS] = {{BS, ""}, {BS, ""}, {BS, ""}, {BS, "", 0, 0, 1}, {BS, "", 0, 0, 1}, {BS, ""},
            {RX, "end,notify(7,sent),restore"}, {BS, "", 1}, {RX, "end,notify(7,failed),restore", 0, 1}},
    [DS] = {{DS, ""}, {DS, ""}, {DS, ""}, {DS, "", 0, 0, 1}, {DS, "", 0, 0, 1}, {DS, "txw"}, {DS, "", 1},
            {RX, "end,notify(9,sent)"}, {RX, "clear,end,notify(9,failed)", 0, 1}},
};

// The machine in a state as the events would have left it
static void enter(rfsm_t *m, rfsm_state_t s)
{
    rfsm_init(m, &OPS);
    m->state = s;
    m->since_ms = now;
    m->scan_wanted = (s == SC);
    if (s == TV) {
        m->ptt_wanted = m->ptt_handled = true;
    }
    if ((s == BL) || (s == BS)) {
        m->current = 7;
    }
    if (s == DS) {
        m->current = 9;
    }
}

static void table(void)
{
    for (int s = 0; s < RFSM_STATES; s++) {
        for (int e = 0; e < RFSM_EVENTS; e++) {
            rfsm_t m;
            char what[64];
            const cell_t *c = &TABLE[s][e];

            allowed = true;
            dmr = true;
            carrier_on = false;
            enter(&m, (rfsm_state_t)s);
            trace[0] = 0;
            rfsm_event(&m, (rfsm_event_t)e, (e == RFSM_DATA_DONE) ? 9 : ((e == RFSM_BEACON_REQ) || (e == RFSM_DATA_REQ)) ? 5 : 0);
            sprintf(what, "%s x %s", rfsm_state_name((rfsm_state_t)s), EVENT_NAMES[e]);
            expect(what, &m, c->next, c->actions);
            expect_counts(what, &m, c->late, c->faults, c->queued);
        }
    }
}

/* ---- sequences ---- */

static void step(rfsm_t *m, rfsm_event_t e, uint16_t id)
{
    trace[0] = 0;
    rfsm_event(m, e, id);
}

static void ptt(rfsm_t *m, bool down)
{
    trace[0] = 0;
    rfsm_ptt(m, down);
}

static void sequences(void)
{
    rfsm_t m;

    allowed = true;
    dmr = true;
    carrier_on = false;

    // a busy beacon channel: given up after RFSM_LISTEN_MS
    rfsm_init(&m, &OPS);
    now = 1000;
    step(&m, RFSM_BEACON_REQ, 1);
    carrier_on = true;
    now = 2500;
    step(&m, RFSM_TICK, 0);
    expect("busy: still listening", &m, BL, "rx");
    now = 3000;
    step(&m, RFSM_TICK, 0);
    expect("busy: given up", &m, RX, "rx,notify(1,busy),restore");
    carrier_on = false;

    // the PTT while listening: the beacon given up, the voice at once; then the release and the end
    rfsm_init(&m, &OPS);
    step(&m, RFSM_BEACON_REQ, 2);
    ptt(&m, true);
    expect("ptt while listening", &m, TV, "notify(2,abandoned),restore,tx_begin(voice)");
    ptt(&m, false);
    expect("released", &m, TE, "ending");
    step(&m, RFSM_TX_DONE, 0);
    expect("ended", &m, RX, "end");

    // two beacons asked while transmitting: sent one after the other once back in RX
    rfsm_init(&m, &OPS);
    ptt(&m, true);
    step(&m, RFSM_BEACON_REQ, 3);
    step(&m, RFSM_BEACON_REQ, 4);
    ptt(&m, false);
    step(&m, RFSM_TX_DONE, 0);
    expect("first beacon after the voice", &m, BL, "end,tune(3)");
    step(&m, RFSM_TICK, 0);
    step(&m, RFSM_TX_DONE, 0);
    expect("first sent, second listening", &m, BL, "end,notify(3,sent),restore,tune(4)");
    expect_counts("second queued no more", &m, 0, 0, 0);

    // a channel asked while transmitting: applied after
    rfsm_init(&m, &OPS);
    ptt(&m, true);
    step(&m, RFSM_CHANNEL, 0);
    ptt(&m, false);
    step(&m, RFSM_TX_DONE, 0);
    expect("channel after the voice", &m, RX, "end,apply");

    // the PTT pressed again during the tail: transmitting again after it
    rfsm_init(&m, &OPS);
    ptt(&m, true);
    ptt(&m, false);
    ptt(&m, true);
    expect("pressed during the tail", &m, TE, "");
    step(&m, RFSM_TX_DONE, 0);
    expect("again after the tail", &m, TV, "end,tx_begin(voice)");

    // the PTT pressed and released while the beacon was sent: no voice after it
    rfsm_init(&m, &OPS);
    step(&m, RFSM_BEACON_REQ, 5);
    step(&m, RFSM_TICK, 0);
    ptt(&m, true);
    ptt(&m, false);
    step(&m, RFSM_TX_DONE, 0);
    expect("tap during the beacon", &m, RX, "end,notify(5,sent),restore");

    // the PTT held through the beacon: the voice after it
    rfsm_init(&m, &OPS);
    step(&m, RFSM_BEACON_REQ, 6);
    step(&m, RFSM_TICK, 0);
    ptt(&m, true);
    step(&m, RFSM_TX_DONE, 0);
    expect("held through the beacon", &m, TV, "end,notify(6,sent),restore,tx_begin(voice)");

    // the queue full
    rfsm_init(&m, &OPS);
    ptt(&m, true);
    for (uint16_t id = 10; id < 10 + RFSM_QUEUE; id++) {
        step(&m, RFSM_BEACON_REQ, id);
    }
    step(&m, RFSM_DATA_REQ, 20);
    expect("queue full", &m, TV, "notify(20,queue_full)");

    // data on a channel that isn't DMR: refused, nothing stuck
    rfsm_init(&m, &OPS);
    dmr = false;
    step(&m, RFSM_DATA_REQ, 21);
    expect("data not on DMR", &m, RX, "notify(21,refused)");
    dmr = true;

    // a burst that never ends: a fault after RFSM_DATA_MS
    rfsm_init(&m, &OPS);
    now = 10000;
    step(&m, RFSM_DATA_REQ, 22);
    now = 12000;
    step(&m, RFSM_TICK, 0);
    expect("burst timeout", &m, RX, "txw,clear,end,notify(22,failed)");
    expect_counts("burst timeout counted", &m, 0, 1, 0);

    // another burst's late end: ignored
    rfsm_init(&m, &OPS);
    step(&m, RFSM_DATA_REQ, 23);
    step(&m, RFSM_DATA_DONE, 99);
    expect("late end of another burst", &m, DS, "");
    expect_counts("late end counted", &m, 1, 0, 0);

    // the PTT on a channel that can't transmit: refused once while held, again on the next press
    rfsm_init(&m, &OPS);
    allowed = false;
    ptt(&m, true);
    expect("ptt refused", &m, RX, "ptt_refused");
    step(&m, RFSM_TICK, 0);
    expect("held: not again", &m, RX, "rx");
    ptt(&m, false);
    ptt(&m, true);
    expect("pressed again: refused again", &m, RX, "ptt_refused");
    allowed = true;

    // the UI scanning: beacons and bursts refused, the PTT as usual
    rfsm_init(&m, &OPS);
    trace[0] = 0;
    rfsm_scan(&m, true);
    expect("scan on", &m, SC, "");
    step(&m, RFSM_BEACON_REQ, 24);
    expect("beacon while scanning", &m, SC, "notify(24,scanning)");
    step(&m, RFSM_DATA_REQ, 25);
    expect("burst while scanning", &m, SC, "notify(25,scanning)");

    // the dual watch: the PTT while scanning, the scan on through the transmission: back to SCAN after it
    ptt(&m, true);
    expect("ptt while scanning", &m, TV, "tx_begin(voice)");
    ptt(&m, false);
    step(&m, RFSM_TX_DONE, 0);
    expect("dual watch: back to scanning", &m, SC, "end");

    // scan.c: the PTT stops the scan (its level off) before the transmission: back to RX after it
    trace[0] = 0;
    rfsm_scan(&m, false);
    expect("scan off", &m, RX, "");
    trace[0] = 0;
    rfsm_scan(&m, true);
    ptt(&m, true);
    trace[0] = 0;
    rfsm_scan(&m, false);
    expect("scan stopped while transmitting: still transmitting", &m, TV, "");
    ptt(&m, false);
    step(&m, RFSM_TX_DONE, 0);
    expect("scan stopped: back to RX", &m, RX, "end");

    // a beacon queued while transmitting from a scan: refused once back to scanning
    rfsm_init(&m, &OPS);
    rfsm_scan(&m, true);
    ptt(&m, true);
    step(&m, RFSM_BEACON_REQ, 26);
    ptt(&m, false);
    step(&m, RFSM_TX_DONE, 0);
    expect("queued beacon, back to scanning", &m, SC, "end,notify(26,scanning)");
}

int main(void)
{
    table();
    sequences();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
