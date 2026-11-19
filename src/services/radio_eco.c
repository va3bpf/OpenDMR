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
 * OpenDMR, service: the radio, receive power saving (radio_internal.h).
 */

#include "FreeRTOS.h"
#include "task.h"
#include "util.h"
#include "fd6818.h"
#include "settings.h"
#include "codeplug.h"
#include "dmr.h"
#include "hotspot.h"
#include "radio_internal.h"

/* Receive power saving (rxPowerSaving.c rxPowerSavingTick(), trxPowerUpDownRxAndC7000()): no signal, key, beep or
   transmission for 12 - 2 x level seconds (at least 4 s), then the receiver (the FD6818 and the LNA) goes off and on
   in turns: on for 130 - 10 x level ms, off for that x 2^(level - 1) (level 1: 120 ms / 120 ms ... level 5: 80 ms /
   1.28 s); on a P25 channel on for 210 ms at least (ECO_P25_RX_MS). A signal found while on, a key (radio_eco_activity()), a P25 sync (the same, p25_radio.c) or anything
   going on ends it. Not on a channel with
   "no eco", nor while scanning, with the VOX, in hotspot mode or while the FM broadcast radio plays (switching the
   receiver cut its audio). The main firmware's slower CPU clock (level 2 and
   over) isn't done here. */

#define ECO_FLAG1_NO_ECO  0x20 // libre_dmr_flag1
#define ECO_SETTLE_MS     25   // after the receiver comes on, before the squelch reads the noise
// P25: on at least this long, an LDU (180 ms: a frame sync each) and the sync and the settling, so that the modem sees
// a sync every time it is on (DMR has one every 60 ms). Measured 2026-11-12 at level 1 (120 ms on): calls found from
// the HDU 3 times in 6, the others 260 ms or more late
#define ECO_P25_RX_MS     210

typedef enum {
    ECO_INACTIVE,
    ECO_WAIT_START,
    ECO_RX_ON,
    ECO_RX_OFF,
} eco_state_t;

static eco_state_t eco = ECO_INACTIVE;
static TickType_t eco_due;
static volatile bool eco_activity = false;
static bool rx_off = false; // the receiver is off for power saving

static void eco_wait(void)
{
    int level = MIN(settings.eco_level, 4);

    eco_due = xTaskGetTickCount() + pdMS_TO_TICKS((12 - (level * 2)) * 1000);
}

static void rx_power(bool on)
{
    if (on == !rx_off) {
        return;
    }
    rx_off = !on;
    if (on) {
        receive_on();
        rx_settle(ECO_SETTLE_MS);
    } else {
        lna(false, 0);
        fd6818_write(0x30, 0x0000); // radioPowerOff(): the FD6818 idle
    }
}

// The channel is set up again (apply_channel()): receiving, the wait starts over
void eco_reset(void)
{
    rx_off = false;
    eco = ECO_INACTIVE;
    eco_wait();
}

// Back to receiving all the time (a channel set, something going on)
static void eco_stop(void)
{
    if (eco != ECO_INACTIVE) {
        rx_power(true);
        eco = ECO_INACTIVE;
    }
    eco_wait();
}

void eco_tick(void)
{
    int level = settings.eco_level;
    bool active = eco_activity;

    eco_activity = false;
    if (!rs.has_channel || (level == 0) || (rs.channel.libre_dmr_flag1 & ECO_FLAG1_NO_ECO) || rs.is_transmitting || rs.ptt_down ||
        rs.has_carrier || rs.speaker_open || rs.audio_needs_amp || rs.beep_needs_amp || rs.broadcast_needs_amp || rs.is_scanning || vox_running() || aprs_pending() || data_pending() ||
        hotspot_active() ||
        (rs.dmr_running && dmr_has_sync()) || rs.nxdn_running || active) { // NXDN: the recording needs the receiver on (for now)
        eco_stop();
        return;
    }
    if ((int32_t)(xTaskGetTickCount() - eco_due) < 0) {
        return;
    }

    uint32_t rx_ms = (uint32_t)(130 - (10 * level));
    uint32_t on_ms = (rs.p25_running && (rx_ms < ECO_P25_RX_MS)) ? ECO_P25_RX_MS : rx_ms;

    switch (eco) {
        case ECO_INACTIVE: // wait before shutting down
            eco_wait();
            eco = ECO_WAIT_START;
            break;
        case ECO_WAIT_START:
        case ECO_RX_ON:
            rx_power(false);
            eco_due = xTaskGetTickCount() + pdMS_TO_TICKS(rx_ms * (1U << (level - 1)));
            eco = ECO_RX_OFF;
            break;
        case ECO_RX_OFF:
            rx_power(true);
            eco_due = xTaskGetTickCount() + pdMS_TO_TICKS(on_ms);
            eco = ECO_RX_ON;
            break;
    }
}

void radio_eco_activity(void)
{
    eco_activity = true;
    if (rx_off) {
        radio_wake();
    }
}

// The receiver off for power saving: nothing to read
bool eco_rx_off(void)
{
    return rx_off;
}
