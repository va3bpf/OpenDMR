/*
 * Copyright (C) 2026      OpenDM32 contributors
 * Copyright (C) 2026      Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, graphics: the Chinese font in the data flash. From firmware/dh4570_IAP/user/SOURCE/graphics/
 * gfxFontExt.c; the file is made by tools/fonts/mkcjk.py and written by tools/fonts/upload_cjk.py.
 *
 * RAM is short: only the header is kept, the glyphs are looked up (binary search in the sorted codes) and decoded
 * straight from the flash; a few lookups are cached, as the same characters come back on each redraw.
 */

#include <string.h>
#include "flash.h"
#include "framebuffer.h"
#include "font_cjk.h"

#define MAGIC       "DM32CJK2"
#define FONTS_MAX   4
#define RECORD_SIZE 8
#define LARGE_LINE  30 // built-in fonts with lines at least this high use the large font
#define CACHE_SIZE  16

typedef struct {
    uint32_t codes; // flash addresses
    uint32_t records;
    uint32_t base;
    uint16_t count;
    uint8_t size;
} cjk_font_t;

typedef struct {
    uint16_t code;
    uint8_t font; // 0xFF: free
    bool found;
    font_cjk_glyph_t glyph;
} cache_entry_t;

static enum {
    NOT_READ = 0,
    PRESENT,
    ABSENT
} state = NOT_READ;
static cjk_font_t fonts[FONTS_MAX];
static uint8_t font_count;
static uint32_t gb2312_table;
static cache_entry_t cache[CACHE_SIZE];

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_header(void)
{
    if (state == NOT_READ) {
        uint8_t header[64];

        state = ABSENT;
        memset(cache, 0xFF, sizeof(cache));

        if (flash_read_raw(FONT_CJK_FLASH_ADDRESS, header, sizeof(header)) && (memcmp(header, MAGIC, 8) == 0) &&
            (get_le32(&header[8]) <= (FONT_CJK_FLASH_END - FONT_CJK_FLASH_ADDRESS))) {
            font_count = ((header[16] > FONTS_MAX) ? FONTS_MAX : header[16]);
            for (int i = 0; i < font_count; i++) {
                const uint8_t *e = &header[20 + (i * 8)];
                cjk_font_t *f = &fonts[i];

                f->base = FONT_CJK_FLASH_ADDRESS + get_le32(e);
                f->count = e[4] | (e[5] << 8);
                f->size = e[6];
                f->codes = f->base;
                f->records = f->base + (((f->count * 2) + 3) & ~3);
            }
            gb2312_table = get_le32(&header[52]);
            if (gb2312_table) {
                gb2312_table += FONT_CJK_FLASH_ADDRESS;
            }
            state = PRESENT;
        }
    }

    return (state == PRESENT);
}

static bool find(const cjk_font_t *f, uint16_t code, font_cjk_glyph_t *glyph)
{
    int32_t low = 0;
    int32_t high = f->count - 1;

    while (low <= high) {
        int32_t mid = (low + high) / 2;
        uint8_t c[2];

        flash_read_raw(f->codes + (mid * 2), c, 2);
        uint16_t mid_code = c[0] | (c[1] << 8);

        if (mid_code == code) {
            uint8_t r[RECORD_SIZE];

            flash_read_raw(f->records + (mid * RECORD_SIZE), r, RECORD_SIZE);
            glyph->bitmap = f->base + (r[0] | (r[1] << 8) | (r[2] << 16));
            glyph->width = r[3];
            glyph->height = r[4];
            glyph->x_offset = (int8_t)r[5];
            glyph->top = (int8_t)r[6];
            glyph->advance = r[7];
            return true;
        }

        if (mid_code < code) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return false;
}

// One font, through the cache
static bool lookup(uint8_t font, uint16_t code, font_cjk_glyph_t *glyph)
{
    cache_entry_t *entry = &cache[(code ^ (font * 5)) % CACHE_SIZE];

    if ((entry->font != font) || (entry->code != code)) {
        entry->code = code;
        entry->font = font;
        entry->found = find(&fonts[font], code, &entry->glyph);
    }

    if (entry->found) {
        *glyph = entry->glyph;
    }

    return entry->found;
}

bool font_cjk_get_glyph(uint32_t code, uint8_t line_height, font_cjk_glyph_t *glyph)
{
    int preferred = 0;

    if ((code > 0xFFFF) || !read_header() || (font_count == 0)) {
        return false;
    }

    // The fonts are in increasing size: the largest one that suits the line, then the others from the smallest
    for (int i = 0; i < font_count; i++) {
        if ((fonts[i].size + 6) <= line_height) {
            preferred = i;
        }
    }
    if ((line_height < LARGE_LINE) && (preferred > 0)) {
        preferred = 0;
    }

    if (lookup(preferred, code, glyph)) {
        return true;
    }
    for (int i = 0; i < font_count; i++) {
        if ((i != preferred) && lookup(i, code, glyph)) {
            return true;
        }
    }

    return false;
}

// The bitmap, read from the flash 32 bytes at a time
typedef struct {
    uint8_t buffer[32];
    uint32_t address;
    uint32_t position;
} bitmap_reader_t;

static uint8_t next_byte(bitmap_reader_t *reader)
{
    if (reader->position >= sizeof(reader->buffer)) {
        flash_read_raw(reader->address, reader->buffer, sizeof(reader->buffer));
        reader->address += sizeof(reader->buffer);
        reader->position = 0;
    }
    return reader->buffer[reader->position++];
}

void font_cjk_draw_glyph(int32_t x, int32_t baseline, const font_cjk_glyph_t *glyph, uint16_t colour)
{
    bitmap_reader_t reader = {.address = glyph->bitmap, .position = sizeof(reader.buffer)};
    int32_t total = glyph->width * glyph->height;
    int32_t pixel = 0;
    int32_t x0 = x + glyph->x_offset;
    int32_t y0 = baseline + glyph->top;

    while (pixel < total) {
        uint8_t token = next_byte(&reader);

        if (token & 0x80) {
            // pixels with their own levels, 2 a byte
            int32_t n = (token & 0x7F) + 1;
            uint8_t packed = 0;

            for (int32_t i = 0; (i < n) && (pixel < total); i++, pixel++) {
                uint8_t level;

                if ((i & 1) == 0) {
                    packed = next_byte(&reader);
                    level = packed >> 4;
                } else {
                    level = packed & 0x0F;
                }
                fb_draw_level(x0 + (pixel % glyph->width), y0 + (pixel / glyph->width), colour, level);
            }
        } else {
            // a run of empty or full pixels
            int32_t n = (token & 0x3F) + 1;

            if (token & 0x40) {
                for (int32_t i = 0; (i < n) && (pixel < total); i++, pixel++) {
                    fb_draw_level(x0 + (pixel % glyph->width), y0 + (pixel / glyph->width), colour, 15);
                }
            } else {
                pixel += n;
            }
        }
    }
}

uint16_t font_cjk_gb2312_to_unicode(uint8_t high, uint8_t low)
{
    uint8_t c[2];

    if ((high < 0xA1) || (high > 0xFE) || (low < 0xA1) || (low > 0xFE) || !read_header() || (gb2312_table == 0)) {
        return 0;
    }

    flash_read_raw(gb2312_table + (((((high - 0xA1) * 94) + (low - 0xA1))) * 2), c, 2);

    return c[0] | (c[1] << 8);
}
