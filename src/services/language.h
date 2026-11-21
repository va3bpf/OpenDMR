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

#ifndef DM32_LANGUAGE_H
#define DM32_LANGUAGE_H

#include <stdbool.h>
#include <stdint.h>

// The interface language (docs/i18n.en.md). English is built in; the others are language files the CPS writes to
// the external flash, each in a 16 KB slot (tools/text/langfile.py has the layout), and a font file when the
// firmware's fonts do not have their glyphs (font_cjk.h). The list: 0 English, then the installed ones in slot order.
#define LANGUAGE_FLASH_ADDRESS 0x280000 // the slots
#define LANGUAGE_SLOT_SIZE     0x4000
#define LANGUAGE_SLOTS         8
#define LANGUAGE_FLASH_END     (LANGUAGE_FLASH_ADDRESS + (LANGUAGE_SLOTS * LANGUAGE_SLOT_SIZE))
#define LANGUAGE_CODE_SIZE     8 // "zh", "pt-BR": NUL terminated

void language_load(void);                          // at power-on: the installed languages, then the chosen one
uint8_t language_count(void);                      // 1 + the installed languages
uint8_t language_get(void);                        // the current one, an index in that list
void language_set(uint8_t index);                  // chooses it, stored in the codeplug
const char *language_code(uint8_t index);          // "en", "zh", ...
const char *language_name(uint8_t index);         // in its own script; in English when its font is missing
const char *language_string(uint16_t id);          // the current language's string of a TEXT_x, NULL: English
void language_rescan_ask(void);                    // the CPS wrote languages or fonts (cable link task)
void language_poll(void);                          // UI task: reads them again when asked

#endif
