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
    return db.entries;
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
