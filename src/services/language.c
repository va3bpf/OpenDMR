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
 * OpenDMR, service: the interface language (docs/i18n.en.md). English is built in (text_en.c); the others are the
 * language files the CPS writes to the slots of the external flash (tools/text/langfile.py has the layout).
 *
 * The chosen language's slot is kept in RAM, checked (CRC, each string ends in the slot, its format specifiers are
 * English's), and its strings found by the hash of their TEXT_x name (text_keys.c): a string missing, unknown or
 * wrong is shown in English. The choice is kept as the language code in OpenDMR's custom data block "DT" 8.
 * Before it: "DT" 4 (version 1, a number: 0 English, 1 Chinese, 2 Spanish) and bitfield_options bit 19 (Chinese),
 * which are still read when there is no "DT" 8; choosing Chinese still sets bit 19, so other CPS see what they saw.
 */

#include <string.h>
#include "flash.h"
#include "settings.h"
#include "custom_data.h"
#include "font_cjk.h"
#include "text.h"
#include "language.h"

#define CHOICE_DATA_TYPE     0x44540008 // a custom data block of this firmware ("DT", 8): the language code
#define CHOICE_VERSION       1
#define OLD_DATA_TYPE        0x44540004 // "DT" 4: the language number of the firmware before language files
#define OLD_VERSION          1
#define SLOT_MAGIC           "DM32LANG"
#define SLOT_HEADER_SIZE     92
#define SLOT_ENTRY_SIZE      6
#define NAME_SIZE            32
#define ENGLISH_NAME_SIZE    24
#define FONT_NAME_SIZE       8

typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    char code[LANGUAGE_CODE_SIZE];
} choice_t;

typedef struct {
    uint8_t slot;
    char code[LANGUAGE_CODE_SIZE];
    char name[NAME_SIZE];
    char english_name[ENGLISH_NAME_SIZE];
    char font[FONT_NAME_SIZE];
} installed_t;

static installed_t installed[LANGUAGE_SLOTS];
static uint8_t installed_count;
static uint8_t current;                         // 0: English, else installed[current - 1]
static uint8_t slot_data[LANGUAGE_SLOT_SIZE];   // the current language's slot
static uint16_t offsets[TEXT_COUNT];            // of each string in slot_data, 0: English
static bool loaded;
static volatile bool rescan_asked;

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFF;

    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
        }
    }
    return ~crc;
}

// A text field of the slot header, NUL terminated
static void copy_field(char *to, const uint8_t *from, uint32_t size)
{
    memcpy(to, from, size);
    to[size - 1] = '\0';
}

// The slot into slot_data, true when it holds a whole language with its CRC right
static bool read_slot(uint8_t slot)
{
    uint32_t length;

    if (!flash_read_raw(LANGUAGE_FLASH_ADDRESS + (slot * LANGUAGE_SLOT_SIZE), slot_data, LANGUAGE_SLOT_SIZE) ||
        (memcmp(slot_data, SLOT_MAGIC, 8) != 0)) {
        return false;
    }
    length = get_le32(&slot_data[8]);
    return (length >= SLOT_HEADER_SIZE) && (length <= LANGUAGE_SLOT_SIZE) &&
           (crc32(&slot_data[16], length - 16) == get_le32(&slot_data[12]));
}

// The installed languages, in slot order; a code seen twice is taken once
static void scan(void)
{
    installed_count = 0;
    loaded = false;
    for (uint8_t slot = 0; slot < LANGUAGE_SLOTS; slot++) {
        installed_t *l = &installed[installed_count];
        bool seen = false;

        if (!read_slot(slot)) {
            continue;
        }
        l->slot = slot;
        copy_field(l->code, &slot_data[16], LANGUAGE_CODE_SIZE);
        copy_field(l->name, &slot_data[24], NAME_SIZE);
        copy_field(l->english_name, &slot_data[56], ENGLISH_NAME_SIZE);
        copy_field(l->font, &slot_data[80], FONT_NAME_SIZE);
        for (uint8_t i = 0; i < installed_count; i++) {
            seen = seen || (strcmp(installed[i].code, l->code) == 0);
        }
        if ((l->code[0] != '\0') && (strcmp(l->code, "en") != 0) && !seen) {
            installed_count++;
        }
    }
}

// The next format specifier of a string, as tools/text/langfile.py SPEC finds them: '%', flags, then a letter or '%'
static const char *next_spec(const char *s, uint32_t *length)
{
    for (; *s != '\0'; s++) {
        const char *p = s + 1;

        if (*s != '%') {
            continue;
        }
        while ((*p != '\0') && (strchr("-+ #0123456789.*", *p) != NULL)) {
            p++;
        }
        if (((*p >= 'a') && (*p <= 'z')) || ((*p >= 'A') && (*p <= 'Z')) || (*p == '%')) {
            *length = (uint32_t)(p - s) + 1;
            return s;
        }
    }
    return NULL;
}

// The same format specifiers in the same order: printf() reads the arguments English gives
static bool specs_match(const char *a, const char *b)
{
    for (;;) {
        uint32_t la = 0, lb = 0;

        a = next_spec(a, &la);
        b = next_spec(b, &lb);
        if ((a == NULL) || (b == NULL)) {
            return (a == NULL) && (b == NULL);
        }
        if ((la != lb) || (memcmp(a, b, la) != 0)) {
            return false;
        }
        a += la;
        b += lb;
    }
}

static int find_key(uint32_t hash)
{
    int low = 0, high = TEXT_COUNT - 1;

    while (low <= high) {
        int mid = (low + high) / 2;

        if (text_key_hashes[mid] == hash) {
            return text_key_ids[mid];
        }
        if (text_key_hashes[mid] < hash) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return -1;
}

// The strings of the current language, and its font first for the glyphs
static void load_current(void)
{
    const installed_t *l = (current > 0) ? &installed[current - 1] : NULL;

    memset(offsets, 0, sizeof(offsets));
    loaded = false;
    // Without the font it needs its strings show "??": that says the font is missing (its menu name is English)
    if ((l != NULL) && read_slot(l->slot)) {
        uint32_t length = get_le32(&slot_data[8]);
        uint32_t count = slot_data[88] | (slot_data[89] << 8);

        if ((SLOT_HEADER_SIZE + (count * SLOT_ENTRY_SIZE)) <= length) {
            for (uint32_t i = 0; i < count; i++) {
                const uint8_t *e = &slot_data[SLOT_HEADER_SIZE + (i * SLOT_ENTRY_SIZE)];
                uint32_t offset = e[4] | (e[5] << 8);
                int id = find_key(get_le32(e));
                const char *s = (const char *)&slot_data[offset];

                if ((id < 0) || (offset < SLOT_HEADER_SIZE) || (offset >= length) ||
                    (memchr(s, '\0', length - offset) == NULL) || !specs_match(s, text_en[id])) {
                    continue;
                }
                offsets[id] = (uint16_t)offset;
            }
            loaded = true;
        }
    }
    font_cjk_prefer((l != NULL) ? l->font : "");
}

static uint8_t find_code(const char *code)
{
    for (uint8_t i = 0; i < installed_count; i++) {
        if (strcmp(installed[i].code, code) == 0) {
            return i + 1;
        }
    }
    return 0;
}

void language_load(void)
{
    choice_t choice;
    uint8_t old[2];
    const char *code = "en";

    scan();
    if (custom_data_read(CHOICE_DATA_TYPE, (uint8_t *)&choice, sizeof(choice)) &&
        (choice.version == CHOICE_VERSION)) {
        choice.code[LANGUAGE_CODE_SIZE - 1] = '\0';
        code = choice.code;
    } else if (custom_data_read(OLD_DATA_TYPE, old, sizeof(old)) && (old[0] == OLD_VERSION)) {
        code = (old[1] == 1) ? "zh" : ((old[1] == 2) ? "es" : "en");
    } else if (settings_valid() && ((settings.bitfield_options & SETTINGS_BIT_SECONDARY_LANGUAGE) != 0)) {
        code = "zh";
    }
    current = find_code(code);
    load_current();
}

uint8_t language_count(void)
{
    return installed_count + 1;
}

uint8_t language_get(void)
{
    return current;
}

void language_set(uint8_t index)
{
    choice_t choice;

    if (index >= language_count()) {
        index = 0;
    }
    if (index == current) {
        return;
    }
    current = index;
    load_current();

    // Chinese still sets bit 19, and "DT" 4 follows when it is there, so other CPS see what they saw before
    settings_set_option(SETTINGS_BIT_SECONDARY_LANGUAGE, strcmp(language_code(index), "zh") == 0);
    settings_changed();
    if (custom_data_find(OLD_DATA_TYPE, NULL) != 0) {
        uint8_t old[2] = {OLD_VERSION, 0};

        old[1] = (strcmp(language_code(index), "zh") == 0) ? 1 : ((strcmp(language_code(index), "es") == 0) ? 2 : 0);
        (void)custom_data_write(OLD_DATA_TYPE, old, sizeof(old));
    }
    memset(&choice, 0, sizeof(choice));
    choice.version = CHOICE_VERSION;
    strncpy(choice.code, language_code(index), LANGUAGE_CODE_SIZE - 1);
    (void)custom_data_write(CHOICE_DATA_TYPE, (const uint8_t *)&choice, sizeof(choice));
}

const char *language_code(uint8_t index)
{
    return ((index > 0) && (index <= installed_count)) ? installed[index - 1].code : "en";
}

const char *language_name(uint8_t index)
{
    const installed_t *l;

    if ((index == 0) || (index > installed_count)) {
        return "English";
    }
    l = &installed[index - 1];
    return ((l->font[0] == '\0') || font_cjk_has(l->font)) ? l->name : l->english_name;
}

const char *language_string(uint16_t id)
{
    return (loaded && (id < TEXT_COUNT) && (offsets[id] != 0)) ? (const char *)&slot_data[offsets[id]] : NULL;
}

void language_rescan_ask(void)
{
    rescan_asked = true;
}

void language_poll(void)
{
    char code[LANGUAGE_CODE_SIZE];

    if (!rescan_asked) {
        return;
    }
    rescan_asked = false;
    strncpy(code, language_code(current), sizeof(code));
    code[sizeof(code) - 1] = '\0';
    font_cjk_reload();
    scan();
    current = find_code(code); // gone: English
    load_current();
}
