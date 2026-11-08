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
 * OpenDMR, service: the auto power off and the auto lock (idle.h). From applicationMain.c and
 * batteryAndPowerManagement.c apoTick() of the main firmware.
 *
 * Auto power off (settings.apo, 30 minute units, 0 off): no key nor knob for that long switches the radio off; the
 * time starts again while transmitting or scanning, and while a signal is heard with "APO with RF" on. One minute before,
 * the APO melody. Auto lock (settings.autolock_timer, 30 second units, 0 off): no key nor knob for that long locks the
 * keypad and the PTT, on the home screens only (they take UI_DATA_AUTOLOCK; another screen on top ignores it).
 */

#include "settings.h"
#include "systime.h"
#include "radio.h"
#include "sound.h"
#include "ui.h"
#include "idle.h"

#define APO_UNIT_MS      (30UL * 60UL * 1000UL)
#define APO_WARNING_MS   60000UL
#define AUTOLOCK_UNIT_MS 30000UL

static volatile uint32_t activity_ms; // the last key or knob
static uint32_t apo_from_ms;          // the auto power off counts from here
static bool apo_warned = false;
static bool autolock_asked = false;

void idle_activity(void)
{
    activity_ms = systime_get_ms();
    autolock_asked = false;
}

bool idle_check_1s(void)
{
    uint32_t now = systime_get_ms();

    if ((settings.autolock_timer != 0) && !autolock_asked &&
        ((now - activity_ms) >= (settings.autolock_timer * AUTOLOCK_UNIT_MS))) {
        ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_AUTOLOCK};

        autolock_asked = ui_post(&ev); // once, until the next key
    }

    if ((int32_t)(activity_ms - apo_from_ms) > 0) {
        apo_from_ms = activity_ms;
    }
    if (radio_is_transmitting() || radio_is_scanning() ||
        ((settings.bitfield_options & SETTINGS_BIT_APO_WITH_RF) && radio_audio_heard())) {
        apo_from_ms = now;
    }
    if (settings.apo == 0) {
        apo_from_ms = now;
        apo_warned = false;
        return false;
    }

    uint32_t idle = now - apo_from_ms;
    uint32_t limit = settings.apo * APO_UNIT_MS;

    if (idle >= limit) {
        return true;
    }
    if (idle >= (limit - APO_WARNING_MS)) {
        if (!apo_warned) {
            apo_warned = true;
            sound_play(SOUND_APO_TRIGGERED);
        }
    } else {
        apo_warned = false;
    }
    return false;
}
