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
 * OpenDMR, service: the DMR receiver in software (dmr_soft.h, dmr_soft/README.md).
 *
 * As the NXDN and M17 receivers (afrx.c runs it): the modem records the FD6818's discriminator, dmr_demod.c finds
 * the TDMA time line and gives each 30 ms burst; dmr_l2.c decodes its fields. Each burst's timeslot: on a BS signal
 * from the CACH before it (TC: the channel of the burst that follows), in TDMA direct mode from its SYNC, an MS
 * signal (direct mode, one transmitter) taken as the channel's timeslot. Per timeslot:
 *
 * - a data burst (its Slot Type, colour code): a voice LC header starts the call (its LC through the DMR filters,
 *   dmr_tg_filter()), a terminator with LC ends it.
 * - a voice burst: A has the voice SYNC; B - F follow it 60 ms apart (also an A whose SYNC was missed), each with its
 *   EMB (colour code; two bad in a row: the superframe lost) and the embedded LC's fragments: a whole LC starts the
 *   call (late entry), or brings its talker alias and GPS position. The call's voice: the 3 AMBE+2 frames of each burst
 *   through our vocoder into the playback (voice_play.c), one call played at a time.
 * - no voice for CALL_TIMEOUT: the call over.
 *
 * The calls are shown as the engine's (dmr.c: the last heard list, the talker alias).
 *
 * Transmit, direct mode (Rx == Tx: soft_transmit()): the recording off, the bursts made here (dmr_l2.c) and sent
 * through the P25 physical layer's FIFO, whose 30 ms blocks of 144 symbols are DMR's timeslots: ours the burst's 132
 * symbols centred in it (10.2.3.1: 6 symbols of guard each side), the other one empty. The voice LC header (one: the
 * transmitter keyed once the first burst's first two AMBE frames are encoded, so the burst follows it at once), the
 * voice superframes (A the voice SYNC; B - E the LC embedded; F the null message), the last one finished with silence
 * after the PTT, then the terminator with LC. The FIFO run dry (zeros sent) an odd number of blocks: one more put in,
 * the bursts kept in their timeslot. Repeater mode: the HR_C7000 engine transmits (dmr_engine_transmit()). The
 * demodulator's state and the voice
 * queue live after the vocoder's RAM (ckcpu.ld .dmrs_rx, where NXDN's is when NXDN runs).
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "systime.h"
#include "modem.h"
#include "vocoder.h"
#include "p25_phy.h"
#include "p25_radio.h"
#include "dmr_compat.h"
#include "hr_c7000.h"
#include "dmr.h"
#include "afrx.h"
#include "radio_hooks.h"
#include "voice_play.h"
#include "dmr_demod.h"
#include "dmr_l2.h"
#include "dmr_soft.h"
#include "ambe.h"

#define PCM_FRAMES    16  // 320 ms of decoded voice
#define PLAY_START    3   // frames queued before the playback starts (they come 3 at a time every 60 ms, with a
#define PLAY_START_MS 25  // jitter of the recording's 16 ms quarters): the first burst's frames, kept this long
#define CALL_TIMEOUT  400 // ms without a voice burst on the call's timeslot: the call over
#define LOST_TIMEOUT  150 // ... its voice superframe no longer followed (no terminator came)
#define TAP_SAMPLES   2048 // the last samples recorded, for tools over the cable (256 ms)
#define BURST_LOG     32   // the last bursts, for tools over the cable
#define A_MSE_MAX     77   // an A without its SYNC taken (the sync missed) when its symbols fit under 0.30 level^2
                           // (Q8): noise is 0.31 - 0.45, a signal at -116 dBm 0.12 - 0.2
#define TX_HEADERS    1    // voice LC headers before the voice: one (ETSI TS 102 361-1 5.1.2.2, conventional: "a voice
                           // LC header shall be sent", burst A immediately after it); three cost ~120 ms more delay
#define TX_KEY_AFTER  2    // the transmitter keyed once this many AMBE frames of the first burst are encoded: the
                           // header goes out while the third is, the burst ready ~50 ms before it is due. After the
                           // first only, 1 call in 12 ran the FIFO dry between the header and the burst (2026-11-20)
#define TX_GUARD      6    // symbols each side of a burst in its 144
#define TX_END_MS     3000 // the engine's transmission ending (a repeater's hang time) at most, before receiving again

#define OVERLAY_BSS __attribute__((section(".dmrs_bss"))) // after the vocoder's RAM (ckcpu.ld), not zeroed

volatile uint32_t dmr_soft_enabled = 1;
volatile uint32_t dmr_soft_tx = 1, dmr_soft_tx_low_power, dmr_soft_tx_stats[4];
volatile uint32_t dmr_soft_tx_gate = 1; // 0: the PA on in both timeslots (tests)
volatile dmr_soft_stats_t dmr_soft_stats;
// tests against known content (over the cable, the transmitter's vocoder_tx_test): not 0, each AMBE+2 frame played
// judged: by its place in the burst, frames, bad (C0 + C1 corrections > 3), the tone frame after the FEC
volatile uint32_t dmr_soft_tone_check;
volatile uint32_t dmr_soft_tone_stats[3][3];

static dmr_demod dm OVERLAY_BSS;
static int16_t pcm[PCM_FRAMES][VOICE_PLAY_SAMPLES] OVERLAY_BSS;
// the recording (ring, dmr_soft_tap_n samples so far; dmr_soft_tap_hold 1: kept as it is, more: samples still to take
// before that)
int16_t dmr_soft_tap[TAP_SAMPLES] OVERLAY_BSS;
volatile uint32_t dmr_soft_tap_n, dmr_soft_tap_hold;
// the last bursts: the 33 bytes, the sync kind, the timeslot, the mse (Q4 level^2, at most 255), slots since a sync
uint8_t dmr_soft_burst_log[BURST_LOG][37] OVERLAY_BSS;
volatile uint32_t dmr_soft_burst_log_n;

typedef struct {
    bool active;      // a call followed: its LC taken
    int pos;          // the last voice burst's place in the superframe: 0 A .. 5 F; -1 none followed
    int bad_emb;      // bursts in a row without a good EMB
    int cc;           // the colour code of the voice followed (its first good EMB; -1 not yet)
    dmr_emb_lc emb;
    TickType_t last;  // the last voice burst or header of the call
} slot_t;

static slot_t slots[2];
static uint32_t burst_n;
static int phase;      // the timeslot of burst n: (n + phase) & 1
static int playing;    // the timeslot played (-1 none)

/* ---- the calls ---- */

volatile uint32_t dmr_soft_tx_realigned;  // the FIFO's timeslot pairs put back in step (put_burst())
volatile uint32_t dmr_soft_log_all_voice; // tests over the cable: every voice burst received into the delay log
static uint32_t rx_voice_logged; // voice bursts of the call in the delay log (p25_radio.c)

static void call_end(int ts)
{
    slot_t *s = &slots[ts];
    s->active = false;
    s->pos = -1;
    if (playing == ts) {
        playing = -1;
        voice_play_end(); // what is queued played out, then the speaker off
        dmr_call_over();
    }
}

// a voice call's LC (a header's, an embedded one): the call on ts followed if the filters take it
static void call_lc(int ts, const uint8_t lc[9])
{
    dmr_lc_t p;
    dmr_lc_parse(lc, &p);
    slot_t *s = &slots[ts];
    if ((p.flco != DMR_FLCO_GROUP) && (p.flco != DMR_FLCO_UNIT)) {
        if (s->active && (playing == ts) && (p.flco >= 4) && (p.flco <= 8)) {
            dmr_lc_heard(lc, (uint8_t)ts); // a talker alias block, a GPS position
        }
        return;
    }
    uint32_t tg_or_pc = (p.flco == DMR_FLCO_UNIT) ? (((uint32_t)PC_CALL_FLAG << 24) | p.dst) : p.dst;
    if (!dmr_tg_filter(tg_or_pc, p.src)) {
        dmr_soft_stats.filtered++;
        if (s->active) {
            call_end(ts);
        }
        return;
    }
    s->last = xTaskGetTickCount();
    if (!s->active) {
        s->active = true;
        rx_voice_logged = 0;
        dmr_soft_stats.calls++;
        if (playing < 0) {
            playing = ts;
            vocoderReset();
            voice_play_prepare(); // the speaker on while the voice comes (the header is before it)
        }
    }
    if (playing == ts) {
        dmr_lc_heard(lc, (uint8_t)ts);
    }
}

static void voice(const uint8_t b[DMR_BURST_BYTES])
{
    uint8_t ambe[3][DMR_AMBE_BYTES];
    dmr_voice_frames(b, ambe);
    uint32_t t0 = systime_get_us();
    for (int f = 0; f < 3; f++) {
        int16_t *out = voice_play_frame();
        if (!out) { // the playback fell behind: the newest frame dropped
            dmr_soft_stats.pcm_drops++;
            continue;
        }
        vocoderDecodeFrame(ambe[f], out);
        voice_play_put();
        if (dmr_soft_tone_check) {
            static const uint8_t tone[DMR_AMBE_BYTES] = {0xCE, 0xA8, 0xFE, 0x83, 0xAC, 0xC4, 0x58, 0x20, 0x0A};
            uint8_t d[49], ref[49];
            int e = ambe_decode_frame(ambe[f], d);
            ambe_decode_frame(tone, ref);
            dmr_soft_tone_stats[f][0]++;
            dmr_soft_tone_stats[f][1] += (e > 3);
            dmr_soft_tone_stats[f][2] += !memcmp(d, ref, sizeof(d));
        }
    }
    uint32_t took = systime_get_us() - t0;
    if (took > dmr_soft_stats.dec_us_max) {
        dmr_soft_stats.dec_us_max = took;
    }
}

/* ---- the bursts ---- */

static bool cc_ok(uint8_t cc)
{
    if ((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) && (cc != trxGetDMRColourCode())) {
        dmr_soft_stats.other_cc++;
        return false;
    }
    return true;
}

static bool ts_ok(int ts)
{
    return !(nonVolatileSettings.dmrCcTsFilter & DMR_TS_FILTER_PATTERN) || (ts == trxGetDMRTimeSlot());
}

static void log_burst(const dmr_burst_t *b, int ts)
{
    uint8_t *e = dmr_soft_burst_log[dmr_soft_burst_log_n % BURST_LOG];
    memcpy(e, b->bits, DMR_BURST_BYTES);
    e[33] = (uint8_t)b->sync;
    e[34] = (uint8_t)ts;
    int32_t m = b->mse >> 4;
    e[35] = (uint8_t)((m > 255) ? 255 : m);
    e[36] = (uint8_t)((b->since_sync > 255) ? 255 : b->since_sync);
    dmr_soft_burst_log_n++;
}

static void on_burst(void *ctx, const dmr_burst_t *b)
{
    (void)ctx;
    uint32_t n = burst_n++;
    // the timeslot
    if (DMR_SYNC_IS_BS(b->sync)) {
        uint8_t at, tc, lcss;
        dmr_tact(b->cach, &at, &tc, &lcss);
        phase = (int)((tc - n) & 1);
    } else if ((b->sync == DMR_SYNC_TS1_VOICE) || (b->sync == DMR_SYNC_TS1_DATA)) {
        phase = (int)((0 - n) & 1);
    } else if ((b->sync == DMR_SYNC_TS2_VOICE) || (b->sync == DMR_SYNC_TS2_DATA)) {
        phase = (int)((1 - n) & 1);
    } else if (b->sync != DMR_SYNC_NONE) { // MS: one transmitter, on the channel's timeslot
        phase = (int)(((uint32_t)trxGetDMRTimeSlot() - n) & 1);
    }
    int ts = (int)((n + (uint32_t)phase) & 1);
    slot_t *s = &slots[ts];
    log_burst(b, ts);
    if (!ts_ok(ts)) {
        return;
    }
    if (DMR_SYNC_IS_DATA(b->sync)) {
        uint8_t cc, dt, lc[12];
        s->pos = -1;
        if (!dmr_slot_type(b->bits, &cc, &dt) || !cc_ok(cc)) {
            return;
        }
        if ((dt == DMR_DT_VOICE_LC_HEADER) && dmr_bptc196(b->bits, lc) && dmr_full_lc_check(lc, DMR_MASK_VOICE_LC_HEADER)) {
            dmr_soft_stats.headers++;
            p25_ev_log(42, (uint16_t)ts); // the delay log (p25_radio.c): a voice LC header received
            call_lc(ts, lc);
        } else if ((dt == DMR_DT_TERMINATOR_LC) && dmr_bptc196(b->bits, lc) &&
                   dmr_full_lc_check(lc, DMR_MASK_TERMINATOR_LC)) {
            dmr_soft_stats.terminators++;
            if (s->active) {
                call_end(ts);
            }
        }
        return;
    }
    // a voice burst: A (its SYNC), or the next of the superframe followed
    if (DMR_SYNC_IS_VOICE(b->sync)) {
        if (s->pos < 0) {
            s->cc = -1;
        }
        s->pos = 0;
        s->bad_emb = 0;
    } else if (s->pos >= 0) {
        s->pos = (s->pos + 1) % 6;
    } else {
        return;
    }
    if (s->pos > 0) {
        uint8_t cc, pi, lcss, frag[4], lc[9];
        // a good EMB of the voice's colour code (a random word passes the QR (16,7,6) one time in 4)
        bool good = dmr_emb(b->bits, &cc, &pi, &lcss) && ((s->cc < 0) || (cc == s->cc));
        if (good) {
            dmr_soft_stats.emb_ok++;
            s->bad_emb = 0;
            if (!cc_ok(cc)) {
                s->pos = -1;
                return;
            }
            s->cc = cc;
            dmr_embedded(b->bits, frag);
            if (dmr_emb_lc_add(&s->emb, lcss, frag, lc)) {
                dmr_soft_stats.emb_lcs++;
                call_lc(ts, lc);
            }
        } else {
            dmr_soft_stats.emb_bad++;
            if (++s->bad_emb >= 2) { // the voice is over (or too weak to follow)
                s->pos = -1;
                return;
            }
        }
    } else if ((b->sync == DMR_SYNC_NONE) && (s->bad_emb || (b->mse > A_MSE_MAX))) {
        s->pos = -1; // an A without its SYNC: not a voice burst seen (the voice over)
        return;
    }
    dmr_soft_stats.voice_bursts++;
    if (s->active) {
        s->last = xTaskGetTickCount();
        if (playing == ts) {
            if ((rx_voice_logged < 3) || dmr_soft_log_all_voice) {
                p25_ev_log(43, (uint16_t)rx_voice_logged++); // a voice burst received, before its decoding
            }
            voice(b->bits);
        }
    }
}

/* ---- the playback (voice_play.c) ---- */

static const voice_play_cfg_t play_cfg = {
    .frames = pcm,
    .count = PCM_FRAMES,
    .start = PLAY_START,
    .start_ms = PLAY_START_MS,
    .underruns = &dmr_soft_stats.underruns,
};

/* ---- receiving (afrx.c runs it) ---- */

static void rx_init(void)
{
    dmr_demod_init(&dm, on_burst, NULL);
    memset(slots, 0, sizeof(slots));
    for (int ts = 0; ts < 2; ts++) {
        slots[ts].pos = -1;
        slots[ts].cc = -1;
        dmr_emb_lc_reset(&slots[ts].emb);
    }
    playing = -1;
    burst_n = 0;
    phase = 0;
    vocoderReset();
}

static void samples(int16_t *x, int n)
{
    for (int i = 0; (i < n) && (dmr_soft_tap_hold != 1); i++) {
        dmr_soft_tap[dmr_soft_tap_n % TAP_SAMPLES] = x[i];
        dmr_soft_tap_n++;
        if (dmr_soft_tap_hold > 1) {
            dmr_soft_tap_hold--;
        }
    }
    dmr_demod_samples(&dm, x, n);
    dmr_soft_stats.bursts = dm.bursts;
    dmr_soft_stats.syncs = dm.syncs;
    dmr_soft_stats.lost = dm.lost;
    dmr_soft_stats.relocks = dm.relocks;
}

static void poll(void)
{
    TickType_t now = xTaskGetTickCount();
    for (int ts = 0; ts < 2; ts++) {
        uint32_t timeout = (slots[ts].pos < 0) ? LOST_TIMEOUT : CALL_TIMEOUT;
        if (slots[ts].active && ((now - slots[ts].last) >= pdMS_TO_TICKS(timeout))) {
            call_end(ts);
        }
    }
}

static void end_call(void)
{
    for (int ts = 0; ts < 2; ts++) {
        if (slots[ts].active) {
            call_end(ts);
        }
    }
}

/* ---- transmit in software (direct mode) ---- */

// the transmission's buffers off the stack: the AMBE encoder takes most of it
static struct {
    uint8_t lc[9], full[12], frags[4][4], b[DMR_BURST_BYTES], ambe[3][DMR_AMBE_BYTES], d[144];
    int16_t pcm[160];
} txm OVERLAY_BSS;

// a timeslot pair into the FIFO: ours (the guard, the burst, the guard), then the other one empty. The FIFO run dry
// sends a block of zeros in its place: an odd number of them would move every burst after into the other timeslot (a
// receiver following the first ones lost all the rest of the call, 2026-11-20): one more then, the pairs in step again
static uint32_t tx_underruns_seen;

static void put_burst(const uint8_t b[DMR_BURST_BYTES])
{
    uint8_t *d = txm.d;
    memset(d, 0, sizeof(txm.d));
    uint32_t u = p25_phy_tx_underrun;
    if ((u - tx_underruns_seen) & 1) {
        p25_phy_tx_put(d, sizeof(txm.d)); // zeros: the other timeslot's block
        dmr_soft_tx_realigned++;
        p25_ev_log(38, (uint16_t)u); // the delay log: the timeslots put back in step (arg underruns so far)
    }
    tx_underruns_seen = u;
    for (int k = 0; k < 132; k++) {
        d[TX_GUARD + k] = (uint8_t)((b[k >> 2] >> (6 - 2 * (k & 3))) & 3);
    }
    while (p25_phy_tx_room() < 2 * sizeof(txm.d)) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    p25_phy_tx_put(d, sizeof(txm.d));
    memset(d, 0, sizeof(txm.d));
    p25_phy_tx_put(d, sizeof(txm.d));
    dmr_soft_tx_stats[1]++;
}

static void soft_transmit(void)
{
    static const uint8_t silence[DMR_AMBE_BYTES] = {0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B};
    static const uint8_t lcss[5] = {DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST, DMR_LCSS_SINGLE};
    static const uint8_t null_frag[4] = {0, 0, 0, 0};
    uint8_t *lc = txm.lc, *full = txm.full, *b = txm.b;
    uint8_t (*frags)[4] = txm.frags;
    uint8_t (*ambe)[DMR_AMBE_BYTES] = txm.ambe;
    uint8_t cc = trxGetDMRColourCode();
    bool pc = (trxTalkGroupOrPcId >> 24) == PC_CALL_FLAG;

    dmr_lc_make(lc, pc ? DMR_FLCO_UNIT : DMR_FLCO_GROUP, 0, trxTalkGroupOrPcId & 0xFFFFFF, trxDMRID);
    dmr_emb_lc_make(lc, frags);
    vocoderReset();
    afrx_fifo_prepare(MODEM_MOD_SCALE_ONE); // the modem's 4FSK level as calibrated: DMR's +-1944 Hz
    tx_underruns_seen = 0; // the FIFO's count from 0 (p25_phy_tx_reset())
    dmr_soft_tx_stats[0]++;
    dmr_soft_tx_stats[1] = dmr_soft_tx_stats[2] = 0;
    dmr_full_lc_make(lc, DMR_MASK_VOICE_LC_HEADER, full);
    dmr_burst_data(b, full, cc, DMR_DT_VOICE_LC_HEADER, DMR_SYNC_MS_DATA);
    for (int i = 0; i < TX_HEADERS; i++) {
        put_burst(b);
        p25_ev_log(33, (uint16_t)i); // the delay log (p25_radio.c): a voice LC header into the FIFO
    }
    bool keyed = false;
    int16_t *pcm = txm.pcm;
    int halves = 0, nf = 0, pos = 0, tx_voice_logged = 0;
    bool ending = false;
    for (;;) {
        if (!afrx_ptt_held()) {
            ending = true;
        }
        if (ending && (nf == 0) && (pos == 0)) {
            break; // the superframe finished
        }
        if (ending) {
            memcpy(ambe[nf], silence, DMR_AMBE_BYTES);
        } else if (sound_mic_take(&pcm[80 * halves])) {
            if (++halves < 2) {
                continue;
            }
            halves = 0;
            uint32_t enc_t0 = systime_get_us();
            vocoderEncodeFrame(pcm, ambe[nf]);
            if (dmr_soft_tx_stats[2] < 4) {
                p25_ev_log(37, (uint16_t)((systime_get_us() - enc_t0) / 100)); // the delay log: a frame encoded (arg
            }                                                                   // its time, 0.1 ms)
            dmr_soft_tx_stats[2]++;
            if (!keyed && (nf + 1 >= TX_KEY_AFTER)) { // on the air now (the mic recording on meanwhile)
                keyed = afrx_fifo_key(dmr_soft_tx_low_power != 0);
                p25_ev_log(35, keyed); // the delay log: keyed
                if (!keyed) {
                    break;
                }
                if (dmr_soft_tx_gate) {
                    radio_phy_tx_gate(true); // the PA on for our timeslot only
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (++nf < 3) {
            continue;
        }
        nf = 0;
        if (pos == 0) {
            dmr_burst_voice_sync(b, (const uint8_t(*)[DMR_AMBE_BYTES])ambe, DMR_SYNC_MS_VOICE);
        } else {
            dmr_burst_voice_emb(b, (const uint8_t(*)[DMR_AMBE_BYTES])ambe, cc, 0, lcss[pos - 1],
                                (pos < 5) ? frags[pos - 1] : null_frag);
        }
        put_burst(b);
        if (tx_voice_logged < 3) {
            p25_ev_log(34, (uint16_t)tx_voice_logged++); // a voice burst into the FIFO
        }
        pos = (pos + 1) % 6;
    }
    if (keyed) {
        dmr_full_lc_make(lc, DMR_MASK_TERMINATOR_LC, full);
        dmr_burst_data(b, full, cc, DMR_DT_TERMINATOR_LC, DMR_SYNC_MS_DATA);
        put_burst(b);
    }
    uint32_t underruns = afrx_fifo_finish(keyed);
    radio_phy_tx_gate(false);
    if (keyed) {
        dmr_soft_tx_stats[3] = underruns;
    }
}

// The PTT held: the recording and the playback are off (afrx.c). Direct mode: in software; repeater mode: the engine
// transmits from its receive state from scratch, until the PTT is released and its transmission (a repeater's hang
// time) is over
static void transmit(void)
{
    if (dmr_soft_tx && currentChannelData && (currentChannelData->rx_freq == currentChannelData->tx_freq)) {
        soft_transmit();
        return;
    }
    dmr_engine_resume();
    dmr_engine_transmit(true);
    while (afrx_ptt_held()) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    dmr_engine_transmit(false);
    TickType_t t0 = xTaskGetTickCount();
    while (dmr_engine_transmitting() && ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(TX_END_MS))) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ---- data (APRS over DMR) ----
   The engine sends the data bursts (HRC7000SendDataBursts()), as it does the repeater mode's voice: the recording
   off meanwhile (afrx.c), the engine resumed, then back to receiving in software. Sent with the recording on, the engine
   standing aside, they never went out (#107: 0.7.0's DMR APRS beacons, 2026-11-21). */
#define DATA_MS 5000 // the bursts sent at most (a repeater woken first)

static const afrx_ops_t ops;

static struct {
    uint8_t types[DMR_DATA_BURSTS_MAX], lengths[DMR_DATA_BURSTS_MAX], payloads[DMR_DATA_BURSTS_MAX][12];
    uint8_t count;
} data_req;
static volatile bool data_pending, data_failed, data_cancel;

static void data(void)
{
    dmr_engine_resume();
    bool ok = HRC7000SendDataBursts(data_req.types, data_req.lengths, (const uint8_t(*)[12])data_req.payloads,
                                    data_req.count);
    TickType_t t0 = xTaskGetTickCount();
    while (ok && !HRC7000DataBurstsDone() && !data_cancel && ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(DATA_MS))) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ok && !HRC7000DataBurstsDone()) {
        HRC7000AbortDataBursts(); // cancelled, or too long
        ok = false;
    }
    data_failed = !ok || HRC7000DataBurstsFailed();
    t0 = xTaskGetTickCount();
    while (dmr_engine_transmitting() && ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(TX_END_MS))) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    data_pending = false;
}

bool dmr_soft_send_data(const uint8_t *types, const uint8_t *lengths, const uint8_t (*payloads)[12], uint8_t count)
{
    if (data_pending) {
        return false; // the last still going (the radio task sends one at a time)
    }
    data_failed = false; // also for the engine's own way (dmr.c, not received in software)
    if (!afrx_running(&ops) || (count > DMR_DATA_BURSTS_MAX)) {
        return false;
    }
    memcpy(data_req.types, types, count);
    memcpy(data_req.lengths, lengths, count);
    memcpy(data_req.payloads, payloads, (size_t)count * 12);
    data_req.count = count;
    data_failed = false;
    data_cancel = false;
    data_pending = true;
    afrx_data();
    return true;
}

bool dmr_soft_data_busy(void)
{
    return data_pending;
}

bool dmr_soft_data_failed(void)
{
    return data_failed;
}

void dmr_soft_data_cancel(void)
{
    data_cancel = true;
}

static const afrx_ops_t ops = {
    .bw = AFREC_BW_FLAT, // no band-pass, no de-emphasis (the RRC is ours)
    .overlay = OVERLAY_AMBE,
    .play = &play_cfg,
    .rx_init = rx_init,
    .samples = samples,
    .poll = poll,
    .end_call = end_call,
    .transmit = transmit,
    .data = data,
    .stats = {
        .halves = &dmr_soft_stats.halves,
        .missed = &dmr_soft_stats.missed_halves,
        .busy_us_max = &dmr_soft_stats.busy_us_max,
    },
};

bool dmr_soft_wanted(void)
{
    return dmr_soft_enabled && (settingsUsbMode != USB_MODE_HOTSPOT);
}

void dmr_soft_start(void)
{
    afrx_start(&ops);
}

void dmr_soft_stop(void)
{
    if (data_pending) { // asked, not sent: failed
        data_pending = false;
        data_failed = true;
    }
    if (afrx_running(&ops)) {
        afrx_stop();
        dmr_call_over();
    }
}

bool dmr_soft_running(void)
{
    return afrx_running(&ops);
}

void dmr_soft_ptt(bool on)
{
    afrx_ptt(on);
}

bool dmr_soft_transmitting(void)
{
    return afrx_running(&ops) && afrx_transmitting();
}

bool dmr_soft_has_sync(void)
{
    return afrx_running(&ops) && dm.locked;
}

bool dmr_soft_busy(void)
{
    return afrx_running(&ops) && (slots[0].active || slots[1].active);
}
