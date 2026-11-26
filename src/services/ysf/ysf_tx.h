// SPDX-License-Identifier: GPL-3.0-or-later
// YSF transmitter, firmware: builds a whole transmission (header ->
// mode-dependent communication cycle -> terminator), one frame at a time.
// Mirror of the RX frame layer (ysf_frame.h): the TX chain is
//
//   ysf_tx_config (callsigns, mode, DG-ID)
//     -> ysf_tx_frame_build() (FICH + DCH rotation + voice blocks)
//     -> ysf_frame_build() (sync + FICH + DCH encode)
//     -> ysf_bytes_to_dibits() -> air_session_tx() (HR_C7000 native 4FSK
//        slot engine, 4800 sym/s, RRC 0.2; the deviation lives in the air
//        profile, ysf_air_mode / ysf_air_mode_narrow)
//
// The host prototype (src/ysf/ysf_tx.h) also has a streamer (ysf_tx_init /
// ysf_tx_dibits / ysf_tx_samples) that feeds dibits through ysf_mod, the
// 8 kHz sample modulator. That streamer is NOT ported: the engine
// (ysf_engine.c) calls ysf_tx_frame_build once per frame and paces the
// frames itself, so it can wait for voice from the vocoder instead of
// sending silence when the talker has not spoken yet.
//
// Frame sequence (Yaesu Fig. 4-1; MMDVM YSFControl.cpp / YSFPayload.cpp):
// one header (FI 0), a mode-dependent communication block (FI 1), one
// terminator (FI 2). Longer transmissions repeat the communication block
// (cycles > 1). FICH: FT gives the last FN of the cycle (also on the header
// and terminator, as observed in DN frames), BN/BT are 0; DT is the
// configured mode on every frame; Dev selects Wide (0) / Narrow (1);
// SC is the DG-ID.
//
// DCH rotation per mode (Yaesu Tables 4-3/4-4; MMDVM processVDMode1Data /
// processVDMode2Data / processDataFRModeData):
//   V/D-1 (DT 0): FN0=CSD1 (Dest+Src), FN1=CSD2 (Down+Up), FN2=CSD3
//     (Rem1+Rem2), FN3..7 = data DT1..DT5 (20 bytes each).
//   DN (DT 2): FN0=Dest, FN1=Src, FN2=Down, FN3=Up, FN4=Rem1+Rem2 (5 bytes
//     each), FN5=Rem3+Rem4, FN6=data DT1 (10 bytes). A voice-only call still
//     sends FN6, so the cycle is FN0..6 / FT=6; a DT1 callback fills it.
//   Data FR (DT 1): FN0: DCH-1=CSD1, DCH-2=CSD2; FN1: DCH-1=CSD3,
//     DCH-2=DT1; FN2..7: DCH-1/DCH-2 = DT2/DT3 .. DT12/DT13 (20 bytes each).
//   VW (DT 3): optionally send CSD3 and two voice blocks in the first CC
//     (FT=1/FN=0). Subsequent communication frames carry five voice blocks.
// The header and the terminator always carry CSD1 + CSD2 (two DCH1s),
// whatever the mode (MMDVM writeHeader / processHeaderData).
//
// Voice: the caller supplies the voice bytes of each 20 ms block through the
// voice callback (9 bytes V/D-1, 13 bytes DN: VCH 9 + VeCH 4, 18 bytes VW).
// With no callback the voice blocks are all zero: a deterministic test
// payload, NOT a valid AMBE silence frame. The engine (ysf_engine.c)
// supplies AMBE+2 for V/D-1 and DN, and IMBE-compatible full-rate voice for VW.
//
// Integer arithmetic only, no allocation.
#ifndef YSF_TX_H
#define YSF_TX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ysf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// Data units: the data callback fills one unit (20 bytes; DN uses the first
// 10). index counts DT units from 0 (DT1): V/D-1 FN3..7 -> 0..4, DN FN6 ->
// 0, Data FR FN1 DCH-2 .. FN7 DCH-2 -> 0..12.
typedef void (*ysf_tx_data_fn)(void *ctx, unsigned index, uint8_t out[20]);
// Voice blocks: fills one 20 ms block's voice bytes (vch_len: 9 V/D-1,
// 13 DN, 18 VW). frame_no is the sequence frame number (1..8*cycles,
// communication frames only). The VW sub-header calls back only for blocks 3 and 4.
typedef void (*ysf_tx_voice_fn)(void *ctx, unsigned frame_no, unsigned block,
								uint8_t *vch, unsigned vch_len);

typedef struct {
	uint8_t dt;  // YSF_DT_VD_MODE1 / DATA_FR / VD_MODE2 / VOICE_FR
	uint8_t dev;  // 0 = Wide (+-2700/+-900 Hz), 1 = Narrow (+-1350/+-450 Hz)
	uint8_t cm;  // YSF_CM_GROUP / RADIO_ID / INDIVIDUAL
	uint8_t mr;  // YSF_MR_DIRECT / DOWNLINK_FREE / DOWNLINK_BUSY
	uint8_t cs;  // YSF_CS_CSD; the builder enforces this fixed value
	uint8_t sql;  // 0 = DG-ID squelch enabled, 1 = disabled
	uint8_t voip;  // 0 = RF, 1 = VoIP
	uint8_t dgid;  // FICH SC: 0 = none, 1..126 = codes, 127 = Break Out
	char dest[YSF_CALLSIGN_LEN];
	char src[YSF_CALLSIGN_LEN];
	char down[YSF_CALLSIGN_LEN];
	char up[YSF_CALLSIGN_LEN];
	char rem1[YSF_CALLSIGN_LEN];
	char rem2[YSF_CALLSIGN_LEN];
	char rem3[YSF_CALLSIGN_LEN];
	char rem4[YSF_CALLSIGN_LEN];
	bool vw_subheader;  // send the CSD3 sub-header in the first VW communication frame
	unsigned cycles;  // communication-cycle repeats, >= 1
	ysf_tx_data_fn data;  // NULL: voice modes omit DT units and rotate only CSD
	ysf_tx_voice_fn voice;  // NULL: voice blocks are zero-filled (test payload, not AMBE)
	void *ctx;
} ysf_tx_config;

// Source callsign field: the CPS callsign with "-N" appended when an SSID is set, space padded to ten characters.
// A long callsign is cut to leave room for the suffix.
void ysf_tx_source_field(char out[YSF_CALLSIGN_LEN], const char *call, unsigned ssid);

// Total frames: header + cycles of the mode's CSD/data sequence + terminator.
// Voice-only V/D-1 cycles have 3 CC frames; DN has 7 (FN0..6); cycles with
// data and the FR modes have 8.
unsigned ysf_tx_total_frames(const ysf_tx_config *cfg);

// Build one frame of the sequence: frame_no 0 = header, followed by the
// communication cycles, last = terminator. False when out of range or unknown.
bool ysf_tx_frame_build(const ysf_tx_config *cfg, unsigned frame_no, uint8_t frame[YSF_FRAME_BYTES]);

#ifdef __cplusplus
}
#endif
#endif
