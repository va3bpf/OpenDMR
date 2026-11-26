// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 lyra XE1POC
// Ported from the host prototype (src/ysf/ysf_frame.c in donttellmeimcute/DM32-nxdn-p25):
// the frame layer is unchanged except for style (tabs, // comments). The
// VCH/VeCH blocks are still filled with the deterministic pattern here;
// the engine (ysf_engine.c) overwrites them with real half-rate AMBE+2
// voice (ysf_voice.c) before the frame goes out, and reads them back on RX.
// YSF frame layer. Provenance, piece by piece (MMDVMHost commit f030273edfa103b9797ccd9407184dadf2138491,
// cloned 2026-11-18):
//
// - Frame sync, FICH field layout and getters: YSFDefines.h / YSFFICH.cpp.
// - FICH FEC chain (CRC-16 -> 4x Golay(24,12,8) -> +4 zero bits -> conv R=1/2
//   K=5 -> interleave M=5 dibits, N=10): YSFFICH.cpp encode()/decode().
// - DCH chains (V/D-1: 160 data + CRC + 4 tail -> conv -> 360, interleave
//   M=9 dibits N=20; DN: 80 data + CRC + 4 tail -> conv -> 200, interleave
//   M=5 N=20; DCH layout inside the 5 payload blocks): YSFPayload.cpp
//   processHeaderData() / processVDMode1Data() / processVDMode2Data().
// - Convolutional encoder and 16-state Viterbi decoder: YSFConvolution.cpp,
//   ported literally (branch tables, tie rule, chainback).
// - CRC-16 (poly x^16+x^12+x^5+1, init 0, inverted at the end): CRC.cpp
//   CCRC::addCCITT162 / checkCCITT162 (table CCITT16_TABLE2); here computed
//   bitwise, which is the same polynomial and the same bit order.
// - Whitening: YSFPayload.cpp WHITENING_DATA (20 fixed bytes) vs the Yaesu
//   PN(9,5) LFSR, seed 0x1C9: the sequence is generated here from the LFSR
//   (right-shifting register, output bit 0, feedback bit0 ^ bit4 into bit 8,
//   bits packed MSB first) and the test compares it byte by byte with
//   MMDVM's table. All 20 bytes agree (checked 2026-11-18).
// - Golay(24,12,8): MMDVM carries it only as tables (ENCODING_TABLE_24128,
//   4096 entries; DECODING_TABLE_23127, 2048 entries; both in Golay24128.cpp,
//   from the Morelos-Zaragoza code). The tables were NOT copied here. Instead
//   the code is implemented algebraically from the standard cyclic
//   Golay(23,12,7) with MMDVM's generator polynomial GENPOL 0xC75
//   (x^11+x^10+x^6+x^5+x^4+x^2+1), data in bits 22..11, parity in bits 10..0,
//   plus the even-parity extension bit as bit 0; decode is syndrome decoding
//   (the (23,12) Golay code is perfect: every 11-bit syndrome maps to exactly
//   one error pattern of weight <= 3). Verified against MMDVM's tables for
//   all 4096 codewords (encode) and all 2048 syndromes (decode) before this
//   file was written.
// - Interleave tables: literal copies of MMDVM's INTERLEAVE_TABLE (YSFFICH.cpp)
//   and INTERLEAVE_TABLE_9_20 / INTERLEAVE_TABLE_5_20 (YSFPayload.cpp),
//   extracted mechanically from the MMDVM sources. They also follow the
//   formula table[i] = 40*(i%M) + 2*(i/M) (M = 5 or 9), which was checked
//   against the literals.
//
// Two MMDVM discrepancies are followed as the spec, not as MMDVM:
// - MR is 3 bits (Yaesu Table 4-1); MMDVM writes 3 bits (setMR) but reads
//   only 2 (getMR masks & 0x03). Here the getter returns all 3.
// - CS (byte 0, bits 5..4) and SQL (byte 3, bit 7) have no MMDVM getters;
//   both are carried here.
//
// DCH order note (MMDVM, not Yaesu prose): the CRC is computed over the
// whitened data -- whiten first, then CRC, then convolution. The decoder
// checks the CRC before de-whitening, mirroring MMDVM exactly.
#include "ysf_frame.h"
#include <string.h>

const uint8_t ysf_sync_bytes[YSF_SYNC_BYTES] = { 0xD4, 0x71, 0xC9, 0x63, 0x4D };

// --- bit helpers (MSB first, as MMDVM's BIT_MASK_TABLE) ---
static int get_bit(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7 - (i & 7))) & 1; }
static void put_bit(uint8_t *p, unsigned i, int b)
{
	if (b) p[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
	else p[i >> 3] &= (uint8_t)~(0x80u >> (i & 7));
}

// --- CRC-16/CCITT-FALSE variant: poly 0x1021, init 0, inverted at the end ---
static uint16_t crc16(const uint8_t *data, size_t n)
{
	uint16_t crc = 0;
	for (size_t i = 0; i < n; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (unsigned k = 0; k < 8; k++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return (uint16_t)~crc;
}
// data[n-2..n-1] hold the CRC, big-endian (high byte first), as MMDVM writes it.
static void crc16_add(uint8_t *data, size_t n)
{
	uint16_t c = crc16(data, n - 2);
	data[n - 2] = (uint8_t)(c >> 8);
	data[n - 1] = (uint8_t)c;
}
static bool crc16_check(const uint8_t *data, size_t n)
{
	uint16_t c = crc16(data, n - 2);
	return (uint8_t)(c >> 8) == data[n - 2] && (uint8_t)c == data[n - 1];
}

// --- Golay(24,12,8): algebraic, generator 0xC75 ---
#define GOLAY_GENPOL 0xC75u  // x^11+x^10+x^6+x^5+x^4+x^2+1
#define GOLAY_X11 0x800u
#define GOLAY_X22 0x400000u
#define GOLAY_MASK12 0xfffff800u

// Remainder of pattern modulo GENPOL (MMDVM's get_syndrome_23127).
static uint32_t golay_syndrome(uint32_t pattern)
{
	uint32_t aux = GOLAY_X22;
	if (pattern >= GOLAY_X11) {
		while (pattern & GOLAY_MASK12) {
			while (!(aux & pattern)) aux >>= 1;
			pattern ^= (aux / GOLAY_X11) * GOLAY_GENPOL;
		}
	}
	return pattern;
}

static unsigned popcount32(uint32_t v)
{
	unsigned n = 0;
	while (v) { n += v & 1u; v >>= 1; }
	return n;
}

uint32_t ysf_golay_encode(uint32_t data12)
{
	uint32_t code23 = ((data12 & 0xFFFu) << 11) | golay_syndrome((data12 & 0xFFFu) << 11);
// extension bit: even overall parity
	return (code23 << 1) | (popcount32(code23) & 1u);
}

bool ysf_golay_decode(uint32_t code24, uint32_t *data12)
{
	uint32_t syndrome = golay_syndrome(code24 >> 1);
// Find the weight <= 3 error pattern with this syndrome. The code is
// perfect, so exactly one exists: errors in the parity bits only, one
// or two data bits plus parity bits, or three data bits.
	uint32_t err = 0;
	bool found = false;
	if (popcount32(syndrome) <= 3) {
		err = syndrome;
		found = true;
	} else {
// The data bits are positions 11..22 of the 23-bit word, the parity
// bits 0..10. A weight <= 3 pattern has 0, 1, 2 or 3 data bits; the
// rest are parity bits, which the syndrome names directly.
		for (unsigned i = 11; i < 23 && !found; i++) {
			uint32_t s = syndrome ^ golay_syndrome(1u << i);
			if (popcount32(s) <= 2) {
				uint32_t cand = (1u << i) | s;
				if (popcount32(cand) <= 3 && golay_syndrome(cand) == syndrome) { err = cand; found = true; }
			}
		}
		for (unsigned i = 11; i < 23 && !found; i++) {
			for (unsigned j = i + 1; j < 23 && !found; j++) {
				uint32_t s = syndrome ^ golay_syndrome(1u << i) ^ golay_syndrome(1u << j);
				if (popcount32(s) <= 1) {
					uint32_t cand = (1u << i) | (1u << j) | s;
					if (popcount32(cand) <= 3 && golay_syndrome(cand) == syndrome) { err = cand; found = true; }
				}
			}
		}
// Three data bits and no parity bit: the three columns must cancel
// the syndrome exactly. (Without this stage, three adjacent data
// bits were missed: the MMDVM oracle caught it, 220 patterns per
// codeword.)
		for (unsigned i = 11; i < 23 && !found; i++) {
			for (unsigned j = i + 1; j < 23 && !found; j++) {
				for (unsigned k = j + 1; k < 23 && !found; k++) {
					if ((golay_syndrome(1u << i) ^ golay_syndrome(1u << j) ^ golay_syndrome(1u << k)) == syndrome) {
						err = (1u << i) | (1u << j) | (1u << k);
						found = true;
					}
				}
			}
		}
	}
	if (!found) return false;  // unreachable for a perfect code; defensive
	uint32_t out = code24 ^ (err << 1);
// Validity: the 24-bit word is an extended Golay codeword (even overall
// parity), so the corrected word must have even parity -- unless the
// extension bit itself was one of the errors. The 23-bit pattern err
// does not cover bit 0, so an odd-parity corrected word is still valid
// when err has weight <= 2 (the flipped parity bit is the third error);
// weight 3 with odd parity would be four errors, which is not correctable.
	bool valid = (popcount32(syndrome) < 3) || !(popcount32(out) & 1u) || (popcount32(err) <= 2);
	*data12 = out >> 12;
	return valid;
}

// --- Convolutional code R=1/2, K=5: G1 = 1+x^3+x^4, G2 = 1+x+x^2+x^4 ---
// Literal port of MMDVM's CYSFConvolution (YSFConvolution.cpp).
static const uint8_t branch_table1[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };
static const uint8_t branch_table2[8] = { 0, 1, 1, 0, 0, 1, 1, 0 };

typedef struct {
	uint16_t metrics1[16], metrics2[16];
	uint16_t *old_metrics, *new_metrics;
	uint16_t decisions[180];  // one 16-bit word per decoded dibit
	uint16_t *dp;
} ysf_viterbi;

static void viterbi_start(ysf_viterbi *v)
{
	memset(v->metrics1, 0, sizeof v->metrics1);
	memset(v->metrics2, 0, sizeof v->metrics2);
	v->old_metrics = v->metrics1;
	v->new_metrics = v->metrics2;
	v->dp = v->decisions;
}

static void viterbi_decode(ysf_viterbi *v, uint8_t s0, uint8_t s1)
{
	uint16_t word = 0;
	for (uint8_t i = 0; i < 8; i++) {
		uint8_t j = (uint8_t)(i * 2);
		uint16_t metric = (uint16_t)((branch_table1[i] ^ s0) + (branch_table2[i] ^ s1));
		uint16_t m0 = (uint16_t)(v->old_metrics[i] + metric);
		uint16_t m1 = (uint16_t)(v->old_metrics[i + 8] + (2 - metric));
		uint8_t decision0 = (m0 >= m1) ? 1 : 0;
		v->new_metrics[j] = decision0 ? m1 : m0;
		m0 = (uint16_t)(v->old_metrics[i] + (2 - metric));
		m1 = (uint16_t)(v->old_metrics[i + 8] + metric);
		uint8_t decision1 = (m0 >= m1) ? 1 : 0;
		v->new_metrics[j + 1] = decision1 ? m1 : m0;
		word |= (uint16_t)((decision1 << (j + 1)) | (decision0 << j));
	}
	*v->dp++ = word;
	uint16_t *tmp = v->old_metrics;
	v->old_metrics = v->new_metrics;
	v->new_metrics = tmp;
}

static void viterbi_chainback(ysf_viterbi *v, uint8_t *out, unsigned nbits)
{
	uint32_t state = 0;
	while (nbits-- > 0) {
		v->dp--;
		uint32_t i = state >> (9 - 5);  // K = 5
		uint8_t bit = (uint8_t)((*v->dp >> i) & 1u);
		state = (bit << 7) | (state >> 1);
		put_bit(out, nbits, bit);
	}
}

static void conv_encode(const uint8_t *in, uint8_t *out, unsigned nbits)
{
	uint8_t d1 = 0, d2 = 0, d3 = 0, d4 = 0;
	unsigned k = 0;
	for (unsigned i = 0; i < nbits; i++) {
		uint8_t d = (uint8_t)get_bit(in, i);
		uint8_t g1 = (uint8_t)((d + d3 + d4) & 1);
		uint8_t g2 = (uint8_t)((d + d1 + d2 + d4) & 1);
		d4 = d3;
		d3 = d2;
		d2 = d1;
		d1 = d;
		put_bit(out, k++, g1);
		put_bit(out, k++, g2);
	}
}

// Interleave tables: literal copies of MMDVMHost's INTERLEAVE_TABLE
// (YSFFICH.cpp) and INTERLEAVE_TABLE_9_20 / INTERLEAVE_TABLE_5_20
// (YSFPayload.cpp). The FICH table and the 5_20 table are identical.
static const uint16_t ysf_interleave_fich[100] = {
      0,  40,  80, 120, 160,   2,  42,  82, 122, 162,
      4,  44,  84, 124, 164,   6,  46,  86, 126, 166,
      8,  48,  88, 128, 168,  10,  50,  90, 130, 170,
     12,  52,  92, 132, 172,  14,  54,  94, 134, 174,
     16,  56,  96, 136, 176,  18,  58,  98, 138, 178,
     20,  60, 100, 140, 180,  22,  62, 102, 142, 182,
     24,  64, 104, 144, 184,  26,  66, 106, 146, 186,
     28,  68, 108, 148, 188,  30,  70, 110, 150, 190,
     32,  72, 112, 152, 192,  34,  74, 114, 154, 194,
     36,  76, 116, 156, 196,  38,  78, 118, 158, 198,
};
static const uint16_t ysf_interleave_9_20[180] = {
      0,  40,  80, 120, 160, 200, 240, 280, 320,   2,
     42,  82, 122, 162, 202, 242, 282, 322,   4,  44,
     84, 124, 164, 204, 244, 284, 324,   6,  46,  86,
    126, 166, 206, 246, 286, 326,   8,  48,  88, 128,
    168, 208, 248, 288, 328,  10,  50,  90, 130, 170,
    210, 250, 290, 330,  12,  52,  92, 132, 172, 212,
    252, 292, 332,  14,  54,  94, 134, 174, 214, 254,
    294, 334,  16,  56,  96, 136, 176, 216, 256, 296,
    336,  18,  58,  98, 138, 178, 218, 258, 298, 338,
     20,  60, 100, 140, 180, 220, 260, 300, 340,  22,
     62, 102, 142, 182, 222, 262, 302, 342,  24,  64,
    104, 144, 184, 224, 264, 304, 344,  26,  66, 106,
    146, 186, 226, 266, 306, 346,  28,  68, 108, 148,
    188, 228, 268, 308, 348,  30,  70, 110, 150, 190,
    230, 270, 310, 350,  32,  72, 112, 152, 192, 232,
    272, 312, 352,  34,  74, 114, 154, 194, 234, 274,
    314, 354,  36,  76, 116, 156, 196, 236, 276, 316,
    356,  38,  78, 118, 158, 198, 238, 278, 318, 358,
};
static const uint16_t ysf_interleave_5_20[100] = {
      0,  40,  80, 120, 160,   2,  42,  82, 122, 162,
      4,  44,  84, 124, 164,   6,  46,  86, 126, 166,
      8,  48,  88, 128, 168,  10,  50,  90, 130, 170,
     12,  52,  92, 132, 172,  14,  54,  94, 134, 174,
     16,  56,  96, 136, 176,  18,  58,  98, 138, 178,
     20,  60, 100, 140, 180,  22,  62, 102, 142, 182,
     24,  64, 104, 144, 184,  26,  66, 106, 146, 186,
     28,  68, 108, 148, 188,  30,  70, 110, 150, 190,
     32,  72, 112, 152, 192,  34,  74, 114, 154, 194,
     36,  76, 116, 156, 196,  38,  78, 118, 158, 198,
};

// --- Whitening: PN(9,5), seed 0x1C9 ---
void ysf_whitening_generate(uint8_t out[20])
{
	uint32_t reg = 0x1C9;  // 111001001
	memset(out, 0, 20);
	for (unsigned i = 0; i < 160; i++) {
// Output is the register's bottom bit; feedback is bit0 ^ bit4,
// shifted in at the top. Verified byte-for-byte against MMDVM's
// WHITENING_DATA table (see the test).
		put_bit(out, i, (int)(reg & 1u));
		uint32_t fb = (reg & 1u) ^ ((reg >> 4) & 1u);
		reg = (reg >> 1) | (fb << 8);
	}
}

// --- FICH ---
void ysf_fich_encode(const ysf_fich *f, uint8_t out[YSF_FICH_BYTES])
{
	uint8_t fich[6];
	fich[0] = (uint8_t)(((f->fi & 3u) << 6) | ((f->cs & 3u) << 4) | ((f->cm & 3u) << 2) | (f->bn & 3u));
	fich[1] = (uint8_t)(((f->bt & 3u) << 6) | ((f->fn & 7u) << 3) | (f->ft & 7u));
	fich[2] = (uint8_t)(((f->dev & 1u) << 6) | ((f->mr & 7u) << 3) | ((f->voip & 1u) << 2) | (f->dt & 3u));
	fich[3] = (uint8_t)(((f->sql & 1u) << 7) | (f->sc & 0x7Fu));
	fich[4] = fich[5] = 0;
	crc16_add(fich, 6);
// 48 bits -> 4 x 12-bit words -> 4 x Golay(24,12,8) = 96 bits + 4 zero tail bits
	uint32_t b0 = ((uint32_t)fich[0] << 4) | ((fich[1] >> 4) & 0x0Fu);
	uint32_t b1 = ((uint32_t)(fich[1] & 0x0Fu) << 8) | fich[2];
	uint32_t b2 = ((uint32_t)fich[3] << 4) | ((fich[4] >> 4) & 0x0Fu);
	uint32_t b3 = ((uint32_t)(fich[4] & 0x0Fu) << 8) | fich[5];
	uint32_t c0 = ysf_golay_encode(b0), c1 = ysf_golay_encode(b1);
	uint32_t c2 = ysf_golay_encode(b2), c3 = ysf_golay_encode(b3);
	uint8_t conv_in[13];
	memset(conv_in, 0, sizeof conv_in);
	conv_in[0] = (uint8_t)(c0 >> 16); conv_in[1] = (uint8_t)(c0 >> 8); conv_in[2] = (uint8_t)c0;
	conv_in[3] = (uint8_t)(c1 >> 16); conv_in[4] = (uint8_t)(c1 >> 8); conv_in[5] = (uint8_t)c1;
	conv_in[6] = (uint8_t)(c2 >> 16); conv_in[7] = (uint8_t)(c2 >> 8); conv_in[8] = (uint8_t)c2;
	conv_in[9] = (uint8_t)(c3 >> 16); conv_in[10] = (uint8_t)(c3 >> 8); conv_in[11] = (uint8_t)c3;
	conv_in[12] = 0;
	uint8_t convolved[25];
	memset(convolved, 0, sizeof convolved);
	conv_encode(conv_in, convolved, 100);
	memset(out, 0, YSF_FICH_BYTES);
	for (unsigned i = 0; i < 100; i++) {
		unsigned n = ysf_interleave_fich[i];
		put_bit(out, n, get_bit(convolved, 2 * i));
		put_bit(out, n + 1, get_bit(convolved, 2 * i + 1));
	}
}

bool ysf_fich_decode(const uint8_t in[YSF_FICH_BYTES], ysf_fich *f)
{
	ysf_viterbi v;
	viterbi_start(&v);
	for (unsigned i = 0; i < 100; i++) {
		unsigned n = ysf_interleave_fich[i];
		viterbi_decode(&v, (uint8_t)get_bit(in, n), (uint8_t)get_bit(in, n + 1));
	}
	uint8_t output[13];
	memset(output, 0, sizeof output);
	viterbi_chainback(&v, output, 96);
	uint32_t b0, b1, b2, b3;
	uint32_t w0 = ((uint32_t)output[0] << 16) | ((uint32_t)output[1] << 8) | output[2];
	uint32_t w1 = ((uint32_t)output[3] << 16) | ((uint32_t)output[4] << 8) | output[5];
	uint32_t w2 = ((uint32_t)output[6] << 16) | ((uint32_t)output[7] << 8) | output[8];
	uint32_t w3 = ((uint32_t)output[9] << 16) | ((uint32_t)output[10] << 8) | output[11];
	if (!ysf_golay_decode(w0, &b0) || !ysf_golay_decode(w1, &b1) ||
		!ysf_golay_decode(w2, &b2) || !ysf_golay_decode(w3, &b3))
		return false;
	uint8_t fich[6];
	fich[0] = (uint8_t)((b0 >> 4) & 0xFFu);
	fich[1] = (uint8_t)(((b0 << 4) & 0xF0u) | ((b1 >> 8) & 0x0Fu));
	fich[2] = (uint8_t)(b1 & 0xFFu);
	fich[3] = (uint8_t)((b2 >> 4) & 0xFFu);
	fich[4] = (uint8_t)(((b2 << 4) & 0xF0u) | ((b3 >> 8) & 0x0Fu));
	fich[5] = (uint8_t)(b3 & 0xFFu);
	if (!crc16_check(fich, 6)) return false;
	f->fi = (uint8_t)((fich[0] >> 6) & 3u);
	f->cs = (uint8_t)((fich[0] >> 4) & 3u);
	f->cm = (uint8_t)((fich[0] >> 2) & 3u);
	f->bn = (uint8_t)(fich[0] & 3u);
	f->bt = (uint8_t)((fich[1] >> 6) & 3u);
	f->fn = (uint8_t)((fich[1] >> 3) & 7u);
	f->ft = (uint8_t)(fich[1] & 7u);
	f->dev = (uint8_t)((fich[2] >> 6) & 1u);
	f->mr = (uint8_t)((fich[2] >> 3) & 7u);
	f->voip = (uint8_t)((fich[2] >> 2) & 1u);
	f->dt = (uint8_t)(fich[2] & 3u);
	f->sql = (uint8_t)((fich[3] >> 7) & 1u);
	f->sc = (uint8_t)(fich[3] & 0x7Fu);
	return true;
}

// --- DCH: shared core over (data_bytes, air_bytes, interleave table) ---
static void dch_encode(const uint8_t *data, unsigned data_len, const uint16_t *table,
					   unsigned pairs, uint8_t *out, unsigned out_len)
{
	uint8_t whitening[20];
	ysf_whitening_generate(whitening);
// Whiten the data, CRC over the whitened data, 4 zero tail bits.
	uint8_t buf[23];
	memset(buf, 0, sizeof buf);
	for (unsigned i = 0; i < data_len; i++) buf[i] = (uint8_t)(data[i] ^ whitening[i]);
	crc16_add(buf, data_len + 2);
	buf[data_len + 2] = 0;
	unsigned nbits = (data_len + 2) * 8 + 4;
	uint8_t convolved[45];
	memset(convolved, 0, sizeof convolved);
	conv_encode(buf, convolved, nbits);
	memset(out, 0, out_len);
	for (unsigned i = 0; i < pairs; i++) {
		unsigned n = table[i];
		put_bit(out, n, get_bit(convolved, 2 * i));
		put_bit(out, n + 1, get_bit(convolved, 2 * i + 1));
	}
}

static bool dch_decode(const uint8_t *in, unsigned data_len, const uint16_t *table,
					   unsigned pairs, uint8_t *data)
{
	ysf_viterbi v;
	viterbi_start(&v);
	for (unsigned i = 0; i < pairs; i++) {
		unsigned n = table[i];
		viterbi_decode(&v, (uint8_t)get_bit(in, n), (uint8_t)get_bit(in, n + 1));
	}
	uint8_t output[23];
	memset(output, 0, sizeof output);
	viterbi_chainback(&v, output, (data_len + 2) * 8);
	if (!crc16_check(output, data_len + 2)) return false;
	uint8_t whitening[20];
	ysf_whitening_generate(whitening);
	for (unsigned i = 0; i < data_len; i++) data[i] = (uint8_t)(output[i] ^ whitening[i]);
	return true;
}

void ysf_dch1_encode(const uint8_t data[20], uint8_t out[45])
{
	dch_encode(data, 20, ysf_interleave_9_20, 180, out, 45);
}

bool ysf_dch1_decode(const uint8_t in[45], uint8_t data[20])
{
	return dch_decode(in, 20, ysf_interleave_9_20, 180, data);
}

void ysf_dch2_encode(const uint8_t data[10], uint8_t out[25])
{
	dch_encode(data, 10, ysf_interleave_5_20, 100, out, 25);
}

bool ysf_dch2_decode(const uint8_t in[25], uint8_t data[10])
{
	return dch_decode(in, 10, ysf_interleave_5_20, 100, data);
}

// --- dibits <-> bytes ---
void ysf_bytes_to_dibits(const uint8_t *bytes, size_t nbytes, uint8_t *dibits)
{
	for (size_t i = 0; i < nbytes; i++)
		for (unsigned k = 0; k < 4; k++) dibits[4 * i + k] = (uint8_t)((bytes[i] >> (6 - 2 * k)) & 3u);
}

void ysf_dibits_to_bytes(const uint8_t *dibits, size_t ndibits, uint8_t *bytes)
{
	size_t nbytes = ndibits / 4;
	memset(bytes, 0, nbytes);
	for (size_t i = 0; i < ndibits; i++) {
		bytes[i / 4] |= (uint8_t)(dibits[i] << (6 - 2 * (i % 4)));
	}
}

void ysf_preamble_dibits(uint8_t *dibits, size_t ndibits)
{
	// 0x77 = 01 11 01 11; dibit 1 is +3 and dibit 3 is -3 (ysf_demod.c's level table).
	for (size_t i = 0; i < ndibits; i++) {
		dibits[i] = (i & 1u) ? 3 : 1;
	}
}

// --- sync search ---
static const uint8_t sync_dibits[20] = { 3, 1, 1, 0, 1, 3, 0, 1, 3, 0, 2, 1, 1, 2, 0, 3, 1, 0, 3, 1 };

unsigned ysf_sync_errors(const uint8_t sync5[YSF_SYNC_BYTES])
{
	unsigned errors = 0;
	for (unsigned i = 0; i < YSF_SYNC_BYTES; i++) {
		uint8_t x = (uint8_t)(sync5[i] ^ ysf_sync_bytes[i]);
		while (x) { errors += x & 1u; x >>= 1; }
	}
	return errors;
}

size_t ysf_find_sync(const uint8_t *dibits, size_t ndibits, unsigned max_errors)
{
	if (ndibits < 20) return SIZE_MAX;
	for (size_t pos = 0; pos + 20 <= ndibits; pos++) {
		unsigned errors = 0;
		for (unsigned i = 0; i < 20; i++) {
			uint8_t x = (uint8_t)(dibits[pos + i] ^ sync_dibits[i]);
			errors += (x & 1u) + ((x >> 1) & 1u);
			if (errors > max_errors) break;
		}
		if (errors <= max_errors) return pos;
	}
	return SIZE_MAX;
}

// --- whole frames ---
// Deterministic voice-block filler: xorshift32, fixed seed, so a built frame
// is reproducible. This is NOT voice: no AMBE is implemented in this phase.
static void fill_voice(uint8_t *p, size_t n)
{
	uint32_t s = 0x59534621u;  // "YSF!"
	for (size_t i = 0; i < n; i++) {
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		p[i] = (uint8_t)(s >> 24);
	}
}

// Payload layout per 20 ms block (18 bytes):
//   V/D-1 / Data FR (DT 0/1): [DCH1 9][VCH 9]
//   V/D-2 (DN, DT 2):         [DCH2 5][VCH 9][VeCH 4]
//   Voice FR (DT 3):          [VCH 18] (no DCH)
// A header or terminator carries two DCH1s: DCH1 in bytes 0..8 of each block,
// DCH2 in bytes 9..17 (MMDVM's processHeaderData gathers both).
void ysf_frame_build(const ysf_fich *f, const uint8_t *dch, const uint8_t *dch2_or_null,
					 uint8_t frame[YSF_FRAME_BYTES])
{
	memcpy(frame, ysf_sync_bytes, YSF_SYNC_BYTES);
	ysf_fich_encode(f, frame + YSF_SYNC_BYTES);
	uint8_t *payload = frame + YSF_SYNC_BYTES + YSF_FICH_BYTES;
// The header and the terminator are mode-independent: two DCH1s (CSD1 +
// CSD2), as MMDVM's writeHeader/processHeaderData always use the 9_20
// DCH1 pair whatever the DT -- and YSFControl calls processHeaderData on
// terminator frames too. Only communication frames follow the DT layout.
	if (f->fi == YSF_FI_HEADER || f->fi == YSF_FI_TERMINATOR) {
		uint8_t air[45], air2[45];
		ysf_dch1_encode(dch, air);
		if (dch2_or_null != NULL) ysf_dch1_encode(dch2_or_null, air2);
		for (unsigned b = 0; b < YSF_BLOCKS; b++) {
			memcpy(payload + 18 * b, air + 9 * b, 9);
			if (dch2_or_null != NULL) memcpy(payload + 18 * b + 9, air2 + 9 * b, 9);
			else fill_voice(payload + 18 * b + 9, 9);
		}
		return;
	}
	if (f->dt == YSF_DT_VD_MODE2) {
		uint8_t air[25];
		ysf_dch2_encode(dch, air);
		for (unsigned b = 0; b < YSF_BLOCKS; b++) {
			memcpy(payload + 18 * b, air + 5 * b, 5);
			fill_voice(payload + 18 * b + 5, 13);
		}
	} else if (f->dt == YSF_DT_VOICE_FR) {
		fill_voice(payload, YSF_PAYLOAD_BYTES);
		if (ysf_vw_subheader(f)) {
			ysf_dch1_encode(dch, payload);	// 45 contiguous bytes, then nine reserved bytes
			memset(payload + 45, 0, 9);
		}
	} else if (f->dt == YSF_DT_DATA_FR && dch2_or_null != NULL) {
// Data FR communication frame: two DCH1s, DCH-1 in bytes 0..8 of each
// block, DCH-2 in bytes 9..17 (MMDVM's processDataFRModeData gathers
// both).
		uint8_t air[45], air2[45];
		ysf_dch1_encode(dch, air);
		ysf_dch1_encode(dch2_or_null, air2);
		for (unsigned b = 0; b < YSF_BLOCKS; b++) {
			memcpy(payload + 18 * b, air + 9 * b, 9);
			memcpy(payload + 18 * b + 9, air2 + 9 * b, 9);
		}
	} else {
		uint8_t air[45];
		ysf_dch1_encode(dch, air);
		for (unsigned b = 0; b < YSF_BLOCKS; b++) {
			memcpy(payload + 18 * b, air + 9 * b, 9);
			fill_voice(payload + 18 * b + 9, 9);
		}
	}
}

bool ysf_frame_decode_fich(const uint8_t frame[YSF_FRAME_BYTES], ysf_fich *f)
{
	return ysf_sync_errors(frame) <= YSF_SYNC_MAX_ERRORS &&
	       ysf_fich_decode(frame + YSF_SYNC_BYTES, f);
}

bool ysf_frame_decode_dch(const uint8_t frame[YSF_FRAME_BYTES], const ysf_fich *f, uint8_t dch[20],
			  uint8_t *dch2_or_null, unsigned *dch_len_or_null)
{
	const uint8_t *payload = frame + YSF_SYNC_BYTES + YSF_FICH_BYTES;
	unsigned dch_len = 0;
	bool ok = true;
	if (f->fi == YSF_FI_HEADER || f->fi == YSF_FI_TERMINATOR) {
		uint8_t air[45], air2[45];
		for (unsigned b = 0; b < YSF_BLOCKS; b++) {
			memcpy(air + 9 * b, payload + 18 * b, 9);
			memcpy(air2 + 9 * b, payload + 18 * b + 9, 9);
		}
		ok = ysf_dch1_decode(air, dch);
		if (ok && dch2_or_null != NULL) ok = ysf_dch1_decode(air2, dch2_or_null);
		dch_len = 20;
	} else if (f->dt == YSF_DT_VD_MODE2) {
		uint8_t air[25];
		for (unsigned b = 0; b < YSF_BLOCKS; b++) memcpy(air + 5 * b, payload + 18 * b, 5);
		uint8_t tmp[10];
		ok = ysf_dch2_decode(air, tmp);
		if (ok) memcpy(dch, tmp, 10);
		dch_len = 10;
	} else if (f->dt == YSF_DT_VOICE_FR) {
		if (ysf_vw_subheader(f)) {
			ok = ysf_dch1_decode(payload, dch);
			dch_len = 20;
		}
	} else if (f->dt == YSF_DT_DATA_FR) {
// Data FR communication frame: two DCH1s, DCH-1 in bytes 0..8 of each
// block, DCH-2 in bytes 9..17.
		uint8_t air[45], air2[45];
		for (unsigned b = 0; b < YSF_BLOCKS; b++) {
			memcpy(air + 9 * b, payload + 18 * b, 9);
			memcpy(air2 + 9 * b, payload + 18 * b + 9, 9);
		}
		ok = ysf_dch1_decode(air, dch);
		if (ok && dch2_or_null != NULL) ok = ysf_dch1_decode(air2, dch2_or_null);
		dch_len = 20;
	} else {
		uint8_t air[45];
		for (unsigned b = 0; b < YSF_BLOCKS; b++) memcpy(air + 9 * b, payload + 18 * b, 9);
		ok = ysf_dch1_decode(air, dch);
		dch_len = 20;
	}
	if (dch_len_or_null != NULL) *dch_len_or_null = dch_len;
	return ok;
}

bool ysf_frame_decode(const uint8_t frame[YSF_FRAME_BYTES], ysf_fich *f, uint8_t dch[20],
		      uint8_t *dch2_or_null, unsigned *dch_len_or_null)
{
	return ysf_frame_decode_fich(frame, f) &&
	       ysf_frame_decode_dch(frame, f, dch, dch2_or_null, dch_len_or_null);
}
