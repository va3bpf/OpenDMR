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
 * OpenDMR, service: the YSF (System Fusion) receiver and transmitter (ysf_radio.h, ysf/README.md).
 *
 * As M17's and P25's (m17_radio.c, p25_radio.c): the modem records its AF input (the FD6818's discriminator, 8 kHz,
 * no de-emphasis) in halves of 256 samples; the software receivers' task (afrx.c) gives each half to p25_demod.c set
 * up with YSF's 20-symbol sync (C4FM, 4800 symbols/s, root raised cosine 0.2: P25's receive filter is YSF's), whose
 * dibits go into ysf_core.c (frames, FICH, DCH, voice). Each V/D mode 2 or 1 frame's five AMBE+2 frames go through
 * our vocoder into the playback's queue of 20 ms frames (voice_play.c, the prompt FIFO).
 *
 * The call: from a header (HC: CSD1 = the destination and the caller) or late, from a V/D frame with its FICH
 * decoded (the caller then from the FN 1 frame's DCH); ended by the terminator (TC) or CALL_TIMEOUT without a frame.
 *
 * Transmit (transmit(), the PTT): the recording stopped, the mic's 20 ms frames through our AMBE+2 encoder, five a
 * frame: the preamble (+3 -3), a header, V/D mode 2 frames (FN 0 .. 6, FT 6: the destination ALL as ten '*', our
 * callsign, ...), the last finished with silence, a terminator; the dibits through the P25 physical layer's transmit
 * FIFO (4800 symbols/s), the modem's 4FSK level scaled to YSF's +-2.7 kHz wide (ysf_tx_dev), as in v0.10.0. A Pi-Star
 * configured for YSF wide needs this; narrow only works there with LoDev=1 in MMDVMHost.
 *
 * The receiver's state lives in the AMBE overlay's RAM after the vocoder's (ckcpu.ld .ysf_rx, as NXDN's).
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "systime.h"
#include "vocoder.h"
#include "dmr_compat.h"
#include "p25_phy.h"
#include "p25_radio.h"
#include "p25_demod.h"
#include "codeplug.h"
#include "ysf_core.h"
#include "ysf_radio.h"
#include "ui.h"
#include "voice_play.h"
#include "afrx.h"

#define PCM_FRAMES    16  // 320 ms of decoded voice
#define PLAY_START    4   // frames queued before the playback starts, and the time from the first: the voice segments
#define PLAY_START_MS 80  // come 15 ms apart within a frame, 40 ms between frames, read in halves of 32 ms (decoded as
                          // they come, ysf_core.c); 40 ms ran dry every ~0.8 s, 60 ms with 3 frames left one micro-cut
                          // in the field; 80 ms with 4 frames adds ~20 ms more headroom for the remaining jitter
#define CALL_TIMEOUT  350 // ms without a frame: the call ended (a frame is 100 ms)
#define VOICE_BAD     9   // V/D mode 2: a voice frame with more triplets split than this (of 27) played as silence
#define TX_LEAD_MAX   960 // dibits of preamble at most: 200 ms of 0x77, as MMDVM/Yaesu key up

#define OVERLAY_BSS __attribute__((section(".ysf_bss"))) // after the vocoder's RAM (ckcpu.ld), not zeroed

volatile ysf_radio_stats_t ysf_radio_stats;
// the 4FSK level (MODEM_MOD_SCALE_ONE 1024: P25's +-1.8 kHz) for YSF's +-2.7 kHz wide (FICH Dev=0), as in v0.10.0.
// MMDVM/Pi-Star hotspots configured for YSF wide only decode this; narrow (768, Dev=1) needs LoDev=1 in MMDVMHost.
volatile uint32_t ysf_tx_dev = 1536;
// 200 ms of +3 -3 (0x77) before the first sync, as MMDVM/Yaesu key up: receivers need it to find sync before the
// header, which is the frame carrying CSD1 (our callsign). With only 40 ms some reflected calls were heard without
// the callsign on the original radios.
volatile uint32_t ysf_tx_lead = 960;
volatile uint32_t ysf_tx_low_power;    // not 0: 50 mW (tests with the radios side by side)
volatile uint32_t ysf_tx_stats[4];     // calls, frames, the longest AMBE encoding (us), underruns (the last call's)

static const p25_demod_sync DEMOD_SYNC = {YSF_SYNC, YSF_SYNC_DIBITS, 17}; // hunting: 17 of 20 signs right

static p25_demod dm OVERLAY_BSS;
static ysf_rx rx OVERLAY_BSS;
static int16_t pcm[PCM_FRAMES][VOICE_PLAY_SAMPLES] OVERLAY_BSS; // the playback's queue
static uint8_t tx_dibits[YSF_FRAME_DIBITS] OVERLAY_BSS;
static uint8_t tx_ambe[5][9] OVERLAY_BSS;
// the last frames' FICH and voice frames for tools over the cable (ysf/py): 4 FICH bytes, sync wrong, the voice
// errors (5), the 5 AMBE frames (45 bytes) = 55 bytes each
#define FRAME_LOG 64
uint8_t ysf_frame_log[FRAME_LOG][56] OVERLAY_BSS;
volatile uint32_t ysf_frame_log_n;

static volatile uint8_t tx_dgid; // the channel's (transmitted; any received, the call's shown)
static ysf_call_t call;
static TickType_t last_frame;
static uint32_t rx_frames_logged; // CC frames of the call in the delay log (p25_radio.c)
static volatile TickType_t heard_at;
static volatile bool heard;

/* ---- the call ---- */

static void call_publish(void)
{
    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_SIGNAL};

    ui_post(&ev);
}

static void call_begin(void)
{
    if (!call.active) {
        memset(&call, 0, sizeof(call));
        call.active = true;
        rx_frames_logged = 0;
        last_frame = xTaskGetTickCount();
        vocoderReset();
        voice_play_prepare(); // the speaker on while the voice comes
        ysf_radio_stats.calls++;
        call_publish();
    }
}

static void call_end(void)
{
    call.active = false;
    voice_play_end(); // what is queued played out, then the speaker off
    call_publish();
}

static void set_text(char *to, const uint8_t *from)
{
    char s[11];
    ysf_callsign(from, s);
    if (strcmp(s, to)) {
        memcpy(to, s, sizeof(s));
        call_publish();
    }
}

/* ---- the voice ---- */

static void voice(const uint8_t ambe[9], bool good)
{
    int16_t *out = voice_play_frame();
    if (!out) { // the playback fell behind: the newest frame dropped
        ysf_radio_stats.pcm_drops++;
        return;
    }
    uint32_t t0 = systime_get_us();
    if (good && !call.muted) {
        vocoderDecodeFrame(ambe, out);
    } else {
        memset(out, 0, sizeof(pcm[0]));
    }
    uint32_t took = systime_get_us() - t0;
    if (took > ysf_radio_stats.dec_us_max) {
        ysf_radio_stats.dec_us_max = took;
    }
    voice_play_put();
    ysf_radio_stats.voice_frames++;
}

static void log_frame(void)
{
    uint8_t *l = ysf_frame_log[ysf_frame_log_n % FRAME_LOG];
    ysf_fich_pack(&rx.fich, l);
    l[4] = (uint8_t)(rx.sync_wrong | (rx.fich_ok ? 0 : 0x80));
    for (int j = 0; j < 5; j++) {
        l[5 + j] = (j < rx.voices) ? rx.voice_err[j] : 0xFF;
        memcpy(&l[10 + 9 * j], rx.voice[j], 9);
    }
    ysf_frame_log_n++;
}

static bool taken; // the frame being read belongs to the call (its FICH said so): its voice played, its DCH read

// a frame's FICH in (ysf_core.c decodes a frame as it comes): the call begins or ends now, not at the frame's end
static void on_fich(void)
{
    const ysf_fich_t *f = &rx.fich;
    taken = false;
    ysf_radio_stats.frames++;
    ysf_radio_stats.fich_bad = rx.fich_bad;
    if ((tx_dgid != 0) && (f->sc != tx_dgid)) { // another DG-ID than the channel's: counted, played all the same
        ysf_radio_stats.other_dgid++;
    }
    heard = true;
    heard_at = xTaskGetTickCount();
    bool vd = (f->dt == YSF_DT_VD1) || (f->dt == YSF_DT_VD2);
    if (f->fi == YSF_FI_TC) {
        if (call.active) {
            call_end();
        }
        return;
    }
    if ((f->fi != YSF_FI_HC) && (f->fi != YSF_FI_CC)) {
        return;
    }
    // a header, or late entry: a frame whose FICH decoded (a guessed one only within a call)
    if (!call.active && !rx.fich_ok) {
        return;
    }
    if (f->fi == YSF_FI_HC) {
        p25_ev_log(62, 0); // the delay log (p25_radio.c): a header's FICH received
    } else if (rx_frames_logged < 3) {
        p25_ev_log(63, (uint16_t)rx_frames_logged++); // a V/D frame's FICH received
    }
    call_begin();
    taken = true;
    last_frame = xTaskGetTickCount();
    call.muted = !vd;
    call.dgid = f->sc;
}

// a voice segment of a V/D frame in: played at once
static void on_voice(void)
{
    if (!taken) {
        return;
    }
    int j = rx.voice_j;
    last_frame = xTaskGetTickCount();
    voice(rx.voice[j], (rx.fich.dt == YSF_DT_VD1) ? (rx.voice_err[j] <= 6) : (rx.voice_err[j] <= VOICE_BAD));
}

// the frame's end: the callsigns from its data channel(s)
static void on_end(void)
{
    const ysf_fich_t *f = &rx.fich;
    if (rx.voices == 5) { // the last voice segment: in with the frame's end
        on_voice();
    }
    ysf_radio_stats.dch_bad = rx.dch_bad;
    ysf_radio_stats.rep_err = rx.rep_err;
    ysf_radio_stats.rep_bits = rx.rep_bits;
    log_frame();
    if (!call.active || !rx.dch_ok[0] || ((f->fi != YSF_FI_HC) && !taken)) {
        return;
    }
    if (f->fi == YSF_FI_HC) { // CSD1: the destination, the caller
        set_text(call.dst, rx.dch[0]);
        set_text(call.src, rx.dch[0] + YSF_CALL_LEN);
    } else if (f->dt == YSF_DT_VD2) { // 10 bytes by the frame number: 0 the destination, 1 the caller
        if (f->fn == 0) {
            set_text(call.dst, rx.dch[0]);
        } else if (f->fn == 1) {
            set_text(call.src, rx.dch[0]);
        }
    } else if ((f->dt == YSF_DT_VD1) && (f->fn == 0)) { // V/D mode 1: CSD1 in frame 0
        set_text(call.dst, rx.dch[0]);
        set_text(call.src, rx.dch[0] + YSF_CALL_LEN);
    }
}

static void on_dibit(void *ctx, int d)
{
    (void)ctx;
    if (d == P25_DEMOD_LOST) {
        ysf_rx_reset(&rx);
        return;
    }
    switch (ysf_rx_dibit(&rx, (uint8_t)d)) {
    case YSF_RX_FICH:
        on_fich();
        break;
    case YSF_RX_VOICE:
        on_voice();
        break;
    case YSF_RX_FRAME:
        on_end();
        break;
    case YSF_RX_LOST:
        ysf_radio_stats.lost++;
        break;
    default:
        break;
    }
}

/* ---- the playback (voice_play.c) ---- */

static const voice_play_cfg_t play_cfg = {
    .frames = pcm,
    .count = PCM_FRAMES,
    .start = PLAY_START,
    .start_ms = PLAY_START_MS,
    .underruns = &ysf_radio_stats.underruns,
};

/* ---- transmit ---- */

static void rx_init(void)
{
    p25_demod_init_sync(&dm, on_dibit, NULL, &DEMOD_SYNC);
    ysf_rx_init(&rx);
    memset(&call, 0, sizeof(call));
    vocoderReset();
}

/* A transmission while the PTT stays held (afrx.c: the call ended, the playback and the recording off): the preamble
   and the header into the FIFO and the transmitter keyed (the first V/D frame comes 5 mic frames and their encoding
   later, ~110 ms: the preamble and the header cover that); then a frame for every 5 mic frames. The PTT released:
   the frame finished with silence, the terminator; once sent, back to receive. */
static void transmit(void)
{
    p25_ev_log(60, 0); // the delay log (p25_radio.c): YSF transmit() begins (the mic on)
    vocoderReset();
    afrx_fifo_prepare(ysf_tx_dev);
    ysf_tx_stats[0]++;
    ysf_tx_stats[1] = 0;

    char src[16];
    codeplug_get_radio_name(src);
    src[YSF_CALL_LEN] = 0;
    const char *dst = "**********"; // ALL (group / CQ)
    ysf_fich_t f = {.fi = YSF_FI_HC, .cs = 2, .cm = 0, .ft = 6, .dev = 0, .dt = YSF_DT_VD2,
                    .sc = (uint8_t)(tx_dgid & 0x7F)};

    uint32_t lead = (ysf_tx_lead > TX_LEAD_MAX) ? TX_LEAD_MAX : ysf_tx_lead;
    while (lead > 0) {
        uint32_t k = (lead > YSF_FRAME_DIBITS) ? YSF_FRAME_DIBITS : lead;
        while (p25_phy_tx_room() < YSF_FRAME_DIBITS + 144) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        ysf_tx_preamble(tx_dibits, (int)k);
        p25_phy_tx_put(tx_dibits, (int)k);
        lead -= k;
    }
    ysf_tx_hc_tc(&f, YSF_FI_HC, dst, src, tx_dibits);
    p25_phy_tx_put(tx_dibits, YSF_FRAME_DIBITS);
    bool started = afrx_fifo_key(ysf_tx_low_power != 0);
    p25_ev_log(61, started); // the preamble and the header in, keyed
    int16_t mic[160];
    int halves = 0, nf = 0, fn = 0;
    bool ending = false;
    while (started) {
        if (!afrx_ptt_held()) {
            ending = true;
        }
        if (ending && (nf == 0)) {
            break;
        }
        bool frame = false;
        if (ending) {
            memset(mic, 0, sizeof(mic)); // silence to the end of the frame of 5
            frame = true;
        } else if (sound_mic_take(&mic[80 * halves])) {
            if (++halves == 2) {
                halves = 0;
                frame = true;
            }
        }
        if (!frame) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        uint32_t t0 = systime_get_us();
        vocoderEncodeFrame(mic, tx_ambe[nf]);
        uint32_t us = systime_get_us() - t0;
        if (us > ysf_tx_stats[2]) {
            ysf_tx_stats[2] = us;
        }
        if (++nf < 5) {
            continue;
        }
        nf = 0;
        uint8_t dch[10];
        f.fn = (uint8_t)fn;
        ysf_vd2_dch(fn, dst, src, NULL, dch);
        ysf_tx_vd2(&f, dch, (const uint8_t(*)[9])tx_ambe, tx_dibits);
        p25_phy_tx_put(tx_dibits, YSF_FRAME_DIBITS);
        if (ysf_tx_stats[1] < 3) {
            p25_ev_log(64, (uint16_t)ysf_tx_stats[1]); // a V/D frame into the FIFO (arg: its number in the call)
        }
        fn = (fn >= 6) ? 0 : fn + 1;
        ysf_tx_stats[1]++;
    }
    if (started) {
        ysf_tx_hc_tc(&f, YSF_FI_TC, dst, src, tx_dibits);
        p25_phy_tx_put(tx_dibits, YSF_FRAME_DIBITS);
    }
    uint32_t underruns = afrx_fifo_finish(started);
    if (started) {
        ysf_tx_stats[3] = underruns;
    }
}

void ysf_radio_set_dgid(uint8_t dgid)
{
    tx_dgid = (dgid <= CODEPLUG_YSF_DGID_MAX) ? dgid : 0;
}

void ysf_radio_transmit(bool on)
{
    afrx_ptt(on);
}

bool ysf_radio_transmitting(void)
{
    return afrx_transmitting();
}

/* ---- receiving (afrx.c runs it) ---- */

static void samples(int16_t *x, int n)
{
    p25_demod_samples(&dm, x, n);
    ysf_radio_stats.demod_syncs = dm.syncs;
}

static void poll(void)
{
    if (call.active && ((xTaskGetTickCount() - last_frame) >= pdMS_TO_TICKS(CALL_TIMEOUT))) {
        call_end();
    }
}

static void end_call(void)
{
    if (call.active) {
        call_end();
    }
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
    .stats = {
        .halves = &ysf_radio_stats.halves,
        .missed = &ysf_radio_stats.missed_halves,
        .busy_us_max = &ysf_radio_stats.busy_us_max,
    },
};

void ysf_radio_start(void)
{
    if (afrx_start(&ops)) {
        heard = false;
    }
}

void ysf_radio_stop(void)
{
    if (!afrx_running(&ops)) {
        return;
    }
    afrx_stop();
    call.active = false;
    call_publish();
}

bool ysf_radio_call(ysf_call_t *out)
{
    *out = call;
    return call.active;
}

bool ysf_radio_heard_within(uint32_t ms)
{
    return heard && ((xTaskGetTickCount() - heard_at) < pdMS_TO_TICKS(ms));
}
