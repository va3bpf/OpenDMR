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
 * OpenDMR, service: positions (position.h). Our own position is the settings' (settings.location_lat / lon, fixed
 * point: sign bit 31, degrees 30:23, 1/100000 below), written by the GPS receiver (gps.c) or typed in (radio_info.c).
 * The caller's position comes from the ETSI GPS Info of the DMR call (dmr.c). The screens show the distance and the
 * direction from ours, or the coordinates when ours is not known.
 */

#include <math.h>
#include <stdio.h>
#include "settings.h"
#include "position.h"

#define LOCATION_UNSET 0x7F000000UL // settings.location_lat never set
#define LOCATION_SOUTH 0x80000000UL // in latitude and longitude: south / west
#define DEG_TO_RAD     0.017453292519943295
#define EARTH_RADIUS_M 6371000.0

static int32_t fixed_e5(uint32_t fixed)
{
    int32_t value = (int32_t)(((fixed & 0x7FFFFFFFUL) >> 23) * 100000UL) + (int32_t)(fixed & 0x7FFFFFUL);

    return (fixed & LOCATION_SOUTH) ? -value : value;
}

bool position_own(int32_t *latitude_e5, int32_t *longitude_e5)
{
    if (!settings_valid() || (settings.location_lat == LOCATION_UNSET)) {
        return false;
    }
    *latitude_e5 = fixed_e5(settings.location_lat);
    *longitude_e5 = fixed_e5(settings.location_lon);
    return true;
}

void position_range(int32_t latitude1_e5, int32_t longitude1_e5, int32_t latitude2_e5, int32_t longitude2_e5,
                    uint32_t *metres, uint16_t *bearing)
{
    double lat1 = (double)latitude1_e5 * 1E-5 * DEG_TO_RAD;
    double lon1 = (double)longitude1_e5 * 1E-5 * DEG_TO_RAD;
    double lat2 = (double)latitude2_e5 * 1E-5 * DEG_TO_RAD;
    double lon2 = (double)longitude2_e5 * 1E-5 * DEG_TO_RAD;
    double dlat = lat2 - lat1;
    double dlon = lon2 - lon1;
    double a = (sin(dlat / 2.0) * sin(dlat / 2.0)) +
               (cos(lat1) * cos(lat2) * sin(dlon / 2.0) * sin(dlon / 2.0));
    double c;

    if (a > 1.0) {
        a = 1.0;
    } else if (a < 0.0) {
        a = 0.0;
    }
    c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    double y = sin(dlon) * cos(lat2);
    double x = (cos(lat1) * sin(lat2)) - (sin(lat1) * cos(lat2) * cos(dlon));
    double b = atan2(y, x) / DEG_TO_RAD;

    if (b < 0.0) {
        b += 360.0;
    }
    *metres = (uint32_t)((EARTH_RADIUS_M * c) + 0.5);
    *bearing = (uint16_t)((int)(b + 0.5) % 360);
}

static const char *direction(uint16_t bearing)
{
    static const char *names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

    return names[((bearing + 22) / 45) % 8];
}

// The distance from our position as one string; false: ours is not known. compact: no space before the unit, for a
// list row ("12.3km NE").
static bool format_distance(char *out, size_t size, int32_t latitude_e5, int32_t longitude_e5, bool compact)
{
    int32_t own_lat, own_lon;
    uint32_t metres;
    uint16_t bearing;

    if (!position_own(&own_lat, &own_lon)) {
        return false;
    }
    position_range(own_lat, own_lon, latitude_e5, longitude_e5, &metres, &bearing);
    if (metres < 1000) {
        snprintf(out, size, (compact ? "%lum %s" : "%lu m %s"), (unsigned long)metres, direction(bearing));
    } else if (metres < 10000) {
        snprintf(out, size, (compact ? "%lu.%lukm %s" : "%lu.%lu km %s"), (unsigned long)(metres / 1000),
                 (unsigned long)((metres % 1000) / 100), direction(bearing));
    } else {
        snprintf(out, size, (compact ? "%lukm %s" : "%lu km %s"), (unsigned long)(metres / 1000),
                 direction(bearing));
    }
    return true;
}

// "34.6037S 58.3816W" (4 decimals) or "34.60S 58.38W" (2 decimals)
static void format_coordinates(char *out, size_t size, int32_t latitude_e5, int32_t longitude_e5, bool full)
{
    uint32_t lat = (uint32_t)((latitude_e5 < 0) ? -latitude_e5 : latitude_e5);
    uint32_t lon = (uint32_t)((longitude_e5 < 0) ? -longitude_e5 : longitude_e5);
    unsigned long lat_decimals = (unsigned long)((lat % 100000UL) / (full ? 10UL : 1000UL));
    unsigned long lon_decimals = (unsigned long)((lon % 100000UL) / (full ? 10UL : 1000UL));

    snprintf(out, size, "%lu.%0*lu%c %lu.%0*lu%c", (unsigned long)(lat / 100000UL), (full ? 4 : 2), lat_decimals,
             (latitude_e5 < 0) ? 'S' : 'N', (unsigned long)(lon / 100000UL), (full ? 4 : 2), lon_decimals,
             (longitude_e5 < 0) ? 'W' : 'E');
}

static void format_position(char *out, size_t size, int32_t latitude_e5, int32_t longitude_e5, bool full_coordinates)
{
    if (!format_distance(out, size, latitude_e5, longitude_e5, !full_coordinates)) {
        format_coordinates(out, size, latitude_e5, longitude_e5, full_coordinates);
    }
}

void position_format(char *out, size_t size, int32_t latitude_e5, int32_t longitude_e5)
{
    format_position(out, size, latitude_e5, longitude_e5, true);
}

void position_format_short(char *out, size_t size, int32_t latitude_e5, int32_t longitude_e5)
{
    format_position(out, size, latitude_e5, longitude_e5, false);
}
