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

#ifndef DM32_TONES_H
#define DM32_TONES_H

#include <stddef.h>
#include <stdint.h>

// CTCSS tones and DCS codes as the codeplug holds them (codeplug.h rx_tone / tx_tone): CTCSS in 0.1 Hz; DCS with
// TONES_DCS set, the octal digits as hex, TONES_DCS_INVERTED; CODEPLUG_CSS_NONE: none
#define TONES_DCS          0x8000
#define TONES_DCS_INVERTED 0x4000
#define TONES_DCS_CODE     0x0FFF

// One list of all of them, for stepping: none, the CTCSS tones, the DCS codes, the inverted DCS codes
int tones_count(void);
int tones_position(uint16_t tone); // 0 (none) for a tone not in the list
uint16_t tones_at(int position);
uint16_t tones_step(uint16_t tone, int step); // stops at the ends

void tones_format(uint16_t tone, char *out, size_t size); // "88.5Hz", "D023N", "" for none

#endif
