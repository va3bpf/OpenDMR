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
 * OpenDMR, graphics: the font files in the data flash (docs/i18n.en.md). From firmware/dh4570_IAP/user/SOURCE/
 * graphics/gfxFontExt.c; the files are made by tools/fonts/mkfont_pack.py and written by the CPS, with a directory
 * sector before them.
 *
 * RAM is short: only the directory and the font tables are kept, the glyphs are looked up (binary search in the
 * sorted codes) and decoded straight from the flash; a few lookups are cached, as the same characters come back on
 * each redraw.
 */

#include <string.h>
#include "flash.h"
#include "framebuffer.h"
#include "font_cjk.h"

#define DIRECTORY_MAGIC "DM32FDIR"
#define FILE_MAGIC      "DM32FNT1"
#define FILES_MAX       16
#define FONTS_MAX       32
#define FILE_HEADER     64
#define NAME_SIZE       8
#define RECORD_SIZE     8
#define LARGE_LINE      30 // built-in fonts with lines at least this high use the large size
#define CACHE_SIZE      16

typedef struct {
    uint32_t codes; // flash addresses
    uint32_t records;
    uint32_t base;
    uint16_t count;
    uint8_t size;
} cjk_font_t;

typedef struct {
    char name[NAME_SIZE + 1];
    uint32_t gb2312;     // flash address of its GB2312 table, 0: none
    uint8_t first_font;  // its fonts in fonts[]
    uint8_t font_count;
} font_file_t;

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
static font_file_t files[FILES_MAX];
static uint8_t file_count;
static cjk_font_t fonts[FONTS_MAX];
static uint8_t font_count;
static char preferred_name[NAME_SIZE + 1];
static int preferred = -1; // the file of preferred_name
static cache_entry_t cache[CACHE_SIZE];

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void find_preferred(void)
{
    preferred = -1;
    for (int i = 0; (i < file_count) && (preferred_name[0] != '\0'); i++) {
        if (strcmp(files[i].name, preferred_name) == 0) {
            preferred = i;
        }
    }
}

// One file of the directory: its header must agree with the entry; its fonts go to fonts[]
static void read_file(const uint8_t *entry)
{
    font_file_t *f = &files[file_count];
    uint32_t address = get_le32(&entry[8]);
    uint32_t length = get_le32(&entry[12]);
    uint8_t header[FILE_HEADER];
    uint32_t table, count;

    if ((address < FONT_FLASH_ADDRESS) || (address >= FONT_FLASH_END) || (length < FILE_HEADER) ||
        (length > (FONT_FLASH_END - address)) || !flash_read_raw(address, header, sizeof(header)) ||
        (memcmp(header, FILE_MAGIC, 8) != 0) || (get_le32(&header[8]) != length) ||
        (memcmp(&header[16], entry, NAME_SIZE) != 0)) {
        return;
    }
    memcpy(f->name, entry, NAME_SIZE);
    f->name[NAME_SIZE] = '\0';
    f->gb2312 = get_le32(&header[36]);
    f->gb2312 = ((f->gb2312 != 0) && (f->gb2312 < length)) ? (address + f->gb2312) : 0;
    f->first_font = font_count;
    f->font_count = 0;
    count = header[32];
    table = get_le32(&header[40]);
    if ((table + (count * 8)) > length) {
        return;
    }
    for (uint32_t i = 0; (i < count) && (font_count < FONTS_MAX); i++) {
        uint8_t e[8];
        cjk_font_t *font = &fonts[font_count];
        uint32_t offset;

        if (!flash_read_raw(address + table + (i * 8), e, sizeof(e))) {
            return;
        }
        offset = get_le32(e);
        font->count = e[4] | (e[5] << 8);
        font->size = e[6];
        if ((offset >= length) || (font->count == 0)) {
            continue;
        }
        font->base = address + offset;
        font->codes = font->base;
        font->records = font->base + (((font->count * 2) + 3) & ~3);
        font_count++;
        f->font_count++;
    }
    file_count++;
}

static bool read_directory(void)
{
    if (state == NOT_READ) {
        uint8_t header[12];

        state = ABSENT;
        file_count = 0;
        font_count = 0;
        memset(cache, 0xFF, sizeof(cache));

        if (flash_read_raw(FONT_DIRECTORY_ADDRESS, header, sizeof(header)) &&
            (memcmp(header, DIRECTORY_MAGIC, 8) == 0)) {
            uint8_t count = (header[8] > FILES_MAX) ? FILES_MAX : header[8];

            for (uint8_t i = 0; i < count; i++) {
                uint8_t entry[20];

                if (flash_read_raw(FONT_DIRECTORY_ADDRESS + 12 + (i * sizeof(entry)), entry, sizeof(entry))) {
                    read_file(entry);
                }
            }
            state = (font_count > 0) ? PRESENT : ABSENT;
        }
        find_preferred();
    }

    return (state == PRESENT);
}

void font_cjk_reload(void)
{
    state = NOT_READ;
}

bool font_cjk_has(const char *name)
{
    read_directory();
    for (int i = 0; i < file_count; i++) {
        if (strcmp(files[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

void font_cjk_prefer(const char *name)
{
    strncpy(preferred_name, name, NAME_SIZE);
    preferred_name[NAME_SIZE] = '\0';
    if (state != NOT_READ) {
        find_preferred();
    }
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

// The glyph in one file: the size that suits the line first (the largest that fits a large line, the smallest for
// the others), then its other sizes
static bool file_glyph(const font_file_t *f, uint16_t code, uint8_t line_height, font_cjk_glyph_t *glyph)
{
    int best = -1;

    for (int i = f->first_font; i < (f->first_font + f->font_count); i++) {
        if (best < 0) {
            best = i;
        } else if (line_height >= LARGE_LINE) {
            if (((fonts[i].size + 6) <= line_height) &&
                (((fonts[best].size + 6) > line_height) || (fonts[i].size > fonts[best].size))) {
                best = i;
            }
        } else if (fonts[i].size < fonts[best].size) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    if (lookup((uint8_t)best, code, glyph)) {
        return true;
    }
    for (int i = f->first_font; i < (f->first_font + f->font_count); i++) {
        if ((i != best) && lookup((uint8_t)i, code, glyph)) {
            return true;
        }
    }
    return false;
}

bool font_cjk_get_glyph(uint32_t code, uint8_t line_height, font_cjk_glyph_t *glyph)
{
    if ((code > 0xFFFF) || !read_directory()) {
        return false;
    }

    // The current language's font first: Chinese and Japanese draw the same Han code points differently
    if ((preferred >= 0) && file_glyph(&files[preferred], (uint16_t)code, line_height, glyph)) {
        return true;
    }
    for (int i = 0; i < file_count; i++) {
        if ((i != preferred) && file_glyph(&files[i], (uint16_t)code, line_height, glyph)) {
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
    uint32_t table = 0;

    if ((high < 0xA1) || (high > 0xFE) || (low < 0xA1) || (low > 0xFE)) {
        return 0;
    }
    read_directory();
    for (int i = 0; (i < file_count) && (table == 0); i++) {
        table = files[i].gb2312;
    }
    if ((table == 0) || !flash_read_raw(table + (((((high - 0xA1) * 94) + (low - 0xA1))) * 2), c, 2)) {
        return 0;
    }

    return c[0] | (c[1] << 8);
}
