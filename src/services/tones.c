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
 * OpenDMR, service: the CTCSS tones and DCS codes (the main firmware's TRX_CTCSSTones[] / TRX_DCSCodes[]).
 */

#include <stdio.h>
#include "codeplug.h"
#include "tones.h"

static const uint16_t ctcss_tones[] = {670,  693,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000,
                                       1035, 1072, 1109, 1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567,
                                       1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966,
                                       1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541};
static const uint16_t dcs_codes[] = {0x023, 0x025, 0x026, 0x031, 0x032, 0x043, 0x047, 0x051, 0x054, 0x065, 0x071, 0x072,
                                     0x073, 0x074, 0x114, 0x115, 0x116, 0x125, 0x131, 0x132, 0x134, 0x143, 0x152, 0x155,
                                     0x156, 0x162, 0x165, 0x172, 0x174, 0x205, 0x223, 0x226, 0x243, 0x244, 0x245, 0x251,
                                     0x261, 0x263, 0x265, 0x271, 0x306, 0x311, 0x315, 0x331, 0x343, 0x345, 0x351, 0x364,
                                     0x365, 0x371, 0x411, 0x412, 0x413, 0x423, 0x431, 0x432, 0x445, 0x464, 0x465, 0x466,
                                     0x503, 0x506, 0x516, 0x532, 0x546, 0x565, 0x606, 0x612, 0x624, 0x627, 0x631, 0x632,
                                     0x654, 0x662, 0x664, 0x703, 0x712, 0x723, 0x731, 0x732, 0x734, 0x743, 0x754};

#define CTCSS_COUNT ((int)(sizeof(ctcss_tones) / sizeof(ctcss_tones[0])))
#define DCS_COUNT   ((int)(sizeof(dcs_codes) / sizeof(dcs_codes[0])))

int tones_count(void)
{
    return 1 + CTCSS_COUNT + (2 * DCS_COUNT);
}

int tones_position(uint16_t tone)
{
    if (tone == CODEPLUG_CSS_NONE) {
        return 0;
    }
    if (tone & TONES_DCS) {
        for (int i = 0; i < DCS_COUNT; i++) {
            if (dcs_codes[i] == (tone & TONES_DCS_CODE)) {
                return 1 + CTCSS_COUNT + ((tone & TONES_DCS_INVERTED) ? DCS_COUNT : 0) + i;
            }
        }
        return 0;
    }
    for (int i = 0; i < CTCSS_COUNT; i++) {
        if (ctcss_tones[i] == tone) {
            return 1 + i;
        }
    }
    return 0;
}

uint16_t tones_at(int position)
{
    if ((position <= 0) || (position >= tones_count())) {
        return CODEPLUG_CSS_NONE;
    }
    if (position <= CTCSS_COUNT) {
        return ctcss_tones[position - 1];
    }
    position -= 1 + CTCSS_COUNT;
    return (uint16_t)(TONES_DCS | ((position >= DCS_COUNT) ? TONES_DCS_INVERTED : 0) | dcs_codes[position % DCS_COUNT]);
}

uint16_t tones_step(uint16_t tone, int step)
{
    int position = tones_position(tone) + step;

    return tones_at((position < 0) ? 0 : ((position >= tones_count()) ? (tones_count() - 1) : position));
}

void tones_format(uint16_t tone, char *out, size_t size)
{
    if (tone == CODEPLUG_CSS_NONE) {
        snprintf(out, size, "%s", "");
    } else if (tone & TONES_DCS) {
        snprintf(out, size, "D%03X%c", tone & TONES_DCS_CODE, ((tone & TONES_DCS_INVERTED) ? 'I' : 'N'));
    } else {
        snprintf(out, size, "%d.%dHz", tone / 10, tone % 10);
    }
}
