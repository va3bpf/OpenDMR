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

#ifndef DM32_WIDGETS_H
#define DM32_WIDGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui.h"
#include "icons.h"
#include "theme.h"
#include "framebuffer.h"

// Building blocks of the screens (docs/ui-design.md, 5 and 6.5). They only draw what they are given, in LCD pixels;
// the screens collect the radio data. Areas of the screen:
#define WIDGET_STATUS_BAR_HEIGHT 32 // y 0 .. 31, then a 2 px line
#define WIDGET_MAIN_TOP          34
#define WIDGET_BOTTOM_BAR_TOP    282 // 2 px line, then the bar down to 319
#define WIDGET_MARGIN            12  // left and right

// ---- Status bar ----
typedef enum {
    STATUS_BAR_IDLE = 0,
    STATUS_BAR_RECEIVING,   // green line and signal
    STATUS_BAR_TRANSMITTING // red bar
} status_bar_state_t;

typedef struct {
    status_bar_state_t state;
    int8_t signal_bars; // 0 to ICONS_SIGNAL_BARS (not shown when transmitting)
    const char *item1;  // e.g. "TS1", "DMO"; NULL: none
    const char *item2;  // e.g. "CC1"
    bool item1_bold;
    bool scanning;
} status_bar_data_t;

// On the right, from the services: the battery, and the time (the battery percentage when there is no clock)
void status_bar_draw(const status_bar_data_t *data);

// ---- Channel panel: zone, channel name, frequency, and a line with an icon (talk group, tone) ----
typedef struct {
    const char *zone; // e.g. "All Channels  12"
    const char *name;
    const char *frequency; // e.g. "438.5000 MHz"
    icons_id_t line_icon;  // ICONS_COUNT: no line
    const char *line_text;
    const char *line_detail; // after line_text, in the label colour; NULL: none
} channel_panel_data_t;

void channel_panel_draw(const channel_panel_data_t *data);

// ---- Bottom bar: mode chip, then items ----
typedef struct {
    const char *mode;  // "DMR", "FM"
    const char *item1; // e.g. "5W"
    const char *item2; // e.g. "12.5K"
    const char *right; // right aligned, e.g. "DMO"; NULL: none
} bottom_bar_data_t;

void bottom_bar_draw(const bottom_bar_data_t *data);
// The power levels of the MD-UV380 platform (settings.tx_power_level 0 - 9, 9: user power "+W-")
const char *bottom_bar_power_label(int level);

// ---- Soft keys: the bottom bar with what the left (Menu / OK) and right (Home / Back) keys do here ----
void bottom_bar_draw_soft_keys(const char *left, const char *right); // NULL: nothing for that key

// ---- S meter: S1 - S9, then +10 - +40 dB (the transmit panel, the lists... come with their screens) ----
#define S_METER_HEIGHT         66
#define S_METER_COMPACT_HEIGHT 24

typedef struct {
    bool compact;
    bool active;    // false: the area is cleared
    bool open;      // squelch open (audio): the reading in the receive colour
    int8_t s_units; // 0 to 9
    int8_t over_s9; // dB over S9, 0 to 60
} s_meter_data_t;

// s_units / over_s9 from the receiver's shown signal (radio_signal(); the scale of the main firmware's
// radioGetRxSignalShown(): S1 at 81, 6 a unit, S9 at 129)
void s_meter_set_signal(s_meter_data_t *data, uint8_t shown);
void s_meter_draw(int32_t y,
                  const s_meter_data_t *data); // rows y .. y + (compact ? S_METER_COMPACT_HEIGHT : S_METER_HEIGHT) - 1

// ---- List: title bar, rows of an icon, a name and a value (menus, settings) ----
#define LIST_VIEW_ROWS       7 // rows on the screen, the selected one in the middle (OpenGD77 menus)
#define LIST_VIEW_ROW_HEIGHT 34
#define LIST_VIEW_TOP        38

#define LIST_VIEW_ROWS2       5 // two-line rows (e.g. last heard) between the title and the soft keys
#define LIST_VIEW_ROW2_HEIGHT 44

typedef struct {
    icons_id_t icon; // ICONS_COUNT: none
    const char *name;
    const char *value; // right aligned; NULL: none
    bool selected;
    bool dimmed;     // e.g. an option that isn't available
    const char *sub; // two-line rows: the second line, in the label colour; NULL: none
    int16_t cursor;  // text entry in the value: 1 + byte index of the character above the underline (after the
                     // text when it is the length); 0: no cursor
    bool swatch;     // a colour box on the right (after the value): a frame, filled by list_view_fill_swatch()
} list_view_row_t;

void list_view_draw_title(const char *title, const char *position); // position e.g. "4/9", NULL: none
void list_view_draw_row(int32_t row, const list_view_row_t *data);  // row 0 (top) .. LIST_VIEW_ROWS - 1
// The swatch of a row in any colour (native format): the theme colours aren't in the palette, so it is sent straight to
// the LCD, after the frame was flushed (until those rows are flushed again)
void list_view_fill_swatch(int32_t row, uint16_t colour);
// Two-line row: name, sub under it, value right aligned on the middle; rows y .. y + LIST_VIEW_ROW2_HEIGHT - 1
// (under the title: y = LIST_VIEW_TOP + (row * LIST_VIEW_ROW2_HEIGHT), row 0 .. LIST_VIEW_ROWS2 - 1)
void list_view_draw_row2(int32_t y, const list_view_row_t *data);
// Text under the title (results, confirmations, empty list): up to two lines, left aligned; NULL: no line
void list_view_draw_text(const char *line1, const char *line2, theme_colour_t colour);

// ---- Digit entry: a number typed on the keypad, large (docs/manual number-entry.png) ----
#define DIGIT_ENTRY_MAX 8

typedef bool (*digit_entry_set_t)(uint32_t value); // true: the number was taken

typedef struct {
    char digits[DIGIT_ENTRY_MAX];
    uint8_t count;      // digits entered, 0: no entry
    uint8_t max;        // the entry is set with this many digits (1 .. DIGIT_ENTRY_MAX)
    uint8_t dot_after;  // digits before the decimal point, 0: no point
    bool zero_fill;     // the value counts the digits not entered as zeros (a frequency)
    bool ok_on_release; // the left key sets on its release (else on its press)
} digit_entry_t;

bool digit_entry_active(const digit_entry_t *entry);
void digit_entry_stop(digit_entry_t *entry);
uint32_t digit_entry_value(const digit_entry_t *entry);
// A key: a digit pressed is added (it starts the entry: the caller filters the digits that can't start one); while
// entering, LEFT deletes, the left key or the last digit gives the number to set() (refused: the last digit is taken
// back), the right key's release cancels. true: handled; false: not one of these keys
bool digit_entry_handle(digit_entry_t *entry, const ui_event_t *ev, digit_entry_set_t set);
// The digits entered, then unlit zeros, the next one underlined; font_roboto44_bold from x, y
void digit_entry_draw(const digit_entry_t *entry, int32_t x, int32_t y);
// A frequency in 10 Hz as MHz with 5 decimals ("438.50000"), without the unit
void digit_entry_format_frequency(uint32_t frequency, char *out, size_t size);

#endif /* DM32_WIDGETS_H */
