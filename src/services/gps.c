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
 * OpenDMR, service: the GPS / BeiDou module. After OpenDM32 user/SOURCE/interfaces/gps.c (DM-32
 * parts, gpsProcessChar(), gpsTick()) and user/DRV/C7000_DRV.c.
 *
 * Hardware: power on GPIO 0x2C (SEL_GPS_Pin, low = on); NMEA on UART1, 9600 8N1, receive only (drivers/uart.c).
 *
 * The interrupt collects the sentences, one line buffer each, and wakes the GPS task at the end of a line. The task
 * checks and parses them: RMC (time, fix, position, speed, course), GGA (height), GSA (2D / 3D), GSV (the satellites in view, GPS
 * and BeiDou); it sets the clock from RMC and the location of the settings from a fix. No UI event: the GPS screen
 * reads the data on its own timer (an event would redraw every screen each second). In the NMEA mode the
 * sentences also go out of the cable.
 *
 * The module stays on while transmitting, as in the stock firmware. Its hiss (and a pulse each second) in the FM
 * transmit came through the baseband's FM transmit path, now left off (modem.c modem_fm_transmit()).
 */

#include <string.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "util.h"
#include "uart.h"
#include "settings.h"
#include "datetime.h"
#include "cable_link.h"
#include "gps.h"

#define PIN_GPS_POWER     0x2C // SEL_GPS_Pin, low = on
#define GPS_BAUD          9600
#define GPS_TASK_PRIORITY 1

#define LINE_LENGTH 84 // NMEA: 82 characters at most
#define LINES       4
#define FIELDS_MAX  24

#define LOCATION_UNSET     0x7F000000UL // settings.location_lat never set
#define LOCATION_SIGN      0x80000000UL // settings: sign bit, integer degrees in bits 30 - 23, 1/100000 below
#define LOCATION_INT_SHIFT 23
#define LOCATION_MOVE      100 // 1/100000 degree (about 100 m): the settings follow a move this large

static StaticTask_t task_tcb;
static StackType_t task_stack[512];
static TaskHandle_t task = NULL;

// Lines from the interrupt: written at write_line, the task reads the ones before it
static char lines[LINES][LINE_LENGTH];
static volatile uint8_t write_line = 0;
static volatile uint8_t write_pos = 0;
static uint8_t read_line = 0;

static gps_data_t data;   // what gps_read() gives, written under a critical section
static gps_data_t parsed; // the task's copy, being updated
static volatile bool enabled = false;

/* The interrupt */

static void RAM_CODE rx_byte(uint8_t c)
{
    if ((c == '\r') || (c == '\n')) {
        if (write_pos > 0) {
            BaseType_t woken = pdFALSE;

            lines[write_line][write_pos] = 0;
            write_line = (uint8_t)((write_line + 1) % LINES); // a line the task didn't take yet is overwritten
            write_pos = 0;
            if (task) {
                vTaskNotifyGiveFromISR(task, &woken);
                portYIELD_FROM_ISR(woken);
            }
        }
    } else if (write_pos < (LINE_LENGTH - 1)) {
        lines[write_line][write_pos++] = (char)c;
    }
}

/* Parsing */

static int hex_digit(char c)
{
    return ((c >= '0') && (c <= '9')) ? (c - '0') : (((c >= 'A') && (c <= 'F')) ? (c - 'A' + 10) : -1);
}

// "$...*hh": the XOR of the characters between '$' and '*'
static bool checksum_ok(const char *line)
{
    uint8_t sum = 0;
    const char *p = line + 1;

    if (line[0] != '$') {
        return false;
    }
    while (*p && (*p != '*')) {
        sum ^= (uint8_t)*p++;
    }
    return (*p == '*') && (hex_digit(p[1]) >= 0) && (hex_digit(p[2]) >= 0) &&
           (((hex_digit(p[1]) << 4) | hex_digit(p[2])) == sum);
}

// The fields of the line, cut at ',' and '*'; field 0 is the "$GPRMC"
static int split(char *line, char *fields[])
{
    int n = 0;

    fields[n++] = line;
    for (char *p = line; *p && (n < FIELDS_MAX); p++) {
        if ((*p == ',') || (*p == '*')) {
            *p = 0;
            fields[n++] = p + 1;
        }
    }
    return n;
}

static int field_int(const char *f)
{
    return (*f != 0) ? atoi(f) : -1;
}

// Two digits at p
static int two(const char *p)
{
    return ((p[0] - '0') * 10) + (p[1] - '0');
}

// "ddmm.mmmmm" / "dddmm.mmmmm" to 1/100000 degree, no floating point
// "123.45" to hundredths, no floating point
static uint32_t nmea_hundredths(const char *f)
{
    const char *dot = strchr(f, '.');
    uint32_t value = (uint32_t)atoi(f) * 100;

    if (dot && (dot[1] >= '0') && (dot[1] <= '9')) {
        value += (uint32_t)(dot[1] - '0') * 10;
        if ((dot[2] >= '0') && (dot[2] <= '9')) {
            value += (uint32_t)(dot[2] - '0');
        }
    }
    return value;
}

static int32_t nmea_degrees(const char *f)
{
    const char *dot = strchr(f, '.');
    int32_t whole = atoi(f); // ddmm
    int32_t fraction = 0;    // minutes, 1/100000
    int32_t scale = 100000;

    if (dot) {
        for (const char *p = dot + 1; (*p >= '0') && (*p <= '9') && (scale > 1); p++) {
            scale /= 10;
            fraction += (*p - '0') * scale;
        }
    }

    int32_t minutes_e5 = ((whole % 100) * 100000) + fraction;

    return ((whole / 100) * 100000) + (minutes_e5 / 60);
}

// Days since 1970-01-01 of a date (Howard Hinnant's days_from_civil)
static int32_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    int32_t yoe = y - (era * 400);
    int32_t doy = ((153 * (m + ((m > 2) ? -3 : 9))) + 2) / 5 + d - 1;
    int32_t doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;

    return (era * 146097) + doe - 719468;
}

// RMC time "hhmmss.ss" and date "ddmmyy" to UTC seconds, 0 without them
static uint32_t nmea_time(const char *time, const char *date)
{
    if ((strlen(time) < 6) || (strlen(date) < 6)) {
        return 0;
    }

    int32_t days = days_from_civil(2000 + two(&date[4]), two(&date[2]), two(&date[0]));

    return (uint32_t)((days * 86400) + (two(&time[0]) * 3600) + (two(&time[2]) * 60) + two(&time[4]));
}

// The settings' fixed point location (gpsLatLongConvert())
static uint32_t location_fixed(int32_t e5)
{
    uint32_t magnitude = (uint32_t)((e5 < 0) ? -e5 : e5);

    return (((magnitude / 100000) << LOCATION_INT_SHIFT) + (magnitude % 100000)) | ((e5 < 0) ? LOCATION_SIGN : 0);
}

static int32_t location_e5(uint32_t fixed)
{
    int32_t value = (int32_t)((((fixed & ~LOCATION_SIGN) >> LOCATION_INT_SHIFT) * 100000) +
                              (fixed & ((1UL << LOCATION_INT_SHIFT) - 1)));

    return (fixed & LOCATION_SIGN) ? -value : value;
}

static void follow_location(int32_t lat_e5, int32_t lon_e5)
{
    if ((settings.location_lat == LOCATION_UNSET) || (abs(location_e5(settings.location_lat) - lat_e5) > LOCATION_MOVE) ||
        (abs(location_e5(settings.location_lon) - lon_e5) > LOCATION_MOVE)) {
        settings.location_lat = location_fixed(lat_e5);
        settings.location_lon = location_fixed(lon_e5);
        settings_changed();
    }
}

// The clock from the module: with a fix, or before one when the clock was never set (the module has the time from the
// first satellite it decodes); only when it is off by a second or more
static void set_clock(uint32_t utc, bool fix)
{
    uint32_t now = datetime_now();

    if ((utc >= DATETIME_VALID_MIN) && (fix || !datetime_is_valid()) && (utc != now)) {
        datetime_set(utc);
    }
}

static void parse_rmc(char *f[], int n)
{
    if (n < 10) {
        return;
    }

    bool fix = (f[2][0] == 'A');

    set_clock(nmea_time(f[1], f[9]), fix);
    parsed.fix = fix;
    if (!fix) {
        parsed.fix_3d = false;
        parsed.has_position = false;
        parsed.has_speed = false;
        return;
    }
    parsed.speed_ckn = f[7][0] ? nmea_hundredths(f[7]) : 0;
    parsed.course_cdeg = (uint16_t)(f[8][0] ? (nmea_hundredths(f[8]) % 36000) : 0);
    parsed.has_speed = (f[7][0] != 0);
    if (f[3][0] && f[5][0]) {
        int32_t lat = nmea_degrees(f[3]) * ((f[4][0] == 'S') ? -1 : 1);
        int32_t lon = nmea_degrees(f[5]) * ((f[6][0] == 'W') ? -1 : 1);

        parsed.latitude = lat / 10; // 1/10000 degree
        parsed.longitude = lon / 10;
        parsed.has_position = true;
        follow_location(lat, lon);
    }
}

static void parse_gga(char *f[], int n)
{
    if ((n > 9) && f[9][0]) {
        parsed.height_m = (int16_t)atoi(f[9]);
        parsed.has_height = true;
    } else {
        parsed.has_height = false;
    }
}

static void parse_gsa(char *f[], int n)
{
    parsed.fix_3d = (n > 2) && (f[2][0] == '3');
}

// One of the GSV sentences of a system: the satellites of the first sentence replace the list
static void parse_gsv(char *f[], int n, gps_satellite_t *list, uint8_t *count)
{
    if (n < 4) {
        return;
    }

    int sentence = field_int(f[2]);
    int index = (sentence - 1) * 4;

    *count = (uint8_t)MAX(field_int(f[3]), 0);
    // groups of 4 fields from field 4; the last field is the checksum
    for (int i = 4; ((i + 3) < (n - 1)) && (index >= 0) && (index < GPS_SATELLITES_MAX); i += 4) {
        list[index].number = (uint16_t)MAX(field_int(f[i]), 0);
        list[index].elevation = (int16_t)field_int(f[i + 1]);
        list[index].azimuth = (int16_t)field_int(f[i + 2]);
        list[index].snr = (int8_t)MAX(field_int(f[i + 3]), 0);
        index++;
    }
}

static void parse(char *line)
{
    char *f[FIELDS_MAX];
    int n;

    if (!checksum_ok(line) || (strlen(line) < 6)) {
        return;
    }
    if ((settings.gps == GPS_MODE_ON_NMEA) && !cable_link_is_busy()) {
        uart0_write((const uint8_t *)line, strlen(line));
        uart0_write((const uint8_t *)"\r\n", 2);
    }

    n = split(line, f);

    const char *type = &f[0][3];                        // "$GPRMC": "RMC"
    bool beidou = (f[0][1] == 'B') || (f[0][2] == 'B'); // $BD..., $GB...

    if (strcmp(type, "RMC") == 0) {
        parse_rmc(f, n);
    } else if (strcmp(type, "GGA") == 0) {
        parse_gga(f, n);
    } else if (strcmp(type, "GSA") == 0) {
        parse_gsa(f, n);
    } else if (strcmp(type, "GSV") == 0) {
        if (beidou) {
            parse_gsv(f, n, parsed.beidou, &parsed.beidou_count);
        } else if ((f[0][1] == 'G') && (f[0][2] == 'P')) {
            parse_gsv(f, n, parsed.gps, &parsed.gps_count);
        }
    }
}

/* The task */

static void gps_task(void *arg)
{
    (void)arg;
    // From the task: switching the receive on handles what the UART has at once, which notifies this task
    gps_set_enabled(settings_valid() && (settings.gps >= GPS_MODE_ON));

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool parsed_any = false;

        while (read_line != write_line) {
            char line[LINE_LENGTH];

            memcpy(line, lines[read_line], LINE_LENGTH);
            read_line = (uint8_t)((read_line + 1) % LINES);
            if (enabled) {
                parse(line);
                parsed_any = true;
            }
        }

        if (parsed_any && enabled) {
            taskENTER_CRITICAL();
            memcpy(&data, &parsed, sizeof(data));
            taskEXIT_CRITICAL();
        }
    }
}

/* The API */

void gps_start(void)
{
    task = xTaskCreateStatic(gps_task, "gps", sizeof(task_stack) / sizeof(task_stack[0]), NULL, GPS_TASK_PRIORITY,
                             task_stack, &task_tcb);
    uart1_init(GPS_BAUD, rx_byte);
}

void gps_set_enabled(bool on)
{
    uart1_receive(false);
    enabled = on;
    taskENTER_CRITICAL();
    memset(&data, 0, sizeof(data));
    memset(&parsed, 0, sizeof(parsed));
    write_pos = 0;
    read_line = write_line;
    taskEXIT_CRITICAL();
    gpio_output(PIN_GPS_POWER, !on);
    uart1_receive(on);
}

void gps_read(gps_data_t *out)
{
    taskENTER_CRITICAL();
    memcpy(out, &data, sizeof(*out));
    taskEXIT_CRITICAL();
}
