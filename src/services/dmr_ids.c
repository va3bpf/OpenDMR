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
 * OpenDMR, service: the DMR ID database (dmr_ids.h), from the main firmware's uiUtilities.c dmrIDCacheInit(),
 * dmrIDLookup(), dmrDbTextDecode(), contactIDLookup() and updateLHItem() (OpenGD77). The OpenGD77 CPS writes it at
 * 0x30000 (12 byte header "Id" + format + record length, the count at 8) and on from 0xB8000 (0x8F400 for the "Idn"
 * format, over the voice prompts): records of a DMR ID (4 byte BCD, or 3 byte binary for the compressed "IdN" / "Idn"
 * formats) and its text, sorted by ID. A binary search, helped by ID_SLICES IDs read at init.
 * Those are flash addresses of the GD-77: for the DM-32 (a C7000 radio) the CPS adds STM32_FLASH_ADDRESS_OFFSET
 * (0x20000, the emulated EEPROM below them), as for the flash part of the codeplug (codeplug.c FLASH_PART_OFFSET), so
 * in our window they are 0x50000, 0xD8000 and 0xAF400.
 */

#include <stdio.h>
#include <string.h>
#include "flash.h"
#include "settings.h"
#include "codeplug.h"
#include "dmr_ids.h"
#include "dmr_countries.h"

#define FLASH_PART          0x20000 // the CPS's STM32_FLASH_ADDRESS_OFFSET (codeplug.c FLASH_PART_OFFSET)
#define LOCATION_1          (FLASH_PART + 0x30000)
#define LOCATION_2          (FLASH_PART + 0xB8000)
#define LOCATION_2_N_FORMAT (FLASH_PART + 0x8F400) // VOICE_PROMPTS_FLASH_HEADER_ADDRESS
#define HEADER_LENGTH       0x0C
#define AREA_1_SPACE        0x40000
#define TEXT_MAX            51 // MAX_DMR_ID_CONTACT_TEXT_LENGTH
#define ID_SLICES           14
#define MIN_ENTRIES_FOR_SLICES 40

// settings.contact_display_priority (CONTACT_DISPLAY_PRIO_x)
enum { PRIO_CC_DB_TA = 0, PRIO_DB_CC_TA, PRIO_TA_CC_DB, PRIO_TA_DB_CC };

static const uint8_t DECOMPRESS_LUT[64] = {
    ' ', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
    'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g',
    'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '.'};

static struct {
    uint32_t entries;
    uint8_t record_length;
    uint8_t id_length; // 4: BCD, 3: binary with compressed text
    uint32_t area_1_size;
    uint32_t location_2;
    uint32_t ids_per_slice;
    uint32_t slices[ID_SLICES]; // [0] the first ID, [ID_SLICES - 1] the last one
} db;

// ---- Ours (tools/ids/id_db.py, its layout there): the header in the window's last sector, the rest below it

#define NEW_HEADER     (FLASH_WINDOW_SIZE - FLASH_SECTOR_SIZE)
#define NEW_BLOCK      4096
#define NEW_BLOCK_MARK 0xD1
#define NEW_CHUNK      16 // a list's entries from each 16th
#define NEW_IDS        10000000
#define NEW_TEXT_MAX   80 // an entry of a list: 2 + 60 bytes from the tool

enum { FIELD_DELTA = 0, FIELD_CALL, FIELD_NAME, FIELD_PLACE, FIELD_OTHER, FIELDS };

typedef struct {
    char magic[4];
    uint8_t version;
    uint8_t reserved[3];
    uint32_t records;
    uint32_t blocks;
    uint32_t countries;
    uint32_t base;
    uint32_t size;
    uint32_t reserved2;
    uint32_t index;         // from the base, as all the offsets
    uint32_t prefix;        // 1000 bytes
    uint32_t country_names; // list
    uint32_t directory;     // per country: its callsign prefixes, states and places (lists)
    uint32_t names;         // list
    uint32_t other;         // u32 each
    uint8_t classes[FIELDS][4];
    uint32_t crc;
} new_header_t;

_Static_assert(sizeof(new_header_t) == 80, "tools/ids/id_db.py header");

static new_header_t ours;
static bool ours_valid = false;

static uint32_t ours_u32(uint32_t offset)
{
    uint32_t value = 0;

    (void)flash_read(ours.base + offset, (uint8_t *)&value, 4); // little endian
    return value;
}

static void ours_init(void)
{
    ours_valid = flash_read(NEW_HEADER, (uint8_t *)&ours, sizeof(ours)) && (memcmp(ours.magic, "ODID", 4) == 0) &&
                 (ours.version == 1) && (ours.records > 0) && (ours.blocks > 0) && (ours.countries < 255) &&
                 (ours.base + ours.size == NEW_HEADER) && (ours.size <= NEW_HEADER);
    for (int f = 0; ours_valid && (f < FIELDS); f++) {
        for (int c = 0; c < 4; c++) {
            ours_valid = ours_valid && (ours.classes[f][c] <= 24);
        }
    }
}

// Entry n of a list (front coded, from each 16th): its bytes, NUL terminated (cut to size), and their count;
// false: no such entry
static bool list_entry_bytes(uint32_t list, uint32_t n, char *out, size_t size, size_t *count)
{
    char text[NEW_TEXT_MAX + 1];
    uint32_t at;
    size_t length = 0;

    if (n >= ours_u32(list)) {
        return false;
    }
    at = list + ours_u32(list + 4 + (4 * (n / NEW_CHUNK)));
    for (uint32_t i = 0; i <= (n % NEW_CHUNK); i++) {
        uint8_t head[2]; // shared, length

        if (!flash_read(ours.base + at, head, 2) || (head[0] > length) || ((head[0] + head[1]) > NEW_TEXT_MAX) ||
            !flash_read(ours.base + at + 2, (uint8_t *)&text[head[0]], head[1])) {
            return false;
        }
        length = head[0] + head[1];
        at += 2 + head[1];
    }
    if (length >= size) {
        length = size - 1;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    if (count) {
        *count = length;
    }
    return true;
}

static bool list_entry(uint32_t list, uint32_t n, char *out, size_t size)
{
    return list_entry_bytes(list, n, out, size, NULL);
}

// The country number of an ID (its first 3 digits), 0xFF: none
static uint8_t ours_country(uint32_t id)
{
    uint8_t country = 0xFF;

    if (id < NEW_IDS) {
        (void)flash_read(ours.base + ours.prefix + (id / 10000), &country, 1);
    }
    return (country < ours.countries) ? country : 0xFF;
}

// The records of a block, bit packed (most significant bit first), read from the flash a little at a time
typedef struct {
    uint32_t address; // of the block's bits
    uint32_t bit;
    uint32_t buffer_at; // byte of buffer[0]
    uint8_t buffer[32];
    bool failed;
} bit_reader_t;

static uint32_t get_bits(bit_reader_t *r, int width)
{
    uint32_t value = 0;

    while (width > 0) {
        uint32_t byte = r->bit / 8;
        int used = (int)(r->bit % 8);
        int take = ((8 - used) < width) ? (8 - used) : width;

        if (byte >= (NEW_BLOCK - 4)) {
            r->failed = true;
            return 0;
        }
        if ((byte < r->buffer_at) || (byte >= (r->buffer_at + sizeof(r->buffer)))) {
            r->buffer_at = byte;
            if (!flash_read(r->address + byte, r->buffer, sizeof(r->buffer))) {
                r->failed = true;
                return 0;
            }
        }
        value = (value << take) | ((r->buffer[byte - r->buffer_at] >> (8 - used - take)) & ((1U << take) - 1));
        r->bit += take;
        width -= take;
    }
    return value;
}

// A number: a class (2 bits), then that class's width of bits
static uint32_t get_number(bit_reader_t *r, int field)
{
    uint32_t c = get_bits(r, 2);
    uint32_t base = 0;

    for (uint32_t i = 0; i < c; i++) {
        base += 1U << ours.classes[field][i];
    }
    return base + get_bits(r, ours.classes[field][c]);
}

static void get_text(bit_reader_t *r, char *out, size_t size)
{
    uint32_t length = get_bits(r, 8);

    for (uint32_t i = 0; i < length; i++) {
        char c = (char)get_bits(r, 8);

        if ((i + 1) < size) {
            out[i] = c;
            out[i + 1] = '\0';
        }
    }
    if (length == 0) {
        out[0] = '\0';
    }
}

static const uint8_t SUFFIX_BITS[5] = {0, 5, 10, 15, 19};

static bool ours_info(uint32_t id, dmr_ids_info_t *info)
{
    uint32_t low = 0;
    uint32_t high = ours.blocks - 1;
    uint32_t block;
    uint8_t head[4];
    bit_reader_t r = {.buffer_at = 0xFFFFFFFF};
    uint32_t current;
    // what the target has, kept from the record that gave it (callsign, place: "the previous record's")
    uint32_t call_id = 0;
    uint32_t call_prefix = 0; // 0: call_text is the whole callsign
    char call_text[sizeof(info->callsign)] = "";
    uint32_t name_no = 0; // 0: info->name has it
    uint32_t place_country = 0xFF;
    uint32_t place_no = 0;

    if ((id < ours_u32(ours.index)) || (id >= NEW_IDS)) {
        return false;
    }
    while (low < high) {
        uint32_t middle = (low + high + 1) / 2;

        if (ours_u32(ours.index + (4 * middle)) <= id) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    block = low;
    if (!flash_read(ours.base + (block * NEW_BLOCK), head, sizeof(head)) || (head[0] != NEW_BLOCK_MARK)) {
        return false;
    }
    r.address = ours.base + (block * NEW_BLOCK) + 4;
    current = ours_u32(ours.index + (4 * block));
    memset(info, 0, sizeof(*info));

    for (uint32_t n = 0; n < (uint32_t)(head[2] | (head[3] << 8)); n++) {
        uint32_t symbol;

        if (n > 0) {
            current += get_number(&r, FIELD_DELTA);
        }
        symbol = get_number(&r, FIELD_CALL);
        if (symbol == 1) {
            get_text(&r, call_text, sizeof(call_text));
            call_prefix = 0;
        } else if (symbol >= 2) {
            uint32_t length = get_bits(&r, 3);
            uint32_t value = get_bits(&r, (length < 5) ? SUFFIX_BITS[length] : 0);

            call_id = current;
            call_prefix = symbol - 1;
            call_text[(length < 5) ? length : 0] = '\0';
            for (int i = (int)length - 1; (i >= 0) && (length < 5); i--) {
                call_text[i] = (char)('A' + (value % 26));
                value /= 26;
            }
        }
        name_no = get_number(&r, FIELD_NAME);
        if (name_no == 0) {
            get_text(&r, info->name, sizeof(info->name));
        }
        symbol = get_number(&r, FIELD_PLACE);
        if (symbol == 1) {
            uint32_t other = ours_u32(ours.other + (4 * get_number(&r, FIELD_OTHER)));

            place_country = other >> 24;
            place_no = other & 0xFFFFFF;
        } else if (symbol >= 2) {
            place_country = ours_country(current);
            place_no = symbol - 2;
        }
        if (r.failed || (current > id)) {
            return false;
        }
        if (current == id) {
            break;
        }
    }
    if (current != id) {
        return false;
    }

    // The target's texts from the lists
    if (call_prefix > 0) {
        char prefix[sizeof(info->callsign)];
        uint8_t country = ours_country(call_id);

        if ((country == 0xFF) ||
            !list_entry(ours_u32(ours.directory + (12 * country)), call_prefix - 1, prefix, sizeof(prefix))) {
            return false;
        }
        snprintf(info->callsign, sizeof(info->callsign), "%s%s", prefix, call_text);
    } else {
        snprintf(info->callsign, sizeof(info->callsign), "%s", call_text);
    }
    if ((name_no > 0) && !list_entry(ours.names, name_no - 1, info->name, sizeof(info->name))) {
        return false;
    }
    if (place_country < ours.countries) {
        char place[NEW_TEXT_MAX + 1];
        size_t length = 0;
        uint32_t lists = ours.directory + (12 * place_country);

        if (list_entry_bytes(ours_u32(lists + 8), place_no, place, sizeof(place), &length) && (length >= 2)) {
            uint32_t state = ((uint32_t)(uint8_t)place[0] << 8) | (uint8_t)place[1];

            snprintf(info->city, sizeof(info->city), "%.63s", &place[2]);
            if (state != 0xFFFF) {
                (void)list_entry(ours_u32(lists + 4), state, info->state, sizeof(info->state));
            }
        }
        (void)list_entry(ours.country_names, place_country, info->country, sizeof(info->country));
    }
    return true;
}

static bool read_record(uint32_t offset, void *data, uint32_t length)
{
    uint32_t address = (offset >= db.area_1_size) ? (db.location_2 + (offset - db.area_1_size))
                                                  : (LOCATION_1 + HEADER_LENGTH + offset);

    return flash_read(address, (uint8_t *)data, length);
}

static uint32_t read_id(uint32_t position)
{
    uint32_t id = 0;

    (void)read_record(db.record_length * position, &id, db.id_length); // little endian
    return id;
}

void dmr_ids_init(void)
{
    uint8_t header[HEADER_LENGTH];

    memset(&db, 0, sizeof(db));
    ours_init();
    // Not the old "ID" tag: too many corrupted databases had it
    if (!flash_read(LOCATION_1, header, sizeof(header)) || (header[0] != 'I') || (header[1] != 'd')) {
        return;
    }
    db.id_length = ((header[2] == 'N') || (header[2] == 'n')) ? 3 : 4;
    db.location_2 = (header[2] == 'n') ? LOCATION_2_N_FORMAT : LOCATION_2;
    db.record_length = (uint8_t)(header[3] - 0x4A);
    if ((db.record_length <= db.id_length) || (db.record_length > (4 + TEXT_MAX))) {
        return;
    }
    db.area_1_size = db.record_length * ((AREA_1_SPACE - HEADER_LENGTH) / db.record_length);
    db.entries = (uint32_t)header[8] | ((uint32_t)header[9] << 8) | ((uint32_t)header[10] << 16) |
                 ((uint32_t)header[11] << 24);
    if (db.entries == 0) {
        return;
    }
    db.slices[0] = read_id(0);
    db.slices[ID_SLICES - 1] = read_id(db.entries - 1);
    if (db.entries > MIN_ENTRIES_FOR_SLICES) {
        db.ids_per_slice = db.entries / (ID_SLICES - 1);
        for (int i = 0; i < (ID_SLICES - 2); i++) {
            db.slices[i + 1] = read_id((db.ids_per_slice * i) + db.ids_per_slice);
        }
    }
}

uint32_t dmr_ids_count(void)
{
    return ours_valid ? ours.records : db.entries;
}

// A 9 digit ID: a 7 digit one and 2 more (a hotspot's ESSID)
static uint32_t person(uint32_t id)
{
    return ((id >= 100000000) && (id < 1000000000)) ? (id / 100) : id;
}

bool dmr_ids_info(uint32_t id, dmr_ids_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!ours_valid) {
        return false;
    }
    if (ours_info(id, info) || ((person(id) != id) && ours_info(person(id), info))) {
        return true;
    }
    memset(info, 0, sizeof(*info));
    return false;
}

bool dmr_ids_country(uint32_t id, char *country, size_t size)
{
    uint32_t prefix = person(id) / 10000;
    uint8_t number;

    country[0] = '\0';
    if (ours_valid) {
        number = ours_country(person(id));
        return (number != 0xFF) && list_entry(ours.country_names, number, country, size);
    }
    // Without ours: the countries built in (dmr_countries.c)
    if ((person(id) >= NEW_IDS) || (prefix < DMR_COUNTRY_FIRST_PREFIX) ||
        (prefix >= (DMR_COUNTRY_FIRST_PREFIX + DMR_COUNTRY_PREFIXES))) {
        return false;
    }
    number = DMR_COUNTRY_OF_PREFIX[prefix - DMR_COUNTRY_FIRST_PREFIX];
    if (number >= DMR_COUNTRIES) {
        return false;
    }
    snprintf(country, size, "%s", DMR_COUNTRY_NAMES[number]);
    return true;
}

static uint32_t to_bcd(uint32_t value)
{
    uint32_t bcd = 0;

    for (int shift = 0; value != 0; shift += 4, value /= 10) {
        bcd |= (value % 10) << shift;
    }
    return bcd;
}

// dmrDbTextDecode(): 4 characters in 3 bytes, the trailing spaces trimmed
static void decode_text(char *out, size_t size, const uint8_t *in, int length)
{
    size_t n = 0;
    int d = 0;

    while ((d < length) && ((n + 4) < size)) {
        uint8_t cb1 = in[d++];

        out[n++] = (char)DECOMPRESS_LUT[cb1 >> 2];
        if (d == length) {
            break;
        }

        uint8_t cb2 = in[d++];

        out[n++] = (char)DECOMPRESS_LUT[((cb1 & 0x03) << 4) + (cb2 >> 4)];
        if (d == length) {
            break;
        }

        uint8_t cb3 = in[d++];

        out[n++] = (char)DECOMPRESS_LUT[((cb2 & 0x0F) << 2) + (cb3 >> 6)];
        out[n++] = (char)DECOMPRESS_LUT[cb3 & 0x3F];
    }
    while ((n > 0) && (out[n - 1] == ' ')) {
        n--;
    }
    out[n] = '\0';
}

static bool read_text(uint32_t position, char *text, size_t size)
{
    uint8_t raw[TEXT_MAX + 4];
    int length = db.record_length - db.id_length;

    if (!read_record((db.record_length * position) + db.id_length, raw, (uint32_t)length)) {
        return false;
    }
    if (db.id_length == 3) {
        decode_text(text, size, raw, length);
    } else {
        int n = (length < (int)(size - 1)) ? length : (int)(size - 1);

        memcpy(text, raw, (size_t)n); // not NUL terminated in the flash
        text[n] = '\0';
    }
    return true;
}

bool dmr_ids_lookup(uint32_t id, char *text, size_t size)
{
    uint32_t target = (db.id_length == 4) ? to_bcd(id) : id;
    uint32_t start = 0;
    uint32_t end;

    text[0] = '\0';
    if (ours_valid) { // ours instead of the OpenGD77 one: "callsign name" as it has
        dmr_ids_info_t info;

        if (!dmr_ids_info(id, &info)) {
            return false;
        }
        snprintf(text, size, "%s%s%s", info.callsign, (info.name[0] ? " " : ""), info.name);
        return true;
    }
    if ((db.entries == 0) || (target < db.slices[0]) || (target > db.slices[ID_SLICES - 1])) {
        return false;
    }
    end = db.entries - 1;
    if (db.entries > MIN_ENTRIES_FOR_SLICES) {
        for (int i = 0; i < (ID_SLICES - 1); i++) {
            bool last = (i == (ID_SLICES - 2));

            if ((target >= db.slices[i]) && (last ? (target <= db.slices[i + 1]) : (target < db.slices[i + 1]))) {
                start = db.ids_per_slice * i;
                end = last ? (db.entries - 1) : (db.ids_per_slice * (i + 1));
                break;
            }
        }
    }
    while (start <= end) {
        uint32_t middle = (start + end) / 2;
        uint32_t found = read_id(middle);

        if (found < target) {
            start = middle + 1;
        } else if (found > target) {
            if (middle == 0) {
                break;
            }
            end = middle - 1;
        } else {
            return read_text(middle, text, size);
        }
    }
    return false;
}

// contactIDLookup() of a private call: the contact's name
static bool contact_name(uint32_t id, char *text, size_t size)
{
    codeplug_contact_t contact;
    char name[CODEPLUG_NAME_LENGTH + 1];
    int index = codeplug_contact_find(id, CODEPLUG_CALL_PRIVATE, 0);

    if ((index == 0) || !codeplug_get_contact(index, &contact)) {
        return false;
    }
    codeplug_name_to_string(contact.name, name, CODEPLUG_NAME_LENGTH);
    snprintf(text, size, "%s", name);
    return true;
}

void dmr_ids_caller_text(uint32_t id, char *text, size_t size)
{
    bool database_first = (settings.contact_display_priority == PRIO_DB_CC_TA) ||
                          (settings.contact_display_priority == PRIO_TA_DB_CC);

    text[0] = '\0';
    if (database_first) {
        if (!dmr_ids_lookup(id, text, size)) {
            (void)contact_name(id, text, size);
        }
    } else if (!contact_name(id, text, size)) {
        (void)dmr_ids_lookup(id, text, size);
    }
}

bool dmr_ids_talker_alias_first(void)
{
    return (settings.contact_display_priority == PRIO_TA_CC_DB) ||
           (settings.contact_display_priority == PRIO_TA_DB_CC);
}
