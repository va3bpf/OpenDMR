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
 * OpenDMR, service: our options (extra_options.h), in a block of ours in the settings' sector, after the PIN block
 * (security.c, 0x6100): not in the OpenGD77 settings, whose bits its CPS has its own meanings for, and not in its
 * custom data, which some codeplugs have no header for. A CPS sector write keeps it. Blank: the defaults (all 0).
 */

#include <string.h>
#include "flash.h"
#include "extra_options.h"

#define ADDRESS 0x6140
#define MAGIC   0x3154504F // "OPT1"

typedef struct {
    uint32_t magic;
    uint32_t bits;
    uint8_t theme; // 0xFF: none chosen (a block written before it)
    uint8_t reserved[3];
} block_t;

static block_t block;

void extra_options_load(void)
{
    if (!flash_read(ADDRESS, (uint8_t *)&block, sizeof(block)) || (block.magic != MAGIC)) {
        memset(&block, 0xFF, sizeof(block));
        block.magic = MAGIC;
        block.bits = 0;
    }
}

bool extra_option(uint32_t bit)
{
    return (block.bits & bit) != 0;
}

void extra_option_set(uint32_t bit, bool on)
{
    uint32_t bits = on ? (block.bits | bit) : (block.bits & ~bit);

    if (bits != block.bits) {
        block.bits = bits;
        (void)flash_write(ADDRESS, (const uint8_t *)&block, sizeof(block));
    }
}

uint8_t extra_options_theme(void)
{
    return block.theme;
}

void extra_options_set_theme(uint8_t theme)
{
    if (theme != block.theme) {
        block.theme = theme;
        (void)flash_write(ADDRESS, (const uint8_t *)&block, sizeof(block));
    }
}

void extra_options_keep_block(uint32_t sector_address, uint8_t *sector)
{
    if ((ADDRESS >= sector_address) && ((ADDRESS + sizeof(block)) <= (sector_address + FLASH_SECTOR_SIZE))) {
        block_t stored;

        if (flash_read(ADDRESS, (uint8_t *)&stored, sizeof(stored))) {
            memcpy(&sector[ADDRESS - sector_address], &stored, sizeof(stored));
        }
    }
}
