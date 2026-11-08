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

#ifndef DM32_GPS_H
#define DM32_GPS_H

#include <stdbool.h>
#include <stdint.h>

// The GPS / BeiDou receiver module (gps.c): its task parses the NMEA sentences; screens read the data with gps_read().
#define GPS_SATELLITES_MAX 16 // kept per system (GPS, BeiDou)

// settings.gps (the main firmware's gpsMode_t)
enum {
    GPS_MODE_NOT_DETECTED = 0,
    GPS_MODE_OFF,
    GPS_MODE_ON,
    GPS_MODE_ON_NMEA,
};

typedef struct {
    uint16_t number;
    int16_t elevation; // degrees, 0 - 90; out of range: no position yet
    int16_t azimuth;   // degrees, 0 - 359
    int8_t snr;        // dB-Hz; 0: not received
} gps_satellite_t;

typedef struct {
    bool fix;
    bool fix_3d;
    bool has_position;
    bool has_height;
    int32_t latitude;  // 1/10000 degree, + north
    int32_t longitude; // 1/10000 degree, + east
    int16_t height_m;
    bool has_speed;      // RMC speed and course (with a fix)
    uint32_t speed_ckn;  // 1/100 knot
    uint16_t course_cdeg; // 1/100 degree, 0 - 35999
    uint8_t gps_count; // satellites in view
    uint8_t beidou_count;
    gps_satellite_t gps[GPS_SATELLITES_MAX];
    gps_satellite_t beidou[GPS_SATELLITES_MAX];
} gps_data_t;

void gps_start(void);            // the task and UART1; the task switches the module on when settings.gps is on
void gps_set_enabled(bool on);   // powers the module and its UART on / off (settings.gps is the caller's)
void gps_read(gps_data_t *data); // a copy of the last data

#endif
