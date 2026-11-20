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
 * OpenDMR, screen: boot splash screen (boot_screen.c).
 * Shown at power on when the "Boot screen" radio option (SETTINGS_BIT_BOOT_SCREEN) is enabled and a custom
 * 240x320 4-bpp image is stored in custom_data (CUSTOM_DATA_TYPE_BOOT_IMAGE). Without one, boots straight to
 * the home screen.
 */

#include <stddef.h>
#include "framebuffer.h"
#include "flash.h"
#include "custom_data.h"
#include "settings.h"
#include "theme.h"
#include "display.h"
#include "ui.h"
#include "boot_screen.h"

#define BOOT_SPLASH_DISPLAY_MS 1800
#define BOOT_PALETTE_COLORS    16
#define BOOT_PALETTE_BYTES     (BOOT_PALETTE_COLORS * sizeof(uint16_t))
#define BOOT_IMAGE_BLOCK_BYTES (BOOT_PALETTE_BYTES + FB_BYTES)

static bool has_custom_image(uint32_t *out_address)
{
    uint32_t length = 0;
    uint32_t address = custom_data_find(CUSTOM_DATA_TYPE_BOOT_IMAGE, &length);
    uint16_t head[2];

    if ((address == 0) || (length != BOOT_IMAGE_BLOCK_BYTES) ||
        !flash_read(address, (uint8_t *)head, sizeof(head)) ||
        ((head[0] == 0xFFFF) && (head[1] == 0xFFFF))) {
        return false;
    }
    if (out_address) {
        *out_address = address;
    }
    return true;
}

static void enter(void *arg)
{
    (void)arg;
    ui_timer_start(BOOT_SPLASH_DISPLAY_MS);
}

static void leave(void)
{
    ui_timer_stop();
    theme_set(theme_current());
    display_theme_apply();
}

static bool handle(const ui_event_t *ev)
{
    if ((ev->type == UI_EVENT_TIMER) || (ev->type == UI_EVENT_KEY_PRESS)) {
        ui_pop();
        return true;
    }
    return false;
}

static void draw(void)
{
    uint32_t address = 0;
    uint16_t custom_palette[BOOT_PALETTE_COLORS];
    uint8_t *fb = fb_get_buffer();

    // Custom boot image format: 32 bytes palette (16x uint16_t) + 38400 bytes 4-bpp pixel data
    if (has_custom_image(&address) &&
        flash_read(address, (uint8_t *)custom_palette, sizeof(custom_palette)) &&
        flash_read(address + sizeof(custom_palette), fb, FB_BYTES)) {
        fb_set_palette(custom_palette);
        fb_mark_rows_drawn(0, FB_HEIGHT);
    }
}

const ui_screen_t boot_screen = {
    .enter = enter,
    .leave = leave,
    .handle = handle,
    .draw = draw,
    .channel = NULL,
};

const ui_screen_t *boot_screen_start(void)
{
    if (settings_valid() && (settings.bitfield_options & SETTINGS_BIT_BOOT_SCREEN) &&
        has_custom_image(NULL)) {
        return &boot_screen;
    }
    return NULL;
}
