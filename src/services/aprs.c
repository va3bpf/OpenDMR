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
 * OpenDMR, service: APRS position beacons (aprs.h), from the main firmware's functions/aprs.c (OpenGD77,
 * VK3KYY / F1RMB): the AX.25 encoder (NRZI, bit stuffing, CRC), the position (plain or compressed, ambiguity, course
 * and speed), aprsBeaconingTick() (each second: the position, then the auto interval with its decay or the smart
 * beaconing), aprsBeaconingSendBeacon(). The frame goes to the radio task (radio_aprs_send()), with the frequency the
 * beacon is sent on when the configuration has one: the task tunes FM to it for the frame and goes back to the
 * channel the user selected. An APRS-over-DMR report goes to the radio task too (radio_data_send()), only on a DMR
 * channel. The settings are the APRS options' (settings.aprs_beaconing_settings_part1 / 2); the
 * keys and the PTT mode only ask (aprs_toggle(), aprs_send(), aprs_after_ptt()), the input task does the rest.
 */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include "settings.h"
#include "systime.h"
#include "codeplug.h"
#include "radio.h"
#include "gps.h"
#include "sound.h"
#include "ui.h"
#include "dmr.h"
#include "aprs_dmr.h"
#include "p25_radio.h"
#include "nxdn_radio.h"
#include "hotspot.h"
#include "tones.h"
#include "aprs.h"
#include "extra_options.h"

#define DESTINATION        "APOG77"
#define BUFFER_SIZE        256
#define CHECK_MS           1000
#define SPEED_MIN_CKN      54  // SMART_BEACONING_SPEED_MIN: more than 1 km/h
#define GPS_SPEED_MIN_CKN  54  // GPS_SPEED_THRESHOLD_MIN as used for the speed sent
#define DECAY_MULT_MIN     1
#define DECAY_MULT_STEP    2
#define DECAY_MULT_MAX     32
#define DECAY_RESET_M      15.0
#define BEARING_INVALID    UINT16_MAX
#define LOCATION_UNSET     0x7F000000UL // settings.location_lat never set
#define APRS_NAME_LENGTH   8             // codeplug_aprs_config_t.name
#define FLAG4_RX_ONLY      0x04          // codeplug.h: the channel receives only

#define DEG_TO_RAD   0.017453292519943295
#define RAD_TO_DEG   57.29577951308232
#define MPS_PER_KNOT 0.51444444
#define MPS_TO_MPH   2.23693629
#define MPS_PER_KMPH 0.277778

// The modes and the state bits of the settings (aprsBeaconingMode_t, aprsBeaconingStates_t)
enum { MODE_OFF, MODE_MANUAL, MODE_PTT, MODE_AUTO, MODE_SMART };
#define STATE_LOCATION_FROM_GPS 0x04
#define STATE_DECAY             0x08
#define STATE_COMPRESSED        0x10

static const uint16_t intervals_s[10] = {12, 30, 60, 120, 180, 300, 600, 1200, 1800, 3600};

typedef struct {
    uint32_t time; // ms
    uint16_t bearing; // 1/100 degree, BEARING_INVALID: no location
    uint32_t speed; // 1/100 knot
    double latitude;
    double longitude;
} location_t;

typedef struct {
    uint8_t mode;
    uint8_t state;
    uint8_t interval;
    uint8_t low_speed;  // km/h
    uint8_t high_speed; // km/h
    uint8_t slow_rate;  // min
    uint8_t fast_rate;  // s
    uint8_t turn_angle; // degrees
    uint8_t turn_slope;
    uint8_t turn_time;  // s
} beaconing_t;

static struct {
    beaconing_t s;
    bool enabled;     // APRS_BEACONING_STATE_ENABLED: on, not suspended
    bool has_config;
    bool has_fix;
    int config_index; // of the channel's configuration read
    codeplug_aprs_config_t config;
    location_t current;
    location_t previous;
    double bearing_lat; // APRS_USE_COURSETO_FOR_BEARING
    double bearing_lon;
    uint16_t course;
    double fixed_lat;   // NAN: not known yet
    double fixed_lon;
    uint32_t check_ms;
    uint32_t next_beacon_ms;
    bool next_beacon_running;
    uint8_t decay;
    bool trigger;
    bool waiting;       // an AFSK frame is with the radio task
    radio_request_t request; // ... its request (radio_result())
    bool waiting_auto;  // ... sent by the auto modes (the trigger stays on a failure)
    bool dmr_waiting;   // an LRRP report is with the radio task (the DMR engine)
    radio_request_t dmr_request; // ... its request (radio_result())
    bool dmr_ok;        // ... it went out
    location_t sending; // the location of that frame
} aprs = {.fixed_lat = NAN, .fixed_lon = NAN, .decay = DECAY_MULT_MIN};

static volatile bool toggle_asked = false;
static volatile bool send_asked = false;
static volatile bool ptt_asked = false;
static volatile bool satellite_asked = false;    // aprs_send_satellite()
static char satellite_asked_name[APRS_NAME_LENGTH + 1]; // ... its satellite

/* AX.25 (enqueue...() of aprs.c) */

static struct {
    uint8_t buffer[BUFFER_SIZE];
    uint16_t bit;
    uint16_t stuffing;
    uint16_t crc;
    bool nrzi;
} ax25;

static void put_bit(bool one)
{
    if ((ax25.bit / 8U) >= BUFFER_SIZE) {
        return;
    }
    if (one) {
        ax25.buffer[ax25.bit / 8U] |= (uint8_t)(1U << (ax25.bit % 8U));
    }
    ax25.bit++;
}

static void update_crc(bool bit)
{
    uint16_t x = (uint16_t)(ax25.crc ^ bit);

    ax25.crc >>= 1;
    if (x & 0x01) {
        ax25.crc ^= 0x8408;
    }
}

static void put_char(uint8_t data, bool stuffing)
{
    for (int i = 0; i < 8; i++) {
        bool bit = (data & 0x01);

        update_crc(bit);
        if (bit) {
            put_bit(ax25.nrzi);
            ax25.stuffing++;
            if (stuffing && (ax25.stuffing == 5)) {
                ax25.nrzi = !ax25.nrzi;
                put_bit(ax25.nrzi);
                ax25.stuffing = 0;
            }
        } else {
            ax25.nrzi = !ax25.nrzi;
            put_bit(ax25.nrzi);
            ax25.stuffing = 0;
        }
        data >>= 1;
    }
}

static void put_string(const char *s)
{
    while (*s) {
        put_char((uint8_t)*s++, true);
    }
}

static void put_flags(int count)
{
    for (int i = 0; i < count; i++) {
        put_char(0x7E, false);
    }
}

// An address field: up to 6 characters shifted left, padded with spaces, then the SSID byte. The name ends at a 0 or
// a 0xFF (the CPS fills its unused bytes with either)
static void put_address(const char *name, size_t max, uint8_t ssid, bool last)
{
    size_t length = 0;

    while ((length < max) && (length < 6) && (name[length] != 0) && ((uint8_t)name[length] != 0xFF)) {
        length++;
    }
    for (size_t i = 0; i < length; i++) {
        put_char((uint8_t)(name[i] << 1), true);
    }
    for (size_t i = length; i < 6; i++) {
        put_char((uint8_t)(' ' << 1), true);
    }
    put_char((uint8_t)(((ssid + '0') << 1) + (last ? 1 : 0)), true);
}

// A digipeater path of the configuration: its name not empty (0 or 0xFF, as the CPS leaves it)
static bool path_used(const codeplug_aprs_path_t *path)
{
    return (path->name[0] != 0) && ((uint8_t)path->name[0] != 0xFF);
}

// Destination, source, then the paths used (none: the source is the last address; the main firmware sent an address
// of spaces then, and took a path 2 of 0xFF for one)
static void put_header(const char *call)
{
    const codeplug_aprs_config_t *c = &aprs.config;
    const codeplug_aprs_path_t *paths[2];
    int count = 0;

    for (int p = 0; p < 2; p++) {
        if (path_used(&c->paths[p])) {
            paths[count++] = &c->paths[p];
        }
    }
    for (size_t i = 0; i < strlen(DESTINATION); i++) {
        put_char((uint8_t)(DESTINATION[i] << 1), true);
    }
    put_char((uint8_t)('0' << 1), true);
    put_address(call, 6, c->sender_ssid, count == 0);
    for (int p = 0; p < count; p++) {
        put_address(paths[p]->name, sizeof(paths[p]->name), paths[p]->ssid, (p == (count - 1)));
    }
    put_char(0x03, true);
    put_char(0xF0, true);
}

// The QSY information ("Transmit QSY", flags bit 2; APRS spec aprs.org/info/freqspec.txt, as OpenGD77 sends it):
// the voice frequency of the channel the station is on, so that others can call it there: "FFF.FFFMHz Tnnn +ooo"
// first in the comment. Tnnn: the channel's Tx tone (CTCSS in whole Hz, Dnnn: DCS, Toff: none), lower case on a
// narrow channel; +ooo: the Tx offset in 10 kHz units (left out beyond 9.99 MHz, a cross band split). False: no
// QSY (not ticked, or no channel)
static bool qsy_text(char *out, size_t size)
{
    codeplug_channel_t channel;
    char tone[8];
    int32_t offset;
    int n;

    if (!(aprs.config.flags & 0x04) || !radio_get_channel(&channel) || (channel.rx_freq == 0) ||
        (channel.rx_freq >= 100000000UL)) {
        return false;
    }
    if (channel.tx_tone == CODEPLUG_CSS_NONE) {
        snprintf(tone, sizeof(tone), "Toff");
    } else if (channel.tx_tone & TONES_DCS) {
        snprintf(tone, sizeof(tone), "D%03X", (unsigned)(channel.tx_tone & TONES_DCS_CODE));
    } else {
        snprintf(tone, sizeof(tone), "T%03u", (unsigned)(channel.tx_tone / 10U));
    }
    if (!(channel.flag4 & 0x02)) {
        tone[0] = (char)(tone[0] - 'A' + 'a'); // narrow
    }
    n = snprintf(out, size, "%3u.%03uMHz %s", (unsigned)(channel.rx_freq / 100000UL),
                 (unsigned)((channel.rx_freq / 100UL) % 1000UL), tone);
    offset = ((int32_t)channel.tx_freq - (int32_t)channel.rx_freq) / 1000; // 10 Hz to 10 kHz
    if ((n > 0) && ((size_t)n < size) && (offset >= -999) && (offset <= 999)) {
        snprintf(out + n, size - (size_t)n, " %c%03ld", (offset < 0) ? '-' : '+',
                 (long)((offset < 0) ? -offset : offset));
    }
    return true;
}

static void put_payload(const char *lat, const char *lon, const char *course_speed)
{
    const codeplug_aprs_config_t *c = &aprs.config;
    char qsy[24];
    bool has_qsy = qsy_text(qsy, sizeof(qsy));
    uint8_t symbol = (uint8_t)(c->icon_index + '!');
    uint8_t table = (c->icon_table == 0) ? '/' : '\\';
    char comment[sizeof(c->comment) + 1];

    put_char('!', true);
    if (aprs.s.state & STATE_COMPRESSED) {
        put_char(table, true);
        put_string(lat);
        put_string(lon);
        put_char(symbol, true);
        put_string(course_speed ? course_speed : "  ");
        put_char(course_speed ? (0x3E + '!') : '!', true);
    } else {
        put_string(lat);
        put_char(table, true);
        put_string(lon);
        put_char(symbol, true);
        if (course_speed) {
            put_string(course_speed);
        }
    }
    memcpy(comment, c->comment, sizeof(c->comment));
    comment[sizeof(c->comment)] = 0;
    for (size_t i = 0; i < sizeof(c->comment); i++) {
        if ((uint8_t)comment[i] == 0xFF) {
            comment[i] = 0;
        }
    }
    if (has_qsy) {
        put_string(qsy);
        if (comment[0] != 0) {
            put_char(' ', true);
        }
    }
    put_string(comment);
}

static void put_crc(void)
{
    uint8_t lo = (uint8_t)(ax25.crc ^ 0xFF);
    uint8_t hi = (uint8_t)((ax25.crc >> 8) ^ 0xFF);

    put_char(lo, true);
    put_char(hi, true);
}

/* Positions */

static double fixed24(const uint8_t v[3])
{
    uint32_t f = v[0] | (v[1] << 8) | ((uint32_t)v[2] << 16);
    double value = ((f & 0x7FFFFF) >> 15) + ((f & 0x7FFF) / 10000.0);

    return (f & 0x800000) ? -value : value;
}

static double fixed32(uint32_t f)
{
    double value = ((f & 0x7FFFFFFF) >> 23) + ((f & 0x7FFFFF) / 100000.0);

    return (f & 0x80000000) ? -value : value;
}

static bool location_valid(const location_t *l)
{
    return l->bearing != BEARING_INVALID;
}

static void location_invalidate(location_t *l)
{
    memset(l, 0, sizeof(*l));
    l->bearing = BEARING_INVALID;
}

// aprsGetFixedPositionLatitude() / Longitude(): the configuration's, else the settings' (NAN: none)
static void fixed_position(double *lat, double *lon)
{
    if (isnan(aprs.fixed_lat)) {
        if (aprs.has_config && (aprs.config.flags & 0x02)) {
            aprs.fixed_lat = fixed24(aprs.config.latitude);
            aprs.fixed_lon = fixed24(aprs.config.longitude);
        } else if (settings.location_lat != LOCATION_UNSET) {
            aprs.fixed_lat = fixed32(settings.location_lat);
            aprs.fixed_lon = fixed32(settings.location_lon);
        }
    }
    *lat = aprs.fixed_lat;
    *lon = aprs.fixed_lon;
}

static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double delta = (lon1 - lon2) * DEG_TO_RAD;
    double sdlon = sin(delta);
    double cdlon = cos(delta);

    lat1 *= DEG_TO_RAD;
    lat2 *= DEG_TO_RAD;

    double slat1 = sin(lat1);
    double clat1 = cos(lat1);
    double slat2 = sin(lat2);
    double clat2 = cos(lat2);

    delta = (clat1 * slat2) - (slat1 * clat2 * cdlon);
    delta = delta * delta;
    delta += (clat2 * sdlon) * (clat2 * sdlon);
    delta = sqrt(delta);
    return atan2(delta, (slat1 * slat2) + (clat1 * clat2 * cdlon)) * 6372795;
}

static double course_to(double lat1, double lon1, double lat2, double lon2)
{
    double dlon = (lon2 - lon1) * DEG_TO_RAD;

    lat1 *= DEG_TO_RAD;
    lat2 *= DEG_TO_RAD;

    double a1 = sin(dlon) * cos(lat2);
    double a2 = cos(lat1) * sin(lat2) - (sin(lat1) * cos(lat2) * cos(dlon));

    a2 = atan2(a1, a2);
    if (a2 < 0.0) {
        a2 += 2 * M_PI;
    }
    return a2 * RAD_TO_DEG;
}

// aprsBeaconingGetBearing(): the course from the position 2 m away or more
static uint16_t bearing(void)
{
    if (distance_m(aprs.bearing_lat, aprs.bearing_lon, aprs.current.latitude, aprs.current.longitude) > 2.0) {
        aprs.course =
            (uint16_t)(course_to(aprs.bearing_lat, aprs.bearing_lon, aprs.current.latitude, aprs.current.longitude) * 1E2);
        aprs.bearing_lat = aprs.current.latitude;
        aprs.bearing_lon = aprs.current.longitude;
    }
    return aprs.course;
}

static void compress(double value, double scale, double offset, char *out)
{
    int r = (int)round(scale * (offset + value));

    out[0] = (char)((r / (91 * 91 * 91)) + '!');
    r %= (91 * 91 * 91);
    out[1] = (char)((r / (91 * 91)) + '!');
    r %= (91 * 91);
    out[2] = (char)((r / 91) + '!');
    out[3] = (char)((r % 91) + '!');
    out[4] = 0;
}

/* The frame (aprsSendPacket()) */

static uint16_t build_frame(const location_t *previous, const location_t *current)
{
    static const double ambiguity_coeff[8] = {1, 5000, 1000, 500, 100, 50, 10, 5};
    const codeplug_aprs_config_t *c = &aprs.config;
    bool gps_valid = (aprs.s.state & STATE_LOCATION_FROM_GPS) && location_valid(previous) && location_valid(current);
    int ambiguity = (c->flags >> 4) & 0x07;
    char call[16];
    char lat_text[16];
    char lon_text[16];
    char course_speed[16];
    bool has_course = false;
    double lat;
    double lon;

    memset(&ax25, 0, sizeof(ax25));
    codeplug_get_radio_name(call);
    call[6] = 0;

    if (c->flags & 0x02) {
        lat = fixed24(c->latitude);
        lon = fixed24(c->longitude);
    } else {
        lat = current->latitude;
        lon = current->longitude;
    }
    if (ambiguity != 0) {
        lat = (int)(lat * ambiguity_coeff[ambiguity]) / ambiguity_coeff[ambiguity];
        lon = (int)(lon * ambiguity_coeff[ambiguity]) / ambiguity_coeff[ambiguity];
    }

    if (aprs.s.state & STATE_COMPRESSED) {
        compress(-lat, 380926.0, 90.0, lat_text);
        compress(lon, 190463.0, 180.0, lon_text);
        if (gps_valid && (current->speed > 0)) {
            int speed = (int)(current->speed / 100);
            int c4 = (int)(current->bearing / 100) / 4;

            c4 = (c4 < 0) ? (c4 + 90) : ((c4 >= 90) ? (c4 - 90) : c4);
            course_speed[0] = (char)(c4 + '!');
            course_speed[1] = (char)((int)round(log(speed + 1.0) / log(1.08)) + '!');
            course_speed[2] = 0;
            has_course = true;
        }
    } else {
        bool north = (lat >= 0);
        bool east = (lon >= 0);
        double alat = fabs(lat);
        double alon = fabs(lon);
        int lat_deg = (int)alat;
        double lat_min = (alat - lat_deg) * 60.0;
        int lon_deg = (int)alon;
        double lon_min = (alon - lon_deg) * 60.0;

        snprintf(lat_text, sizeof(lat_text), "%02d%02d.%02d%c", lat_deg, (int)lat_min,
                 (int)round((lat_min - (int)lat_min) * 1E2), north ? 'N' : 'S');
        snprintf(lon_text, sizeof(lon_text), "%03d%02d.%02d%c", lon_deg, (int)lon_min,
                 (int)round((lon_min - (int)lon_min) * 1E2), east ? 'E' : 'W');
        if (gps_valid && (current->speed > 0)) {
            snprintf(course_speed, sizeof(course_speed), "%03u/%03u", (unsigned)(current->bearing / 100),
                     (unsigned)(current->speed / 100));
            has_course = true;
        }
    }

    put_flags(32); // ~213 ms of flags: 16 flags (107 ms) cut the beginning at some receivers (tested on the air)
    ax25.crc = 0xFFFF; // the CRC over the data only
    put_header(call);
    put_payload(lat_text, lon_text, has_course ? course_speed : NULL);
    put_crc();
    put_flags(3);
    return ax25.bit;
}

/* Beaconing */

static void read_settings(void)
{
    uint32_t p0 = settings.aprs_beaconing_settings_part1[0];
    uint32_t p1 = settings.aprs_beaconing_settings_part1[1];
    uint16_t p2 = settings.aprs_beaconing_settings_part2;

    aprs.s.mode = (p0 >> 29) & 0x07;
    aprs.s.low_speed = (p0 >> 24) & 0x1F;
    aprs.s.fast_rate = (p0 >> 16) & 0xFF;
    aprs.s.turn_slope = (p0 >> 8) & 0xFF;
    aprs.s.turn_time = p0 & 0xFF;
    aprs.s.slow_rate = (p1 >> 14) & 0x7F;
    aprs.s.high_speed = (p1 >> 7) & 0x7F;
    aprs.s.turn_angle = p1 & 0x7F;
    aprs.s.state = (p2 >> 4) & 0xFF;
    aprs.s.interval = (p2 & 0x0F) % 10;
}

static uint32_t interval_ms(void)
{
    return intervals_s[aprs.s.interval] * 1000U;
}

// aprsBeaconingResetTimers(): on a channel change or a new mode
static void reset(int config_index)
{
    aprs.decay = DECAY_MULT_MIN;
    location_invalidate(&aprs.current);
    location_invalidate(&aprs.previous);
    aprs.fixed_lat = NAN;
    aprs.fixed_lon = NAN;
    aprs.check_ms = systime_get_ms();
    aprs.next_beacon_running = false;
    aprs.trigger = false;
    aprs.config_index = config_index;
    aprs.has_config = (config_index != 0) && codeplug_get_aprs_config(config_index, &aprs.config);
}

static bool gps_on(void)
{
    return settings_valid() && (settings.gps >= GPS_MODE_ON);
}

static bool position_valid(void)
{
    double lat;
    double lon;

    if (aprs.s.state & STATE_LOCATION_FROM_GPS) {
        return aprs.has_fix; // the GPS's position, not the fixed one of the channel / settings
    }
    fixed_position(&lat, &lon);
    return !isnan(lat) && !isnan(lon);
}

// The frequency the beacon is sent on (OpenGD77's "Transmit frequency", the CPS's "Frec TX"), 10 Hz units; 0: not set.
// Codeplugs not written by our CPS may leave 0xFFFFFFFF there: take it as 0, the channel's frequency.
static uint32_t config_frequency(const codeplug_aprs_config_t *c)
{
    uint32_t frequency = (uint32_t)c->beacon_freq[0] | ((uint32_t)c->beacon_freq[1] << 8) |
                         ((uint32_t)c->beacon_freq[2] << 16) | ((uint32_t)c->beacon_freq[3] << 24);

    return (frequency == 0xFFFFFFFFu) ? 0 : frequency;
}

// A call being received on a DMR, P25 or NXDN channel: the beacon waits (going to FM for it would cut the call)
static bool digital_call_active(void)
{
    codeplug_channel_t channel;
    dmr_call_t call;
    p25_call_t p25;
    nxdn_call_t nxdn;

    if (!radio_get_channel(&channel) || (channel.mode == CODEPLUG_MODE_FM)) {
        return false;
    }
    if (radio_is_receiving()) {
        return true;
    }
    if (channel.mode == CODEPLUG_MODE_DMR) {
        return dmr_call(&call);
    }
    if (channel.mode == CODEPLUG_MODE_P25) {
        return p25_radio_call(&p25) && p25.active;
    }
    if (channel.mode == CODEPLUG_MODE_NXDN) {
        return nxdn_radio_call(&nxdn) && nxdn.active;
    }
    return false;
}

/* APRS over DMR: the LRRP report (aprs_dmr.c builds it, dmr.c hands it to the engine) as a data burst of the radio
   task (radio_data_send()): it calls these in its task once the DMR channel is free */

static struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint16_t speed_knots;
    uint16_t course;
    uint32_t gateway;
    uint32_t own_id;
} dmr_report; // in use until its result isn't RADIO_PENDING

static bool dmr_report_start(void)
{
    // A voice transmission may have left the engine in the failed wake state: reset it, or the report never starts
    if (dmr_repeater_failed()) {
        dmr_aprs_cancel();
    }
    return dmr_aprs_send_position(dmr_report.latitude_e7, dmr_report.longitude_e7, dmr_report.speed_knots,
                                  dmr_report.course, dmr_report.gateway, dmr_report.own_id);
}

static bool dmr_report_failed(void)
{
    return dmr_aprs_failed() || dmr_repeater_failed();
}

static const radio_data_t DMR_REPORT = {dmr_report_start, dmr_aprs_busy, dmr_report_failed, dmr_aprs_cancel};

// aprsBeaconingSendBeacon(): true when the frame went to the radio task. forced: SK1 + 3 (or the menu item): it sends
// in any mode but OFF, not only in Manual / PTT, and the caller gives the error tone (once)
static bool send_beacon(bool forced)
{
    bool manual = forced || (aprs.s.mode == MODE_PTT) || (aprs.s.mode == MODE_MANUAL);
    uint8_t via = extra_options_aprs_via();
    bool want_afsk = (via != APRS_VIA_DMR);
    bool want_dmr = (via != APRS_VIA_AFSK);
    bool afsk_sent = false;
    bool dmr_sent = false;
    codeplug_channel_t channel;
    uint32_t frequency = 0;
    bool afsk_possible;
    uint16_t bits;

    if (radio_is_scanning() || !radio_get_channel(&channel) || radio_is_transmitting() || hotspot_active() ||
        (aprs.s.mode == MODE_OFF) || !aprs.enabled || aprs.waiting || aprs.dmr_waiting) {
        return false;
    }
    // A receive-only channel never beacons, and a channel opts in by naming an APRS configuration.
    if ((channel.flag4 & FLAG4_RX_ONLY) || !aprs.has_config) {
        return false;
    }
    if (!forced && (aprs.s.mode == MODE_PTT) && aprs.next_beacon_running &&
        ((int32_t)(systime_get_ms() - aprs.next_beacon_ms) < 0)) {
        return false; // not yet
    }
    if (manual && !position_valid()) {
        if (!forced) {
            sound_play(SOUND_ERROR); // "APRS?" / "Location?" of the main firmware
        }
        return false;
    }
    // A DMR or P25 call being received: not now, the beacon goes out on the next round (no call cut)
    if (digital_call_active()) {
        return false;
    }
    // PTT mode beacons go out on the channel after the transmission, so the configuration's frequency does not apply
    // there (as OpenGD77). In the other modes the beacon goes out in FM on that frequency when the configuration has
    // one, whatever the channel is: the radio task tunes to it for the frame and comes back, without touching the
    // channel the user selected. On an FM channel without one it goes out on the channel's frequency; another mode
    // needs the configuration's frequency.
    if (aprs.s.mode != MODE_PTT) {
        frequency = config_frequency(&aprs.config);
        afsk_possible = (frequency != 0) || (channel.mode == CODEPLUG_MODE_FM);
    } else {
        afsk_possible = (channel.mode == CODEPLUG_MODE_FM);
    }

    // AFSK: the frame where the rules above say (the beacon's frequency, or an FM channel). The LRRP report: on a
    // DMR channel, no retune. "Both" uses each path the selected channel can take.
    if (want_afsk && afsk_possible) {
        bits = build_frame(&aprs.previous, &aprs.current);
        aprs.request = radio_aprs_send(ax25.buffer, bits, (aprs.config.flags & 0x01) != 0, frequency);
        afsk_sent = (aprs.request != 0);
    }

    if (want_dmr && (channel.mode == CODEPLUG_MODE_DMR)) {
        uint32_t own_id = codeplug_get_dmr_id();
        uint32_t gateway = extra_options_aprs_dmr_gateway();

        if ((own_id != 0) && (gateway != 0)) {
            dmr_report.latitude_e7 = (int32_t)llround(aprs.current.latitude * 1E7);
            dmr_report.longitude_e7 = (int32_t)llround(aprs.current.longitude * 1E7);
            dmr_report.speed_knots = (aprs.current.speed > SPEED_MIN_CKN) ? (uint16_t)(aprs.current.speed / 100u)
                                                                          : APRS_DMR_SPEED_UNKNOWN;
            dmr_report.course = (aprs.current.bearing != BEARING_INVALID) ? (uint16_t)(aprs.current.bearing / 100u)
                                                                           : APRS_DMR_COURSE_UNKNOWN;
            dmr_report.gateway = gateway;
            dmr_report.own_id = own_id;
            aprs.dmr_request = radio_data_send(&DMR_REPORT);
            dmr_sent = (aprs.dmr_request != 0);
        }
    }

    if (!afsk_sent && !dmr_sent) {
        return false;
    }
    aprs.waiting = afsk_sent;
    aprs.dmr_waiting = dmr_sent;
    aprs.dmr_ok = false; // until the radio task reports the outcome
    aprs.waiting_auto = !manual;
    aprs.sending = aprs.current;
    if (aprs.s.mode == MODE_PTT) {
        aprs.next_beacon_ms = systime_get_ms() + interval_ms();
        aprs.next_beacon_running = true;
    }
    if (manual) {
        aprs.previous = aprs.current;
    }
    return true;
}

/* The satellite screen (#55), as OpenGD77: a beacon through the satellite's digipeater, with the APRS configuration
   named as the satellite (its path: RS0ISS for the ISS, DP0SNX for SONATE-2 ...), on the channel the screen tuned (the
   satellite's APRS uplink, Doppler corrected) */

// A configuration's name (8 characters, 0 or 0xFF after the last) is name: case blind, trailing spaces left out
static bool named(const char config_name[APRS_NAME_LENGTH], const char *name)
{
    int a_len = 0;
    int b_len = (int)strlen(name);

    while ((a_len < APRS_NAME_LENGTH) && (config_name[a_len] != 0) && ((uint8_t)config_name[a_len] != 0xFF)) {
        a_len++;
    }
    while ((a_len > 0) && (config_name[a_len - 1] == ' ')) {
        a_len--;
    }
    while ((b_len > 0) && (name[b_len - 1] == ' ')) {
        b_len--;
    }
    if ((a_len == 0) || (a_len != b_len)) {
        return false;
    }
    for (int i = 0; i < a_len; i++) {
        char a = config_name[i];
        char b = name[i];

        a = ((a >= 'a') && (a <= 'z')) ? (char)(a - 'a' + 'A') : a;
        b = ((b >= 'a') && (b <= 'z')) ? (char)(b - 'a' + 'A') : b;
        if (a != b) {
            return false;
        }
    }
    return true;
}

// The position now, for a beacon outside the beaconing: the GPS's when the APRS options say so and it is on, else
// the fixed one (the configuration's, or the settings' location)
static bool position_now(location_t *here)
{
    double lat;
    double lon;

    if ((aprs.s.state & STATE_LOCATION_FROM_GPS) && gps_on()) {
        gps_data_t g;

        gps_read(&g);
        if (!(g.fix && g.has_position)) {
            return false;
        }
        lat = g.latitude / 10000.0;
        lon = g.longitude / 10000.0;
    } else {
        fixed_position(&lat, &lon);
        if (isnan(lat)) {
            return false;
        }
    }
    here->time = systime_get_ms();
    here->latitude = lat;
    here->longitude = lon;
    here->bearing = 0;
    here->speed = 0;
    return true;
}

// false: no configuration of that name, no position, or the radio refused (no transmit frequency, busy ...)
static bool send_satellite_beacon(void)
{
    static codeplug_aprs_config_t channel_config; // the channel's, kept meanwhile (not on the input task's stack)
    bool channel_has_config = aprs.has_config;
    location_t here;
    bool ok = false;
    int index = 0;

    if (aprs.waiting || aprs.dmr_waiting) {
        return false;
    }
    for (int i = 1; (i <= CODEPLUG_APRS_CONFIGS_MAX) && (index == 0); i++) {
        if (codeplug_get_aprs_config(i, &channel_config) && named(channel_config.name, satellite_asked_name)) {
            index = i;
        }
    }
    if (index == 0) {
        return false;
    }
    // The satellite's configuration for this frame (its path, comment, symbol, fixed position), then the channel's
    // again
    memcpy(&channel_config, &aprs.config, sizeof(channel_config));
    aprs.has_config = codeplug_get_aprs_config(index, &aprs.config);
    aprs.fixed_lat = NAN;
    aprs.fixed_lon = NAN;
    if (aprs.has_config && position_now(&here)) {
        uint16_t bits = build_frame(&here, &here);

        aprs.request = radio_aprs_send(ax25.buffer, bits, (aprs.config.flags & 0x01) != 0, 0);
        ok = (aprs.request != 0);
    }
    memcpy(&aprs.config, &channel_config, sizeof(aprs.config));
    aprs.has_config = channel_has_config;
    aprs.fixed_lat = NAN;
    aprs.fixed_lon = NAN;
    if (ok) {
        aprs.waiting = true;
        aprs.waiting_auto = false; // as a manual beacon: an error tone if it doesn't go out
        aprs.sending = here;
    }
    return ok;
}

static bool corner_pegging(double *speed_mps, uint32_t *time_diff)
{
    *speed_mps = (aprs.current.speed * 1E-2) * MPS_PER_KNOT;
    *time_diff = aprs.current.time - aprs.previous.time;
    if (aprs.current.speed <= SPEED_MIN_CKN) {
        return false;
    }
    if (!location_valid(&aprs.current)) {
        return false;
    }
    if (!location_valid(&aprs.previous)) {
        return (*time_diff / 1000) >= aprs.s.turn_time;
    }

    double turn = fmod(fabs((aprs.current.bearing * 1E-2) - (aprs.previous.bearing * 1E-2)), 360.0);
    double threshold = fmin(120.0, aprs.s.turn_angle + ((aprs.s.turn_slope * 10.0) / (*speed_mps * MPS_TO_MPH)));

    turn = (turn <= 180.0) ? turn : (360.0 - turn);
    return ((*time_diff / 1000) >= aprs.s.turn_time) && (turn > threshold);
}

static uint32_t speed_rate_s(double speed_mps)
{
    int32_t slow = aprs.s.slow_rate * 60;
    int32_t fast = aprs.s.fast_rate;
    double low = aprs.s.low_speed * MPS_PER_KMPH;
    double high = aprs.s.high_speed * MPS_PER_KMPH;

    if (speed_mps <= low) {
        return (uint32_t)slow;
    }
    if ((speed_mps >= high) || (high <= low)) {
        return (uint32_t)fast;
    }
    return (uint32_t)(fast + (slow - fast) * (high - speed_mps) / (high - low));
}

static bool smart_check(void)
{
    double speed_mps;
    uint32_t time_diff;

    if (!location_valid(&aprs.previous) || corner_pegging(&speed_mps, &time_diff)) {
        return true;
    }

    double dist = distance_m(aprs.current.latitude, aprs.current.longitude, aprs.previous.latitude, aprs.previous.longitude);
    double seconds = (time_diff / 1000) ? (time_diff / 1000.0) : 1.0;
    double max_speed = fmax(fmax(dist / seconds, speed_mps), (aprs.previous.speed * 1E-2) * MPS_PER_KNOT);

    return (time_diff / 1000) >= speed_rate_s(max_speed);
}

static void decay(bool reset_it)
{
    if ((aprs.s.mode != MODE_SMART) && (aprs.s.state & STATE_DECAY)) {
        if (reset_it) {
            aprs.decay = DECAY_MULT_MIN;
        } else if (aprs.decay < DECAY_MULT_MAX) {
            aprs.decay *= DECAY_MULT_STEP;
        }
    }
}

// The frame that was with the radio task: done
static void sent(bool ok)
{
    aprs.waiting = false;
    aprs.dmr_waiting = false;
    if (!aprs.waiting_auto) {
        if (!ok) {
            sound_play(SOUND_ERROR); // the radio task could not send it: busy channel, Rx only, out of band
        }
        return;
    }
    if (!ok) {
        aprs.trigger = true; // again at the next check (the main firmware's busy channel)
        return;
    }
    if (aprs.s.mode != MODE_SMART) {
        double dist = 0.0;

        if (location_valid(&aprs.sending) && location_valid(&aprs.previous)) {
            dist = distance_m(aprs.sending.latitude, aprs.sending.longitude, aprs.previous.latitude, aprs.previous.longitude);
        }
        aprs.next_beacon_ms = systime_get_ms() + (interval_ms() * aprs.decay);
        aprs.next_beacon_running = true;
        decay(dist > DECAY_RESET_M);
    }
    aprs.previous = aprs.sending;
}

// aprsBeaconingTick(), each second
static void check(void)
{
    bool from_gps = (aprs.s.state & STATE_LOCATION_FROM_GPS) != 0;
    double lat;
    double lon;
    gps_data_t g;

    fixed_position(&lat, &lon);
    if (from_gps && gps_on()) {
        gps_read(&g);
    } else {
        memset(&g, 0, sizeof(g));
    }

    bool gps_pvt = from_gps && g.fix && g.has_position && g.has_speed;
    bool force = !from_gps || isnan(lat);

    if (!(gps_pvt || force)) {
        return;
    }

    bool fix = from_gps ? (g.fix && g.has_position) : true;
    bool check_beaconing = from_gps ? (isnan(lat) ? true : fix) : true;

    if (from_gps && ((!fix && location_valid(&aprs.previous)) || (fix && !aprs.has_fix))) {
        location_invalidate(&aprs.previous);
        aprs.has_fix = fix;
    }
    if (!check_beaconing) {
        return;
    }

    aprs.current.time = systime_get_ms();
    if (from_gps && fix) {
        aprs.current.latitude = g.latitude / 10000.0;
        aprs.current.longitude = g.longitude / 10000.0;
        aprs.current.bearing = (g.speed_ckn > SPEED_MIN_CKN) ? bearing() : 0;
        aprs.current.speed = (g.speed_ckn > GPS_SPEED_MIN_CKN) ? g.speed_ckn : 0;
    } else {
        aprs.current.latitude = lat;
        aprs.current.longitude = lon;
        aprs.current.bearing = 0;
        aprs.current.speed = 0;
    }
    if (isnan(aprs.current.latitude)) { // no position at all
        location_invalidate(&aprs.current);
        location_invalidate(&aprs.previous);
        aprs.fixed_lat = NAN;
        aprs.fixed_lon = NAN;
        return;
    }

    if (!aprs.trigger) {
        if (aprs.s.mode == MODE_SMART) {
            aprs.trigger = smart_check();
        } else if ((aprs.s.mode == MODE_AUTO) &&
                   (!aprs.next_beacon_running || ((int32_t)(systime_get_ms() - aprs.next_beacon_ms) >= 0))) {
            aprs.trigger = true;
        }
    }
    if (aprs.trigger && ((aprs.s.mode == MODE_AUTO) || (aprs.s.mode == MODE_SMART)) && send_beacon(false)) {
        aprs.trigger = false;
    }
}

void aprs_tick(void)
{
    codeplug_channel_t channel;
    uint8_t mode = aprs.s.mode;

    if (!settings_valid()) {
        return;
    }
    read_settings();
    if (aprs.s.mode != mode) {
        aprs.enabled = (aprs.s.mode != MODE_OFF); // aprsBeaconingStart() / Stop()
        reset(aprs.config_index);
    }
    // In every mode, the configuration is the channel's own: a DMR, P25, AM or USB channel can name one, and then its
    // beacon frequency is what the beacon uses. No configuration, no beacon.
    if (radio_get_channel(&channel) && (channel.aprs_config_index != aprs.config_index)) {
        reset(channel.aprs_config_index);
    }
    // The AFSK frame and the LRRP report (each with the radio task when it was taken): done when both are
    bool afsk_done = !aprs.waiting || (radio_result(aprs.request) != RADIO_PENDING);
    bool dmr_done = !aprs.dmr_waiting || (radio_result(aprs.dmr_request) != RADIO_PENDING);

    if ((aprs.waiting || aprs.dmr_waiting) && afsk_done && dmr_done) {
        bool ok = true;

        if (aprs.waiting) {
            ok = ok && (radio_result(aprs.request) == RADIO_SENT);
        }
        if (aprs.dmr_waiting) {
            aprs.dmr_ok = (radio_result(aprs.dmr_request) == RADIO_SENT);
            ok = ok && aprs.dmr_ok;
        }
        sent(ok);
    }

    if (toggle_asked) {
        toggle_asked = false;
        if (aprs.s.mode != MODE_OFF) {
            aprs.enabled = !aprs.enabled;
            if (aprs.enabled) {
                reset(aprs.config_index);
            }
            sound_play(aprs.enabled ? SOUND_ACK : SOUND_NACK);
            ui_event_t ev = {.type = UI_EVENT_DATA}; // the bottom bar shows "@" while beaconing is on

            ui_post(&ev);
        }
    }
    if (send_asked) {
        send_asked = false;
        if ((aprs.s.mode == MODE_OFF) || !send_beacon(true)) {
            sound_play(SOUND_ERROR);
        }
    }
    if (satellite_asked) {
        satellite_asked = false;
        if (!send_satellite_beacon()) {
            sound_play(SOUND_ERROR);
        }
    }
    if (ptt_asked && !radio_is_transmitting()) {
        ptt_asked = false;
        if (aprs.s.mode == MODE_PTT) {
            send_beacon(false);
        }
    }

    if ((aprs.s.mode == MODE_OFF) || !aprs.enabled || !aprs.has_config || radio_is_scanning() ||
        ((int32_t)(systime_get_ms() - aprs.check_ms) < 0)) {
        return;
    }
    aprs.check_ms = systime_get_ms() + CHECK_MS;
    check();
}

// The beaconing is on (SK1 + 1), not suspended; the bottom bar's "@"
bool aprs_is_active(void)
{
    return (aprs.s.mode != MODE_OFF) && aprs.enabled;
}

void aprs_toggle(void)
{
    toggle_asked = true;
}

void aprs_send(void)
{
    send_asked = true;
}

void aprs_after_ptt(void)
{
    ptt_asked = true;
}

void aprs_send_satellite(const char *name)
{
    if (satellite_asked) {
        return;
    }
    strncpy(satellite_asked_name, name, APRS_NAME_LENGTH);
    satellite_asked_name[APRS_NAME_LENGTH] = 0;
    satellite_asked = true;
}

bool aprs_is_sending(void)
{
    return aprs.waiting || satellite_asked;
}

void aprs_debug_status(uint32_t *flags, int32_t *lat_e7, int32_t *lon_e7)
{
    double lat;
    double lon;

    fixed_position(&lat, &lon);
    *flags = ((uint32_t)aprs.enabled << 0) | ((uint32_t)aprs.has_config << 1) |
             ((uint32_t)position_valid() << 2) | ((uint32_t)aprs.dmr_waiting << 3) |
             ((uint32_t)aprs.dmr_ok << 4) | ((uint32_t)aprs.s.mode << 5);
    *lat_e7 = isnan(lat) ? 0 : (int32_t)llround(lat * 1E7);
    *lon_e7 = isnan(lon) ? 0 : (int32_t)llround(lon * 1E7);
}
