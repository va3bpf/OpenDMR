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
 * OpenDMR, service: reading the codeplug. After OpenDM32 user/SOURCE/functions/codeplug.c.
 *
 * Layout (data flash window addresses): the "EEPROM" part from 0 (the GD-77 had an EEPROM there), the "flash" part
 * from FLASH_PART_OFFSET (128 KB on the MD-UV380 platform the DM-32 build uses). Channels 1 - 128 are in the EEPROM
 * part, the others in the flash part in banks of 128, each bank after a 16 byte table of the channels in use.
 */

#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "flash.h"
#include "codeplug.h"

#define FLASH_PART_OFFSET (128 * 1024)

#define ADDR_USER_CALLSIGN         0x00E0
#define ADDR_USER_DMRID            0x00E8
#define ADDR_LAST_USED             0x6000 // "LUCZ", then a byte a zone; "all channels" as 16 bits (channel - 1)
#define ADDR_CHANNEL_HEADER_EEPROM 0x3780
#define ADDR_CHANNEL_EEPROM        0x3790
#define ADDR_ZONE_IN_USE           0x8010
#define ADDR_ZONE_LIST             0x8030
#define ADDR_ZONE_FORMAT_CHECK     0x8060 // see check_channels_per_zone()
#define ADDR_CHANNEL_HEADER_FLASH  0x7B1B0
#define ADDR_CHANNEL_FLASH         0x7B1C0

#define ADDR_VFO_A 0x7590 // VFO B follows

#define CHANNEL_SIZE      56
#define BANK_HEADER_SIZE  16 // the channels in use of a bank, before it
#define CHANNELS_PER_BANK 128
#define CHANNEL_BANKS     8
#define ZONE_IN_USE_SIZE  32
#define ZONE_SIZE_16      48  // 16 channels a zone
#define ZONE_SIZE_80      176 // 80 channels a zone
#define ZONES_MAX         68
#define LAST_USED_SIZE    (ZONES_MAX + 2)
#define SQUELCH_MAX       21
#define SQUELCH_DEFAULT   10

#define VFO_DEFAULT_FREQUENCY 14400000 // 144 MHz, DEFAULT_USER_FREQUENCY_BANDS[RADIO_BAND_VHF].minFreq
#define VFO_STEP_MASK         0xF0     // vfo_flag5: the step index in the high nibble

_Static_assert(offsetof(codeplug_channel_t, not_in_codeplug_flag) == CHANNEL_SIZE,
               "a channel is 56 bytes in the codeplug");

static uint8_t zones_in_use[ZONE_IN_USE_SIZE];
static uint8_t channels_in_use[(CHANNEL_BANKS * CHANNELS_PER_BANK) / 8];
static int channels_per_zone = 16;
static int channel_total;
static uint8_t last_used[LAST_USED_SIZE];

uint32_t codeplug_bcd_to_int(uint32_t bcd)
{
    uint32_t result = 0;
    uint32_t multiplier = 1;

    while (bcd) {
        result += (bcd & 0x0F) * multiplier;
        multiplier *= 10;
        bcd >>= 4;
    }
    return result;
}

static uint32_t swap32(uint32_t n)
{
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) | ((n >> 8) & 0xFF00) | (n >> 24);
}

void codeplug_name_to_string(const char *name, char *out, int length)
{
    int i;

    for (i = 0; (i < length) && (name[i] != (char)0xFF) && (name[i] != 0); i++) {
        out[i] = name[i];
    }
    out[i] = 0;
}

// Zone format: at 0x806F is the last byte of the second zone's name with 16 channels a zone (0xFF filled by the CPS),
// or the high byte of a channel number with 80 channels a zone (0 - 4, as there are at most 1024 channels)
static void check_channels_per_zone(void)
{
    uint8_t buffer[16];

    if (flash_read(ADDR_ZONE_FORMAT_CHECK, buffer, sizeof(buffer)) && (buffer[15] <= 0x04)) {
        channels_per_zone = 80;
    }
}

static uint32_t bank_header_address(int bank)
{
    if (bank == 0) {
        return ADDR_CHANNEL_HEADER_EEPROM;
    }
    return FLASH_PART_OFFSET + ADDR_CHANNEL_HEADER_FLASH +
           ((bank - 1) * ((CHANNELS_PER_BANK * CHANNEL_SIZE) + BANK_HEADER_SIZE));
}

void codeplug_init(void)
{
    flash_read(ADDR_ZONE_IN_USE, zones_in_use, sizeof(zones_in_use));
    for (int bank = 0; bank < CHANNEL_BANKS; bank++) {
        flash_read(bank_header_address(bank), &channels_in_use[bank * 16], 16);
    }
    check_channels_per_zone();

    uint8_t tag[4];

    memset(last_used, 0, sizeof(last_used));
    if (flash_read(ADDR_LAST_USED, tag, sizeof(tag)) && (memcmp(tag, "LUCZ", 4) == 0)) {
        flash_read(ADDR_LAST_USED + 4, last_used, sizeof(last_used));
    }

    channel_total = 0;
    for (unsigned i = 0; i < sizeof(channels_in_use); i++) {
        channel_total += __builtin_popcount(channels_in_use[i]);
    }

    codeplug_contacts_init();
}

void codeplug_get_radio_name(char *out)
{
    char name[8];

    flash_read(ADDR_USER_CALLSIGN, (uint8_t *)name, sizeof(name));
    codeplug_name_to_string(name, out, sizeof(name));
}

uint32_t codeplug_get_dmr_id(void)
{
    uint32_t id = 0;

    flash_read(ADDR_USER_DMRID, (uint8_t *)&id, sizeof(id));
    return codeplug_bcd_to_int(swap32(id));
}

int codeplug_zone_count(void)
{
    int count = 1; // the "all channels" zone

    for (int i = 0; i < ZONE_IN_USE_SIZE; i++) {
        count += __builtin_popcount(zones_in_use[i]);
    }
    return count;
}

bool codeplug_get_zone(int number, codeplug_zone_t *zone)
{
    int found = -1;
    int count = -1;

    memset(zone, 0, sizeof(*zone));

    if (number == (codeplug_zone_count() - 1)) {
        strncpy(zone->name, "All channels", sizeof(zone->name));
        zone->channel_count = channel_total;
        zone->index = -1;
        return true;
    }

    // The zone list may have holes: the number-th zone in use
    for (int i = 0; (i < (ZONE_IN_USE_SIZE * 8)) && (found < 0); i++) {
        if ((zones_in_use[i / 8] >> (i % 8)) & 1) {
            if (++count == number) {
                found = i;
            }
        }
    }
    if (found < 0) {
        zone->index = -2;
        return false;
    }

    zone->index = found;
    flash_read(ADDR_ZONE_LIST + (found * (16 + (2 * channels_per_zone))), (uint8_t *)zone,
               ((channels_per_zone == 16) ? ZONE_SIZE_16 : ZONE_SIZE_80));
    for (int i = channels_per_zone; i < CODEPLUG_ZONE_CHANNELS; i++) {
        zone->channels[i] = 0;
    }
    while ((zone->channel_count < channels_per_zone) && (zone->channels[zone->channel_count] != 0)) {
        zone->channel_count++;
    }
    return true;
}

// The main firmware reads "all channels" one byte before the table (SAFE_MIN(-1, ...) is -1), over zone 0: here it is
// read where it was meant to be, the last two bytes, and channel 1 is used when that is not a channel in use
int codeplug_get_last_used_channel(int zone_index)
{
    if (zone_index >= 0) {
        return last_used[(zone_index < ZONES_MAX) ? zone_index : (ZONES_MAX - 1)];
    }

    int number = (last_used[ZONES_MAX] | (last_used[ZONES_MAX + 1] << 8)) + 1;

    return codeplug_channel_in_use(number) ? number : CODEPLUG_CHANNELS_MIN;
}

int codeplug_channel_count(void)
{
    return channel_total;
}

bool codeplug_channel_in_use(int number)
{
    if ((number < CODEPLUG_CHANNELS_MIN) || (number > CODEPLUG_CHANNELS_MAX)) {
        return false;
    }
    number--;
    return ((channels_in_use[number / 8] >> (number % 8)) & 1) != 0;
}

static uint32_t int_to_bcd(uint32_t n)
{
    uint32_t result = 0;

    for (int shift = 0; (n > 0) && (shift < 32); shift += 4) {
        result |= (n % 10) << shift;
        n /= 10;
    }
    return result;
}

// codeplugSetUserDMRID()
bool codeplug_set_dmr_id(uint32_t id)
{
    uint32_t stored = swap32(int_to_bcd(id));

    return flash_write(ADDR_USER_DMRID, (const uint8_t *)&stored, sizeof(stored));
}

// CTCSS tones are BCD in the codeplug, DCS codes have flags in the high bits
static uint16_t css_from_codeplug(uint16_t tone)
{
    if ((tone == 0) || (tone == CODEPLUG_CSS_NONE)) {
        return CODEPLUG_CSS_NONE;
    }
    return ((tone & 0xC000) == 0) ? (uint16_t)codeplug_bcd_to_int(tone) : tone;
}

static uint16_t css_to_codeplug(uint16_t tone)
{
    if ((tone == 0) || (tone == CODEPLUG_CSS_NONE)) {
        return CODEPLUG_CSS_NONE;
    }
    return ((tone & 0xC000) == 0) ? (uint16_t)int_to_bcd(tone) : tone;
}

static uint32_t channel_address(int number)
{
    int index = number - 1;

    if (index < CHANNELS_PER_BANK) {
        return ADDR_CHANNEL_EEPROM + (index * CHANNEL_SIZE);
    }
    index -= CHANNELS_PER_BANK;
    return FLASH_PART_OFFSET + ADDR_CHANNEL_FLASH + (BANK_HEADER_SIZE * (index / CHANNELS_PER_BANK)) +
           (index * CHANNEL_SIZE);
}

// The 56 stored bytes to numbers (frequencies in 10 Hz, tones as numbers)
// The order of the mode key, and the analog modes as stored in opendmr_mode (codeplug.h)
static const uint8_t mode_order[] = {CODEPLUG_MODE_FM, CODEPLUG_MODE_AM, CODEPLUG_MODE_USB, CODEPLUG_MODE_LSB,
                                     CODEPLUG_MODE_DMR};
// opendmr_mode: OPENDMR_MODE_TAG | 1 AM, 2 USB, 3 LSB; anything else FM (the byte was the GD-77's Tx signalling, 0 ..
// 3 in an old codeplug: the tag keeps such a value from being taken for a mode)
static const uint8_t analog_modes[] = {CODEPLUG_MODE_FM, CODEPLUG_MODE_AM, CODEPLUG_MODE_USB, CODEPLUG_MODE_LSB};
#define ANALOG_MODES     ((uint8_t)(sizeof(analog_modes) / sizeof(analog_modes[0])))
#define OPENDMR_MODE_TAG 0xA0
#define MODE_ORDER   ((int)(sizeof(mode_order) / sizeof(mode_order[0])))

const char *codeplug_mode_name(uint8_t mode)
{
    static const char *const names[CODEPLUG_MODES] = {
        [CODEPLUG_MODE_FM] = "FM", [CODEPLUG_MODE_DMR] = "DMR", [CODEPLUG_MODE_AM] = "AM",
        [CODEPLUG_MODE_USB] = "USB", [CODEPLUG_MODE_LSB] = "LSB",
    };

    return (mode < CODEPLUG_MODES) ? names[mode] : "FM";
}

uint8_t codeplug_mode_step(uint8_t mode, int direction)
{
    int at = 0;

    while ((at < MODE_ORDER) && (mode_order[at] != mode)) {
        at++;
    }
    at = (at + ((direction < 0) ? (MODE_ORDER - 1) : 1)) % MODE_ORDER;
    return mode_order[at];
}

static void channel_from_codeplug(codeplug_channel_t *channel)
{
    if (channel->mode != 0) {
        channel->mode = CODEPLUG_MODE_DMR;
    } else {
        uint8_t index = (uint8_t)(channel->opendmr_mode ^ OPENDMR_MODE_TAG);

        channel->mode = (index < ANALOG_MODES) ? analog_modes[index] : CODEPLUG_MODE_FM;
    }
    channel->rx_freq = codeplug_bcd_to_int(channel->rx_freq);
    channel->tx_freq = codeplug_bcd_to_int(channel->tx_freq);
    channel->rx_tone = css_from_codeplug(channel->rx_tone);
    channel->tx_tone = css_from_codeplug(channel->tx_tone);
    channel->not_in_codeplug_flag = 0;
    channel->not_in_codeplug_distance_x10 = -1;
    if (channel->sql > SQUELCH_MAX) {
        channel->sql = SQUELCH_DEFAULT; // not used by the stock firmware: may be anything
    }
}

static void channel_to_codeplug(const codeplug_channel_t *channel, codeplug_channel_t *stored)
{
    memcpy(stored, channel, CHANNEL_SIZE);
    stored->mode = ((channel->mode == CODEPLUG_MODE_DMR) ? 1 : 0);
    stored->opendmr_mode = 0; // FM, DMR: as the CPS writes it
    for (uint8_t i = 1; i < ANALOG_MODES; i++) {
        if (analog_modes[i] == channel->mode) {
            stored->opendmr_mode = (uint8_t)(OPENDMR_MODE_TAG | i);
        }
    }
    stored->rx_freq = int_to_bcd(channel->rx_freq);
    stored->tx_freq = int_to_bcd(channel->tx_freq);
    stored->rx_tone = css_to_codeplug(channel->rx_tone);
    stored->tx_tone = css_to_codeplug(channel->tx_tone);
}

bool codeplug_get_channel(int number, codeplug_channel_t *channel)
{
    if ((number < CODEPLUG_CHANNELS_MIN) || (number > CODEPLUG_CHANNELS_MAX) ||
        !flash_read(channel_address(number), (uint8_t *)channel, CHANNEL_SIZE)) {
        return false;
    }
    channel_from_codeplug(channel);
    return true;
}

/*
 * ---- Writing channels, the VFOs ----
 * From codeplug.c codeplugChannelSaveDataForIndex(), codeplugAllChannelsIndexSetUsed(), codeplugGetVFO_ChannelData(),
 * codeplugSetVFO_ChannelData(). flash_write() erases a sector only when its data change.
 */

// RADIO_HARDWARE_FREQUENCY_BANDS (trx.c, not the datasheet ranges): VHF, 220 MHz, UHF, in 10 Hz
static const struct {
    uint32_t min;
    uint32_t max;
} bands[] = {
    {13700000, 18000000},
    {20000000, 27997500},
    {28000000, 52000000},
};

bool codeplug_frequency_is_valid(uint32_t frequency)
{
    for (unsigned i = 0; i < (sizeof(bands) / sizeof(bands[0])); i++) {
        if ((frequency >= bands[i].min) && (frequency <= bands[i].max)) {
            return true;
        }
    }
    return false;
}

bool codeplug_write_channel(int number, const codeplug_channel_t *channel)
{
    static codeplug_channel_t stored; // not on the UI task stack

    if ((number < CODEPLUG_CHANNELS_MIN) || (number > CODEPLUG_CHANNELS_MAX)) {
        return false;
    }
    channel_to_codeplug(channel, &stored);
    return flash_write(channel_address(number), (const uint8_t *)&stored, CHANNEL_SIZE);
}

// The bit of the channel in the table before its bank, then the tables read again
int codeplug_write_new_channel(const codeplug_channel_t *channel)
{
    int number = CODEPLUG_CHANNELS_MIN;

    while ((number <= CODEPLUG_CHANNELS_MAX) && codeplug_channel_in_use(number)) {
        number++;
    }
    if ((number > CODEPLUG_CHANNELS_MAX) || !codeplug_write_channel(number, channel)) {
        return 0;
    }

    int index = number - 1;
    uint32_t address = bank_header_address(index / CHANNELS_PER_BANK) + ((index % CHANNELS_PER_BANK) / 8);
    uint8_t byte = 0;

    if (!flash_read(address, &byte, 1)) {
        return 0;
    }
    byte |= (uint8_t)(1 << (index % 8));
    if (!flash_write(address, &byte, 1)) {
        return 0;
    }
    codeplug_init();
    return number;
}

bool codeplug_read_vfo(int vfo, codeplug_channel_t *channel)
{
    if ((vfo != CODEPLUG_VFO_A) && (vfo != CODEPLUG_VFO_B)) {
        return false;
    }

    bool ok = flash_read(ADDR_VFO_A + (vfo * CHANNEL_SIZE), (uint8_t *)channel, CHANNEL_SIZE);

    channel_from_codeplug(channel);
    channel->not_in_codeplug_flag = ((vfo == CODEPLUG_VFO_A) ? 0x01 : 0x03);
    if (!ok || !codeplug_frequency_is_valid(channel->rx_freq) || !codeplug_frequency_is_valid(channel->tx_freq)) {
        channel->mode = CODEPLUG_MODE_FM;
        channel->rx_freq = channel->tx_freq = VFO_DEFAULT_FREQUENCY;
        channel->rx_tone = channel->tx_tone = CODEPLUG_CSS_NONE;
        channel->sql = SQUELCH_DEFAULT;
        channel->vfo_flag5 &= (uint8_t)~VFO_STEP_MASK; // the first step, 2.5 kHz
    }
    return ok;
}

bool codeplug_write_vfo(int vfo, const codeplug_channel_t *channel)
{
    static codeplug_channel_t stored;

    if ((vfo != CODEPLUG_VFO_A) && (vfo != CODEPLUG_VFO_B)) {
        return false;
    }
    channel_to_codeplug(channel, &stored);
    return flash_write(ADDR_VFO_A + (vfo * CHANNEL_SIZE), (const uint8_t *)&stored, CHANNEL_SIZE);
}

/*
 * ---- Contacts, DTMF contacts, RX group lists ----
 * From codeplug.c codeplugInitContactsCache(), codeplugContactSaveDataForIndex(), codeplugRxGroupGetDataForIndex()...
 * Contacts: 24 bytes each from ADDR_CONTACTS in the flash part, free when the first byte of the name is 0xFF.
 * DTMF contacts: 32 bytes each in the EEPROM part, free when the first byte is 0xFF, or 0x00 (the last one held the
 * zone basic data in codeplugs from before 63 DTMF contacts). RX group lists: a byte a list from ADDR_RX_GROUP_LENGTH
 * (0: not in use), the lists of 80 bytes from ADDR_RX_GROUP.
 */

#define ADDR_CONTACTS        (FLASH_PART_OFFSET + 0x87620)
#define ADDR_RX_GROUP_LENGTH (FLASH_PART_OFFSET + 0x8D620)
#define ADDR_RX_GROUP        (FLASH_PART_OFFSET + 0x8D6A0)
#define ADDR_DTMF_CONTACTS   0x02F88
#define CONTACT_SIZE         24
#define CONTACT_HEADER_SIZE  21 // name, number, call type
#define CONTACTS_PER_READ    8  // the scan reads this many at once (192 bytes of stack)
#define DTMF_CONTACT_SIZE    32
#define RX_GROUP_SIZE        80
#define CONTACT_FREE         0xFFFFFFFFUL
#define CONTACT_TYPE_SHIFT   24
#define CONTACT_ID_MASK      0xFFFFFFUL

_Static_assert(offsetof(codeplug_contact_t, index) == CONTACT_SIZE, "a contact is 24 bytes in the codeplug");
_Static_assert(sizeof(codeplug_dtmf_contact_t) == DTMF_CONTACT_SIZE, "a DTMF contact is 32 bytes in the codeplug");
_Static_assert(offsetof(codeplug_rx_group_t, contact_count) == RX_GROUP_SIZE, "an RX group list is 80 bytes");

// A contact: its number | its call type << 24, CONTACT_FREE when free (4 KB; the original keeps 8 KB). The lists and
// the lookups of the callers use it instead of reading the flash
static uint32_t contacts[CODEPLUG_CONTACTS_MAX];
static uint16_t contact_counts[CODEPLUG_CALL_TYPES];
static uint64_t dtmf_in_use; // bit n: DTMF contact n + 1
static uint8_t rx_group_lengths[CODEPLUG_RX_GROUPS_MAX];

static uint32_t contact_entry(const codeplug_contact_t *contact)
{
    if (((uint8_t)contact->name[0] == 0xFF) || (contact->call_type >= CODEPLUG_CALL_TYPES)) {
        return CONTACT_FREE;
    }
    return (contact->id & CONTACT_ID_MASK) | ((uint32_t)contact->call_type << CONTACT_TYPE_SHIFT);
}

// Only the UI task changes the table, the DMR receive will read it: an entry and the counts change at once
static void set_entry(int index, uint32_t entry)
{
    uint32_t old = contacts[index - 1];

    taskENTER_CRITICAL();
    if (old != CONTACT_FREE) {
        contact_counts[old >> CONTACT_TYPE_SHIFT]--;
    }
    if (entry != CONTACT_FREE) {
        contact_counts[entry >> CONTACT_TYPE_SHIFT]++;
    }
    contacts[index - 1] = entry;
    taskEXIT_CRITICAL();
}

void codeplug_contacts_init(void)
{
    uint8_t buffer[CONTACTS_PER_READ * CONTACT_SIZE];

    memset(contact_counts, 0, sizeof(contact_counts));
    for (int first = 0; first < CODEPLUG_CONTACTS_MAX; first += CONTACTS_PER_READ) {
        if (!flash_read(ADDR_CONTACTS + (first * CONTACT_SIZE), buffer, sizeof(buffer))) {
            memset(buffer, 0xFF, sizeof(buffer));
        }
        for (int i = 0; i < CONTACTS_PER_READ; i++) {
            codeplug_contact_t contact;

            memcpy(&contact, &buffer[i * CONTACT_SIZE], CONTACT_HEADER_SIZE);
            contact.id = codeplug_bcd_to_int(swap32(contact.id));
            contacts[first + i] = contact_entry(&contact);
            if (contacts[first + i] != CONTACT_FREE) {
                contact_counts[contact.call_type]++;
            }
        }
    }

    dtmf_in_use = 0;
    for (int i = 0; i < CODEPLUG_DTMF_CONTACTS_MAX; i++) {
        uint8_t first_byte;

        if (flash_read(ADDR_DTMF_CONTACTS + (i * DTMF_CONTACT_SIZE), &first_byte, 1) && (first_byte != 0xFF) &&
            (first_byte != 0x00)) {
            dtmf_in_use |= (1ULL << i);
        }
    }

    if (!flash_read(ADDR_RX_GROUP_LENGTH, rx_group_lengths, sizeof(rx_group_lengths))) {
        memset(rx_group_lengths, 0, sizeof(rx_group_lengths));
    }
}

int codeplug_contact_count(int call_type)
{
    if (call_type < 0) {
        return contact_counts[CODEPLUG_CALL_GROUP] + contact_counts[CODEPLUG_CALL_PRIVATE] +
               contact_counts[CODEPLUG_CALL_ALL];
    }
    return (call_type < CODEPLUG_CALL_TYPES) ? contact_counts[call_type] : 0;
}

int codeplug_contact_at(int position, int call_type)
{
    for (int i = 0; (i < CODEPLUG_CONTACTS_MAX) && (position >= 0); i++) {
        if ((contacts[i] != CONTACT_FREE) &&
            ((call_type < 0) || ((int)(contacts[i] >> CONTACT_TYPE_SHIFT) == call_type)) && (position-- == 0)) {
            return i + 1;
        }
    }
    return 0;
}

bool codeplug_get_contact(int index, codeplug_contact_t *contact)
{
    if ((index < CODEPLUG_CONTACTS_MIN) || (index > CODEPLUG_CONTACTS_MAX) ||
        !flash_read(ADDR_CONTACTS + ((index - 1) * CONTACT_SIZE), (uint8_t *)contact, CONTACT_SIZE)) {
        return false;
    }
    contact->id = codeplug_bcd_to_int(swap32(contact->id));
    contact->index = index;
    return true;
}

int codeplug_contact_timeslot(const codeplug_contact_t *contact)
{
    if (contact->ts_override & CODEPLUG_CONTACT_NO_TS_OVERRIDE) {
        return 0;
    }
    return (contact->ts_override & CODEPLUG_CONTACT_TS2) ? 2 : 1;
}

int codeplug_contact_find(uint32_t id, int call_type, int timeslot)
{
    int first = 0;

    id &= CONTACT_ID_MASK;
    for (int i = 0; i < CODEPLUG_CONTACTS_MAX; i++) {
        uint32_t entry = contacts[i];

        if ((entry == CONTACT_FREE) || ((entry & CONTACT_ID_MASK) != id) ||
            ((id != CODEPLUG_CONTACT_ID_MAX) && ((int)(entry >> CONTACT_TYPE_SHIFT) != call_type))) {
            continue;
        }
        if (timeslot == 0) {
            return i + 1;
        }

        // Only that byte of the contact: the override is not in the table
        codeplug_contact_t c;

        if (flash_read(ADDR_CONTACTS + (i * CONTACT_SIZE) + offsetof(codeplug_contact_t, ts_override), &c.ts_override,
                       1) &&
            (codeplug_contact_timeslot(&c) == timeslot)) {
            return i + 1;
        }
        if (first == 0) {
            first = i + 1;
        }
    }
    return first;
}

int codeplug_contact_find_same(const codeplug_contact_t *contact, int except_index)
{
    uint32_t entry = contact_entry(contact);
    int timeslot = codeplug_contact_timeslot(contact);

    for (int i = 0; (i < CODEPLUG_CONTACTS_MAX) && (entry != CONTACT_FREE); i++) {
        codeplug_contact_t other;

        if ((contacts[i] == entry) && ((i + 1) != except_index) &&
            flash_read(ADDR_CONTACTS + (i * CONTACT_SIZE) + offsetof(codeplug_contact_t, ts_override),
                       &other.ts_override, 1) &&
            (codeplug_contact_timeslot(&other) == timeslot)) {
            return i + 1;
        }
    }
    return 0;
}

int codeplug_contact_free_index(void)
{
    for (int i = 0; i < CODEPLUG_CONTACTS_MAX; i++) {
        if (contacts[i] == CONTACT_FREE) {
            return i + 1;
        }
    }
    return 0;
}

bool codeplug_save_contact(int index, const codeplug_contact_t *contact)
{
    codeplug_contact_t stored;

    if ((index < CODEPLUG_CONTACTS_MIN) || (index > CODEPLUG_CONTACTS_MAX)) {
        return false;
    }
    memcpy(&stored, contact, CONTACT_SIZE);
    stored.id = swap32(int_to_bcd(contact->id & CONTACT_ID_MASK));
    if (!flash_write(ADDR_CONTACTS + ((index - 1) * CONTACT_SIZE), (const uint8_t *)&stored, CONTACT_SIZE)) {
        return false;
    }
    set_entry(index, contact_entry(contact));
    return true;
}

// As the main firmware: the name and the call type 0xFF, the number 0
bool codeplug_delete_contact(int index)
{
    codeplug_contact_t contact;

    memset(&contact, 0xFF, sizeof(contact));
    contact.id = 0;
    return codeplug_save_contact(index, &contact);
}

bool codeplug_contact_in_rx_group(int index)
{
    static codeplug_rx_group_t group; // 84 bytes: not on the stack (UI task only)

    for (int number = 1; number <= CODEPLUG_RX_GROUPS_MAX; number++) {
        if (!codeplug_get_rx_group(number, &group)) {
            continue;
        }
        for (int i = 0; i < group.contact_count; i++) {
            if (group.contacts[i] == index) {
                return true;
            }
        }
    }
    return false;
}

int codeplug_dtmf_contact_count(void)
{
    return __builtin_popcountll(dtmf_in_use);
}

bool codeplug_get_dtmf_contact(int position, codeplug_dtmf_contact_t *contact)
{
    for (int i = 0; (i < CODEPLUG_DTMF_CONTACTS_MAX) && (position >= 0); i++) {
        if (((dtmf_in_use >> i) & 1) && (position-- == 0)) {
            return flash_read(ADDR_DTMF_CONTACTS + (i * DTMF_CONTACT_SIZE), (uint8_t *)contact, DTMF_CONTACT_SIZE);
        }
    }
    memset(contact, 0xFF, sizeof(*contact));
    return false;
}

// The signalling DTMF settings (0x1400 in the EEPROM part), the durations at 0x72. The default codeplugs had 128 as the
// tail: above 1 s it is 500 ms; a rate out of 1 .. 10 is 2, as an unread one
#define ADDR_DTMF_DURATIONS (0x1400 + 0x72)
#define DTMF_TAIL_MAX       10
#define DTMF_TAIL_DEFAULT   5
#define DTMF_RATE_DEFAULT   2
#define DTMF_RATE_MAX       10

_Static_assert(sizeof(codeplug_dtmf_durations_t) == 5, "the DTMF durations are 5 bytes in the codeplug");

void codeplug_get_dtmf_durations(codeplug_dtmf_durations_t *durations)
{
    if (!flash_read(ADDR_DTMF_DURATIONS, (uint8_t *)durations, sizeof(*durations))) {
        memset(durations, 0, sizeof(*durations));
    }
    if (durations->tail > DTMF_TAIL_MAX) {
        durations->tail = DTMF_TAIL_DEFAULT;
    }
    if ((durations->rate < 1) || (durations->rate > DTMF_RATE_MAX)) {
        durations->rate = DTMF_RATE_DEFAULT;
    }
}

// codeplugAPRSGetDataForIndex(): valid when named and with the magic version
#define ADDR_APRS_CONFIGS  0x1588
#define APRS_MAGIC_VERSION 0x4152

_Static_assert(sizeof(codeplug_aprs_config_t) == 64, "an APRS configuration is 64 bytes in the codeplug");

bool codeplug_get_aprs_config(int index, codeplug_aprs_config_t *config)
{
    if ((index < 1) || (index > CODEPLUG_APRS_CONFIGS_MAX) ||
        !flash_read(ADDR_APRS_CONFIGS + ((index - 1) * sizeof(*config)), (uint8_t *)config, sizeof(*config))) {
        return false;
    }
    return ((uint8_t)config->name[0] != 0xFF) &&
           ((uint16_t)(config->magic[0] | (config->magic[1] << 8)) == APRS_MAGIC_VERSION);
}

bool codeplug_get_rx_group(int number, codeplug_rx_group_t *group)
{
    memset(group, 0, sizeof(*group));
    if ((number < 1) || (number > CODEPLUG_RX_GROUPS_MAX) || (rx_group_lengths[number - 1] == 0) ||
        !flash_read(ADDR_RX_GROUP + ((number - 1) * RX_GROUP_SIZE), (uint8_t *)group, RX_GROUP_SIZE)) {
        return false;
    }
    // The places after the last contact are 0
    while ((group->contact_count < CODEPLUG_RX_GROUP_CONTACTS) && (group->contacts[group->contact_count] != 0)) {
        group->contact_count++;
    }
    return true;
}
