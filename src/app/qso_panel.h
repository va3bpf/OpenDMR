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

#ifndef DM32_QSO_PANEL_H
#define DM32_QSO_PANEL_H

#include <stdbool.h>
#include "dmr.h"
#include "p25_radio.h"
#include "nxdn_radio.h"
#include "m17_radio.h"
#include "ysf/ysf_radio.h"

// The DMR caller on the channel and VFO screens (docs/ui-design.md 5.3, uiUtilities.c renderQSODataDM32()): the channel
// (or VFO) name and the caller's ID, the callsign large (the ID when not known), the name, the city and state, the
// country, the position, the talk group (or whom a private call is for), the S meter, the bottom bar as the screen's
// (the mode, the power, the bandwidth); the timeslot and the channel's colour code in the status bar
void qso_panel_draw(const dmr_call_t *call, const char *channel_name, uint8_t colour_code, const char *power);
// A P25 caller the same way: "P25" in the bottom bar, the NAC of the call in the status bar (no timeslot)
void qso_panel_draw_p25(const p25_call_t *call, const char *channel_name, const char *power);
// An NXDN caller: "NXDN" in the bottom bar, the RAN of the call in the status bar
void qso_panel_draw_nxdn(const nxdn_call_t *call, const char *channel_name, const char *power);
// An M17 caller (once the LSF or the LICH told it, as P25's and NXDN's once the caller is known): its callsign
// large, the destination over the S meter ("ALL" for a broadcast), "M17" in the bottom bar
void qso_panel_draw_m17(const m17_call_t *call, const char *channel_name, const char *power);
// A YSF Fusion caller: its callsign large, the destination over the S meter, the DG-ID in the status bar
void qso_panel_draw_ysf(const ysf_call_t *call, const char *channel_name, const char *power);

#endif
