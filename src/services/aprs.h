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

#ifndef DM32_APRS_H
#define DM32_APRS_H

#include <stdbool.h>
#include <stdint.h>

// APRS position beacons (aprs.c, from the main firmware's functions/aprs.c): the AX.25 UI frame of the channel's APRS
// configuration (CPS), its position from the GPS or fixed, sent as AFSK by the radio task. The mode is the APRS
// options': off, manual (SK1 + 3), PTT (after a transmission on the channel), auto (each interval, decaying when not
// moving) or smart beaconing (by the speed and the turns). FM channels with an APRS configuration only.

void aprs_tick(void);   // the input task, each pass
bool aprs_is_active(void); // beaconing on (SK1 + 1), not suspended
void aprs_toggle(void); // SK1 + 1: beaconing on / off (aprsBeaconingToggles())
void aprs_send(void);   // SK1 + 3 or the menu's "send now": a beacon now, in any mode but OFF
void aprs_after_ptt(void); // a transmission of the PTT ended (the PTT mode)
// The satellite screen (#55), as OpenGD77: a beacon now through the satellite's digipeater, with the APRS
// configuration named as the satellite (its path: RS0ISS for the ISS ...), on the channel the screen tuned (the
// satellite's APRS uplink, Doppler corrected); in any beaconing mode. An error tone when there is no configuration of
// that name, no position, or the radio refused it
void aprs_send_satellite(const char *name);
bool aprs_is_sending(void); // a beacon asked or with the radio task (the satellite screen doesn't retune meanwhile)

// The cable test interface (test_link.c) reports why a manual beacon was or was not sent. Bits of flags: 0 enabled,
// 1 the channel has an APRS configuration, 2 a usable position, 3 an LRRP is with the DMR engine, 4 it was queued,
// 5..7 the beaconing mode. The position is in 1E-7 degrees, 0 when unknown.
void aprs_debug_status(uint32_t *flags, int32_t *lat_e7, int32_t *lon_e7);

#endif
