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
 * OpenDMR, service: the interface language (docs/i18n.en.md 1). The number is kept in OpenDMR's own custom data
 * block of the codeplug (0x44540004, "DT" 4), as the key map is ("DT" 2). Without that block, bitfield_options bit 19
 * set means Chinese (as stored today), and choosing Chinese still sets it, so other CPS see what they saw before.
 */

#include "settings.h"
#include "custom_data.h"
#include "language.h"

#define LANGUAGE_DATA_TYPE 0x44540004 // a custom data block of this firmware ("DT", 4)
#define LANGUAGE_VERSION   1

typedef struct {
    uint8_t version;
    uint8_t language;
} stored_t;

static uint8_t current;

void language_load(void)
{
    stored_t stored;

    if (custom_data_read(LANGUAGE_DATA_TYPE, (uint8_t *)&stored, sizeof(stored)) &&
        (stored.version == LANGUAGE_VERSION) && (stored.language < LANGUAGE_COUNT)) {
        current = stored.language;
        return;
    }
    // No block, or one from another version: bit 19, as the settings store the language today
    current = ((settings_valid() && ((settings.bitfield_options & SETTINGS_BIT_SECONDARY_LANGUAGE) != 0))
                   ? LANGUAGE_CHINESE
                   : LANGUAGE_ENGLISH);
}

uint8_t language_get(void)
{
    return current;
}

void language_set(uint8_t language)
{
    stored_t stored;

    if (language >= LANGUAGE_COUNT) {
        language = LANGUAGE_ENGLISH;
    }
    if (language == current) {
        return;
    }
    current = language;
    // Choosing Chinese still sets bit 19, so other CPS see what they saw before
    settings_set_option(SETTINGS_BIT_SECONDARY_LANGUAGE, language == LANGUAGE_CHINESE);
    settings_changed();
    stored.version = LANGUAGE_VERSION;
    stored.language = language;
    (void)custom_data_write(LANGUAGE_DATA_TYPE, (const uint8_t *)&stored, sizeof(stored));
}
