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

#ifndef DM32_HOTSPOT_H
#define DM32_HOTSPOT_H

#include <stdbool.h>
#include <stdint.h>

// The DMR hotspot (hotspot.c, from the main firmware's functions/hotspot.c, OpenGD77 VK3KYY / F1RMB): the radio as
// the MMDVM modem of MMDVMHost (Pi-Star) or BlueDV, on the programming cable (115200 8N1). With the hotspot type of
// the general options set, an MMDVM frame on the cable starts it: the UI shows the hotspot screen (hotspot_screen.c),
// which starts and ends it; the radio transmits what the network sends and sends the network what it receives, DMR
// simplex (DMO) on the frequencies, colour code and power MMDVMHost sets. MMDVMHost silent for 20 s ends it; BlueDV:
// the right key. The CW ID of MMDVMHost is sent in FM.

void hotspot_init(void); // its task

// From the cable link's interrupt (cable_link.c), each byte: true when it is the hotspot's (an MMDVM frame;
// frame_start: no CPS request is being received)
bool hotspot_rx_byte(uint8_t c, bool frame_start);

bool hotspot_wanted(void); // a host talks MMDVM: the hotspot screen should be shown
bool hotspot_active(void); // started (hotspot_start()), not ended yet
void hotspot_start(void);  // the hotspot screen: on the radio's channel
void hotspot_stop(void);   // BlueDV: the right key

typedef enum {
    HOTSPOT_SHOW_WAITING, // no host yet
    HOTSPOT_SHOW_IDLE,
    HOTSPOT_SHOW_RX,      // a call received, sent to the network
    HOTSPOT_SHOW_TX,      // a call of the network, transmitted
    HOTSPOT_SHOW_CW,      // the CW ID
} hotspot_show_t;

typedef struct {
    hotspot_show_t show;
    bool bluedv;
    bool pocsag;           // MMDVMHost has set the POCSAG mode
    uint32_t rx_freq;      // 10 Hz units
    uint32_t tx_freq;
    uint8_t colour_code;
    int power_level;       // the radio's levels (0 - 9); -1: the settings'
    uint32_t source;       // the call shown (RX or TX)
    uint32_t destination;
    bool private_call;
    char ip[22];           // from MMDVMHost's CAST display driver
} hotspot_status_t;

void hotspot_get_status(hotspot_status_t *status);

#endif
