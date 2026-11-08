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

#ifndef DM32_MESSAGE_BOX_H
#define DM32_MESSAGE_BOX_H

#include <stdbool.h>
#include <stdint.h>
#include "theme.h"
#include "ui.h"

// Message box and notification (uiMessageBox.c, uiNotification.c): a page with a title and one or two lines, answered
// with the keys or closed by itself. ui_push(&message_box_screen, (void *)&a_box); the box must stay valid while it
// shows (static). Results ("Saved", "Deleted") are MESSAGE_BOX_OK with close_ms, questions MESSAGE_BOX_YES_NO.
typedef enum {
    MESSAGE_BOX_OK,     // the left key or a short press of the right key closes it (yes)
    MESSAGE_BOX_YES_NO, // the left key: yes, the right key: no
    MESSAGE_BOX_NONE,   // no keys (they are swallowed): closed by close_ms or by the code (ui_pop())
} message_box_buttons_t;

typedef struct {
    const char *title;
    const char *line1; // NULL: none
    const char *line2;
    theme_colour_t colour; // of the lines; 0 (THEME_COLOUR_BACKGROUND) is taken as THEME_COLOUR_TEXT
    message_box_buttons_t buttons;
    uint32_t close_ms;          // closes itself after this (answer: yes); 0: stays
    void (*answered)(bool yes); // after the box is popped (it may push another screen); NULL: none
} message_box_t;

extern const ui_screen_t message_box_screen;

#endif
