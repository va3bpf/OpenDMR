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
 * OpenDMR, screen: the PIN entry (pin_screen.h). Each digit shows as a dot. Five wrong PINs in a row: no more tries
 * for 30 s (the count is kept until the power off). The power-on PIN screen tunes nothing (the receiver is off) and
 * takes every key; the hotspot isn't started under it (main.c). The unlock screen goes back to the lock after a while
 * without a key.
 */

#include <stdio.h>
#include <string.h>
#include "widgets.h"
#include "draw.h"
#include "fonts.h"
#include "text.h"
#include "sound.h"
#include "systime.h"
#include "security.h"
#include "lock_screen.h"
#include "pin_screen.h"

#define LABEL_Y        44
#define DIGITS_Y       80
#define HINT_Y         130
#define TRIES_MAX      5
#define WAIT_MS        30000
#define UNLOCK_IDLE_MS 15000
#define TICK_MS        1000

typedef enum {
    STEP_ENTER, // the PIN to check
    STEP_NEW,
    STEP_CONFIRM,
} step_t;

static struct {
    void *mode;
    step_t step;
    char digits[SECURITY_PIN_MAX + 1];
    uint8_t length;
    char first[SECURITY_PIN_MAX + 1]; // STEP_NEW's PIN
    text_id_t message;                 // under the dots; TEXT_COUNT: the hint
    uint32_t idle_ms;                  // the last key (the unlock screen)
} pin;

static bool in_use = false;       // pushed, not popped yet (enter() gets NULL also when back on top)
static uint8_t wrong = 0;          // in a row, kept until the power off
static uint32_t wait_until_ms = 0; // after TRIES_MAX wrong

static bool waiting(void)
{
    return (wrong >= TRIES_MAX) && ((int32_t)(wait_until_ms - systime_get_ms()) > 0);
}

static void clear_digits(void)
{
    memset(pin.digits, 0, sizeof(pin.digits));
    pin.length = 0;
}

static void enter(void *arg)
{
    if (!in_use || (arg != NULL)) { // pushed (not back on top)
        in_use = true;
        memset(&pin, 0, sizeof(pin));
        pin.mode = arg;
        pin.step = ((arg == PIN_SCREEN_BOOT) || (arg == PIN_SCREEN_UNLOCK)) ? STEP_ENTER : STEP_NEW;
        pin.message = TEXT_COUNT;
    }
    pin.idle_ms = systime_get_ms();
    ui_timer_start(TICK_MS);
}

static void pin_close(void)
{
    clear_digits();
    in_use = false;
    ui_pop();
}

static void done(void)
{
    void *mode = pin.mode;

    pin_close();
    if (mode == PIN_SCREEN_BOOT) {
        security_boot_unlocked();
    } else if (mode == PIN_SCREEN_UNLOCK) {
        lock_screen_unlock();
    }
}

static void take(void)
{
    if (pin.step == STEP_ENTER) {
        if (waiting()) {
            sound_play(SOUND_ERROR);
            return;
        }
        if ((pin.mode == PIN_SCREEN_BOOT) ? security_check_boot_pin(pin.digits)
                                          : security_check_lock_pin(pin.digits)) {
            wrong = 0;
            done();
            return;
        }
        if (++wrong >= TRIES_MAX) {
            wait_until_ms = systime_get_ms() + WAIT_MS;
        }
        pin.message = TEXT_UI_WRONG_PIN;
        sound_play(SOUND_ERROR);
    } else if (pin.length < SECURITY_PIN_MIN) {
        pin.message = TEXT_UI_PIN_DIGITS;
        sound_play(SOUND_ERROR);
        return;
    } else if (pin.step == STEP_NEW) {
        memcpy(pin.first, pin.digits, sizeof(pin.first));
        pin.step = STEP_CONFIRM;
        pin.message = TEXT_COUNT;
    } else if (strcmp(pin.first, pin.digits) != 0) {
        pin.step = STEP_NEW; // not the same: again from the start
        pin.message = TEXT_UI_PIN_MISMATCH;
        sound_play(SOUND_ERROR);
    } else if (!security_set_pin(pin.digits)) {
        pin.step = STEP_NEW;
        pin.message = TEXT_ERROR; // the flash write
        sound_play(SOUND_ERROR);
    } else {
        if (pin.mode == PIN_SCREEN_SET_BOOT) {
            security_set_boot_pin(true);
        } else if (pin.mode == PIN_SCREEN_SET_LOCK) {
            security_set_lock_pin(true);
        }
        sound_play(SOUND_ACK);
        done();
        return;
    }
    clear_digits();
}

static bool handle(const ui_event_t *ev)
{
    int digit;

    if (ev->type == UI_EVENT_TIMER) {
        if ((pin.mode == PIN_SCREEN_UNLOCK) && ((systime_get_ms() - pin.idle_ms) >= UNLOCK_IDLE_MS)) {
            pin_close(); // back to the lock
            return false;
        }
        ui_timer_start(TICK_MS);
        return waiting() || (wrong >= TRIES_MAX); // the count down, and once more when it ends
    }
    if (ev->type != UI_EVENT_KEY_PRESS) {
        return false;
    }
    pin.idle_ms = systime_get_ms();
    digit = ui_key_digit(ev->key);
    if (digit >= 0) {
        if (pin.length < SECURITY_PIN_MAX) {
            pin.digits[pin.length++] = (char)('0' + digit);
            pin.message = TEXT_COUNT;
        }
        return true;
    }
    switch (ev->key) {
        case UI_KEY_LEFT:
            if (pin.length > 0) {
                pin.digits[--pin.length] = '\0';
            }
            return true;
        case UI_KEY_GREEN:
            take();
            return true;
        case UI_KEY_RED:
            if (pin.mode == PIN_SCREEN_BOOT) {
                clear_digits(); // nowhere to go back to
            } else {
                pin_close();
                return false;
            }
            return true;
        default:
            return false; // the PTT and the other keys do nothing here
    }
}

static void draw(void)
{
    char dots[(2 * SECURITY_PIN_MAX) + 1];
    char buffer[40];
    text_id_t label = (pin.step == STEP_NEW) ? TEXT_UI_NEW_PIN
                                             : ((pin.step == STEP_CONFIRM) ? TEXT_UI_CONFIRM_PIN : TEXT_UI_ENTER_PIN);
    theme_colour_t colour = THEME_COLOUR_TEXT_LABEL;
    const char *hint = text(TEXT_UI_PIN_DIGITS);

    draw_text(WIDGET_MARGIN, LABEL_Y, text(label), &font_roboto17_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);
    for (uint8_t i = 0; i < pin.length; i++) {
        dots[2 * i] = '*';
        dots[(2 * i) + 1] = ' ';
    }
    dots[(pin.length > 0) ? ((2 * pin.length) - 1) : 0] = '\0';
    draw_text(WIDGET_MARGIN, DIGITS_Y, dots, &font_roboto32_bold, FONT_ALIGN_LEFT, THEME_COLOUR_TEXT);

    if ((pin.step == STEP_ENTER) && waiting()) {
        snprintf(buffer, sizeof(buffer), text(TEXT_UI_PIN_WAIT),
                 (unsigned)(((wait_until_ms - systime_get_ms()) + 999) / 1000));
        hint = buffer;
        colour = THEME_COLOUR_WARNING;
    } else if (pin.message != TEXT_COUNT) {
        hint = text(pin.message);
        colour = THEME_COLOUR_WARNING;
    }
    draw_text(WIDGET_MARGIN, HINT_Y, hint, &font_roboto17, FONT_ALIGN_LEFT, colour);
    bottom_bar_draw_soft_keys(text(TEXT_UI_OK), (pin.mode == PIN_SCREEN_BOOT) ? NULL : text(TEXT_UI_BACK));
}

// The power-on PIN: the receiver off; else the channel of the screen under it (the lock: still receiving)
static const codeplug_channel_t *channel(void)
{
    return (pin.mode == PIN_SCREEN_BOOT) ? NULL : UI_CHANNEL_UNDER;
}

const ui_screen_t pin_screen = {
    .enter = enter,
    .handle = handle,
    .draw = draw,
    .channel = channel,
};
