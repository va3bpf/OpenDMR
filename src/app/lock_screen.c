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
 * OpenDMR, screen: the keypad and PTT lock (uiLockScreen.c; the key filter of applicationMain.c, the * and #
 * keys of the main menu in menuDisplayMenuList.c).
 *
 * The lock only exists on the home screens: locking goes home, as the original pops to the root menu. While the
 * keypad is locked every key, the side buttons and the rotary are taken; a key press shows the lock page a moment,
 * SK2 + * unlocks both. The PTT still works unless the PTT is locked too. The page takes the keys itself, so the
 * unlock works while it shows. The auto lock: idle.c asks the home screens (UI_DATA_AUTOLOCK). TODO: the voice
 * prompts.
 */

#include <stddef.h>
#include "widgets.h"
#include "text.h"
#include "lock_screen.h"

#define SHOW_MS 1000

static bool keypad_locked = false;
static bool ptt_locked = false;
static const ui_screen_t lock_page;

static void show(void)
{
    if (ui_arg() == (void *)&lock_page) {
        ui_timer_start(SHOW_MS); // shows already: again from now
        ui_redraw();
    } else {
        ui_push(&lock_page, (void *)&lock_page);
    }
}

void lock_screen_lock(bool keypad, bool ptt)
{
    keypad_locked = keypad;
    ptt_locked = ptt;
    ui_home();
    show();
}

bool lock_screen_is_keypad_locked(void)
{
    return keypad_locked;
}

bool lock_screen_is_ptt_locked(void)
{
    return ptt_locked;
}

bool lock_screen_filter(const ui_event_t *ev)
{
    bool key = (ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_LONG) ||
               (ev->type == UI_EVENT_KEY_REPEAT) || (ev->type == UI_EVENT_KEY_RELEASE);

    if (ev->type == UI_EVENT_ROTARY) {
        return keypad_locked;
    }
    if (!key) {
        return false;
    }
    if (ev->key == UI_KEY_PTT) {
        if (ptt_locked && (ev->type == UI_EVENT_KEY_PRESS)) {
            show();
        }
        return ptt_locked;
    }
    if (!keypad_locked) {
        return false;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_STAR) && (ev->modifiers & UI_MODIFIER_SK2)) {
        keypad_locked = false;
        ptt_locked = false;
        show(); // "Unlocked"
    } else if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key != UI_KEY_SK2)) {
        show(); // SK2 alone is the first half of the unlock
    }
    return true;
}

static void page_enter(void *arg)
{
    (void)arg;
    ui_timer_start(SHOW_MS); // also back on top
}

static bool page_handle(const ui_event_t *ev)
{
    if (ev->type == UI_EVENT_TIMER) {
        ui_pop();
        return false;
    }
    if (lock_screen_filter(ev)) {
        return false; // show() restarted the timer and redraws
    }
    // Unlocked (or only the PTT locked): the key is for the home screen
    ui_pop();
    return false;
}

static void page_draw(void)
{
    bool locked = keypad_locked || ptt_locked;

    list_view_draw_title(text(TEXT_KEYPAD), NULL);
    if (!locked) {
        list_view_draw_text(text(TEXT_UNLOCKED), NULL, THEME_COLOUR_RX);
    } else {
        list_view_draw_text(text(TEXT_LOCKED), (keypad_locked ? text(TEXT_UI_UNLOCK_HINT) : text(TEXT_PTT)),
                            THEME_COLOUR_WARNING);
    }
    bottom_bar_draw_soft_keys(NULL, NULL);
}

static const ui_screen_t lock_page = {
    .enter = page_enter,
    .handle = page_handle,
    .draw = page_draw,
};
