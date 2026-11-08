/*
 * Copyright (C) 2026      OpenDM32 contributors
 * Copyright (C) 2026      Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, service: debug commands over the programming cable, for tools/debug/dm32dbg.py (use
 * --elf src/build/opendmr.elf for the symbols). From firmware/dh4570_IAP/user/SOURCE/functions/
 * debugCommands.c; the commands of the drivers not brought over yet (FD6818, CSS, RDA5807, squelch) answer '-'
 * for now.
 *
 * Values are big endian. Reply: 'D' + command (+ data), or '-' for an unknown command.
 *   'D' 'm' addr(4) len(2)      memory read, bytes                     -> 'D' 'm' data
 *   'D' 'W' addr(4) len(2) data memory write, bytes                    -> 'D' 'W'
 *   'D' 'v' addr(4) width(1)    read one value with an 8/16/32 bit access -> 'D' 'v' value(4)
 *   'D' 'V' addr(4) width(1) value(4)              write one value      -> 'D' 'V'
 *   'D' 'b' addr(4) width(1) mask(4) value(4)      read-modify-write    -> 'D' 'b' old(4) new(4)
 *   'D' 'x' addr(4) r0(4) r1(4) r2(4) r3(4)        call a function      -> 'D' 'x' r0(4)
 *   'D' 'P' addr(4) len(2) data   write in the data flash window (flash_write())  -> 'D' 'P' ok(1)
 *   'D' 'F' addr(4) part(1) sum(2) data(1024)    a quarter of a 4K sector of the Chinese font (tools/fonts/upload_cjk.py),
 *                               at a raw flash address in [FONT_CJK_FLASH_ADDRESS, FONT_CJK_FLASH_END) only; sum: of the
 *                               data bytes, 16 bits (a request cut short by the UART is refused). The sector is erased,
 *                               programmed and read back with part 3                      -> 'D' 'F' ok(1)
 */

#include <string.h>
#include "debug.h"
#include "flash.h"
#include "font_cjk.h"

static uint8_t font_sector[FLASH_SECTOR_SIZE]; // the Chinese font sector being uploaded

static uint32_t get_word(volatile uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_word(volatile uint8_t *p, uint32_t value)
{
    p[0] = (value >> 24) & 0xFF;
    p[1] = (value >> 16) & 0xFF;
    p[2] = (value >> 8) & 0xFF;
    p[3] = value & 0xFF;
}

static uint32_t read_value(uint32_t address, uint8_t width)
{
    switch (width) {
        case 1:
            return *(volatile uint8_t *)address;
        case 2:
            return *(volatile uint16_t *)address;
        default:
            return *(volatile uint32_t *)address;
    }
}

static void write_value(uint32_t address, uint8_t width, uint32_t value)
{
    switch (width) {
        case 1:
            *(volatile uint8_t *)address = value;
            break;
        case 2:
            *(volatile uint16_t *)address = value;
            break;
        default:
            *(volatile uint32_t *)address = value;
            break;
    }
}

uint32_t debug_handle_command(volatile uint8_t *request, volatile uint8_t *reply, uint32_t reply_size)
{
    uint32_t address = get_word(&request[2]);
    uint32_t reply_length = 2;

    reply[0] = 'D';
    reply[1] = request[1];

    switch (request[1]) {
        case 'm': {
            uint32_t length = (request[6] << 8) | request[7];

            if (length > (reply_size - 2)) {
                length = reply_size - 2;
            }
            memcpy((uint8_t *)&reply[2], (uint8_t *)address, length);
            reply_length = 2 + length;
        } break;

        case 'W':
            memcpy((uint8_t *)address, (uint8_t *)&request[8], (request[6] << 8) | request[7]);
            break;

        case 'v':
            put_word(&reply[2], read_value(address, request[6]));
            reply_length = 6;
            break;

        case 'V':
            write_value(address, request[6], get_word(&request[7]));
            break;

        case 'b': {
            uint32_t mask = get_word(&request[7]);
            uint32_t old = read_value(address, request[6]);
            uint32_t new = (old & ~mask) | (get_word(&request[11]) & mask);

            write_value(address, request[6], new);
            put_word(&reply[2], old);
            put_word(&reply[6], new);
            reply_length = 10;
        } break;

        case 'x': {
            uint32_t (*function)(uint32_t, uint32_t, uint32_t, uint32_t) =
                (uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))address;

            put_word(&reply[2], function(get_word(&request[6]), get_word(&request[10]), get_word(&request[14]),
                                         get_word(&request[18])));
            reply_length = 6;
        } break;

        case 'P':
            reply[2] = flash_write(address, (const uint8_t *)&request[8], (request[6] << 8) | request[7]) ? 1 : 0;
            reply_length = 3;
            break;

        case 'F': {
            uint8_t part = request[6];
            uint16_t sum = (request[7] << 8) | request[8];

            reply[2] = 0;
            reply_length = 3;
            for (int i = 0; i < 1024; i++) {
                sum -= request[9 + i];
            }

            if ((sum == 0) && (address >= FONT_CJK_FLASH_ADDRESS) && (address < FONT_CJK_FLASH_END) &&
                ((address % FLASH_SECTOR_SIZE) == 0) && (part < 4)) {
                memcpy(&font_sector[part * 1024], (uint8_t *)&request[9], 1024);
                reply[2] = 1;

                if (part == 3) {
                    uint8_t check[256];

                    reply[2] = flash_write_sector_raw(address, font_sector) ? 1 : 0;
                    for (uint32_t i = 0; (i < FLASH_SECTOR_SIZE) && reply[2]; i += sizeof(check)) {
                        flash_read_raw(address + i, check, sizeof(check));
                        reply[2] = (memcmp(check, &font_sector[i], sizeof(check)) == 0) ? 1 : 0;
                    }
                }
            }
        } break;

        default:
            reply[0] = '-';
            reply_length = 1;
            break;
    }

    return reply_length;
}
