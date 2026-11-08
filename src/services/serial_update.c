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
 * OpenDMR, service: firmware update over the programming cable, without the bootloader's upgrade mode
 * (tools/debug/fw_update.py; started by the debug request 'D' 'U' "UPD!" of the cable link).
 *
 * Only a loader: the update program itself comes from the PC (tools/debug/updater/updater.c, linked for the area this
 * firmware reports), so it can change without flashing. The loader runs from RAM with the interrupts off, polls the
 * UART and makes no library calls; the radio is already stopped (cable_link.c).
 *
 * Requests from the PC, big endian, each answered:
 *   'H'                                   -> "UPL1" area(4) size(4) (also sent once at the start)
 *   'L' addr(4) len(4) data(len) crc32(4) -> 'K' loaded, or 'E' error(1): 1 damaged, 2 not in the area, 5 cut short
 *   'J' addr(4)                           -> 'J', then a jump to addr (in the area), never back
 */

#include <stdbool.h>
#include <stdint.h>
#include "hw.h"
#include "serial_update.h"

#define LOADER __attribute__((section(".staticcode"), optimize("no-tree-loop-distribute-patterns", "no-jump-tables")))

#define UART0(offset) (*(volatile uint32_t *)(0x14030000 + (offset)))
#define UART_RBR      0x00
#define UART_THR      0x00
#define UART_LSR      0x14
#define UART_USR      0x7C
#define BYTE_POLLS    30000000 // UART polls before a request cut short is dropped (about 2 s)

#define ERR_CRC     1
#define ERR_ADDRESS 2
#define ERR_TIMEOUT 5

extern uint8_t __update_area_start[], __update_area_end[]; // ckcpu.ld

// In the firmware image: fw_update.py writes only a firmware with this loader in it (else the next update would need
// the upgrade mode again)
const char serial_update_marker[] __attribute__((used)) = "DM32-SERIAL-UPDATE-LOADER-1";

uint32_t serial_update_area(void)
{
    return (uint32_t)__update_area_start;
}

uint32_t serial_update_area_size(void)
{
    return (uint32_t)(__update_area_end - __update_area_start);
}

static void LOADER uart_put(uint8_t c)
{
    while ((UART0(UART_USR) & 0x02) == 0) {
    }
    UART0(UART_THR) = c;
}

// A byte, or -1 after BYTE_POLLS empty polls (wait: no limit)
static int LOADER uart_get(bool wait)
{
    for (uint32_t n = 0; wait || (n < BYTE_POLLS); n++) {
        if (UART0(UART_LSR) & 0x01) {
            return UART0(UART_RBR) & 0xFF;
        }
    }
    return -1;
}

static bool LOADER uart_get_word(uint32_t *v)
{
    uint32_t w = 0;

    for (int i = 0; i < 4; i++) {
        int c = uart_get(false);
        if (c < 0) {
            return false;
        }
        w = (w << 8) | (uint32_t)c;
    }
    *v = w;
    return true;
}

static uint32_t LOADER crc32_add(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int k = 0; k < 8; k++) {
        crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return crc;
}

static void LOADER uart_put_word(uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        uart_put(v >> shift);
    }
}

static void LOADER hello(uint32_t start, uint32_t end)
{
    uart_put('U');
    uart_put('P');
    uart_put('L');
    uart_put('1');
    uart_put_word(start);
    uart_put_word(end - start);
}

static bool LOADER in_area(uint32_t start, uint32_t end, uint32_t address, uint32_t length)
{
    return (address >= start) && (address <= end) && (length <= (end - address));
}

static void LOADER load(uint32_t start, uint32_t end)
{
    uint32_t address = 0, length = 0, crc_sent, crc = 0xFFFFFFFF;
    int error = 0;

    if (!uart_get_word(&address) || !uart_get_word(&length)) {
        error = ERR_TIMEOUT;
    } else if (!in_area(start, end, address, length)) {
        error = ERR_ADDRESS; // the data is still read, and dropped
    }
    for (uint32_t i = 0; (error != ERR_TIMEOUT) && (i < length); i++) {
        int c = uart_get(false);
        if (c < 0) {
            error = ERR_TIMEOUT;
        } else {
            crc = crc32_add(crc, c);
            if (error == 0) {
                ((volatile uint8_t *)address)[i] = c;
            }
        }
    }
    if ((error == 0) && !uart_get_word(&crc_sent)) {
        error = ERR_TIMEOUT;
    }
    if ((error == 0) && (~crc != crc_sent)) {
        error = ERR_CRC;
    }
    if (error == 0) {
        uart_put('K');
    } else {
        uart_put('E');
        uart_put(error);
    }
}

void LOADER serial_update_run(void)
{
    uint32_t start = (uint32_t)__update_area_start, end = (uint32_t)__update_area_end;

    __asm__ volatile("psrclr ie" ::: "memory");
    WDG_LOCK = WDG_UNLOCK; // the watchdog off: nothing feeds it any more (the update program resets with it)
    WDG_EN = 0;
    WDG_LOCK = 0;
    hello(start, end);
    for (;;) {
        int request = uart_get(true);
        uint32_t address;

        if (request == 'H') {
            hello(start, end);
        } else if (request == 'L') {
            load(start, end);
        } else if ((request == 'J') && uart_get_word(&address)) {
            if (in_area(start, end, address, 2)) {
                uart_put('J');
                ((void (*)(void))address)();
            }
            uart_put('E');
            uart_put(ERR_ADDRESS);
        }
    }
}
