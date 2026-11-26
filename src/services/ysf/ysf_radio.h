// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 lyra XE1POC
// YSF (Fusion/C4FM) conventional channels, the radio service: the C4FM receiver on the shared afrx runner and the
// 4FSK transmitter through the P25 physical layer's FIFO. See ysf_radio.c. No OS or hardware types in the protocol
// core (ysf_frame/ysf_voice/ysf_tx); this is the firmware glue, as p25_radio.h and nxdn_radio.h are for those modes.
#ifndef YSF_RADIO_H
#define YSF_RADIO_H

#include <stdbool.h>
#include <stdint.h>

#include "ysf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char src[YSF_CALLSIGN_LEN + 1]; // the caller's callsign, space padded as on the air
    char dst[YSF_CALLSIGN_LEN + 1]; // the destination ("ALL" or ten asterisks for a group call)
    uint8_t dgid;                   // FICH SC
    uint8_t dt;                     // YSF_DT_*
    bool active;                    // a call is being received
} ysf_call_t;

typedef struct {
    uint32_t frames;       // frames whose sync and FICH decoded
    uint32_t bad_frames;   // frames whose DCH failed (or with an unexpected mode)
    uint32_t voice_blocks; // voice blocks decoded and played
    uint32_t pcm_drops;    // voice blocks dropped: the playback's queue was full
    uint32_t underruns;    // playback underruns
    uint32_t tx_frames;    // frames put into the transmit FIFO
    uint32_t tx_underruns; // transmit FIFO underruns
    uint32_t vw_fails;     // VW frames dropped: the IMBE overlay could not be loaded
} ysf_radio_stats_t;

extern volatile ysf_radio_stats_t ysf_radio_stats;

void ysf_radio_start(void);
void ysf_radio_stop(void);
bool ysf_radio_call(ysf_call_t *out);
bool ysf_radio_heard_within(uint32_t ms);
void ysf_radio_set_target(uint8_t dgid, const char *dest);
void ysf_radio_transmit(bool on);
bool ysf_radio_transmitting(void);

#ifdef __cplusplus
}
#endif
#endif
