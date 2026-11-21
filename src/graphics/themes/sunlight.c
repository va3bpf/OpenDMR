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
 * OpenDMR, theme "Sunlight" (theme.h theme_t). Clear sky made for outdoors: black text and every layer dark
 * enough to stay readable in bright light (labels 11:1, dim text 7:1, lines near 4:1 on white; the colours 6 - 7:1).
 */

#include "themes.h"
#include "text.h"

const theme_t theme_sunlight = {
    .name = TEXT_THEME_SUNLIGHT,
    .dark = false,
    .colours = {
        0xFFFFFF, // background
        0x7A8696, // line: the separators of the main area
        0xDDE2E8, // off
        0x2E3A48, // dim text
        0x101820, // labels
        0x000000, // second text
        0x000000, // text
        0xB0420A, // accent
        0x008A3E, // receive
        0xE0102E, // transmit
        0x8A5600, // warning
        0xFFFFFF, // on the receive / transmit / warning fills
        0xFFFFFF, // on the accent
        0x0B3D91, // panel
        0xFFFFFF, // panel text
        0xD4E2F8, // panel labels
    },
};
