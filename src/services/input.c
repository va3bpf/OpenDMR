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
 * OpenDMR, service: the input task (docs/ui-design.md 6). After OpenDM32 keyboard.c / buttons.c
 * (long press after settings.keypad_timer_long x 100 ms, repeat every settings.keypad_timer_repeat x 100 ms).
 *
 * Every 10 ms: the keys (a press, long press, repeats, release for each), the rotary steps; every second: the battery
 * percentage and the minute of the clock (UI_EVENT_DATA when they change); the backlight timeout (display.c), the
 * auto power off and lock (idle.c); the power off (power.c), after saving
 * the settings. It is also what tells the monitor (systime.c) that the tasks run: the UI task sleeps until an event.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "keys.h"
#include "rotary.h"
#include "settings.h"
#include "battery.h"
#include "datetime.h"
#include "systime.h"
#include "ui.h"
#include "power.h"
#include "idle.h"
#include "radio.h"
#include "display.h"
#include "aprs.h"
#include "input.h"

#define INPUT_PERIOD_MS     10
#define INPUT_TASK_PRIORITY 3
#define DEFAULT_LONG_MS     400 // settings 4 x 100 ms
#define DEFAULT_REPEAT_MS   200

static StaticTask_t task_tcb;
static StackType_t task_stack[1024]; // the APRS beacons (aprs_tick()): floating point, snprintf()

static uint32_t down_since_ms[UI_KEY_COUNT];
static uint32_t next_repeat_ms[UI_KEY_COUNT];
static bool long_sent[UI_KEY_COUNT];

static uint32_t long_ms(void)
{
    return (settings_valid() && settings.keypad_timer_long) ? (settings.keypad_timer_long * 100U) : DEFAULT_LONG_MS;
}

static uint32_t repeat_ms(void)
{
    return (settings_valid() && settings.keypad_timer_repeat) ? (settings.keypad_timer_repeat * 100U)
                                                              : DEFAULT_REPEAT_MS;
}

static void post_key(ui_event_type_t type, int key, uint8_t modifiers)
{
    ui_event_t ev = {.type = type, .key = (uint8_t)key, .modifiers = modifiers, .long_press = long_sent[key]};

    ui_post(&ev);
}

// All the keys as one mask: the keypad bits (keys.h), then SK1, SK2, PTT
static uint32_t read_keys(void)
{
    uint32_t buttons = keys_read_buttons();
    uint32_t mask = keys_read_keypad();

    mask |= (buttons & KEYS_BUTTON_SK1) ? (1UL << UI_KEY_SK1) : 0;
    mask |= (buttons & KEYS_BUTTON_SK2) ? (1UL << UI_KEY_SK2) : 0;
    mask |= (buttons & KEYS_BUTTON_PTT) ? (1UL << UI_KEY_PTT) : 0;
    return mask;
}

static uint8_t modifiers_of(uint32_t keys)
{
    return ((keys & (1UL << UI_KEY_SK1)) ? UI_MODIFIER_SK1 : 0) | ((keys & (1UL << UI_KEY_SK2)) ? UI_MODIFIER_SK2 : 0);
}

static void scan(uint32_t *last, uint32_t now)
{
    uint32_t keys = read_keys();
    uint32_t changed = keys ^ *last;
    uint8_t modifiers = modifiers_of(keys);

    if (changed) {
        idle_activity();
        radio_eco_activity();
        radio_vox_key((keys & (1UL << UI_KEY_PTT)) != 0);
        display_light_key((changed & keys) == (1UL << UI_KEY_SK1) && (keys == (1UL << UI_KEY_SK1)));
    }

    for (int key = 0; key < UI_KEY_COUNT; key++) {
        uint32_t bit = 1UL << key;

        if (changed & bit) {
            if (keys & bit) {
                down_since_ms[key] = now;
                long_sent[key] = false;
                post_key(UI_EVENT_KEY_PRESS, key, modifiers);
            } else {
                post_key(UI_EVENT_KEY_RELEASE, key, modifiers);
                long_sent[key] = false;
            }
        } else if (keys & bit) {
            if (!long_sent[key] && ((now - down_since_ms[key]) >= long_ms())) {
                long_sent[key] = true;
                next_repeat_ms[key] = now + repeat_ms();
                post_key(UI_EVENT_KEY_LONG, key, modifiers);
            } else if (long_sent[key] && ((int32_t)(now - next_repeat_ms[key]) >= 0)) {
                next_repeat_ms[key] = now + repeat_ms();
                post_key(UI_EVENT_KEY_REPEAT, key, modifiers);
            }
        }
    }
    *last = keys;
}

static void input_task(void *arg)
{
    uint32_t last_keys = 0;
    int32_t last_rotary = rotary_get_count();
    int last_battery = battery_percent();
    uint32_t last_minute = datetime_now() / 60;
    uint32_t last_data_check = 0;
    bool auto_off = false;
    TickType_t wake = xTaskGetTickCount();

    (void)arg;

    for (;;) {
        uint32_t now = systime_get_ms();
        int32_t rotary = rotary_get_count();

        if (power_off_pending() || auto_off) {
            settings_save_if_changed();
            power_off_save_all();
            power_off();
        }

        scan(&last_keys, now);

        if (rotary != last_rotary) {
            ui_event_t ev = {.type = UI_EVENT_ROTARY,
                             .modifiers = modifiers_of(last_keys),
                             .steps = (int16_t)(rotary - last_rotary)};

            idle_activity();
            radio_eco_activity();
            display_light_key(false);
            if (ui_post(&ev)) {
                last_rotary = rotary;
            }
        }

        if ((now - last_data_check) >= 1000) {
            ui_event_t ev = {.type = UI_EVENT_DATA};
            int battery = battery_percent();
            uint32_t minute = datetime_now() / 60;

            last_data_check = now;
            auto_off = idle_check_1s(); // switched off at the next pass, as with the switch
            ev.what |= (battery != last_battery) ? UI_DATA_BATTERY : 0;
            ev.what |= (minute != last_minute) ? UI_DATA_TIME : 0;
            if (ev.what && ui_post(&ev)) {
                last_battery = battery;
                last_minute = minute;
            }
        }

        display_light_tick(INPUT_PERIOD_MS);
        aprs_tick();
        systime_app_passes++; // alive, for the monitor (systime.c)
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_PERIOD_MS));
    }
}

void input_start(void)
{
    xTaskCreateStatic(input_task, "input", sizeof(task_stack) / sizeof(task_stack[0]), NULL, INPUT_TASK_PRIORITY,
                      task_stack, &task_tcb);
}
