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
 * OpenDMR, service: the P25 Phase 1 receiver (docs/p25.md).
 *
 * The physical layer interrupt (p25_phy.c) copies each 30 ms block into a ring; the task puts the block back into the
 * order on the air (HR_C7000 Figure 70), gives its 144 dibits to p25-core's receiver (sync, NID, status dibits, data
 * units), decodes the 9 IMBE frames of each LDU into a queue of 20 ms frames and keeps the modem's playback buffers
 * (dmr_sound.c, 10 ms each) filled to PLAY_LEVEL from it: an LDU only comes whole every 180 ms. A block the same as
 * the one before (the modem didn't write it: seen once, docs/p25.md) is dropped and the receiver hunts for the sync.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "leds.h"
#include "dmr_compat.h"
#include "vocoder.h"
#include "radio.h"
#include "imbe.h"
#include "imbe_fx.h"
#include "p25_frame.h"
#include "p25_phy.h"
#include "p25_radio.h"
#include "codeplug.h"

#define RING          8   // blocks between the interrupt and the task
#define PCM_FRAMES    20  // 400 ms of decoded voice
#define PLAY_LEVEL    14  // playback buffers (10 ms) kept filled
#define PLAY_START_DMR 12 // dmr_sound.c WAV_BUFFER_AMBE_PREBUFFERING_COUNT
// buffers (10 ms) before the playback starts (dmr_sound.c sound_rx_prebuffer): the voice frames come as decoded
// (receive()), in steps of the 30 ms blocks with the LDU headers between. Over the cable
volatile uint32_t p25_play_start = 8; // measured 2026-11-12: 6 and 8 the same delay, 4 broke up
static bool play_logged;
#define CALL_TIMEOUT  400 // ms without an LDU: the call ended
#define REARM_MS      1000 // ms of blocks without a data unit: the receive armed again (the modem waits for a sync)
#define HUNT_REARM_MS 5000 // ms without any block: armed again all the same (the modem stuck, see task_main())
#define BLOCK_BYTES   P25_PHY_BLOCK_BYTES

volatile p25_radio_stats_t p25_radio_stats;

/* for tools/debug/p25_monitor.py: the last blocks as received, and the last calls */
p25_tap_block_t p25_tap[P25_TAP_BLOCKS];
volatile uint32_t p25_tap_head; // blocks written, p25_tap[head % P25_TAP_BLOCKS] the next
p25_call_log_t p25_call_log[P25_CALL_LOG];
volatile uint32_t p25_call_log_head;
static p25_radio_stats_t call_start_stats;
static uint32_t call_start_ms;

/* the ring, filled by the interrupt (main RAM) */
static struct {
    uint32_t tick; // when it came (the interrupt): where it lies on the air
    uint32_t type;
    uint8_t data[BLOCK_BYTES];
} ring[RING];
static volatile uint32_t ring_in, ring_out;

/* the receiver, the decoder and the voice queue: in the P25 overlay (ckcpu.ld), zeroed by its load */
#define OVERLAY_BSS __attribute__((section(".p25_bss")))
static p25_rx rx OVERLAY_BSS;
static int ldu_voiced; // voice frames of the LDU coming in decoded already (receive())
static imbe_decoder dec OVERLAY_BSS;
static int16_t pcm[PCM_FRAMES][IMBE_N] OVERLAY_BSS;
static uint32_t pcm_in OVERLAY_BSS, pcm_out OVERLAY_BSS;

static StaticTask_t task_tcb;
static StackType_t task_stack[2048]; // 8 KB: the IMBE encoder (transmit) runs on it, as AMBE's on its 8 KB
static TaskHandle_t task;
static volatile bool running;
static volatile bool busy; // the task is using the overlay
static uint32_t rearm_mark;  // ring_in when last armed: no block since while they are the same

static uint8_t last_raw[BLOCK_BYTES];
static bool have_last;
/* the last block, held back until the next one tells where that one starts (see place()) */
static uint8_t held[144];
static uint32_t held_tick;
static bool have_held;
static p25_call_t call;
static TickType_t last_ldu;
static TickType_t last_unit; // a data unit decoded, or the receive (re)armed
static bool audio_on;
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
static volatile bool tx_want, tx_busy;
volatile uint32_t p25_tx_source; // 0 the mic, 1 the test frames of p25_phy_tx_frame(), 2 a 1011 Hz tone into the
                                 // encoder (the P25 test tone; 1 and 2 paced by the mic); over the cable
#define TONE_STEP ((uint32_t)(1011ull * 4294967296ull / 8000ull)) // 1011 Hz at 8 kHz, 2^32 a turn
#define TONE_LEVEL 2048 // x cos (Q12): 8192 peak, about -12 dBFS
// Blocks of +3, -3, ... before the HDU (the MMDVM preamble, 0x77 bytes); none by default, as a standard transmitter.
// An HR_C7000 finding a sync from nothing gives the block with it and loses the next 112 dibits, up to 189 past the
// sync (measured, also with a hotspot sending): a preamble with a sync kept that loss off the HDU. Since 2026-11-12
// the receiver takes those dibits as erasures and the HDU's RS(36,20) puts them right (13 erasures), so no sync is
// sent before the HDU any more: an MMDVM hotspot took any sync (2 bits wrong at most) as the start of frames in step
// with it and missed the real ones for 4 LDU times; a TDU misled it too. Over the cable
volatile uint32_t p25_tx_lead = 0;
// The RF on once this many IMBE frames of the first LDU are encoded; 0: at the key. The preamble and the HDU (4.75
// blocks) go out meanwhile; the LDU goes into the FIFO frame by frame (p25_tx_ldu_ready()), its first frame some 50 ms
// after the key, the 5th block (the HDU's end and the LDU's start) is taken about 90 ms after the RF on. Measured
// (2026-11-12): key to speaker 415 ms after 4 frames, 302 - 322 ms at the key; no underrun at 0. Over the cable
volatile uint32_t p25_tx_rf_after = 0;
volatile uint32_t p25_tx_low_power; // not 0: 50 mW (tests with the radios side by side); over the cable
volatile uint32_t p25_tx_fn0;    // the first test frame's number (tells test transmissions apart)
volatile uint32_t p25_tx_stats[4]; // calls, LDUs, encoder frames, mic buffers

static void RAM_CODE block_isr(uint32_t type, const volatile uint8_t *data)
{
    if ((ring_in - ring_out) < RING) {
        uint32_t i = ring_in % RING;
        ring[i].tick = xTaskGetTickCountFromISR();
        ring[i].type = type;
        for (int k = 0; k < BLOCK_BYTES; k++) {
            ring[i].data[k] = data[k];
        }
        ring_in++;
    } else {
        p25_radio_stats.overruns++;
    }
    if (task) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/* the 288 bits of a block, from the modem's order to the order on the air, as dibits (Figure 70, RX_TYPE_INFO[1:0]):
   0, 1: CACH 24, sync 48, info 108, info 108 -> CACH, info, sync, info
   2:    CACH 24, slot type 10, slot type 10, sync 48, info 98, info 98 -> CACH, info, slot type, sync, slot type, info
   false for 3 (not described) */
static bool to_air(uint32_t type, const uint8_t *data, uint8_t *dibits)
{
    static const uint16_t L01[][2] = {{0, 24}, {72, 108}, {24, 48}, {180, 108}};          // (from, length) in order
    static const uint16_t L2[][2] = {{0, 24}, {92, 98}, {24, 10}, {44, 48}, {34, 10}, {190, 98}};
    const uint16_t(*map)[2];
    int parts;
    uint8_t bits[288];

    switch (type & 3) {
    case 0:
    case 1:
        map = L01;
        parts = 4;
        break;
    case 2:
        map = L2;
        parts = 6;
        break;
    default:
        return false;
    }
    int n = 0;
    for (int p = 0; p < parts; p++) {
        for (int i = 0; i < map[p][1]; i++) {
            int b = map[p][0] + i;
            bits[n++] = (uint8_t)((data[b >> 3] >> (7 - (b & 7))) & 1);
        }
    }
    for (int i = 0; i < 144; i++) {
        dibits[i] = (uint8_t)((bits[2 * i] << 1) | bits[2 * i + 1]);
    }
    return true;
}

static void audio(bool on)
{
    if (on != audio_on) {
        audio_on = on;
        play_logged = false;
        if (!on) {
            sound_rx_prebuffer = PLAY_START_DMR;
        }
        if (on) {
            ev(11, 0);
        }
        radio_dmr_audio(on);
        leds_green(on);
        if (!on) {
            soundTerminateSound();
        }
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// The event log (ms, event, arg) for the delays, read over the cable (cleared by writing p25_ev_n 0): 1 the key, 2 an
// LDU into the FIFO (arg: its number), 3 the RF on (p25_phy_tx_begin() done), 10 a data unit received (arg: DUID),
// 11 the playback on, 12 the playback starts; from DMR (p25_ev_log()), to compare: 20 its playback starts, 21 its key
#define P25_EV 64
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

// the modem back to waiting for a sync, the blocks queued meanwhile dropped, the receiver hunting
static void rearm(void)
{
    p25_phy_rx_rearm();
    ring_out = ring_in;
    rearm_mark = ring_in;
    p25_rx_init(&rx, P25_NAC_ANY);
    have_last = false;
    have_held = false;
    p25_radio_stats.rearms++;
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
    pcm_in = pcm_out = 0;
    audio(false);
    imbe_dec_init(&dec);
    rearm(); // the next signal aligned afresh
}

static void call_begin(void)
{
    if (!call.active) {
        memset(&call, 0, sizeof(call));
        call.active = true;
        imbe_dec_init(&dec);
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

// A voice frame decoded into the queue for the playback
static void voice(const uint8_t imbe[18])
{
    if ((pcm_in - pcm_out) >= PCM_FRAMES) {
        p25_radio_stats.pcm_drops++;
        pcm_out++;
    }
    int kind = imbe_dec_frame(&dec, imbe, pcm[pcm_in % PCM_FRAMES]);
    p25_radio_stats.imbe_frames++;
    if (kind != 'V') {
        p25_radio_stats.imbe_repeat++;
    }
    if (call.encrypted) {
        memset(pcm[pcm_in % PCM_FRAMES], 0, sizeof(pcm[0]));
    }
    pcm_in++;
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
    last_unit = xTaskGetTickCount();
    switch (duid) {
    case P25_DUID_HDU:
        call_begin();
        // the call's clock starts here: else the last call's (or a TDU's) ends it at once, and its rearm loses the
        // first LDU (the modem hunts for a sync again)
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
            last_ldu = xTaskGetTickCount() - pdMS_TO_TICKS(CALL_TIMEOUT); // ends once the voice queued has played
        }
        break;
    default:
        break;
    }
}

// the playback buffers filled from the queue; the playback started once there is enough
static void feed(void)
{
    if (call.encrypted) { // no key: not played, the voice dropped (else it stays queued and the call never ends)
        pcm_out = pcm_in;
        if (audio_on) {
            audio(false);
        } else if (wavbuffer_count) {
            soundTerminateSound();
        }
        return;
    }
    while ((pcm_in != pcm_out) && (wavbuffer_count <= (PLAY_LEVEL - 2))) {
        const int16_t *frame = pcm[pcm_out % PCM_FRAMES];
        for (int half = 0; half < 2; half++) {
            soundSetupBuffer();
            memcpy(currentWaveBuffer, &frame[80 * half], 160);
            soundStoreBuffer();
        }
        pcm_out++;
        audio(true);
    }
    if (audio_on) {
        if (!play_logged && (wavbuffer_count >= (int)p25_play_start)) {
            play_logged = true;
            ev(12, (uint16_t)wavbuffer_count); // the playback starts (soundTickRXBuffer())
        }
        sound_rx_prebuffer = (int)p25_play_start;
        soundTickRXBuffer();
        if ((pcm_in == pcm_out) && (wavbuffer_count == 0) && call.active) {
            p25_radio_stats.underruns++;
        }
    }
}

// Each voice frame of an LDU decoded as soon as it is in (p25_rx_ldu_frames()), not with the whole LDU 180 ms later:
// the call's delay that much shorter. The LDU's LC / ES and the rest at its end (unit()).
static void receive(const uint8_t *dibits, int n)
{
    for (int d = 0; d < n; d++) {
        int duid = p25_rx_dibit(&rx, dibits ? dibits[d] : P25_DIBIT_ERASED);
        if (duid >= 0) {
            unit(duid);
            ldu_voiced = 0;
            continue;
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
}

/* A block's place on the air from when it came after the one before (docs/p25.md, measured against a known
   transmission): 30 ms, the next 144 dibits; a block after a sync found from nothing comes 53 ms after it, 112 dibits
   of the air not given (erasures in their place: the frame stays in step, and its Reed-Solomon codes take the words
   there as erasures - an HDU is decoded without them); a sync block put in
   while the blocks were going on comes some 7 ms after the last one and starts 32 dibits into it (the rest of that
   one is again in it); more than ~2 blocks late: the stream broke, the receiver hunts. The first sync found after
   power on is different: a block 30 ms after the sync block, of what the receive RAM held before (not the air), then
   the air from 222 past the sync on - that block's place, 78 .. 221, taken as not received (measured 2026-11-12).
   The held block is given up to where this one starts, and this one held. */
static void place(uint32_t tick, const uint8_t *dibits)
{
    static bool held_first; // the held block is the first since the modem was armed (its sync block)
    bool first = !have_held;
    if (have_held) {
        uint32_t dt = tick - held_tick;
        if (held_first && dt < 42) {
            dibits = NULL;
            p25_radio_stats.gaps++;
        }
        if (dt < 18) {
            receive(held, 32);
            p25_radio_stats.overlaps++;
        } else if (dt < 42) {
            receive(held, 144);
        } else if (dt < 66) {
            receive(held, 144);
            receive(NULL, 112);
            p25_radio_stats.gaps++;
        } else {
            receive(held, 144);
            p25_rx_init(&rx, P25_NAC_ANY);
        }
    }
    if (dibits) {
        memcpy(held, dibits, 144);
    } else {
        memset(held, P25_DIBIT_ERASED, 144);
    }
    held_tick = tick;
    have_held = true;
    held_first = first;
}

/* A transmission, while tx_want (the PTT) stays set: the receive off, the mic on, 20 ms frames into the encoder, 9
   frames an LDU (LDU1, LDU2 in turn) into the physical layer's FIFO after an HDU; the transmitter keyed once the first
   LDU is there (the FIFO holds the HDU and it then, and the mic keeps it ahead). The PTT released: the LDU finished
   with silence, an LDU2 last, a TDU; once sent, back to receive. */
static void transmit(void)
{
    tx_busy = true;
    ev(1, 0);
    if (call.active) {
        call_end();
    }
    audio(false);
    p25_phy_rx_stop();
    imbe_enc_init(&enc);
    sound_mic_start();
    p25_phy_tx_reset();

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

    int16_t pcm[IMBE_N];
    uint32_t tone_phase = 0;
    int halves = 0, nf = 0;
    uint32_t k = 0, fn = p25_tx_fn0;
    bool started = false, ending = false;
    int ldu_put = 0; // dibits of the LDU being sent in the FIFO already
    p25_tx_stats[0]++;
    bool failed = false;
    if (p25_tx_rf_after == 0) { // at once: the preamble and the HDU out while the first frames are encoded
        failed = (p25_phy_tx_begin(p25_tx_low_power != 0) != 0);
        if (!failed) {
            ev(3, 0);
            started = true;
        }
    }
    while (!failed) {
        if (!tx_want) {
            ending = true;
        }
        if (ending && !started && (k == 0) && (nf == 0)) {
            break; // released before anything was sent
        }
        bool frame = false;
        if (ending) {
            if (p25_phy_tx_room() > 2 * P25_FRAME_MAX) { // silence, as fast as the FIFO takes it
                memset(pcm, 0, sizeof(pcm));
                frame = true;
            }
        } else if (sound_mic_take(&pcm[80 * halves])) {
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
                pcm[i] = (int16_t)((cos_q12(tone_phase) * TONE_LEVEL) >> 10);
                tone_phase += TONE_STEP;
            }
        }
        uint8_t u[88];
        int have;
        if (p25_tx_source == 1) {
            p25_phy_tx_frame(fn++, u);
            have = 1;
        } else {
            have = imbe_enc_frame(&enc, pcm, u);
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
        if (!started && (nf >= p25_tx_rf_after)) { // the preamble and the HDU are in the FIFO already
            if (p25_phy_tx_begin(p25_tx_low_power != 0) != 0) {
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
        p25_phy_tx_pad(); // the TDU's block whole: sent
        while ((p25_phy_tx_level() >= P25_PHY_BLOCK_BYTES * 4) && (p25_phy_tx_underrun < 10)) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        vTaskDelay(pdMS_TO_TICKS(60)); // the last blocks out of the modem
        p25_phy_tx_end();
    }
    soundStopMicCapture();

    // back to receive
    p25_rx_init(&rx, P25_NAC_ANY);
    imbe_dec_init(&dec);
    pcm_in = pcm_out = 0;
    have_last = false;
    have_held = false;
    ring_out = ring_in;
    p25_phy_rx_start(block_isr);
    last_unit = xTaskGetTickCount();
    rearm_mark = ring_in;
    tx_busy = false;
}

void p25_radio_transmit(bool on)
{
    tx_want = on;
    if (task) {
        xTaskNotifyGive(task);
    }
}

bool p25_radio_transmitting(void)
{
    return tx_want || tx_busy;
}

static void task_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        busy = true;
        if (running && tx_want) {
            transmit();
        }
        if (!running) {
            ring_out = ring_in;
            busy = false;
            continue;
        }
        if (ring_out != ring_in) {
            // the modem found a sync (it gives no block before): the receive power saving ends (radio.c eco_tick(),
            // which knows the FM carrier and the DMR sync only), else the receiver went off and on in turns under a
            // call and the modem lost it, blocks of fill and errors for hundreds of ms (2026-11-12)
            radio_eco_activity();
        }
        while (ring_out != ring_in) {
            uint32_t i = ring_out % RING;
            uint8_t dibits[144];
            p25_radio_stats.blocks++;
            p25_tap_block_t *t = &p25_tap[p25_tap_head % P25_TAP_BLOCKS];
            t->ms = ring[i].tick * portTICK_PERIOD_MS; // when it came
            t->type = ring[i].type;
            memcpy(t->data, ring[i].data, BLOCK_BYTES);
            p25_tap_head++;
            uint32_t tick = ring[i].tick;
            if (!have_held) { // the first since armed: a sync found, REARM_MS from here for its data unit
                last_unit = xTaskGetTickCount();
            }
            if (have_last && (memcmp(ring[i].data, last_raw, BLOCK_BYTES) == 0)) {
                // not written (the modem gives the last one again while it is not in step with the signal): a
                // block's time of the air not received, erasures in its place
                p25_radio_stats.stale++;
                ring_out++;
                place(tick, NULL);
                continue;
            }
            memcpy(last_raw, ring[i].data, BLOCK_BYTES);
            have_last = true;
            bool ok = to_air(ring[i].type, ring[i].data, dibits);
            ring_out++;
            place(tick, ok ? dibits : NULL);
        }
        feed();
        // Once it found a sync, the modem gives a block every 30 ms for good, noise after the signal: with no data
        // unit for REARM_MS from the sync on, the receive is armed again and the modem waits for the next sync. Also
        // with no block at all, every HUNT_REARM_MS: after some signals (a transmission from the other radio cut
        // short, measured 2026-11-12) the modem gave none and found no sync any more until armed again. Not every
        // REARM_MS then: an arming as a call's sync is found loses its first data unit (1 call in 7, 2026-11-12)
        bool hunting = (ring_in == rearm_mark); // no block since armed: the modem waiting for a sync
        bool idle = (xTaskGetTickCount() - last_unit) >= pdMS_TO_TICKS(hunting ? HUNT_REARM_MS : REARM_MS);
        if (!call.active && idle) {
            rearm();
            last_unit = xTaskGetTickCount();
        }
        if (call.active && (pcm_in == pcm_out) && (wavbuffer_count == 0) &&
            ((xTaskGetTickCount() - last_ldu) >= pdMS_TO_TICKS(CALL_TIMEOUT))) {
            call_end();
        }
        busy = false;
    }
}

void p25_radio_start(void)
{
    if (running) {
        return;
    }
    if (!task) {
        task = xTaskCreateStatic(task_main, "p25", sizeof(task_stack) / sizeof(task_stack[0]), NULL,
                                 tskIDLE_PRIORITY + 3, task_stack, &task_tcb);
    }
    // the modem to the physical layer first: the DMR engine stands aside from then on (its task no longer touches the
    // overlay RAM), then the decoder into it
    p25_phy_rx_start(block_isr);
    vTaskDelay(pdMS_TO_TICKS(5));
    overlay_use(OVERLAY_P25);
    p25_rx_init(&rx, P25_NAC_ANY);
    imbe_dec_init(&dec);
    pcm_in = pcm_out = 0;
    memset(&call, 0, sizeof(call));
    have_last = false;
    have_held = false;
    audio_on = false;
    ring_out = ring_in;
    rearm_mark = ring_in;
    last_unit = xTaskGetTickCount();
    running = true;
}

void p25_radio_stop(void)
{
    if (!running) {
        return;
    }
    running = false;
    while (busy) { // the task done with the overlay: DMR may load its vocoder next
        vTaskDelay(1);
    }
    p25_phy_rx_stop();
    call.active = false;
    audio(false);
}

bool p25_radio_call(p25_call_t *out)
{
    *out = call;
    return call.active;
}
