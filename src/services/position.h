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

#ifndef DM32_POSITION_H
#define DM32_POSITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Positions in 1/100000 degree, + north / + east (the settings' fixed point, converted)

// Our own position, from the settings (the GPS receiver or the place typed in radio_info.c); false: not known
bool position_own(int32_t *latitude_e5, int32_t *longitude_e5);

// Distance in metres and bearing in degrees (0 north, 90 east) from the first position to the second
void position_range(int32_t latitude1_e5, int32_t longitude1_e5, int32_t latitude2_e5, int32_t longitude2_e5,
                    uint32_t *metres, uint16_t *bearing);

// A received position as the screens show it: the distance and the direction from ours ("12.3 km NE"), or the
// coordinates ("34.6037S 58.3816W") when ours is not known. position_format_short() is the compact form for a list
// row: no space before the unit ("12.3km NE") and the coordinates with two decimals ("34.60S 58.38W").
void position_format(char *out, size_t size, int32_t latitude_e5, int32_t longitude_e5);
void position_format_short(char *out, size_t size, int32_t latitude_e5, int32_t longitude_e5);

#endif
