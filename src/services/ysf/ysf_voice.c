// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 lyra XE1POC
// YSF voice: half-rate AMBE+2 between a decoded YSF frame and the 9-byte
// DMR-format AMBE frame the vocoder path speaks. See ysf_voice.h for what
// is verified and what is not.
#include "ysf_voice.h"

#include <string.h>

#include "ambe.h"

// The DMR AMBE+2 frame layer (vocoder/c/ambe/ambe_frame.c) is the same code
// as the fork's ambeFec.c: 49 parameter bits <-> the same 9-byte air frame.
#define AMBE_PARAM_BITS 49

// Bit helpers, MSB first (the same order as ysf_frame.c).
static int get_bit(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7 - (i & 7))) & 1; }
static void put_bit(uint8_t *p, unsigned i, int b)
{
	if (b) p[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
	else p[i >> 3] &= (uint8_t)~(0x80u >> (i & 7));
}

unsigned ysf_voice_len(uint8_t dt)
{
	switch (dt) {
	case YSF_DT_VD_MODE1: return 9;	// VCH
	case YSF_DT_VD_MODE2: return 13;	// VCH 9 + VeCH 4
	case YSF_DT_VOICE_FR: return 18;	// VCH
	default: return 0;			// Data FR: no voice blocks
	}
}

unsigned ysf_voice_get(const uint8_t frame[YSF_FRAME_BYTES], uint8_t dt, unsigned block, uint8_t out[18])
{
	unsigned len = ysf_voice_len(dt);

	if (block >= YSF_BLOCKS || !len)
		return 0;
	// Payload layout per 20 ms block (18 bytes): V/D-1 [DCH1 9][VCH 9],
	// DN [DCH2 5][VCH 9][VeCH 4], VW [VCH 18].
	const uint8_t *payload = frame + YSF_SYNC_BYTES + YSF_FICH_BYTES + 18 * block;
	switch (dt) {
	case YSF_DT_VD_MODE1: memcpy(out, payload + 9, 9); break;
	case YSF_DT_VD_MODE2: memcpy(out, payload + 5, 13); break;
	default: memcpy(out, payload, 18); break;
	}
	return len;
}

// DN voice interleave, 26x4 over the 104 bits: TABLE[i] = 4*(i mod 26) + (i div 26)
// (MMDVM AMBEFEC::regenerateYSFDN; Yaesu Fig. 4-32).
static unsigned dn_table(unsigned i) { return 4 * (i % 26) + (i / 26); }

void ysf_voice_dn_to_frame(const uint8_t field[13], uint8_t frame9[9], unsigned *errors)
{
	uint8_t vch[13];
	uint8_t white[20];
	uint8_t params[AMBE_PARAM_BITS];
	unsigned errs = 0;

	// De-interleave: vch[i] = field[TABLE[i]].
	memset(vch, 0, sizeof vch);
	for (unsigned i = 0; i < 104; i++)
		put_bit(vch, i, get_bit(field, dn_table(i)));
	// De-whiten with the first 13 bytes of the PN(9,5) sequence.
	ysf_whitening_generate(white);
	for (unsigned i = 0; i < 13; i++)
		vch[i] ^= white[i];
	// The first 27 parameters are tripled: majority vote. A non-unanimous
	// triplet counts as one error (MMDVM counts it the same way).
	for (unsigned k = 0; k < 27; k++) {
		unsigned ones = (unsigned)get_bit(vch, 3 * k) + (unsigned)get_bit(vch, 3 * k + 1) +
				(unsigned)get_bit(vch, 3 * k + 2);

		params[k] = ones >= 2 ? 1 : 0;
		if (ones != 0 && ones != 3)
			errs++;
	}
	// The last 22 parameters are direct: bits 81..102.
	for (unsigned k = 27; k < 49; k++)
		params[k] = (uint8_t)get_bit(vch, k + 54);
	// Bit 103 is padding: ignored.
	if (errors)
		*errors = errs;
	ambe_encode_frame(params, frame9);
}

ysf_codec_t ysf_codec_for_dt(uint8_t dt)
{
    if (dt == YSF_DT_VD_MODE1 || dt == YSF_DT_VD_MODE2) {
        return YSF_CODEC_AMBE;
    }
    if (dt == YSF_DT_VOICE_FR) {
        return YSF_CODEC_IMBE;
    }
    return YSF_CODEC_NONE;
}

static uint8_t voice_mode_dt(uint8_t dt)
{
    return ysf_codec_for_dt(dt) == YSF_CODEC_NONE ? YSF_DT_VD_MODE1 : dt;
}

void ysf_voice_path_init(ysf_voice_path_t *p, uint8_t configured_dt)
{
    p->configured = voice_mode_dt(configured_dt);
    p->active = 0xFF;
}

ysf_codec_t ysf_voice_path_rx(ysf_voice_path_t *p, uint8_t received_dt)
{
    ysf_codec_t codec = ysf_codec_for_dt(received_dt);

    if (codec != YSF_CODEC_NONE) {
        p->active = received_dt;
    }
    return codec;
}

ysf_codec_t ysf_voice_path_end(ysf_voice_path_t *p)
{
    p->active = 0xFF;
    return ysf_codec_for_dt(p->configured);
}

ysf_codec_t ysf_voice_path_tx(const ysf_voice_path_t *p)
{
    return ysf_codec_for_dt(p->configured);
}

void ysf_voice_dn_from_frame(const uint8_t frame9[9], uint8_t field[13])
{
	uint8_t params[AMBE_PARAM_BITS];
	uint8_t vch[13];
	uint8_t white[20];

	ambe_decode_frame(frame9, params);
	memset(vch, 0, sizeof vch);
	for (unsigned k = 0; k < 27; k++) {
		put_bit(vch, 3 * k, params[k]);
		put_bit(vch, 3 * k + 1, params[k]);
		put_bit(vch, 3 * k + 2, params[k]);
	}
	for (unsigned k = 27; k < 49; k++)
		put_bit(vch, k + 54, params[k]);
	// Bit 103 stays 0 (padding).
	ysf_whitening_generate(white);
	for (unsigned i = 0; i < 13; i++)
		vch[i] ^= white[i];
	// Interleave: field[TABLE[i]] = vch[i].
	memset(field, 0, 13);
	for (unsigned i = 0; i < 104; i++)
		put_bit(field, dn_table(i), get_bit(vch, i));
}
