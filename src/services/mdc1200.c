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
 * OpenDMR, service: MDC1200 packets. After the MDC1200 encoder of the UV-K5 firmware (OneOfEleven
 * uv-k5-firmware-custom mdc1200.c, which sends it with the BK4819's FFSK, as fd6818_tx_fsk() does).
 *
 * A single packet: op, arg, the unit ID (high byte first), the CRC (CCITT reflected, inverted, low byte first) and a
 * 0, then their R = 1/2, K = 7 convolutional code; the 14 bytes interleaved bit by bit (16 x 7), the whole packet
 * with the preamble and the sync differentially coded and inverted. Checked against the known packet: 01 80 1234 gives
 * the CRC 2E3E and the code 6580A862DD8808.
 */

#include <string.h>
#include "mdc1200.h"

#define DATA_SIZE     7 // op, arg, ID x 2, CRC x 2, 0
#define CODED_SIZE    (2 * DATA_SIZE)
#define CODED_BITS    (8 * CODED_SIZE)
#define INTERLEAVE    16 // the bits go in columns of 16

static const uint8_t preamble[3] = {0x00, 0x00, 0x00};
static const uint8_t sync[5] = {0x07, 0x09, 0x2A, 0x44, 0x6F};

static uint16_t crc(const uint8_t *data, int length)
{
    uint16_t value = 0;

    for (int i = 0; i < length; i++) {
        value ^= data[i];
        for (int k = 0; k < 8; k++) {
            value = (value & 1) ? ((value >> 1) ^ 0x8408) : (value >> 1);
        }
    }
    return value ^ 0xFFFF;
}

// The data (bit 0 first) through the coder: bit n + n-2 + n-5 + n-6, after the data
static void add_code(uint8_t data[CODED_SIZE])
{
    uint8_t shift = 0;

    for (int i = 0; i < DATA_SIZE; i++) {
        uint8_t out = 0;

        for (int bit = 0; bit < 8; bit++) {
            shift = (uint8_t)((shift << 1) | ((data[i] >> bit) & 1));
            out |= (uint8_t)((((shift >> 6) ^ (shift >> 5) ^ (shift >> 2) ^ shift) & 1) << bit);
        }
        data[DATA_SIZE + i] = out;
    }
}

// Bit k of the input (bit 0 first in each byte) to place k x 16 (mod 111), sent most significant bit first
static void interleave(uint8_t data[CODED_SIZE])
{
    uint8_t bits[CODED_BITS];
    int place = 0;

    for (int i = 0; i < CODED_SIZE; i++) {
        for (int bit = 0; bit < 8; bit++) {
            bits[place] = (data[i] >> bit) & 1;
            place += INTERLEAVE;
            if (place >= CODED_BITS) {
                place -= CODED_BITS - 1;
            }
        }
    }
    place = 0;
    for (int i = 0; i < CODED_SIZE; i++) {
        uint8_t byte = 0;

        for (int bit = 7; bit >= 0; bit--) {
            byte |= (uint8_t)(bits[place++] << bit);
        }
        data[i] = byte;
    }
}

// A 1 where a bit differs from the one before, then all inverted
static void differential(uint8_t *data, int length)
{
    uint8_t previous = 0;

    for (int i = 0; i < length; i++) {
        uint8_t out = 0;

        for (int bit = 7; bit >= 0; bit--) {
            uint8_t now = (data[i] >> bit) & 1;

            out |= (uint8_t)((now != previous) << bit);
            previous = now;
        }
        data[i] = (uint8_t)~out;
    }
}

void mdc1200_encode(uint8_t packet[MDC1200_PACKET_SIZE], uint8_t op, uint8_t arg, uint16_t unit_id)
{
    uint8_t *data = packet + sizeof(preamble) + sizeof(sync);
    uint16_t check;

    memcpy(packet, preamble, sizeof(preamble));
    memcpy(packet + sizeof(preamble), sync, sizeof(sync));
    data[0] = op;
    data[1] = arg;
    data[2] = (uint8_t)(unit_id >> 8);
    data[3] = (uint8_t)unit_id;
    check = crc(data, 4);
    data[4] = (uint8_t)check;
    data[5] = (uint8_t)(check >> 8);
    data[6] = 0;
    add_code(data);
    interleave(data);
    differential(packet, MDC1200_PACKET_SIZE);
}
