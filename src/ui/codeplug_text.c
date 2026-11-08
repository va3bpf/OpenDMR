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
 * OpenDMR, UI: the codeplug names as text. The CPS writes them in GB2312; the screens draw UTF-8.
 */

#include <stdint.h>
#include "font_cjk.h"
#include "codeplug_text.h"

void codeplug_text_name(const char *name, int length, char *out, size_t out_size)
{
    size_t o = 0;

    for (int i = 0; (i < length) && (name[i] != 0) && (name[i] != (char)0xFF) && ((o + 4) < out_size); i++) {
        uint8_t c = (uint8_t)name[i];
        uint16_t u = 0;

        if ((c >= 0xA1) && ((i + 1) < length)) {
            u = font_cjk_gb2312_to_unicode(c, (uint8_t)name[i + 1]);
        }
        if (u >= 0x800) {
            out[o++] = 0xE0 | (u >> 12);
            out[o++] = 0x80 | ((u >> 6) & 0x3F);
            out[o++] = 0x80 | (u & 0x3F);
            i++;
        } else if (u >= 0x80) {
            out[o++] = 0xC0 | (u >> 6);
            out[o++] = 0x80 | (u & 0x3F);
            i++;
        } else {
            out[o++] = (c < 0x80) ? c : '?';
        }
    }
    out[o] = 0;
}
