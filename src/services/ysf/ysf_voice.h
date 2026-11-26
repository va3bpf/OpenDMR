// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 lyra XE1POC
// YSF voice: half-rate AMBE+2 (49 parameter bits) between a decoded YSF
// frame and the 9-byte DMR-format AMBE frame the vocoder path speaks.
//
// V/D-1 (DT 0): the 72-bit VCH is exactly the DMR AMBE+2 3600x2450 frame
// (same code words, same interleaving; core/dmr/ambe/ambeFec.c), so its
// bytes pass through untouched. DN (DT 2): the 104-bit voice field (VCH 72
// + VeCH 32, block bits 40..143) carries the 49 parameters raw: the first
// 27 parameters tripled, the last 22 direct, one zero pad bit, whitened
// with the first 13 bytes of the PN(9,5) sequence and interleaved 26x4
// (MMDVM AMBEFEC::regenerateYSFDN / AMBEFEC.cpp; Yaesu Fig. 4-32).
//
// What is NOT verified here: that the 49 YSF parameters are semantically
// the same AMBE+2 parameters the DVSI blob expects. The channel mapping is
// proven against MMDVMHost (see the test), the parameter semantics are not:
// no public YSF voice vector or air capture exists to prove them against.
//
// VW (DT 3) carries one 18-byte IMBE full-rate frame per block, straight from
// the VCH. On the radio it is decoded/encoded by the P25 overlay's IMBE codec
// (dmr/vocoder.c overlay_imbe_enter()); the host tests use p25/c/imbe.
#ifndef YSF_VOICE_H
#define YSF_VOICE_H
#include <stdbool.h>
#include <stdint.h>

#include "ysf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// Voice bytes of one 20 ms block in this mode: V/D-1 9, DN 13, VW 18,
// Data FR 0.
unsigned ysf_voice_len(uint8_t dt);

// Voice bytes of block `block` (0..4) of a decoded frame, into out (18
// bytes). Returns the length (0, 9, 13 or 18); 0 when the mode has no voice
// blocks or the block is out of range.
unsigned ysf_voice_get(const uint8_t frame[YSF_FRAME_BYTES], uint8_t dt, unsigned block, uint8_t out[18]);

// DN voice field (13 bytes: VCH 9 + VeCH 4) -> 9-byte DMR-format AMBE frame:
// de-interleave, de-whiten, majority vote per triplet, ambeFecEncode.
// errors counts the non-unanimous triplets (NULL to ignore); the pad bit
// (bit 103) is ignored -- MMDVM notes it is apparently not always 0 on air.
void ysf_voice_dn_to_frame(const uint8_t field[13], uint8_t frame9[9], unsigned *errors);

// 9-byte DMR-format AMBE frame -> DN voice field: ambeFecDecode, triplicate
// the first 27 parameters, whiten, interleave. The pad bit is sent as 0.
void ysf_voice_dn_from_frame(const uint8_t frame9[9], uint8_t field[13]);

// Which vocoder a data type needs: V/D-1 and DN are half-rate AMBE+2, VW is
// full-rate IMBE, Data FR has no voice at all.
typedef enum { YSF_CODEC_NONE = 0, YSF_CODEC_AMBE, YSF_CODEC_IMBE } ysf_codec_t;
ysf_codec_t ysf_codec_for_dt(uint8_t dt);

// The receive path follows the DT on the air even when this channel transmits
// another mode. Ending the call (or starting a PTT) gives the configured path
// back. The switching itself is in ysf_radio.c; keeping the decision here lets
// the host tests exercise cross-mode calls without the radio's overlay RAM.
typedef struct {
    uint8_t configured; // channel DT
    uint8_t active;     // DT of the call being received, 0xFF between calls
} ysf_voice_path_t;
void ysf_voice_path_init(ysf_voice_path_t *p, uint8_t configured_dt);
ysf_codec_t ysf_voice_path_rx(ysf_voice_path_t *p, uint8_t received_dt);
ysf_codec_t ysf_voice_path_end(ysf_voice_path_t *p);
ysf_codec_t ysf_voice_path_tx(const ysf_voice_path_t *p);

#ifdef __cplusplus
}
#endif
#endif
