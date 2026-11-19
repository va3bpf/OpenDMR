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
 * OpenDMR, service: the radio, the VOX (vox.h; radio_internal.h): it presses and releases the PTT through the UI as
 * a key would.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "settings.h"
#include "ui.h"
#include "codeplug.h"
#include "systime.h"
#include "vox.h"
#include "adc.h"
#include "security.h"
#include "radio_internal.h"

// The VOX (vox.h): on for a channel with VOX and a VOX level set, on the stock firmware's VOX ADC channel. As the
// stock firmware, 10 samples (1 s) are left out after the channel is set and after a received signal ends; also
// after a key (the main firmware) or a beep (the speaker into the mic). Not after its own transmission: it keys
// again at once when the speech goes on.
#define VOX_SETTLE_MS 1000
static bool vox_on = false;
static bool vox_keyed = false;         // the VOX posted a PTT press, its release is due
static volatile bool vox_key = false;  // the input task saw a key (radio_vox_key())
static volatile bool vox_ptt = false;  // ... and the PTT was held
static uint32_t vox_next_ms;
static volatile bool vox_heard = false; // a received signal was heard: start again
static uint16_t vox_window_max = 0;    // the loudest ADC reading of this VOX_SAMPLE_MS window

static bool vox_wanted(void)
{
    return rs.has_channel && (rs.channel.flag4 & FLAG4_VOX) && !(rs.channel.flag4 & FLAG4_RX_ONLY) &&
           codeplug_mode_can_transmit(rs.channel.mode) && (settings.vox_threshold != 0);
}

static void vox_post(bool press)
{
    ui_event_t ev = {.type = press ? UI_EVENT_KEY_PRESS : UI_EVENT_KEY_RELEASE, .key = UI_KEY_PTT,
                     .what = UI_KEY_FROM_VOX};

    ui_post(&ev);
}

// After the channel is set, or the VOX level changed: on or off
void vox_setup(void)
{
    bool wanted = vox_wanted();

    if (wanted && !vox_on) {
        vox_reset(VOX_SETTLE_MS);
    }
    if (!wanted && vox_keyed) {
        vox_post(false);
        vox_keyed = false;
    }
    vox_on = wanted;
}

// Every VOX_SAMPLE_MS (voxTick() and the PTT of applicationMain.c): not while a sound plays (the speaker into the
// mic), a key gives the transmitter back to the keys
static void vox_check(void)
{
    uint32_t now = systime_get_ms();

    if (vox_key) {
        vox_key = false;
        if (vox_keyed) {
            if (!vox_ptt) {
                vox_post(false);
            }
            vox_keyed = false; // the PTT held: its release ends it
        }
        vox_reset(VOX_SETTLE_MS);
        return;
    }
    // the ADC each pass (25 ms), the loudest of the VOX_SAMPLE_MS window decides: one reading a window, as the stock
    // firmware, often fell between the syllables and cut the speech
    uint16_t level = adc_read_vox();

    if (level > vox_window_max) {
        vox_window_max = level;
    }
    if ((int32_t)(now - vox_next_ms) < 0) {
        return;
    }
    vox_next_ms = now + VOX_SAMPLE_MS;
    level = vox_window_max;
    vox_window_max = 0;
    // the power-on PIN not entered yet, or locked with the lock PIN (security.h): the VOX stays off
    if (security_secured()) {
        if (vox_keyed) {
            vox_post(false);
            vox_keyed = false;
        }
        vox_reset(VOX_SETTLE_MS);
        return;
    }
    // a sound into the mic starts it again, but not once it keyed the transmitter (the DMR start beep plays before
    // the transmission: that release cut every DMR VOX transmission at once)
    if (!vox_keyed && (vox_heard || (!rs.transmitting && rs.amp_for_beep))) {
        vox_heard = false;
        vox_reset(VOX_SETTLE_MS);
        return;
    }
    vox_heard = false;

    bool held = vox_sample(level, settings.vox_threshold, settings.vox_tail_units);

    if (held && !vox_keyed && !rs.tx_wanted) {
        vox_post(true);
        vox_keyed = true;
    } else if (!held && vox_keyed) {
        vox_post(false);
        vox_keyed = false;
    }
}

void radio_vox_key(bool ptt_held)
{
    vox_ptt = ptt_held;
    vox_key = true;
}

// A pass of the task: while a received signal plays, the quiet wait starts again (vox_check()); else the VOX on or
// off as the channel and the menu say, and its check
void vox_pass(void)
{
    if (!rs.transmitting && (rs.speaker || rs.amp_for_audio)) {
        vox_heard = vox_on; // flash code waits: the quiet wait while heard (vox_check() starts it again)
    } else {
        if (!rs.transmitting && (vox_wanted() != vox_on)) {
            vox_setup(); // the VOX threshold set or cleared in the menu
        }
        if (vox_on) {
            vox_check();
        }
    }
}

bool vox_running(void)
{
    return vox_on;
}
