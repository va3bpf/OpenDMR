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

#ifndef DM32_UI_H
#define DM32_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug.h"

// The UI framework (docs/ui-design.md 6): events from the input service and the other services go into a queue; the
// UI task gives them to the screen on top of the stack and draws the whole screen into the frame buffer when the
// screen asks, the frame buffer sends what changed.

// Keys: the keypad (bit numbers of keys.h), then the side buttons and the PTT
enum {
    UI_KEY_GREEN = 0, // the left key: menu / OK
    UI_KEY_3,
    UI_KEY_2,
    UI_KEY_1,
    UI_KEY_LEFT,
    UI_KEY_6,
    UI_KEY_5,
    UI_KEY_4,
    UI_KEY_RIGHT,
    UI_KEY_9,
    UI_KEY_8,
    UI_KEY_7,
    UI_KEY_RED, // the right key: back / home
    UI_KEY_HASH,
    UI_KEY_0,
    UI_KEY_STAR,
    UI_KEY_UP,
    UI_KEY_DOWN,
    UI_KEY_SK1,
    UI_KEY_SK2,
    UI_KEY_PTT,
    UI_KEY_COUNT
};

int ui_key_digit(uint8_t key); // UI_KEY_0 .. UI_KEY_9: 0 .. 9; -1: another key

// Modifiers: the side buttons held when the event happened
#define UI_MODIFIER_SK1 0x01
#define UI_MODIFIER_SK2 0x02

typedef enum {
    UI_EVENT_KEY_PRESS,
    UI_EVENT_KEY_LONG,    // held for the long press time (settings.keypad_timer_long x 100 ms)
    UI_EVENT_KEY_REPEAT,  // then every settings.keypad_timer_repeat x 100 ms
    UI_EVENT_KEY_RELEASE, // long: a UI_EVENT_KEY_LONG came before
    UI_EVENT_ROTARY,      // steps: + clockwise
    UI_EVENT_DATA,        // what: UI_DATA_x changed
    UI_EVENT_TIMER,       // the timer of the screen (ui_timer_start())
} ui_event_type_t;

// What changed, for UI_EVENT_DATA
#define UI_DATA_BATTERY    0x0001
#define UI_DATA_TIME       0x0002 // the minute
#define UI_DATA_LAST_HEARD 0x0004 // last_heard.c
#define UI_DATA_SIGNAL     0x0008 // radio.c: the squelch, the shown signal, receive / transmit
#define UI_DATA_AUTOLOCK   0x0010 // idle.c: no key for the auto lock time (the home screens lock)
#define UI_DATA_HOTSPOT    0x0020 // hotspot.c: a host wants it, it ended, or what it shows changed
#define UI_DATA_CPS        0x0040 // cps_screen.c: the CPS opened, rendered or closed its screen, or more data went

typedef struct {
    ui_event_type_t type;
    uint8_t key;
    uint8_t modifiers;
    bool long_press;
    int16_t steps;
    uint16_t what;
} ui_event_t;

// The channel of a screen (ui_screen_t.channel) besides a channel, or NULL (none: the receiver off)
#define UI_CHANNEL_UNDER ((const codeplug_channel_t *)1) // the one of the screen under it
#define UI_CHANNEL_KEPT  ((const codeplug_channel_t *)2) // a service tunes the radio itself (the hotspot): left alone

typedef struct {
    void (*enter)(void *arg);             // pushed (arg), or back on top (arg NULL); may be NULL
    void (*leave)(void);                  // popped or covered; may be NULL
    bool (*handle)(const ui_event_t *ev); // true: draw the screen again
    void (*draw)(void);                   // the whole screen, into the frame buffer
    // The channel the radio is on, made from the screen's data as they are now; NULL (most screens): the one of the
    // screen under it. Screens don't tune the radio: the UI task asks after each event, as it draws (ui_tune()).
    const codeplug_channel_t *(*channel)(void);
} ui_screen_t;

// Starts the UI task with the home screen (channel or VFO) at the bottom, and on_top (NULL: none) over it, entered
// from the UI task
void ui_start(const ui_screen_t *home, const ui_screen_t *on_top);
// Sees every event before the screen on top (e.g. to put a screen over all others); true: the event is taken
void ui_set_event_hook(bool (*hook)(const ui_event_t *ev));

bool ui_post(const ui_event_t *ev); // from a task
bool ui_post_from_isr(const ui_event_t *ev);

void ui_push(const ui_screen_t *screen, void *arg);
void ui_pop(void);
void ui_replace(const ui_screen_t *screen, void *arg);
void ui_home(void);
// The channel of the screens (the first from the top that has one) to the radio, which takes the same one again
// without a change. The UI task does it after each event; before a transmission it is done at once (tx_screen.c)
void ui_tune(void);
void ui_redraw(void);    // after the current event
const ui_screen_t *ui_top_screen(void); // the screen on top
void *ui_arg(void);      // the arg the screen on top was pushed with
int32_t *ui_state(void); // a value kept for the screen on top, 0 when pushed (e.g. its selected row)

void ui_timer_start(uint32_t ms); // one UI_EVENT_TIMER for the screen on top, after ms
void ui_key_beep(const int16_t *melody); // from handle(): the beep of this key instead of the key beep; NULL: none
void ui_timer_stop(void);

#endif
