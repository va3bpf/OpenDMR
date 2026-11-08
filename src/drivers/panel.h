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

#ifndef DM32_PANEL_H
#define DM32_PANEL_H

#include <stdint.h>
#include <stdbool.h>

#define PANEL_WIDTH  240
#define PANEL_HEIGHT 320

// ST7789 commands used outside panel.c
#define PANEL_CMD_CASET 0x2A // column address set
#define PANEL_CMD_RASET 0x2B // row address set
#define PANEL_CMD_RAMWR 0x2C // memory write

// RGB565 for panel_fill(): red in the high 5 bits
#define PANEL_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void panel_init(void);
void panel_fill(int x0, int y0, int x1, int y1, uint16_t colour); // straight to the panel, [x0, x1) x [y0, y1)
void panel_bus_take(void);                                        // the bus is shared with the keypad
void panel_bus_give(void);

// Bus writes (the caller holds the bus)
void panel_write_cmd(uint8_t cmd);
void panel_write_data(uint8_t data);
void panel_write_cmd_data(uint8_t cmd, const uint8_t *data, int len);

uint16_t panel_rgb888_to_native(uint32_t rgb888); // for the frame buffer palette (theme.c)

#endif
