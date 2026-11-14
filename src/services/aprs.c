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
 * beaconing), aprsBeaconingSendBeacon(). The frame goes to the radio task (radio_aprs_send()), which waits for a busy
 * channel and sends the AFSK. The settings are the APRS options' (settings.aprs_beaconing_settings_part1 / 2); the
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
#include "aprs.h"

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
    bool waiting;       // a frame is with the radio task
    bool waiting_auto;  // ... sent by the auto modes (the trigger stays on a failure)
    location_t sending; // the location of that frame
} aprs = {.fixed_lat = NAN, .fixed_lon = NAN, .decay = DECAY_MULT_MIN};

static volatile bool toggle_asked = false;
static volatile bool send_asked = false;
static volatile bool ptt_asked = false;

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

// The CPS's APRS frequency ("Frec TX"), 10 Hz units like a channel; 0: not set
static uint32_t config_frequency(const codeplug_aprs_config_t *c)
{
    return (uint32_t)c->tx_freq[0] | ((uint32_t)c->tx_freq[1] << 8) |
           ((uint32_t)c->tx_freq[2] << 16) | ((uint32_t)c->tx_freq[3] << 24);
}

// The QSY information of the CPS's "Transmit QSY" (flags bit 2): the frequency of the configuration after the comment,
// as the stock firmware sends it ("144.390MHz", which aprs.fi reads as the frequency the station listens on)
static void put_qsy(const codeplug_aprs_config_t *c)
{
    uint32_t frequency = config_frequency(c);

    if ((c->flags & 0x04) && (frequency != 0)) {
        char text[16];

        snprintf(text, sizeof(text), " %u.%03uMHz", (unsigned)(frequency / 100000UL),
                 (unsigned)((frequency / 100UL) % 1000UL));
        put_string(text);
    }
}

static void put_payload(const char *lat, const char *lon, const char *course_speed)
{
    const codeplug_aprs_config_t *c = &aprs.config;
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
    put_string(comment);
    put_qsy(c);
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

    put_flags(16);
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

// aprsBeaconingSendBeacon(): true when the frame went to the radio task. forced: SK1 + 3 (or the menu item): it sends
// in any mode but OFF, not only in Manual / PTT, and the caller gives the error tone (once)
static bool send_beacon(bool forced)
{
    bool manual = forced || (aprs.s.mode == MODE_PTT) || (aprs.s.mode == MODE_MANUAL);
    codeplug_channel_t channel;

    if (radio_is_scanning() || !radio_get_channel(&channel) || (channel.mode != CODEPLUG_MODE_FM) ||
        (aprs.s.mode == MODE_OFF) || !aprs.enabled || aprs.waiting) {
        return false;
    }
    if (!forced && (aprs.s.mode == MODE_PTT) &&
        ((aprs.next_beacon_running && ((int32_t)(systime_get_ms() - aprs.next_beacon_ms) < 0)) || !aprs.has_config)) {
        return false;
    }
    if (manual && !(aprs.has_config && position_valid())) {
        if (!forced) {
            sound_play(SOUND_ERROR); // "APRS?" / "Location?" of the main firmware
        }
        return false;
    }
    if (!aprs.has_config) {
        return false;
    }

    uint16_t bits = build_frame(&aprs.previous, &aprs.current);

    if (!radio_aprs_send(ax25.buffer, bits, (aprs.config.flags & 0x01) != 0)) {
        return false;
    }
    aprs.waiting = true;
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
    int index = 0;
    uint8_t mode = aprs.s.mode;

    if (!settings_valid()) {
        return;
    }
    read_settings();
    if (aprs.s.mode != mode) {
        aprs.enabled = (aprs.s.mode != MODE_OFF); // aprsBeaconingStart() / Stop()
        reset(aprs.config_index);
    }
    if (radio_get_channel(&channel) && (channel.mode == CODEPLUG_MODE_FM)) {
        index = channel.aprs_config_index;
    }
    if (index != aprs.config_index) {
        reset(index);
    }
    if (aprs.waiting && !radio_aprs_busy()) {
        sent(radio_aprs_sent());
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
