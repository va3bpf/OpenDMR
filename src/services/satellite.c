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
 * OpenDMR, service: amateur satellite predictions. After OpenDM32 user/SOURCE/functions/satellite.c
 * (Plan 13 by G3RUH, as OpenGD77), menuSatelliteScreen.c (loading the keps, the passes) and functions/codeplug.c
 * (the custom data blocks).
 *
 * The keps: a custom data block of the codeplug (type 3), 25 records of 100 bytes, the two TLE lines compressed to
 * 4 bits a character. The firmware is built without libm (Makefile LIBS): the few functions needed are here, in
 * float, accurate to about 1e-7 (enough: the orbit itself is computed in float, as the main firmware does).
 *
 * RAM: about 170 bytes a satellite (keps, frequencies, SATELLITE_PASSES passes) instead of the 470 of the main
 * firmware (15 passes each).
 */

#include <string.h>
#include "flash.h"
#include "settings.h"
#include "custom_data.h"
#include "satellite.h"

/* The codeplug's custom data */

#define RECORD_SIZE       100 // codeplugSatelliteData_t, packed
#define RECORD_NAME       0
#define RECORD_TLE1       8  // 12 bytes, 24 characters
#define RECORD_TLE2       20 // 28 bytes, 56 characters
#define RECORD_RX1        48
#define RECORD_TX1        52
#define RECORD_TX_CTCSS1  56
#define RECORD_ARM_CTCSS1 58
#define RECORD_RX2        60
#define RECORD_TX2        64
#define RECORD_RX3        68
#define RECORD_TX3        72
#define TLE1_CHARACTERS   24
#define TLE2_CHARACTERS   56

/* Plan 13 constants */
#define PI_F            3.14159265358979f
#define TWO_PI_F        (2.0f * PI_F)
#define DEG_TO_RAD      (PI_F / 180.0f)
#define RAD_TO_DEG      (180.0f / PI_F)
#define EARTH_RE        6378.137f // WGS-84, km
#define EARTH_FL        (1.0f / 298.257224f)
#define EARTH_RP        (EARTH_RE * (1.0f - EARTH_FL))
#define EARTH_XX        (EARTH_RE * EARTH_RE)
#define EARTH_ZZ        (EARTH_RP * EARTH_RP)
#define YEAR_TROPICAL   365.2421970f
#define EARTH_WW        (TWO_PI_F / YEAR_TROPICAL)         // rotation, rad / whole day
#define EARTH_W0        ((TWO_PI_F + EARTH_WW) / 86400.0f) // rad / s
#define EARTH_GM        3.986E5                            // km^3 / s^2
#define EARTH_J2        1.08263E-3f
#define GHAA_YEAR       2010 // GHA of Aries on Jan 0.0 of this year
#define GHAA_G0         99.5578f
#define LIGHT_KM_S      299792.0f
#define UNIX_DAY_NUMBER 719178 // the day number (day_number()) of 1970-01-01

/* Predictions (menuSatelliteScreen.c, satellitePredictNextPassFromDateTimeSecs()) */
#define STEP_INITIAL_S     256
#define STEP_COARSE_S      (STEP_INITIAL_S * 4) // while far below the horizon
#define FAR_BELOW_DEG      -30.0f
#define PASS_ITERATIONS    1000      // per pass: beyond, the satellite is given up
#define VISIBLE_BACK_S     (30 * 60) // visible now: start this much earlier, to find the start of the pass
#define AFTER_PASS_S       (30 * 60) // the next pass searched from this after the last one
#define MAX_ELEVATION_STEP 2         // s
#define MAX_ELEVATION_MIN  0.01f     // degrees: the climb stops

#define LOCATION_UNSET     0x7F000000UL // settings.location_lat never set (SETTINGS_UNITIALISED_LOCATION_LAT)
#define LOCATION_SIGN      0x80000000UL // fixed point: sign bit, integer degrees in bits 30 - 23, 1/100000 below
#define LOCATION_INT_SHIFT 23
#define LOCATION_DECIMALS  100000.0f

typedef struct {
    float de;      // epoch: day number
    float te_frac; // epoch: fraction of the day
    float si;      // sin, cos inclination
    float ci;
    float ra;   // RAAN, rad
    float ec;   // eccentricity
    float wp;   // argument of perigee, rad
    float ma;   // mean anomaly, rad
    float mm;   // mean motion, rad / day
    float n0;   // mean motion, rad / s
    float a0;   // semi major axis, km
    float b0;   // semi minor axis, km
    float qd;   // node precession, rad / day
    float wd;   // perigee precession, rad / day
    float dc;   // drag coefficient
    float ghae; // GHA Aries at epoch, rad, 0 - 2 pi
} keps_t;

typedef struct {
    char name[SATELLITE_NAME_LENGTH + 1];
    keps_t keps;
    satellite_freq_t freqs[SATELLITE_FREQS];
    satellite_pass_t passes[SATELLITE_PASSES];
    uint8_t pass_count;
    bool done; // no more passes to look for until one is over
} satellite_t;

typedef struct {
    float ux, uy, uz; // unit vectors up, east, north (geocentric)
    float ex, ey;
    float nx, ny, nz;
    float ox, oy, oz; // position, km
    float vox, voy;   // velocity, km / s
} observer_t;

typedef enum {
    SEARCH_IDLE,
    SEARCH_AOS,
    SEARCH_LOS
} search_phase_t;

typedef struct {
    int sat;
    search_phase_t phase;
    uint32_t t;
    uint32_t limit;
    int32_t step;
    int8_t direction;
    bool found_start;
    int iterations;
    uint32_t aos;
} search_t;

static satellite_t satellites[SATELLITE_MAX];
static int count = 0;
static observer_t observer;
static search_t search;

/* Maths (no libm) */

// To -pi .. pi
static float wrap_pi(float x)
{
    int32_t turns = (int32_t)(x / TWO_PI_F + ((x < 0) ? -0.5f : 0.5f));

    return x - ((float)turns * TWO_PI_F);
}

static float sin_f(float x)
{
    float x2;

    x = wrap_pi(x);
    if (x > (PI_F / 2)) {
        x = PI_F - x;
    } else if (x < -(PI_F / 2)) {
        x = -PI_F - x;
    }
    x2 = x * x;
    return x *
           (1.0f - (x2 / 6.0f) *
                       (1.0f - (x2 / 20.0f) * (1.0f - (x2 / 42.0f) * (1.0f - (x2 / 72.0f) * (1.0f - (x2 / 110.0f))))));
}

static float cos_f(float x)
{
    return sin_f(x + (PI_F / 2));
}

static float sqrt_f(float x)
{
    union {
        float f;
        uint32_t u;
    } v = {.f = x};

    if (x <= 0.0f) {
        return 0.0f;
    }
    v.u = (v.u >> 1) + 0x1FC00000UL; // a first guess from the exponent
    for (int i = 0; i < 4; i++) {
        v.f = 0.5f * (v.f + (x / v.f));
    }
    return v.f;
}

// atan for |x| <= 1
static float atan_unit(float x)
{
    const float tan_pi_8 = 0.41421356f;
    float offset = 0.0f;
    float x2;
    float sum = 0.0f;
    float term;

    if (x > tan_pi_8) {
        offset = PI_F / 4;
        x = (x - 1.0f) / (x + 1.0f);
    } else if (x < -tan_pi_8) {
        offset = -PI_F / 4;
        x = (x + 1.0f) / (1.0f - x);
    }
    x2 = x * x;
    term = x;
    for (int n = 1; n <= 15; n += 2) { // |x| <= 0.414: x^17 / 17 < 2e-8
        sum += term / (float)n * (((n / 2) & 1) ? -1.0f : 1.0f);
        term *= x2;
    }
    return offset + sum;
}

// 0 .. 2 pi (satelliteAtnFn())
static float atan2_positive(float y, float x)
{
    float a;

    if ((x == 0.0f) && (y == 0.0f)) {
        return 0.0f;
    }
    if (((x < 0) ? -x : x) >= ((y < 0) ? -y : y)) {
        a = atan_unit(y / x);
        if (x < 0) {
            a += PI_F;
        }
    } else {
        a = (PI_F / 2) - atan_unit(x / y);
        if (y < 0) {
            a += PI_F;
        }
    }
    return (a < 0) ? (a + TWO_PI_F) : ((a >= TWO_PI_F) ? (a - TWO_PI_F) : a);
}

static float asin_f(float x)
{
    float a = atan2_positive(x, sqrt_f(1.0f - (x * x)));

    return (a > PI_F) ? (a - TWO_PI_F) : a;
}

static double cbrt_d(double v)
{
    double y = 7000.0; // km, a low orbit

    for (int i = 0; i < 40; i++) {
        y -= ((y * y * y) - v) / (3.0 * y * y);
    }
    return y;
}

// Day number of the main firmware (satelliteDayFn()), valid from 1900 to 2100
static uint32_t day_number(int year, int month, int day)
{
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    return (uint32_t)((year * 36525) / 100) + (uint32_t)(((month + 1) * 306) / 10) + day - 428;
}

/* Keps */

static const char decompress[16] = "0123456789. +-*";

// A decimal number of a TLE field (as atof() on these characters: it stops at a second sign)
static float tle_number(const char *field, int length)
{
    float value = 0.0f;
    float scale = 0.0f;
    bool negative = false;
    bool started = false;

    for (int i = 0; i < length; i++) {
        char c = field[i];

        if ((c == ' ') && !started) {
            continue;
        }
        if (((c == '-') || (c == '+')) && !started) {
            negative = (c == '-');
            started = true;
        } else if (c == '.') {
            if (scale != 0.0f) {
                break;
            }
            scale = 1.0f;
            started = true;
        } else if ((c >= '0') && (c <= '9')) {
            value = (value * 10.0f) + (float)(c - '0');
            scale *= 10.0f;
            started = true;
        } else {
            break;
        }
    }
    if (scale > 1.0f) {
        value /= scale;
    }
    return negative ? -value : value;
}

static void tle_decompress(const uint8_t *in, char *out, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        *out++ = decompress[in[i] >> 4];
        *out++ = decompress[in[i] & 0x0F];
    }
}

static void keps_from_tle(const uint8_t *record, keps_t *k)
{
    char l1[TLE1_CHARACTERS];
    char l2[TLE2_CHARACTERS];

    tle_decompress(&record[RECORD_TLE1], l1, TLE1_CHARACTERS / 2);
    tle_decompress(&record[RECORD_TLE2], l2, TLE2_CHARACTERS / 2);

    int year = (int)tle_number(&l1[0], 2) + 2000;
    double te = tle_number(&l1[2], 12); // day of the year, with its fraction
    double m2 = tle_number(&l1[14], 10) * 2.0 * PI_F;
    float inclination = tle_number(&l2[0], 8) * DEG_TO_RAD;
    double ec = tle_number(&l2[16], 7) * 1.0e-7;
    double mm = tle_number(&l2[39], 11) * 2.0 * PI_F;
    int te_days = (int)te;

    k->ra = tle_number(&l2[8], 8) * DEG_TO_RAD;
    k->ec = (float)ec;
    k->wp = tle_number(&l2[23], 8) * DEG_TO_RAD;
    k->ma = tle_number(&l2[31], 8) * DEG_TO_RAD;
    k->mm = (float)mm;
    k->n0 = (float)(mm / 86400.0);

    double n0 = mm / 86400.0;
    double a0 = cbrt_d(EARTH_GM / (n0 * n0));

    k->a0 = (float)a0;
    k->te_frac = (float)(te - te_days);
    k->de = (float)(day_number(year, 1, 0) + te_days);
    k->si = sin_f(inclination);
    k->ci = cos_f(inclination);
    k->b0 = (float)(a0 * sqrt_f((float)(1.0 - (ec * ec))));

    double pc = EARTH_RE * a0 / ((double)k->b0 * k->b0);

    pc = 1.5 * EARTH_J2 * pc * pc * mm; // precession constant, rad / day
    k->qd = (float)(-pc * k->ci);
    k->wd = (float)(pc * ((5.0 * k->ci * k->ci) - 1.0) / 2.0);
    k->dc = (float)(-2.0 * m2 / mm / 3.0);

    // GHA of Aries at epoch, reduced to a turn in double (float would lose 0.1 degree here)
    double elapsed = (double)(day_number(year, 1, 0) + te_days - day_number(GHAA_YEAR, 1, 0)) + k->te_frac;
    double ghae = (GHAA_G0 * DEG_TO_RAD) + (elapsed * (2.0 * PI_F + EARTH_WW));

    ghae -= (double)(int32_t)(ghae / (2.0 * PI_F)) * (2.0 * PI_F);
    k->ghae = (float)ghae;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

int satellite_load(void)
{
    static uint8_t record[RECORD_SIZE]; // not on the UI task stack
    uint32_t address = custom_data_find(CUSTOM_DATA_TYPE_SATELLITE_TLE, NULL);

    count = 0;
    search.phase = SEARCH_IDLE;
    if (address == 0) {
        return 0;
    }
    while ((count < SATELLITE_MAX) && flash_read(address + (count * RECORD_SIZE), record, sizeof(record)) &&
           (record[RECORD_NAME] != 0) && (record[RECORD_NAME] != 0xFF)) {
        satellite_t *s = &satellites[count];

        memset(s, 0, sizeof(*s));
        memcpy(s->name, &record[RECORD_NAME], SATELLITE_NAME_LENGTH); // not always 0 terminated
        keps_from_tle(record, &s->keps);
        s->freqs[SATELLITE_FREQ_VOICE].rx_hz = le32(&record[RECORD_RX1]);
        s->freqs[SATELLITE_FREQ_VOICE].tx_hz = le32(&record[RECORD_TX1]);
        s->freqs[SATELLITE_FREQ_VOICE].tx_ctcss = record[RECORD_TX_CTCSS1] | (record[RECORD_TX_CTCSS1 + 1] << 8);
        s->freqs[SATELLITE_FREQ_VOICE].arm_ctcss = record[RECORD_ARM_CTCSS1] | (record[RECORD_ARM_CTCSS1 + 1] << 8);
        s->freqs[SATELLITE_FREQ_APRS].rx_hz = le32(&record[RECORD_RX2]);
        s->freqs[SATELLITE_FREQ_APRS].tx_hz = le32(&record[RECORD_TX2]);
        s->freqs[SATELLITE_FREQ_OTHER].rx_hz = le32(&record[RECORD_RX3]);
        s->freqs[SATELLITE_FREQ_OTHER].tx_hz = le32(&record[RECORD_TX3]);
        count++;
    }
    return count;
}

int satellite_count(void)
{
    return count;
}

const char *satellite_name(int sat)
{
    return ((sat >= 0) && (sat < count)) ? satellites[sat].name : "";
}

const satellite_freq_t *satellite_freq(int sat, satellite_freq_kind_t kind)
{
    return &satellites[sat].freqs[kind];
}

/* Observer */

static float location_degrees(uint32_t fixed)
{
    float value = (float)((fixed & ~LOCATION_SIGN) >> LOCATION_INT_SHIFT) +
                  ((float)(fixed & ((1UL << LOCATION_INT_SHIFT) - 1)) / LOCATION_DECIMALS);

    return (fixed & LOCATION_SIGN) ? -value : value;
}

bool satellite_set_observer_from_settings(void)
{
    if (!settings_valid() || (settings.location_lat == LOCATION_UNSET)) {
        return false;
    }

    float lat = location_degrees(settings.location_lat) * DEG_TO_RAD;
    float lon = location_degrees(settings.location_lon) * DEG_TO_RAD;
    float cos_lat = cos_f(lat);
    float sin_lat = sin_f(lat);
    float cos_lon = cos_f(lon);
    float sin_lon = sin_f(lon);
    float d = sqrt_f((EARTH_XX * cos_lat * cos_lat) + (EARTH_ZZ * sin_lat * sin_lat));
    float rx = EARTH_XX / d; // the height makes virtually no difference: 0, as the main firmware
    float rz = EARTH_ZZ / d;

    observer.ux = cos_lat * cos_lon;
    observer.ex = -sin_lon;
    observer.nx = -sin_lat * cos_lon;
    observer.uy = cos_lat * sin_lon;
    observer.ey = cos_lon;
    observer.ny = -sin_lat * sin_lon;
    observer.uz = sin_lat;
    observer.nz = cos_lat;
    observer.ox = rx * observer.ux;
    observer.oy = rx * observer.uy;
    observer.oz = rz * observer.uz;
    observer.vox = -observer.oy * EARTH_W0;
    observer.voy = observer.ox * EARTH_W0;
    return true;
}

/* Position (satelliteCalculateForDateTimeSecs()) */

typedef struct {
    float elevation; // degrees
    float azimuth;
    float doppler; // range rate / c
} position_t;

static void compute(const keps_t *k, uint32_t utc, bool full, position_t *out)
{
    uint32_t days = utc / 86400UL;
    float tn = (float)(utc % 86400UL) / 86400.0f;
    float t = ((float)((int32_t)(days + UNIX_DAY_NUMBER) - (int32_t)k->de)) + (tn - k->te_frac); // since epoch
    float dt = k->dc * t / 2.0f;                                                                 // linear drag terms
    float kd = 1.0f + (4.0f * dt);
    float kdp = 1.0f - (7.0f * dt);
    float m = wrap_pi(k->ma + (k->mm * t * (1.0f - (3.0f * dt)))); // mean anomaly
    float ea = m;
    float c = 1.0f;
    float s = 0.0f;
    float dnom = 1.0f;
    float delta;

    // M = EA - EC * sin(EA), for EA by Newton's method
    for (int i = 0; i < 20; i++) {
        c = cos_f(ea);
        s = sin_f(ea);
        dnom = 1.0f - (k->ec * c);
        delta = (ea - (k->ec * s) - m) / dnom;
        ea -= delta;
        if (((delta < 0) ? -delta : delta) <= 1.0e-5f) {
            break;
        }
    }

    float a = k->a0 * kd;
    float b = k->b0 * kd;
    // In the plane of the ellipse
    float sx = a * (c - k->ec);
    float vx = -a * s / dnom * k->n0;
    float sy = b * s;
    float vy = b * c / dnom * k->n0;

    float ap = k->wp + (k->wd * t * kdp);
    float cw = cos_f(ap);
    float sw = sin_f(ap);
    float raan = k->ra + (k->qd * t * kdp);
    float co = cos_f(raan);
    float so = sin_f(raan);

    // Plane -> celestial: [RAAN] * [IN] * [AP]
    float cxx = (cw * co) - (sw * k->ci * so);
    float cxy = (-sw * co) - (cw * k->ci * so);
    float cyx = (cw * so) + (sw * k->ci * co);
    float cyy = (-sw * so) + (cw * k->ci * co);
    float czx = sw * k->si;
    float czy = cw * k->si;

    float sat_x = (sx * cxx) + (sy * cxy);
    float vel_x = (vx * cxx) + (vy * cxy);
    float sat_y = (sx * cyx) + (sy * cyy);
    float vel_y = (vx * cyx) + (vy * cyy);
    float sat_z = (sx * czx) + (sy * czy);
    float vel_z = (vx * czx) + (vy * czy);

    // Geocentric: GHA of Aries now, the whole turns of the day left out
    int32_t whole = (int32_t)t;
    float gha = k->ghae + (TWO_PI_F * (t - (float)whole)) + (EARTH_WW * t);

    c = cos_f(-gha);
    s = sin_f(-gha);
    sx = (sat_x * c) - (sat_y * s);
    vx = (vel_x * c) - (vel_y * s);
    sy = (sat_x * s) + (sat_y * c);
    vy = (vel_x * s) + (vel_y * c);

    float rx = sx - observer.ox;
    float ry = sy - observer.oy;
    float rz = sat_z - observer.oz;
    float r = sqrt_f((rx * rx) + (ry * ry) + (rz * rz));

    rx /= r;
    ry /= r;
    rz /= r;
    out->elevation = asin_f((rx * observer.ux) + (ry * observer.uy) + (rz * observer.uz)) * RAD_TO_DEG;
    if (!full) {
        return;
    }

    float e = (rx * observer.ex) + (ry * observer.ey);
    float n = (rx * observer.nx) + (ry * observer.ny) + (rz * observer.nz);

    out->azimuth = atan2_positive(e, n) * RAD_TO_DEG;
    out->doppler = (((vx - observer.vox) * rx) + ((vy - observer.voy) * ry) + (vel_z * rz)) / LIGHT_KM_S;
}

static int16_t round_degrees(float degrees)
{
    return (int16_t)((degrees < 0) ? (degrees - 0.5f) : (degrees + 0.5f));
}

void satellite_look(int sat, uint32_t utc, satellite_look_t *look)
{
    const satellite_t *s = &satellites[sat];
    position_t p;

    compute(&s->keps, utc, true, &p);
    look->elevation = round_degrees(p.elevation);
    look->azimuth = round_degrees(p.azimuth);
    if (look->azimuth >= 360) {
        look->azimuth -= 360;
    }
    for (int kind = 0; kind < SATELLITE_FREQS; kind++) {
        // Receive lower when the satellite moves away (range rate > 0), transmit higher
        int32_t rx_shift = (int32_t)((float)s->freqs[kind].rx_hz * p.doppler);
        int32_t tx_shift = (int32_t)((float)s->freqs[kind].tx_hz * p.doppler);

        look->rx_hz[kind] = s->freqs[kind].rx_hz ? (uint32_t)((int32_t)s->freqs[kind].rx_hz - rx_shift) : 0;
        look->tx_hz[kind] = s->freqs[kind].tx_hz ? (uint32_t)((int32_t)s->freqs[kind].tx_hz + tx_shift) : 0;
    }
}

static float elevation(const satellite_t *s, uint32_t utc)
{
    position_t p;

    compute(&s->keps, utc, false, &p);
    return p.elevation;
}

/* Predictions */

static void search_begin(int sat, uint32_t from, uint32_t now)
{
    search.sat = sat;
    search.phase = SEARCH_AOS;
    search.t = from;
    search.limit = now + (SATELLITE_PREDICT_HOURS * 3600UL);
    search.step = STEP_INITIAL_S;
    search.direction = 1;
    search.found_start = false;
    search.iterations = 0;
}

// The next satellite still to predict, from the one searched last; -1: all done
static int next_to_predict(void)
{
    for (int i = 0; i < count; i++) {
        int sat = (search.sat + i) % count;

        if (!satellites[sat].done) {
            return sat;
        }
    }
    return -1;
}

// Where to look for the next pass of a satellite
static uint32_t search_from(const satellite_t *s, uint32_t now)
{
    if (s->pass_count > 0) {
        return s->passes[s->pass_count - 1].los + AFTER_PASS_S;
    }
    return (elevation(s, now) < 0.0f) ? now : (now - VISIBLE_BACK_S);
}

// One position more of the search (the bisection of the main firmware: 256 s steps, 1024 far below the horizon, then
// halved around the crossing down to 1 s)
static void search_iterate(void)
{
    satellite_t *s = &satellites[search.sat];
    float e;
    bool up;

    search.t += search.step * search.direction;
    e = elevation(s, search.t);
    up = (e >= 0.0f);
    if (search.phase == SEARCH_LOS) {
        up = !up; // looking for the satellite going down
    }

    if (!search.found_start && up) {
        search.found_start = true;
    }
    if (search.found_start) {
        if (search.step == 1) {
            if (up) {
                if (search.phase == SEARCH_AOS) {
                    search.aos = search.t;
                    search.phase = SEARCH_LOS;
                    search.step = STEP_INITIAL_S;
                    search.direction = 1;
                    search.found_start = false;
                } else {
                    satellite_pass_t *pass = &s->passes[s->pass_count++];

                    pass->aos = search.aos;
                    pass->los = search.t;
                    pass->max_elevation = -1;
                    search.phase = SEARCH_IDLE;
                    s->done = (s->pass_count >= SATELLITE_PASSES);
                }
            } else {
                search.direction = 1;
            }
        } else {
            search.step /= 2;
            search.direction = up ? -1 : 1;
        }
    } else if (search.phase == SEARCH_AOS) {
        bool far = (e < FAR_BELOW_DEG);

        if (far && (search.step == STEP_INITIAL_S)) {
            search.step = STEP_COARSE_S;
        } else if (!far && (search.step == STEP_COARSE_S)) {
            search.step = STEP_INITIAL_S;
        }
    }

    if ((search.phase != SEARCH_IDLE) && ((search.t >= search.limit) || (++search.iterations >= PASS_ITERATIONS))) {
        s->done = true; // no pass before the limit (or the search went wrong): until a pass is over
        search.phase = SEARCH_IDLE;
    }
}

void satellite_predict_start(uint32_t now)
{
    for (int sat = 0; sat < count; sat++) {
        satellites[sat].pass_count = 0;
        satellites[sat].done = false;
    }
    search.sat = 0;
    search.phase = SEARCH_IDLE;
    (void)now;
}

// Drops the passes that are over; the satellite is predicted further
static void drop_old_passes(uint32_t now)
{
    for (int sat = 0; sat < count; sat++) {
        satellite_t *s = &satellites[sat];
        int old = 0;

        while ((old < s->pass_count) && (s->passes[old].los < now)) {
            old++;
        }
        if ((old > 0) && !((search.phase != SEARCH_IDLE) && (search.sat == sat))) {
            memmove(&s->passes[0], &s->passes[old], (s->pass_count - old) * sizeof(s->passes[0]));
            s->pass_count -= old;
            s->done = false;
        }
    }
}

bool satellite_predict_step(uint32_t now, int budget)
{
    drop_old_passes(now);
    while (budget-- > 0) {
        if (search.phase == SEARCH_IDLE) {
            int sat = next_to_predict();

            if (sat < 0) {
                return true;
            }
            search_begin(sat, search_from(&satellites[sat], now), now);
        }
        search_iterate();
    }
    return (search.phase == SEARCH_IDLE) && (next_to_predict() < 0);
}

int satellite_predicted_count(void)
{
    int n = 0;

    for (int sat = 0; sat < count; sat++) {
        n += satellites[sat].done ? 1 : 0;
    }
    return n;
}

int satellite_pass_count(int sat)
{
    return satellites[sat].pass_count;
}

const satellite_pass_t *satellite_pass(int sat, int n)
{
    return &satellites[sat].passes[n];
}

// From the middle of the pass, climbing in steps of 2 s the way the elevation rises (satelliteGetMaximumElevation())
int satellite_max_elevation(int sat, int n)
{
    satellite_t *s = &satellites[sat];
    satellite_pass_t *pass = &s->passes[n];
    uint32_t t = pass->aos + ((pass->los - pass->aos) / 2);
    float best = elevation(s, t);

    if (pass->max_elevation >= 0) {
        return pass->max_elevation;
    }
    for (int direction = -1; direction <= 1; direction += 2) {
        uint32_t at = t;
        float last = best;

        for (;;) {
            float e;

            at += direction * MAX_ELEVATION_STEP;
            e = elevation(s, at);
            if ((e - last) <= MAX_ELEVATION_MIN) {
                break;
            }
            last = e;
        }
        if (last > best) {
            best = last;
            break;
        }
    }
    pass->max_elevation = (best < 0) ? 0 : round_degrees(best);
    return pass->max_elevation;
}
