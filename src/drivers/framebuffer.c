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

#include <stdbool.h>
#include <string.h>
#include "hw.h"
#include "hal.h"
#include "framebuffer.h"
#include "panel.h"
#include "util.h"

static uint8_t frame_buffer_data[FB_BYTES] __attribute__((aligned(4))); // read 4 bytes at a time by segHash()
static uint8_t *frame_buffer = frame_buffer_data;

// Rows drawn into since they were last flushed. lcdFlushRows() compares only those with what the LCD shows, so the screens
// and widgets can draw whatever they like whenever they like: unchanged pixels are never sent, and a flush with nothing
// drawn costs nothing. Code writing the buffer directly calls fb_mark_rows_drawn().
static uint8_t row_drawn[(FB_HEIGHT + 7) / 8];

static inline void mark_row_drawn(int32_t y)
{
    row_drawn[y >> 3] |= (1 << (y & 7));
}

void fb_mark_rows_drawn(int32_t y0, int32_t y1)
{
    y0 = CLAMP(y0, 0, FB_HEIGHT);
    y1 = CLAMP(y1, 0, FB_HEIGHT);

    for (int32_t y = y0; y < y1; y++) {
        mark_row_drawn(y);
    }
}

// Palette entries in the native colour format. Normally fixed (fb_set_palette(), the colours of the design); until it is set,
// colours are added as they are used, starting again from the background colour when the whole buffer is cleared.
// A colour that isn't in the palette gets the nearest one.
static uint16_t palette[16];
static uint8_t palette_count = 0;
static bool palette_fixed = false;
static uint16_t last_colour = 0;
static uint8_t last_index = 0xFF;

static int32_t colour_distance(uint16_t a, uint16_t b)
{
    a = __builtin_bswap16(a);
    b = __builtin_bswap16(b);
    int32_t d1 = (int32_t)((a >> 11) & 0x1F) - (int32_t)((b >> 11) & 0x1F);
    int32_t d2 = (int32_t)((a >> 5) & 0x3F) - (int32_t)((b >> 5) & 0x3F);
    int32_t d3 = (int32_t)(a & 0x1F) - (int32_t)(b & 0x1F);

    return (d1 * d1 * 4) + (d2 * d2) + (d3 * d3 * 4);
}

static uint8_t RAM_CODE palette_index(uint16_t colour)
{
    if ((last_index != 0xFF) && (colour == last_colour)) {
        return last_index;
    }

    last_colour = colour;
    last_index = 0xFF;

    for (int i = 0; i < palette_count; i++) {
        if (palette[i] == colour) {
            last_index = i;
            return i;
        }
    }

    if (palette_count < 16) {
        palette[palette_count] = colour;
        last_index = palette_count;
        return palette_count++;
    }

    uint8_t best = 0;
    int32_t best_distance = INT32_MAX;

    for (int i = 0; i < 16; i++) {
        int32_t d = colour_distance(palette[i], colour);

        if (d < best_distance) {
            best_distance = d;
            best = i;
        }
    }

    last_index = best;
    return best;
}

// alpha 0 to FB_ALPHA_OPAQUE of fg over bg
static uint16_t blend(uint16_t fg, uint16_t bg, int32_t alpha)
{
    fg = __builtin_bswap16(fg);
    bg = __builtin_bswap16(bg);
    int32_t inv = FB_ALPHA_OPAQUE - alpha;
    uint16_t r = ((((fg >> 11) & 0x1F) * alpha) + (((bg >> 11) & 0x1F) * inv)) / FB_ALPHA_OPAQUE;
    uint16_t g = ((((fg >> 5) & 0x3F) * alpha) + (((bg >> 5) & 0x3F) * inv)) / FB_ALPHA_OPAQUE;
    uint16_t b = (((fg & 0x1F) * alpha) + ((bg & 0x1F) * inv)) / FB_ALPHA_OPAQUE;

    return __builtin_bswap16((r << 11) | (g << 5) | b);
}

static inline uint8_t get_index(int32_t x, int32_t y)
{
    uint8_t v = frame_buffer[((y * FB_WIDTH) + x) >> 1];

    return (x & 1) ? (v & 0x0F) : (v >> 4);
}

static inline void set_index(int32_t x, int32_t y, uint8_t index)
{
    uint8_t *p = &frame_buffer[((y * FB_WIDTH) + x) >> 1];

    *p = (x & 1) ? ((*p & 0xF0) | index) : ((*p & 0x0F) | (index << 4));
}

uint8_t *fb_get_buffer(void)
{
    return frame_buffer;
}

uint8_t *fb_get_primary_buffer(void)
{
    return frame_buffer_data;
}

void fb_set_buffer(uint8_t *buffer)
{
    frame_buffer = (buffer ? buffer : frame_buffer_data);
    memset(row_drawn, 0xFF, sizeof(row_drawn)); // another buffer: any row can differ
}

// Anti-aliasing computed once per text colour: the palette entry of colour fg over colour bg at each of the 16 levels
// of the fonts (0: bg, 15: fg). Text is then drawn with one lookup per pixel, instead of blending the colours and
// searching the palette for the nearest one at every pixel (5 ms to redraw the status bar and the S meter).
static uint8_t blend_table[16][16][16]; // [fg][bg][level]
static uint16_t blend_table_ready = 0;  // bit per fg

static const uint8_t *blend_table_for(uint8_t fg)
{
    if ((blend_table_ready & (1 << fg)) == 0) {
        for (int bg = 0; bg < 16; bg++) {
            for (int level = 0; level < 16; level++) {
                blend_table[fg][bg][level] =
                    ((level == 0)
                         ? bg
                         : ((level == 15)
                                ? fg
                                : palette_index(blend(palette[fg], palette[bg], (level * FB_ALPHA_OPAQUE) / 15))));
            }
        }
        blend_table_ready |= (1 << fg);
    }

    return &blend_table[fg][0][0];
}

void fb_set_palette(const uint16_t colours[16])
{
    memcpy(palette, colours, sizeof(palette));
    palette_count = 16;
    palette_fixed = true;
    last_index = 0xFF;
    blend_table_ready = 0;
}

void RAM_CODE fb_clear(uint16_t colour)
{
    if (!palette_fixed) {
        palette[0] = colour;
        palette_count = 1;
        last_index = 0xFF;
    }

    uint8_t index = palette_index(colour);

    memset(frame_buffer, (index << 4) | index, FB_BYTES);
    memset(row_drawn, 0xFF, sizeof(row_drawn));
}

void RAM_CODE fb_fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t colour)
{
    x0 = CLAMP(x0, 0, FB_WIDTH);
    x1 = CLAMP(x1, 0, FB_WIDTH);
    y0 = CLAMP(y0, 0, FB_HEIGHT);
    y1 = CLAMP(y1, 0, FB_HEIGHT);

    if ((x0 >= x1) || (y0 >= y1)) {
        return;
    }

    uint8_t index = palette_index(colour);
    uint8_t both = (index << 4) | index;

    for (int32_t y = y0; y < y1; y++) {
        int32_t x = x0;
        int32_t end = x1;

        mark_row_drawn(y);
        if (x & 1) {
            set_index(x++, y, index);
        }
        if (end & 1) {
            set_index(--end, y, index);
        }
        if (x < end) {
            memset(&frame_buffer[((y * FB_WIDTH) + x) >> 1], both, (end - x) >> 1);
        }
    }
}

void RAM_CODE fb_set_pixel(int32_t x, int32_t y, uint16_t colour)
{
    if ((x >= 0) && (y >= 0) && (x < FB_WIDTH) && (y < FB_HEIGHT)) {
        mark_row_drawn(y);
        set_index(x, y, palette_index(colour));
    }
}

void RAM_CODE fb_blend_pixel(int32_t x, int32_t y, uint16_t colour, int32_t alpha)
{
    if ((alpha <= 0) || (x < 0) || (y < 0) || (x >= FB_WIDTH) || (y >= FB_HEIGHT)) {
        return;
    }

    if (alpha < FB_ALPHA_OPAQUE) {
        colour = blend(colour, palette[get_index(x, y)], alpha);
    }

    mark_row_drawn(y);
    set_index(x, y, palette_index(colour));
}

// Levels 1..15 of colour over what is there, 0 leaves the pixel. Fonts: 2 pixels a byte (high nibble first), rows of
// (width + 1) / 2 bytes.
void RAM_CODE fb_draw_levels(int32_t x0, int32_t y0, const uint8_t *levels, int32_t width, int32_t height,
                             uint16_t colour)
{
    int32_t bytes_per_row = (width + 1) >> 1;

    if (!palette_fixed) {
        for (int32_t gy = 0; gy < height; gy++) {
            for (int32_t gx = 0; gx < width; gx++) {
                uint8_t level = (gx & 1) ? (levels[(gy * bytes_per_row) + (gx >> 1)] & 0x0F)
                                         : (levels[(gy * bytes_per_row) + (gx >> 1)] >> 4);

                fb_blend_pixel(x0 + gx, y0 + gy, colour, (level * FB_ALPHA_OPAQUE) / 15);
            }
        }
        return;
    }

    const uint8_t *table = blend_table_for(palette_index(colour));

    for (int32_t gy = 0; gy < height; gy++, levels += bytes_per_row) {
        int32_t y = y0 + gy;

        if ((y < 0) || (y >= FB_HEIGHT)) {
            continue;
        }

        mark_row_drawn(y);
        for (int32_t gx = 0; gx < width; gx++) {
            int32_t x = x0 + gx;
            uint8_t level = (gx & 1) ? (levels[gx >> 1] & 0x0F) : (levels[gx >> 1] >> 4);

            if ((level != 0) && (x >= 0) && (x < FB_WIDTH)) {
                set_index(x, y, table[(get_index(x, y) << 4) | level]);
            }
        }
    }
}

void RAM_CODE fb_draw_level(int32_t x, int32_t y, uint16_t colour, uint8_t level)
{
    if ((level == 0) || (x < 0) || (y < 0) || (x >= FB_WIDTH) || (y >= FB_HEIGHT)) {
        return;
    }

    if (!palette_fixed) {
        fb_blend_pixel(x, y, colour, (level * FB_ALPHA_OPAQUE) / 15);
        return;
    }

    mark_row_drawn(y);
    set_index(x, y, blend_table_for(palette_index(colour))[(get_index(x, y) << 4) | level]);
}

// What the LCD shows, to send only the rectangle that changed, as the stock firmware writes only what changes (the
// screens redraw everything for a digit). A copy of the buffer (38 KB) doesn't fit in RAM: a hash of each 32 pixels
// (16 bytes) of each row as it was sent, and the palette it was sent with.
#define SEG_BYTES 16
#define ROW_SEGS  (((FB_WIDTH >> 1) + SEG_BYTES - 1) / SEG_BYTES)
static uint32_t sent_seg_hash[FB_HEIGHT][ROW_SEGS];
static uint16_t sent_palette[16];

// Hash of a segment, 4 bytes at a time (segments and rows are multiples of 4 bytes, the buffer is aligned)
static uint32_t RAM_CODE seg_hash(const uint8_t *p, int32_t len)
{
    const uint32_t *w = (const uint32_t *)p;
    uint32_t h = 2166136261u;

    for (len >>= 2; len > 0; len--) {
        h = (h ^ *w++) * 16777619u;
        h ^= (h >> 15);
    }

    return h;
}
static uint8_t row_sent_valid[(FB_HEIGHT + 7) / 8]; // 0 at start: the first flush sends everything

static void RAM_CODE invalidate_rows(int32_t y0, int32_t y1)
{
    for (int32_t y = y0; y < y1; y++) {
        row_sent_valid[y >> 3] &= ~(1 << (y & 7));
    }
}

// Pixels [b0 * 2, b1 * 2) x [y0, y1) (2 pixels a byte)
static void RAM_CODE send_rect(int32_t b0, int32_t b1, int32_t y0, int32_t y1)
{
    int32_t x0 = b0 * 2;
    int32_t x1 = b1 * 2;
    uint8_t opts_x[] = {(x0 >> 8), (x0 & 0xFF), ((x1 - 1) >> 8), ((x1 - 1) & 0xFF)};
    uint8_t opts_y[] = {(y0 >> 8), (y0 & 0xFF), ((y1 - 1) >> 8), ((y1 - 1) & 0xFF)};

    panel_write_cmd_data(PANEL_CMD_CASET, opts_x, sizeof(opts_x));
    panel_write_cmd_data(PANEL_CMD_RASET, opts_y, sizeof(opts_y));
    panel_write_cmd(PANEL_CMD_RAMWR);

    for (int32_t y = y0; y < y1; y++) {
        const uint8_t *p = &frame_buffer[((y * FB_WIDTH) >> 1) + b0];
        const uint8_t *end = p + (b1 - b0);

        while (p < end) {
            uint16_t c1 = palette[*p >> 4];
            uint16_t c2 = palette[*p & 0x0F];

            panel_write_data(c1 & 0xFF);
            panel_write_data(c1 >> 8);
            panel_write_data(c2 & 0xFF);
            panel_write_data(c2 >> 8);
            p++;
        }
    }
}

static void RAM_CODE flush_rows(int32_t y0, int32_t y1)
{
    const int32_t row_bytes = (FB_WIDTH >> 1);

    y0 = CLAMP(y0, 0, FB_HEIGHT);
    y1 = CLAMP(y1, 0, FB_HEIGHT);

    // Another palette: the colours of every row on the display are wrong
    if (memcmp(sent_palette, palette, sizeof(palette)) != 0) {
        memcpy(sent_palette, palette, sizeof(palette));
        invalidate_rows(0, FB_HEIGHT);
    }

    // Runs of changed rows, each sent as the rectangle around their changed bytes
    int32_t run_start = -1;
    int32_t run_b0 = row_bytes;
    int32_t run_b1 = 0;

    for (int32_t y = y0; y <= y1; y++) {
        int32_t b0 = row_bytes;
        int32_t b1 = 0;

        if ((y < y1) &&
            ((((row_drawn[y >> 3] >> (y & 7)) & 1) != 0) || (((row_sent_valid[y >> 3] >> (y & 7)) & 1) == 0))) {
            const uint8_t *row = &frame_buffer[y * row_bytes];
            bool valid = (((row_sent_valid[y >> 3] >> (y & 7)) & 1) != 0);

            row_drawn[y >> 3] &= ~(1 << (y & 7));
            for (int32_t seg = 0; seg < ROW_SEGS; seg++) {
                int32_t s0 = seg * SEG_BYTES;
                int32_t len = MIN(SEG_BYTES, row_bytes - s0);
                uint32_t h = seg_hash(&row[s0], len);

                if (!valid || (h != sent_seg_hash[y][seg])) {
                    sent_seg_hash[y][seg] = h;
                    b0 = MIN(b0, s0);
                    b1 = MAX(b1, s0 + len);
                }
            }

            row_sent_valid[y >> 3] |= (1 << (y & 7));
        }

        if (b0 < b1) {
            if (run_start < 0) {
                run_start = y;
            }
            run_b0 = MIN(run_b0, b0);
            run_b1 = MAX(run_b1, b1);
        } else if (run_start >= 0) {
            send_rect(run_b0, run_b1, run_start, y);
            run_start = -1;
            run_b0 = row_bytes;
            run_b1 = 0;
        }
    }
}

// Sends what changed in the rows drawn since the last flush
// The bus is taken: the keypad scan uses LCD_DB0 - DB4 (keys.c)
void RAM_CODE fb_flush(void)
{
    panel_bus_take();
    flush_rows(0, FB_HEIGHT);
    panel_bus_give();
}

// A rectangle in a colour that isn't in the palette (theme colour swatches), sent straight to the display: it stays until
// those rows are flushed again
void fb_fill_direct(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t colour)
{
    x0 = CLAMP(x0, 0, FB_WIDTH);
    x1 = CLAMP(x1, 0, FB_WIDTH);
    y0 = CLAMP(y0, 0, FB_HEIGHT);
    y1 = CLAMP(y1, 0, FB_HEIGHT);

    if ((x0 >= x1) || (y0 >= y1)) {
        return;
    }

    uint8_t opts_x[] = {(x0 >> 8), (x0 & 0xFF), ((x1 - 1) >> 8), ((x1 - 1) & 0xFF)};
    uint8_t opts_y[] = {(y0 >> 8), (y0 & 0xFF), ((y1 - 1) >> 8), ((y1 - 1) & 0xFF)};
    int32_t count = (x1 - x0) * (y1 - y0);

    panel_write_cmd_data(PANEL_CMD_CASET, opts_x, sizeof(opts_x));
    panel_write_cmd_data(PANEL_CMD_RASET, opts_y, sizeof(opts_y));
    panel_write_cmd(PANEL_CMD_RAMWR);

    while (count-- > 0) {
        panel_write_data(colour & 0xFF);
        panel_write_data(colour >> 8);
    }

    invalidate_rows(y0, y1); // the display no longer shows the buffer there
}
