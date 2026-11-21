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
 * OpenDMR, service: the P25 Phase 1 receiver and transmitter (docs/p25.md).
 *
 * Received in software (as NXDN, M17 and DMR, afrx.c): the modem records the discriminator (8 kHz), p25_demod.c
 * demodulates the C4FM and gives the dibits on the air one by one to p25-core's receiver (sync, NID, status dibits,
 * data units); each IMBE frame of an LDU is decoded as soon as it is in, into the playback's queue of 20 ms frames
 * (voice_play.c, the prompt FIFO). Until 2026-11 the HR_C7000's physical layer received (its blocks of 144 dibits,
 * 112 dibits lost when it found a sync, a reset after each transmission: docs/p25.md).
 *
 * Transmit (transmit(), the PTT): the recording off, the mic's 20 ms frames through our IMBE encoder, an HDU, LDUs,
 * a TDU through the physical layer's transmit FIFO (p25_phy.c, 4800 symbols/s); then the recording again.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "dmr_compat.h"
#include "modem.h"
#include "vocoder.h"
#include "radio.h"
#include "radio_hooks.h"
#include "imbe.h"
#include "imbe_fx.h"
#include "p25_frame.h"
#include "p25_phy.h"
#include "p25_demod.h"
#include "p25_radio.h"
#include "codeplug.h"
#include "ui.h"
#include "last_heard.h"
#include "dmr_ids.h"
#include "voice_play.h"
#include "afrx.h"

#define PCM_FRAMES     20 // 400 ms of decoded voice
#define PLAY_START     3  // frames queued before the playback starts: they come as their parts of the LDU are in, a
#define PLAY_START_MS  25 // frame about every 20 ms with the LDU's header between (the first of an LDU: kept this long)
#define CALL_TIMEOUT   400 // ms without an LDU: the call ended

volatile p25_radio_stats_t p25_radio_stats;

/* for tools/debug/p25_monitor.py and p25_txcheck.py: the last dibits received in blocks of 144 (type P25_TAP_AIR: in
   the order on the air), and the last calls */
#define P25_TAP_AIR 0x10
p25_tap_block_t p25_tap[P25_TAP_BLOCKS];
volatile uint32_t p25_tap_head; // blocks written, p25_tap[head % P25_TAP_BLOCKS] the next
p25_call_log_t p25_call_log[P25_CALL_LOG];
volatile uint32_t p25_call_log_head;
static p25_radio_stats_t call_start_stats;
static uint32_t call_start_ms;

/* the demodulator, the receiver, the decoder and the voice queue: in the P25 overlay (ckcpu.ld), claimed while the
   receiver runs (afrx.c) */
#define OVERLAY_BSS __attribute__((section(".p25_bss")))
static p25_demod dm OVERLAY_BSS;
static p25_rx rx OVERLAY_BSS;
static int ldu_voiced; // voice frames of the LDU coming in decoded already (receive())
static imbe_decoder dec OVERLAY_BSS;
static int16_t pcm[PCM_FRAMES][IMBE_N] OVERLAY_BSS;
static uint8_t tap_dibits[144];
static int tap_n;

static p25_call_t call;
static TickType_t last_ldu;
static volatile TickType_t heard_at; // a data unit with the channel's NAC decoded (p25_radio_heard_within())
static volatile bool heard;
static void ev(uint16_t event, uint16_t arg);

/* transmit (transmit()): the encoder and the data unit being made, in the overlay */
static volatile uint16_t tx_nac = CODEPLUG_P25_NAC_DEFAULT; // p25_radio_set_target()
static volatile uint16_t tx_tgid = 1;
#define TX_NAC     tx_nac
#define TX_TGID    tx_tgid
#define TX_STATUS  2     // the status dibits: 10, "unknown, for talkaround"
static imbe_encoder enc OVERLAY_BSS;
static uint8_t tx_imbe[9][18] OVERLAY_BSS;
static uint8_t tx_unit[P25_FRAME_MAX] OVERLAY_BSS;
volatile uint32_t p25_tx_source; // 0 the mic, 1 the test frames of p25_phy_tx_frame(), 2 a 1011 Hz tone into the
                                 // encoder (the P25 test tone; 1 and 2 paced by the mic); over the cable
#define TONE_STEP ((uint32_t)(1011ull * 4294967296ull / 8000ull)) // 1011 Hz at 8 kHz, 2^32 a turn
#define TONE_LEVEL 2048 // x cos (Q12): 8192 peak, about -12 dBFS
// Blocks of +3, -3, ... before the HDU (the MMDVM preamble, 0x77 bytes); none by default, as a standard transmitter:
// an MMDVM hotspot took any sync (2 bits wrong at most) as the start of frames in step with it and missed the real
// ones for 4 LDU times; a TDU misled it too. Over the cable
volatile uint32_t p25_tx_lead = 0;
// The RF on once this many IMBE frames of the first LDU are encoded; 0: at the key. The preamble and the HDU (4.75
// blocks) go out meanwhile; the LDU goes into the FIFO frame by frame (p25_tx_ldu_ready()), its first frame some 50 ms
// after the key, the 5th block (the HDU's end and the LDU's start) is taken about 90 ms after the RF on. Measured
// (2026-11-12): key to speaker 415 ms after 4 frames, 302 - 322 ms at the key; no underrun at 0. Over the cable
volatile uint32_t p25_tx_rf_after = 0;
volatile uint32_t p25_tx_low_power; // not 0: 50 mW (tests with the radios side by side); over the cable
volatile uint32_t p25_tx_fn0;    // the first test frame's number (tells test transmissions apart)
volatile uint32_t p25_tx_stats[4]; // calls, LDUs, encoder frames, mic buffers

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// The event log (ms, event, arg) for the delays, read over the cable (cleared by writing p25_ev_n 0): 1 the key, 2 an
// LDU into the FIFO (arg: its number), 3 the RF on (p25_phy_tx_begin() done), 10 a data unit received (arg: DUID);
// from the others (p25_ev_log(), tools/debug/voice_delay.py)
#ifdef OPENDMR_DEBUG
#define P25_EV 64
#else
#define P25_EV 1 // the first event only (Makefile DEBUG)
#endif
struct {
    uint32_t ms;
    uint16_t event, arg;
} p25_ev[P25_EV];
volatile uint32_t p25_ev_n;

void p25_ev_log(uint16_t event, uint16_t arg)
{
    ev(event, arg);
}

static void ev(uint16_t event, uint16_t arg)
{
    uint32_t i = p25_ev_n;
    if (i < P25_EV) {
        p25_ev[i].ms = now_ms();
        p25_ev[i].event = event;
        p25_ev[i].arg = arg;
        p25_ev_n = i + 1;
    }
}

// The UI told when the call begins, its caller or talk group becomes known or changes, and it ends (the screens show
// the caller panel, qso_panel.c); a caller goes into Last heard with its text from the contacts or the DMR IDs (a
// P25 unit ID is the DMR ID on the amateur networks)
static struct {
    bool active;
    uint32_t src;
    uint32_t tg_or_pc;
} published;

static void call_publish(void)
{
    uint32_t tg_or_pc = p25_call_tg_or_pc(&call);
    char text[LAST_HEARD_TEXT_SIZE];

    if ((call.active == published.active) && (call.src == published.src) && (tg_or_pc == published.tg_or_pc)) {
        return;
    }
    if (call.active && (call.src != 0) && ((call.src != published.src) || (tg_or_pc != published.tg_or_pc))) {
        dmr_ids_caller_text(call.src, text, sizeof(text));
        last_heard_add(call.src, tg_or_pc, LAST_HEARD_TS_UNKNOWN, text[0] ? text : NULL);
    }
    published.active = call.active;
    published.src = call.src;
    published.tg_or_pc = tg_or_pc;

    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_SIGNAL};

    ui_post(&ev);
}

static void call_end(void)
{
    if (call.active) {
        p25_call_log_t *l = &p25_call_log[p25_call_log_head % P25_CALL_LOG];
        l->start_ms = call_start_ms;
        l->end_ms = now_ms();
        l->nac = call.nac;
        l->tgid = call.tgid;
        l->src = call.src;
        l->dst = call.dst;
        l->lco = call.lco;
        l->encrypted = call.encrypted;
        l->ldus = p25_radio_stats.ldus - call_start_stats.ldus;
        l->lc_bad = p25_radio_stats.lc_bad - call_start_stats.lc_bad;
        l->imbe_frames = p25_radio_stats.imbe_frames - call_start_stats.imbe_frames;
        l->imbe_repeat = p25_radio_stats.imbe_repeat - call_start_stats.imbe_repeat;
        p25_call_log_head++;
    }
    call.active = false;
    voice_play_end(); // what is queued played out, then the speaker off
    call_publish();
}

static void call_begin(void)
{
    if (!call.active) {
        memset(&call, 0, sizeof(call));
        call.active = true;
        imbe_dec_init(&dec);
        last_ldu = xTaskGetTickCount(); // the timeout from now (else the last call's time ends it at once)
        voice_play_prepare();           // the speaker on while the voice comes (the HDU is before it)
        call_start_stats = p25_radio_stats;
        call_start_ms = now_ms();
    }
}

// The channel's NAC: a call with another one isn't ours (as CTCSS in FM)
static bool nac_ours(uint16_t nac)
{
    uint16_t want = tx_nac;

    return (want == CODEPLUG_P25_NAC_RX_ANY) || (want == 0xF7F) || (nac == want);
}

void p25_radio_set_target(uint16_t nac, uint16_t tgid)
{
    tx_nac = (uint16_t)(nac & CODEPLUG_P25_NAC_MAX);
    tx_tgid = (tgid != 0) ? tgid : 1;
}

// A voice frame decoded into the playback's queue (an encrypted call's: not played, no key)
static void voice(const uint8_t imbe[18])
{
    int16_t *out = voice_play_frame();
    if (!out) { // the playback fell behind: the newest frame dropped
        p25_radio_stats.pcm_drops++;
        return;
    }
    int kind = imbe_dec_frame(&dec, imbe, out);
    p25_radio_stats.imbe_frames++;
    if (kind != 'V') {
        p25_radio_stats.imbe_repeat++;
    }
    if (call.encrypted) {
        return;
    }
    voice_play_put();
}

static void unit(int duid)
{
    const p25_frame *f = &rx.frame;
    p25_radio_stats.units++;
    if (!nac_ours(f->nac)) {
        p25_radio_stats.nac_other++;
        return;
    }
    ev(10, (uint16_t)duid);
    heard_at = xTaskGetTickCount();
    heard = true;
    radio_eco_activity(); // the receive power saving ends (it knows the FM carrier and the DMR sync only)
    switch (duid) {
    case P25_DUID_HDU:
        call_begin();
        last_ldu = xTaskGetTickCount();
        call.nac = f->nac;
        if (f->es.ok) {
            call.tgid = f->tgid;
            call.encrypted = (f->es.algid != P25_ALGID_CLEAR) && (f->es.algid != 0);
        }
        break;
    case P25_DUID_LDU1:
    case P25_DUID_LDU2:
        call_begin();
        call.nac = f->nac;
        last_ldu = xTaskGetTickCount();
        p25_radio_stats.ldus++;
        if (duid == P25_DUID_LDU1) {
            if (f->lc.ok && !f->lc.protect) {
                call.lco = f->lc.lco;
                call.tgid = f->lc.tgid;
                call.src = f->lc.src;
                call.dst = f->lc.dst;
            } else if (!f->lc.ok) {
                p25_radio_stats.lc_bad++;
            }
        } else if (f->es.ok) {
            call.encrypted = (f->es.algid != P25_ALGID_CLEAR) && (f->es.algid != 0);
        }
        for (int k = ldu_voiced; k < 9; k++) { // the ones not decoded as they came (receive())
            voice(f->imbe[k]);
        }
        break;
    case P25_DUID_TDU:
    case P25_DUID_TDULC:
        if (call.active) {
            call_end(); // the voice queued plays out
        }
        break;
    default:
        break;
    }
    call_publish();
}

// Each voice frame of an LDU decoded as soon as it is in (p25_rx_ldu_frames()), not with the whole LDU 180 ms later:
// the call's delay that much shorter. The LDU's LC / ES and the rest at its end (unit()).
static void receive(uint8_t dibit)
{
    int duid = p25_rx_dibit(&rx, dibit);
    if (duid >= 0) {
        unit(duid);
        ldu_voiced = 0;
        return;
    }
    int ready = p25_rx_ldu_frames(&rx);
    if (ready < 0) {
        ldu_voiced = 0;
    } else if ((ready > ldu_voiced) && nac_ours(rx.frame.nac)) {
        call_begin();
        last_ldu = xTaskGetTickCount();
        while (ldu_voiced < ready) {
            uint8_t imbe[18];
            p25_rx_ldu_frame(&rx, ldu_voiced++, imbe);
            voice(imbe);
        }
    }
}

// the demodulator's dibits (P25_DEMOD_LOST: the signal lost, the receiver hunts again); the tap in blocks of 144
static void on_dibit(void *ctx, int dibit)
{
    (void)ctx;
    if (dibit == P25_DEMOD_LOST) {
        p25_rx_init(&rx, P25_NAC_ANY);
        ldu_voiced = 0;
        return;
    }
    receive((uint8_t)dibit);
    tap_dibits[tap_n++] = (uint8_t)dibit;
    if (tap_n == 144) {
        tap_n = 0;
        p25_radio_stats.blocks++;
        p25_tap_block_t *t = &p25_tap[p25_tap_head % P25_TAP_BLOCKS];
        t->ms = now_ms();
        t->type = P25_TAP_AIR;
        for (int i = 0; i < 36; i++) {
            t->data[i] = (uint8_t)((tap_dibits[4 * i] << 6) | (tap_dibits[4 * i + 1] << 4) |
                                   (tap_dibits[4 * i + 2] << 2) | tap_dibits[4 * i + 3]);
        }
        p25_tap_head++;
    }
}

/* ---- receiving (afrx.c runs it) ---- */

static const voice_play_cfg_t play_cfg = {
    .frames = pcm,
    .count = PCM_FRAMES,
    .start = PLAY_START,
    .start_ms = PLAY_START_MS,
    .underruns = &p25_radio_stats.underruns,
};

// the receiver from scratch: starting, and after transmitting
static void rx_init(void)
{
    p25_demod_init(&dm, on_dibit, NULL);
    p25_rx_init(&rx, P25_NAC_ANY);
    imbe_dec_init(&dec);
    ldu_voiced = 0;
    tap_n = 0;
    memset(&call, 0, sizeof(call));
}

static void samples(int16_t *x, int n)
{
    p25_demod_samples(&dm, x, n);
}

static void poll(void)
{
    if (call.active && ((xTaskGetTickCount() - last_ldu) >= pdMS_TO_TICKS(CALL_TIMEOUT))) {
        call_end();
    }
}

static void end_call(void)
{
    if (call.active) {
        call_end();
    }
}

/* A transmission while the PTT stays held (afrx.c: the playback and the recording off): the mic on, 20 ms frames into
   the encoder, 9 frames an LDU (LDU1, LDU2 in turn) into the physical layer's FIFO after an HDU; the transmitter
   keyed at once (p25_tx_rf_after 0: the HDU goes out while the first frames are encoded). The PTT released: the LDU
   finished with silence, an LDU2 last, a TDU; once sent, back to receive (afrx.c). */
static void transmit(void)
{
    ev(1, 0);
    afrx_fifo_prepare(MODEM_MOD_SCALE_ONE); // the mic on, the FIFO empty, P25's deviation (the modem's calibration)
    imbe_enc_init(&enc);

    p25_lc lc;
    memset(&lc, 0, sizeof(lc));
    lc.lco = 0x00;
    lc.tgid = TX_TGID;
    lc.src = codeplug_get_dmr_id() & 0xFFFFFF;
    p25_lc_pack(&lc);
    p25_es es;
    memset(&es, 0, sizeof(es));
    es.algid = P25_ALGID_CLEAR;
    for (uint32_t k = 0; k < p25_tx_lead; k++) {
        for (uint32_t i = 0; i < P25_PHY_BLOCK_BYTES * 4; i++) {
            tx_unit[i] = (i & 1) ? 3 : 1;
        }
        p25_phy_tx_put(tx_unit, P25_PHY_BLOCK_BYTES * 4);
    }
    p25_phy_tx_put(tx_unit, p25_tx_hdu(tx_unit, TX_NAC, &es, 0, TX_TGID, TX_STATUS));

    int16_t mic[IMBE_N];
    uint32_t tone_phase = 0;
    int halves = 0, nf = 0;
    uint32_t k = 0, fn = p25_tx_fn0;
    bool started = false, ending = false;
    int ldu_put = 0; // dibits of the LDU being sent in the FIFO already
    p25_tx_stats[0]++;
    bool failed = false;
    if (p25_tx_rf_after == 0) { // at once: the preamble and the HDU out while the first frames are encoded
        failed = !afrx_fifo_key(p25_tx_low_power != 0);
        if (!failed) {
            ev(3, 0);
            started = true;
        }
    }
    while (!failed) {
        if (!afrx_ptt_held()) {
            ending = true;
        }
        if (ending && !started && (k == 0) && (nf == 0)) {
            break; // released before anything was sent
        }
        bool frame = false;
        if (ending) {
            if (p25_phy_tx_room() > 2 * P25_FRAME_MAX) { // silence, as fast as the FIFO takes it
                memset(mic, 0, sizeof(mic));
                frame = true;
            }
        } else if (sound_mic_take(&mic[80 * halves])) {
            p25_tx_stats[3]++;
            if (++halves == 2) {
                halves = 0;
                frame = true;
            }
        }
        if (!frame) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if ((p25_tx_source == 2) && !ending) {
            for (int i = 0; i < IMBE_N; i++) {
                mic[i] = (int16_t)((cos_q12(tone_phase) * TONE_LEVEL) >> 10);
                tone_phase += TONE_STEP;
            }
        }
        uint8_t u[88];
        int have;
        if (p25_tx_source == 1) {
            p25_phy_tx_frame(fn++, u);
            have = 1;
        } else {
            have = imbe_enc_frame(&enc, mic, u);
        }
        if (!have) {
            continue;
        }
        p25_tx_stats[2]++;
        imbe_encode_frame(u, tx_imbe[nf++]);
        // the LDU as far as its frames encoded fix it into the FIFO now (p25_tx_ldu_ready()), not all of it once the
        // ninth is: a frame goes out about as soon as it is encoded
        int ready = p25_tx_ldu_ready((int)nf);
        if (ready > ldu_put) {
            uint8_t lsd[2] = {0, 0};
            p25_tx_ldu(tx_unit, TX_NAC, (k & 1) ? P25_DUID_LDU2 : P25_DUID_LDU1, (const uint8_t(*)[18])tx_imbe, &lc,
                       &es, lsd, TX_STATUS);
            p25_phy_tx_put(tx_unit + ldu_put, ready - ldu_put);
            ldu_put = ready;
        }
        if (!started && (nf >= (int)p25_tx_rf_after)) { // the preamble and the HDU are in the FIFO already
            if (!afrx_fifo_key(p25_tx_low_power != 0)) {
                break;
            }
            ev(3, (uint16_t)nf);
            started = true;
        }
        if (nf < 9) {
            continue;
        }
        nf = 0;
        ldu_put = 0;
        k++;
        ev(2, (uint16_t)k);
        p25_tx_stats[1]++;
        if (ending && ((k & 1) == 0)) {
            break; // an LDU2 last
        }
    }
    if (started) {
        p25_phy_tx_put(tx_unit, p25_tx_tdu(tx_unit, TX_NAC, TX_STATUS));
    }
    afrx_fifo_finish(started); // sent out, the RF off, the mic off
}

static const afrx_ops_t ops = {
    .bw = AFREC_BW_FLAT, // no band-pass, no de-emphasis
    .overlay = OVERLAY_P25,
    .play = &play_cfg,
    .rx_init = rx_init,
    .samples = samples,
    .poll = poll,
    .end_call = end_call,
    .transmit = transmit,
};

void p25_radio_transmit(bool on)
{
    afrx_ptt(on);
}

bool p25_radio_transmitting(void)
{
    return afrx_transmitting();
}

bool p25_radio_heard_within(uint32_t ms)
{
    return heard && ((xTaskGetTickCount() - heard_at) < pdMS_TO_TICKS(ms));
}

void p25_radio_start(void)
{
    if (afrx_start(&ops)) {
        heard = false;
        memset(&published, 0, sizeof(published));
    }
}

void p25_radio_stop(void)
{
    if (!afrx_running(&ops)) {
        return;
    }
    afrx_stop();
    call.active = false;
    call_publish();
}

uint32_t p25_call_tg_or_pc(const p25_call_t *c)
{
    return (c->lco == P25_LCO_UNIT) ? (((uint32_t)LAST_HEARD_PC_FLAG << 24) | (c->dst & 0xFFFFFF)) : c->tgid;
}

bool p25_radio_call(p25_call_t *out)
{
    *out = call;
    return call.active;
}
