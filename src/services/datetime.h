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

#ifndef DM32_DATETIME_H
#define DM32_DATETIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DATETIME_VALID_MIN 1735689600UL // 2025-01-01: an earlier clock was never set

void datetime_init(void);                // reads the real time clock once
uint32_t datetime_now(void);             // UTC, seconds since 1970 (0 when the clock could not be read)
bool datetime_set(uint32_t utc_seconds); // the clock and the real time clock
bool datetime_is_valid(void);
bool datetime_format_local_time(char *buffer, size_t size); // "14:32" as datetime_shown(); false: no clock

typedef struct {
    uint16_t year;
    uint8_t month; // 1 - 12
    uint8_t day;   // 1 - 31
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} datetime_fields_t;

// The time as shown: in the time zone of the settings when they say local time (timezone bit 7), else UTC
uint32_t datetime_shown(void);
bool datetime_shown_is_utc(void);
void datetime_to_fields(uint32_t seconds, datetime_fields_t *fields);
uint32_t datetime_from_fields(const datetime_fields_t *fields);

#endif
