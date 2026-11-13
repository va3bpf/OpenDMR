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
 * OpenDMR (docs/architecture.md, docs/ui-design.md 6): start. The hardware, the services, the input and
 * cable link tasks, then the UI with the channel screen as the home screen.
 */

#include "keymap.h"
#include "quick_keys.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "panel.h"
#include "theme.h"
#include "keys.h"
#include "rotary.h"
#include "leds.h"
#include "backlight.h"
#include "display.h"
#include "vfo_screen.h"
#include "fm_radio.h"
#include "radio.h"
#include "gps.h"
#include "rx_capture.h"
#include "sound.h"
#include "flash.h"
#include "systime.h"
#include "cable_link.h"
#include "settings.h"
#include "language.h"
#include "codeplug.h"
#include "dmr_ids.h"
#include "adc.h"
#include "battery.h"
#include "datetime.h"
#include "input.h"
#include "ui.h"
#include "list_screen.h"
#include "channel_screen.h"
#include "hotspot.h"
#include "hotspot_screen.h"
#include "cps_screen.h"

volatile unsigned long freertos_assert_line = 0;

// The screens that go over all others: the hotspot's, the CPS's
static bool screen_hooks(const ui_event_t *ev)
{
    return hotspot_screen_hook(ev) || cps_screen_hook(ev);
}

int main(void)
{
    clock_init();
    pins_init();
    irq_init();
    leds_init();

    flash_init();
    panel_init();
    theme_init();
    backlight_set_percent(100);

    keys_init();
    rotary_init();

    settings_load();
    display_light_start();
    codeplug_init();
    dmr_ids_init();
    keymap_load();
    language_load();
    ui_list_places_load();
    quick_keys_init();
    radio_init(); // the factory calibration, the FD6818 and the baseband
    datetime_init();
    adc_init();
    battery_sample_1s(); // a first value before the screen shows it

    systick_init(configTICK_RATE_HZ);
    systime_init();
    hotspot_init();
    cable_link_init();
    input_start();
    radio_start();
    gps_start();
    rx_capture_start(); // debug: the received AMBE frames to the flash
    sound_start();
    sound_play_boot();
    ui_set_event_hook(screen_hooks);
    ui_start(((settings_valid() && (settings.initial_menu_number == VFO_SCREEN_MENU_VFO)) ? &vfo_screen
                                                                                          : &channel_screen),
             (fm_radio_was_playing() ? &fm_radio_screen : NULL)); // switched off on the FM radio: back on it
    systime_watchdog_start(); // a stall from here on resets the radio

    vTaskStartScheduler();

    // not reached: the scheduler doesn't return
    leds_red(true);
    for (;;) {
    }
}
