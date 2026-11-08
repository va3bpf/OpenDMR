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
 * OpenDMR, service: the last heard list. From uiUtilities.c (lastHeardListUpdate(), lastHeardInitList()):
 * there it is a linked list in the UI globals, here a table of entries and their order, behind a critical section
 * because the DMR receive will write it from its task while the UI task reads it. Kept in RAM only, as the original.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "systime.h"
#include "ui.h"
#include "last_heard.h"

static last_heard_entry_t entries[LAST_HEARD_MAX]; // 2.6 KB
static uint8_t order[LAST_HEARD_MAX];              // entry numbers, the most recent first
static int count;

static void copy_text(char *to, const char *from)
{
    strncpy(to, from, LAST_HEARD_TEXT_SIZE - 1);
    to[LAST_HEARD_TEXT_SIZE - 1] = 0;
}

static void changed(void)
{
    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_LAST_HEARD};

    ui_post(&ev);
}

// Where the caller is in the order; -1: not there
static int find(uint32_t id)
{
    for (int i = 0; i < count; i++) {
        if (entries[order[i]].id == id) {
            return i;
        }
    }
    return -1;
}

void last_heard_add(uint32_t id, uint32_t tg_or_pc, uint8_t timeslot, const char *text)
{
    taskENTER_CRITICAL();

    int at = find(id);
    uint8_t slot;

    if (at < 0) {
        // A new one: a free entry, or the oldest one
        if (count < LAST_HEARD_MAX) {
            order[count] = (uint8_t)count;
            count++;
        }
        at = count - 1;
        slot = order[at];
        memset(&entries[slot], 0, sizeof(entries[slot]));
        entries[slot].id = id;
    }
    slot = order[at];
    memmove(&order[1], &order[0], at);
    order[0] = slot;

    entries[slot].tg_or_pc = tg_or_pc;
    entries[slot].timeslot = timeslot;
    entries[slot].time_ms = systime_get_ms();
    if (text) {
        copy_text(entries[slot].text, text);
    }

    taskEXIT_CRITICAL();
    changed();
}

void last_heard_set_talker_alias(uint32_t id, const char *alias)
{
    taskENTER_CRITICAL();

    int at = find(id);

    if (at >= 0) {
        copy_text(entries[order[at]].talker_alias, alias);
    }
    taskEXIT_CRITICAL();
    if (at >= 0) {
        changed();
    }
}

void last_heard_clear(void)
{
    taskENTER_CRITICAL();
    count = 0;
    taskEXIT_CRITICAL();
    changed();
}

int last_heard_count(void)
{
    return count;
}

bool last_heard_get(int position, last_heard_entry_t *entry)
{
    bool found;

    taskENTER_CRITICAL();
    found = (position >= 0) && (position < count);
    if (found) {
        *entry = entries[order[position]];
    }
    taskEXIT_CRITICAL();
    return found;
}
