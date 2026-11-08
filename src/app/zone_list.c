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
 * OpenDMR, screen: the zone list (menuZoneList.c). The zones of the codeplug and "all channels", from the
 * current one; the left key makes the zone current (the first channel of its list, no talkgroup override) and goes
 * back to the channel screen.
 */

#include <stdio.h>
#include <string.h>
#include "codeplug_text.h"
#include "settings.h"
#include "codeplug.h"
#include "menus.h"

static int zone_count(void)
{
    return codeplug_zone_count();
}

static int zone_current(void)
{
    return settings_valid() ? settings.current_zone : 0;
}

static void zone_name(int index, char *buffer, size_t size)
{
    static codeplug_zone_t zone; // 176 bytes: not on the UI task stack

    if (!codeplug_get_zone(index, &zone)) {
        snprintf(buffer, size, "--");
    } else if (zone.index < 0) {
        snprintf(buffer, size, "%s", text(TEXT_ALL_CHANNELS));
    } else {
        codeplug_text_name(zone.name, CODEPLUG_NAME_LENGTH, buffer, size);
    }
}

static void zone_pick(int index)
{
    settings.current_zone = (int16_t)index;
    settings.override_tg = 0;
    settings.current_index_in_trx_group_list[0] = 0;
    settings_changed();
    settings_save_if_changed();
    ui_home();
}

const ui_pick_list_t zone_list = {
    .title = TEXT_ZONES,
    .icon = ICONS_ZONE,
    .count = zone_count,
    .current = zone_current,
    .name = zone_name,
    .pick = zone_pick,
};
