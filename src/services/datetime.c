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
 * OpenDMR, service: the date and time. The real time clock is read once at start; after that the time is
 * counted from the 1 ms timer, so the screens don't access the I2C bus each time they show it.
 * After OpenDM32 (getRtcTime_custom(), uiUtilityFormatLocalTime(); time zone: settings.timezone, lower 7
 * bits in 15 minute steps, 64 = UTC).
 */

#include <stdio.h>
#include "rtc.h"
#include "settings.h"
#include "systime.h"
#include "datetime.h"

#define TIMEZONE_UTC    64
#define TIMEZONE_STEP_S (15 * 60)

static uint32_t base_seconds = 0; // UTC at base_ms
static uint32_t base_ms = 0;

void datetime_init(void)
{
    uint32_t seconds;

    rtc_init();
    if (rtc_read(&seconds)) {
        base_seconds = seconds;
        base_ms = systime_get_ms();
    }
}

uint32_t datetime_now(void)
{
    return base_seconds + ((systime_get_ms() - base_ms) / 1000);
}

bool datetime_set(uint32_t utc_seconds)
{
    base_seconds = utc_seconds;
    base_ms = systime_get_ms();
    return rtc_write(utc_seconds);
}

bool datetime_is_valid(void)
{
    return (datetime_now() >= DATETIME_VALID_MIN);
}

bool datetime_shown_is_utc(void)
{
    return !settings_valid() || ((settings.timezone & 0x80) == 0);
}

uint32_t datetime_shown(void)
{
    int32_t offset = ((int32_t)(settings.timezone & 0x7F) - TIMEZONE_UTC) * TIMEZONE_STEP_S;

    return datetime_now() + (datetime_shown_is_utc() ? 0 : offset);
}

bool datetime_format_local_time(char *buffer, size_t size)
{
    if (!datetime_is_valid()) {
        return false;
    }

    uint32_t in_day = datetime_shown() % 86400UL;

    snprintf(buffer, size, "%02lu:%02lu", (unsigned long)(in_day / 3600), (unsigned long)((in_day / 60) % 60));
    return true;
}

// Days since 1970 to the civil date (H. Hinnant's days_from_civil, inverted), from 1970 on
// The other way (days from civil, the same algorithm); the fields as given, no check
uint32_t datetime_from_fields(const datetime_fields_t *fields)
{
    uint32_t year = fields->year - ((fields->month <= 2) ? 1 : 0);
    uint32_t era = year / 400;
    uint32_t yoe = year - (era * 400);
    uint32_t mp = (fields->month > 2) ? (fields->month - 3) : (fields->month + 9);
    uint32_t doy = (((153 * mp) + 2) / 5) + fields->day - 1;
    uint32_t doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    uint32_t days = (era * 146097UL) + doe - 719468UL;

    return (days * 86400UL) + (fields->hour * 3600UL) + (fields->minute * 60UL) + fields->second;
}

void datetime_to_fields(uint32_t seconds, datetime_fields_t *fields)
{
    uint32_t days = seconds / 86400UL;
    uint32_t in_day = seconds % 86400UL;
    uint32_t z = days + 719468UL;
    uint32_t era = z / 146097UL;
    uint32_t doe = z - (era * 146097UL);
    uint32_t yoe = (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
    uint32_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
    uint32_t mp = ((5 * doy) + 2) / 153;
    uint32_t month = (mp < 10) ? (mp + 3) : (mp - 9);

    fields->year = (uint16_t)(yoe + (era * 400) + ((month <= 2) ? 1 : 0));
    fields->month = (uint8_t)month;
    fields->day = (uint8_t)(doy - (((153 * mp) + 2) / 5) + 1);
    fields->hour = (uint8_t)(in_day / 3600);
    fields->minute = (uint8_t)((in_day / 60) % 60);
    fields->second = (uint8_t)(in_day % 60);
}
