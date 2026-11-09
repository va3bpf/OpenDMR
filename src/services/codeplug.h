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

#ifndef DM32_CODEPLUG_H
#define DM32_CODEPLUG_H

#include <stdbool.h>
#include <stdint.h>

// The codeplug (channels, zones...) in the data flash window, written by the CPS. Format of the GD-77 / OpenGD77
// codeplug as the main firmware reads it (OpenDM32 codeplug.c); names after docs/coding-style.md.

#define CODEPLUG_CHANNELS_MIN  1
#define CODEPLUG_CHANNELS_MAX  1024
#define CODEPLUG_ZONE_CHANNELS 80 // the most per zone (OpenGD77 format; 16 in the original one)
#define CODEPLUG_NAME_LENGTH   16
#define CODEPLUG_CSS_NONE      0xFFFF

// The codeplug knows FM (0) and DMR (1). AM, USB and LSB (receive only) are stored as an analog channel (0, what the
// CPS shows) with the mode in the byte the OpenGD77 format leaves unused (opendmr_mode: 0xA1 AM, 0xA2 USB, 0xA3 LSB,
// anything else FM)
typedef enum {
    CODEPLUG_MODE_FM = 0,
    CODEPLUG_MODE_DMR = 1,
    CODEPLUG_MODE_AM = 2,
    CODEPLUG_MODE_USB = 3,
    CODEPLUG_MODE_LSB = 4,
    CODEPLUG_MODES
} codeplug_mode_t;

static inline bool codeplug_mode_is_analog(uint8_t mode)
{
    return mode != CODEPLUG_MODE_DMR;
}

static inline bool codeplug_mode_is_ssb(uint8_t mode)
{
    return (mode == CODEPLUG_MODE_USB) || (mode == CODEPLUG_MODE_LSB);
}

// AM, USB and LSB are receive only: the FD6818 modulates FM only
static inline bool codeplug_mode_can_transmit(uint8_t mode)
{
    return (mode == CODEPLUG_MODE_FM) || (mode == CODEPLUG_MODE_DMR);
}

const char *codeplug_mode_name(uint8_t mode);  // "FM", "DMR", "AM", "USB", "LSB"
uint8_t codeplug_mode_step(uint8_t mode, int direction); // the mode key's order: FM, AM, USB, LSB, DMR, around

// A channel as stored (56 bytes), then two values worked out when it is read
typedef struct {
    char name[CODEPLUG_NAME_LENGTH]; // 0xFF filled
    uint32_t rx_freq;                // 10 Hz units once read (BCD in the flash)
    uint32_t tx_freq;
    uint8_t mode; // codeplug_mode_t once read
    uint8_t libre_dmr_power;
    uint8_t location_lat0;
    uint8_t tot; // 15 s units
    uint8_t location_lat1;
    uint8_t location_lat2;
    uint8_t location_lon0;
    uint8_t location_lon1;
    uint16_t rx_tone; // CTCSS: Hz * 10 once read; DCS: flags and octal digits; CODEPLUG_CSS_NONE
    uint16_t tx_tone;
    uint8_t location_lon2;
    uint8_t opendmr_mode; // unused in the OpenGD77 format: AM, USB, LSB (codeplug_mode_t comment); read: as stored
    uint8_t libre_dmr_flag1;
    uint8_t rx_signaling;
    uint8_t arts_interval;
    uint8_t encrypt;
    uint8_t unused2;
    uint8_t rx_group_list;
    uint8_t tx_colour;
    uint8_t aprs_config_index;
    uint16_t contact;
    uint8_t flag1;
    uint8_t flag2; // 0x40: timeslot 2
    uint8_t flag3;
    uint8_t flag4; // 0x80 power, 0x40 VOX, 0x04 Rx only, 0x02 bandwidth 25 kHz, 0x01 squelch
    uint16_t vfo_offset_freq;
    uint8_t vfo_flag5;
    uint8_t sql;
    uint8_t not_in_codeplug_flag; // 0x01: a VFO
    int not_in_codeplug_distance_x10;
} codeplug_channel_t;

typedef struct {
    char name[CODEPLUG_NAME_LENGTH];
    uint16_t channels[CODEPLUG_ZONE_CHANNELS]; // channel numbers (1 ..), 0 after the last one
    int channel_count;                         // not in the codeplug
    int index;                                 // place in the zone list; -1: the virtual "all channels" zone
} codeplug_zone_t;

void codeplug_init(void); // reads the zone and channel use tables

void codeplug_name_to_string(const char *name, char *out, int length); // 0xFF filled name, out: length + 1 chars
uint32_t codeplug_bcd_to_int(uint32_t bcd);

void codeplug_get_radio_name(char *out); // 9 chars
uint32_t codeplug_get_dmr_id(void);
bool codeplug_set_dmr_id(uint32_t id);

int codeplug_zone_count(void); // the zones of the codeplug, then "all channels"
bool codeplug_get_zone(int number, codeplug_zone_t *zone);

// The channel last used in a zone: its position in the zone (0 ..); in "all channels" (index -1) its number (1 ..)
int codeplug_get_last_used_channel(int zone_index);

int codeplug_channel_count(void);
bool codeplug_channel_in_use(int number);
bool codeplug_get_channel(int number, codeplug_channel_t *channel);

// ---- Contacts, DTMF contacts, RX group lists ----
// Contacts (1 - 1024) in the flash part, their numbers and call types kept in RAM for the lists and the lookups;
// DTMF contacts (1 - 63) in the EEPROM part; RX group lists (1 - 76) of up to 32 contact numbers.

#define CODEPLUG_CONTACTS_MIN      1
#define CODEPLUG_CONTACTS_MAX      1024
#define CODEPLUG_DTMF_CONTACTS_MAX 63
#define CODEPLUG_DTMF_CODE_LENGTH  16
#define CODEPLUG_RX_GROUPS_MAX     76
#define CODEPLUG_RX_GROUP_CONTACTS 32
#define CODEPLUG_CONTACT_ID_MIN    1
#define CODEPLUG_CONTACT_ID_MAX    0xFFFFFF // 16777215, also the number of the all call
#define CODEPLUG_CONTACT_ID_DIGITS 8

typedef enum {
    CODEPLUG_CALL_GROUP = 0,
    CODEPLUG_CALL_PRIVATE = 1,
    CODEPLUG_CALL_ALL = 2,
    CODEPLUG_CALL_TYPES
} codeplug_call_type_t;

// ts_override of a contact: bit 0 set: none, else bit 1 is the timeslot - 1
#define CODEPLUG_CONTACT_NO_TS_OVERRIDE 0x01
#define CODEPLUG_CONTACT_TS2            0x02

// A contact as stored (24 bytes), then its number
typedef struct {
    char name[CODEPLUG_NAME_LENGTH]; // 0xFF filled
    uint32_t id;                     // talk group or DMR ID once read (byte swapped BCD in the flash)
    uint8_t call_type;               // codeplug_call_type_t; 0xFF: free
    uint8_t rx_tone;
    uint8_t ring_style;
    uint8_t ts_override;
    int index; // 1 .. CODEPLUG_CONTACTS_MAX; not in the codeplug
} codeplug_contact_t;

typedef struct {
    char name[CODEPLUG_NAME_LENGTH];
    uint8_t code[CODEPLUG_DTMF_CODE_LENGTH]; // 0 - 15: "0123456789ABCD*#", 0xFF after the last one
} codeplug_dtmf_contact_t;

typedef struct {
    char name[CODEPLUG_NAME_LENGTH];
    uint16_t contacts[CODEPLUG_RX_GROUP_CONTACTS]; // contact numbers, 0 after the last one
    int contact_count;                             // not in the codeplug
} codeplug_rx_group_t;

void codeplug_contacts_init(void); // reads the contact numbers and types (codeplug_init() calls it)

int codeplug_contact_count(int call_type);            // of a codeplug_call_type_t; -1: all
int codeplug_contact_at(int position, int call_type); // number of the position-th (0 ..) contact of a type; 0: none
bool codeplug_get_contact(int index, codeplug_contact_t *contact);
// The contact with this number and call type (all call: any type); timeslot 1 / 2: the one with that override if
// there is one, 0: the first one. Returns its index, 0: none
int codeplug_contact_find(uint32_t id, int call_type, int timeslot);
int codeplug_contact_timeslot(const codeplug_contact_t *contact); // its override: 1, 2; 0: none
// Another contact than except_index with the same number, call type and timeslot override (a duplicate: the same
// number may be there three times, without override, for TS1 and for TS2); 0: none
int codeplug_contact_find_same(const codeplug_contact_t *contact, int except_index);
int codeplug_contact_free_index(void); // 0: the list is full
bool codeplug_save_contact(int index, const codeplug_contact_t *contact);
bool codeplug_delete_contact(int index);
bool codeplug_contact_in_rx_group(int index); // used by an RX group list (then it can't be deleted)

int codeplug_dtmf_contact_count(void);
bool codeplug_get_dtmf_contact(int position, codeplug_dtmf_contact_t *contact); // position 0 .. count - 1

// The DTMF timing of the signalling settings (codeplugGetSignallingDTMFDurations()), 100 ms units but the rate
typedef struct {
    uint8_t first_digit_delay;
    uint8_t first_duration; // added to the first digit
    uint8_t other_duration; // added to * and #
    uint8_t rate;           // digits a second, 1 .. 10
    uint8_t tail;           // after the last one (libreDMR_Tail)
} codeplug_dtmf_durations_t;

void codeplug_get_dtmf_durations(codeplug_dtmf_durations_t *durations);

bool codeplug_get_rx_group(int number, codeplug_rx_group_t *group); // number 1 .. CODEPLUG_RX_GROUPS_MAX

// The APRS configurations of the CPS (codeplugAPRS_Config_t, 64 bytes, up to 8 from 0x1588 of the EEPROM part); a
// channel's aprs_config_index (1 ..) names one. Position fixed point 24 bits: sign bit 23, degrees 22:15, 1/10000 below
#define CODEPLUG_APRS_CONFIGS_MAX 8

typedef struct {
    char name[6];
    uint8_t ssid;
} codeplug_aprs_path_t;

typedef struct {
    char name[8];
    uint8_t sender_ssid;
    uint8_t latitude[3];
    uint8_t longitude[3];
    codeplug_aprs_path_t paths[2];
    uint8_t icon_table; // 0: '/', else '\'
    uint8_t icon_index; // the symbol - '!'
    char comment[24];
    uint8_t reserved[6];
    uint8_t flags;      // bit 0: 300 baud, bit 1: the fixed position above, 7:4 the position ambiguity
    uint8_t magic[2];   // little endian APRS magic version
} codeplug_aprs_config_t;

bool codeplug_get_aprs_config(int index, codeplug_aprs_config_t *config); // index 1 .. CODEPLUG_APRS_CONFIGS_MAX

// Writing channels and the two VFOs back to the codeplug (OpenDM32 codeplug.c
// codeplugChannelSaveDataForIndex(), codeplugGetVFO_ChannelData(), codeplugSetVFO_ChannelData()). The channels are
// given as codeplug_get_channel() reads them (frequencies in 10 Hz, tones as numbers); they are stored in the codeplug
// format (BCD).

#define CODEPLUG_VFO_A 0
#define CODEPLUG_VFO_B 1

// An existing channel (1 ..); the channels in use table is not changed
bool codeplug_write_channel(int number, const codeplug_channel_t *channel);

// A new channel in the first free number, marked in use (not added to a zone); its number, 0: none free / error
int codeplug_write_new_channel(const codeplug_channel_t *channel);

// The VFOs are stored as channels after the "EEPROM" channels. A VFO out of the bands is read as 144 MHz FM (the main
// firmware's settingsVFOSanityCheck()).
bool codeplug_read_vfo(int vfo, codeplug_channel_t *channel);
bool codeplug_write_vfo(int vfo, const codeplug_channel_t *channel);

// The frequency (10 Hz) is in one of the bands of the radio (RADIO_HARDWARE_FREQUENCY_BANDS of the main firmware)
bool codeplug_frequency_is_valid(uint32_t frequency);

#endif
