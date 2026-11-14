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

#ifndef P25_RADIO_H
#define P25_RADIO_H

#include <stdbool.h>
#include <stdint.h>

// The P25 Phase 1 receiver (docs/p25.md): the HR_C7000's physical layer receive (p25_phy.c) into p25-core's receiver
// and IMBE decoder (the p25/ submodule; the decoder in the overlay RAM), the voice to the speaker. Started by radio.c
// on a P25 channel, once the RF and the modem are set up as for DMR.

typedef struct {
    bool active;     // a call is being received
    bool encrypted;  // its ALGID isn't clear: muted
    uint16_t nac;
    uint16_t tgid;   // group call (lco 0)
    uint32_t src;    // 0: not known yet
    uint32_t dst;    // unit to unit (lco 3)
    uint8_t lco;
} p25_call_t;

void p25_radio_start(void);
void p25_radio_stop(void);
bool p25_radio_call(p25_call_t *out); // the call now (false: none)
#define P25_LCO_UNIT 3 // the link control of a unit to unit call
// Its talk group, or LAST_HEARD_PC_FLAG << 24 | the called unit (a unit to unit call), as dmr_call_t.tg_or_pc
uint32_t p25_call_tg_or_pc(const p25_call_t *call);
// The PTT on a P25 channel: a transmission while on (the mic, IMBE, HDU / LDUs / TDU on the physical layer), back to
// receive after it is released and the last data unit is out
void p25_radio_transmit(bool on);
// The channel's NAC (transmitted; received: only calls with it, CODEPLUG_P25_NAC_RX_ANY / 0xF7F any) and the talk
// group transmitted to (tx_target.c; 0: none, 1 then)
void p25_radio_set_target(uint16_t nac, uint16_t tgid);
void p25_ev_log(uint16_t event, uint16_t arg); // the delay log (p25_radio.c), also for DMR to compare
bool p25_radio_transmitting(void); // asked, or still sending
extern volatile uint32_t p25_tx_source; // 0 the mic, 1 test frames (p25_phy_tx_frame())
extern volatile uint32_t p25_tx_stats[4]; // calls, LDUs, encoder frames, mic buffers

// counters, read over the cable
typedef struct {
    uint32_t blocks, stale, overruns, units, ldus, lc_bad, imbe_frames, imbe_repeat, pcm_drops, underruns;
    uint32_t rearms; // the receive armed again after blocks without a data unit
    uint32_t overlaps, gaps; // blocks placed back on the air by their time (p25_radio.c place())
    uint32_t nac_other;      // data units with another NAC than the channel's: not received
} p25_radio_stats_t;
extern volatile p25_radio_stats_t p25_radio_stats;

// for tools/debug/p25_monitor.py: the blocks as received (the modem's order) and the calls, read over the cable
#define P25_TAP_BLOCKS 128 // 3.8 s
typedef struct {
    uint32_t ms;   // when it came (the interrupt, ms)
    uint32_t type; // RX_TYPE_INFO
    uint8_t data[36];
} p25_tap_block_t;
extern p25_tap_block_t p25_tap[P25_TAP_BLOCKS];
extern volatile uint32_t p25_tap_head;

#define P25_CALL_LOG 16
typedef struct {
    uint32_t start_ms, end_ms;
    uint16_t nac, tgid;
    uint32_t src, dst;
    uint8_t lco, encrypted, pad[2];
    uint32_t ldus, lc_bad, imbe_frames, imbe_repeat; // during the call
} p25_call_log_t;
extern p25_call_log_t p25_call_log[P25_CALL_LOG];
extern volatile uint32_t p25_call_log_head;

#endif
