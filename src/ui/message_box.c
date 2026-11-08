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
 * OpenDMR, UI: the message box and the notification (uiMessageBox.c, uiNotification.c of
 * OpenDM32 user/SOURCE/user_interface). Drawn like the result pages of the lists (title bar, the lines
 * under it, the soft keys). The pin code entry of the original message box is not here.
 */

#include <stddef.h>
#include "widgets.h"
#include "text.h"
#include "message_box.h"

static const message_box_t *box(void)
{
    return (const message_box_t *)ui_arg();
}

static void enter(void *arg)
{
    const message_box_t *b = arg;

    if (b && (b->close_ms > 0)) {
        ui_timer_start(b->close_ms);
    }
}

// Popped first: the answer may push another screen
static void close_box(bool yes)
{
    void (*answered)(bool) = box()->answered;

    ui_pop();
    if (answered) {
        answered(yes);
    }
}

static bool handle(const ui_event_t *ev)
{
    const message_box_t *b = box();
    bool green = (ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN);
    bool red_up = (ev->type == UI_EVENT_KEY_RELEASE) && (ev->key == UI_KEY_RED) && !ev->long_press;

    if (!b) {
        return false;
    }
    if (ev->type == UI_EVENT_TIMER) {
        close_box(true);
    } else if (b->buttons == MESSAGE_BOX_NONE) {
        // the keys are swallowed
    } else if (green) {
        close_box(true);
    } else if (red_up) {
        close_box(b->buttons == MESSAGE_BOX_OK);
    } else if ((ev->type == UI_EVENT_KEY_LONG) && (ev->key == UI_KEY_RED)) {
        ui_home(); // no answer
    }
    return false;
}

static void draw(void)
{
    const message_box_t *b = box();

    if (!b) {
        return;
    }
    list_view_draw_title(b->title, NULL);
    list_view_draw_text(b->line1, b->line2, (b->colour == THEME_COLOUR_BACKGROUND) ? THEME_COLOUR_TEXT : b->colour);
    switch (b->buttons) {
        case MESSAGE_BOX_OK:
            bottom_bar_draw_soft_keys(text(TEXT_UI_OK), NULL);
            break;
        case MESSAGE_BOX_YES_NO:
            bottom_bar_draw_soft_keys(text(TEXT_YES), text(TEXT_NO));
            break;
        default:
            bottom_bar_draw_soft_keys(NULL, NULL);
            break;
    }
}

const ui_screen_t message_box_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
};
