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
 * OpenDMR, UI framework: event queue, UI task, screen stack, timer (docs/ui-design.md 6).
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "framebuffer.h"
#include "theme.h"
#include "systime.h"
#include "sound.h"
#include "radio.h"
#include "ui.h"

#define STACK_DEPTH      8
#define QUEUE_LENGTH     16
#define UI_TASK_PRIORITY 2

static StaticTask_t task_tcb;
static StackType_t task_stack[1536];
static StaticQueue_t queue_buffer;
static uint8_t queue_storage[QUEUE_LENGTH * sizeof(ui_event_t)];
static QueueHandle_t queue = NULL;

typedef struct {
    const ui_screen_t *screen;
    void *arg;     // given to ui_push(), kept for the level
    int32_t state; // the screen's own value for this level (e.g. the selected row of a list)
} level_t;

static level_t stack[STACK_DEPTH];
static int depth = 0;
static bool redraw = false;

static const int16_t *key_beep = SOUND_KEY; // the next key beep (nextKeyBeepMelody, ui_key_beep()), then back to it

static bool timer_running = false;
static uint32_t timer_due_ms;

int ui_key_digit(uint8_t key)
{
    // The keypad bits aren't in digit order (keys.h)
    static const int8_t digits[UI_KEY_COUNT] = {
        [UI_KEY_0] = 1, [UI_KEY_1] = 2, [UI_KEY_2] = 3, [UI_KEY_3] = 4, [UI_KEY_4] = 5,
        [UI_KEY_5] = 6, [UI_KEY_6] = 7, [UI_KEY_7] = 8, [UI_KEY_8] = 9, [UI_KEY_9] = 10,
    };

    return (key < UI_KEY_COUNT) ? (digits[key] - 1) : -1;
}

bool ui_post(const ui_event_t *ev)
{
    return queue && (xQueueSend(queue, ev, 0) == pdTRUE);
}

bool ui_post_from_isr(const ui_event_t *ev)
{
    BaseType_t woken = pdFALSE;
    bool ok = queue && (xQueueSendFromISR(queue, ev, &woken) == pdTRUE);

    portYIELD_FROM_ISR(woken);
    return ok;
}

static const ui_screen_t *top(void)
{
    return (depth > 0) ? stack[depth - 1].screen : NULL;
}

static void leave_top(void)
{
    if (top() && top()->leave) {
        top()->leave();
    }
    timer_running = false;
}

static void enter_top(void *arg)
{
    if (top() && top()->enter) {
        top()->enter(arg);
    }
    redraw = true;
}

void ui_push(const ui_screen_t *screen, void *arg)
{
    if (depth >= STACK_DEPTH) {
        return;
    }
    leave_top();
    stack[depth].screen = screen;
    stack[depth].arg = arg;
    stack[depth].state = 0;
    depth++;
    enter_top(arg);
}

void ui_pop(void)
{
    if (depth <= 1) {
        return; // the home screen stays
    }
    leave_top();
    depth--;
    enter_top(NULL);
}

void ui_replace(const ui_screen_t *screen, void *arg)
{
    leave_top();
    if (depth == 0) {
        depth = 1;
    }
    stack[depth - 1].screen = screen;
    stack[depth - 1].arg = arg;
    stack[depth - 1].state = 0;
    enter_top(arg);
}

void ui_home(void)
{
    if (depth <= 1) {
        return;
    }
    leave_top();
    depth = 1;
    enter_top(NULL);
}

void *ui_arg(void)
{
    return (depth > 0) ? stack[depth - 1].arg : NULL;
}

int32_t *ui_state(void)
{
    static int32_t none;

    return (depth > 0) ? &stack[depth - 1].state : &none;
}

void ui_key_beep(const int16_t *melody)
{
    key_beep = melody;
}

void ui_tune(void)
{
    for (int i = depth - 1; i >= 0; i--) {
        const codeplug_channel_t *channel =
            stack[i].screen->channel ? stack[i].screen->channel() : UI_CHANNEL_UNDER;

        if (channel == UI_CHANNEL_KEPT) {
            return;
        }
        if (channel != UI_CHANNEL_UNDER) {
            radio_set_channel(channel);
            return;
        }
    }
}

void ui_redraw(void)
{
    redraw = true;
}

void ui_timer_start(uint32_t ms)
{
    timer_due_ms = systime_get_ms() + ms;
    timer_running = true;
}

void ui_timer_stop(void)
{
    timer_running = false;
}

static void draw(void)
{
    fb_clear(theme_colour(THEME_COLOUR_BACKGROUND));
    if (top() && top()->draw) {
        top()->draw();
    }
    fb_flush();
}

static const ui_screen_t *start_on_top; // pushed over the home screen when the UI task starts

static bool (*event_hook)(const ui_event_t *ev) = NULL;

void ui_set_event_hook(bool (*hook)(const ui_event_t *ev))
{
    event_hook = hook;
}

static void ui_task(void *arg)
{
    ui_event_t ev;

    (void)arg;
    enter_top(NULL);
    if (start_on_top) {
        ui_push(start_on_top, NULL);
    }
    ui_tune();

    for (;;) {
        if (redraw) {
            redraw = false;
            draw();
        }

        TickType_t wait = portMAX_DELAY;

        if (timer_running) {
            int32_t left = (int32_t)(timer_due_ms - systime_get_ms());

            wait = (left > 0) ? pdMS_TO_TICKS(left) : 0;
        }

        if (xQueueReceive(queue, &ev, wait) != pdTRUE) {
            if (!timer_running || ((int32_t)(timer_due_ms - systime_get_ms()) > 0)) {
                continue;
            }
            timer_running = false;
            ev.type = UI_EVENT_TIMER;
        }

        // The event changes the screens' data, or the screens: then the radio on the channel they make now (once,
        // after the screen and anything it pushed, popped or replaced are done)
        if (event_hook && event_hook(&ev)) {
            ui_tune();
            continue;
        }
        if (top() && top()->handle && top()->handle(&ev)) {
            redraw = true;
        }
        ui_tune();
        // The key beep after the screen handled the key (keyBeepHandler()): on a release (a short press) or a long
        // press, not for the PTT. A screen may have set another one (ui_key_beep()), on the press or now; it plays
        // once, then the key beep is back. NULL: none for this key.
        if ((((ev.type == UI_EVENT_KEY_RELEASE) && !ev.long_press) || (ev.type == UI_EVENT_KEY_LONG)) &&
            (ev.key != UI_KEY_PTT)) {
            if (key_beep) {
                sound_play(((ev.type == UI_EVENT_KEY_LONG) && (key_beep == SOUND_KEY)) ? SOUND_KEY_LONG : key_beep);
            }
            key_beep = SOUND_KEY;
        } else if (ev.type == UI_EVENT_ROTARY) {
            key_beep = SOUND_KEY; // the rotary has no beep: one set for it is gone
        }
    }
}

void ui_start(const ui_screen_t *home, const ui_screen_t *on_top)
{
    start_on_top = on_top;
    stack[0].screen = home;
    stack[0].arg = NULL;
    stack[0].state = 0;
    depth = 1;
    queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(ui_event_t), queue_storage, &queue_buffer);
    xTaskCreateStatic(ui_task, "ui", sizeof(task_stack) / sizeof(task_stack[0]), NULL, UI_TASK_PRIORITY, task_stack,
                      &task_tcb);
}
