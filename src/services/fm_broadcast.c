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
 * OpenDMR, service: the FM broadcast receiver. From firmware/dh4570_IAP/user/SOURCE/functions/fmRadio.c (the
 * register use and the RDS decoding) and hardware/RDA5807.c (the chip access).
 *
 * The chip is an RDA5807M on a software I2C (SDIO GPIO 0x19, SCLK GPIO 0x1A, the DM-32 pin map of the OpenDM32 forum thread),
 * 16-bit registers through the random access address 0x11 (drivers/rda5807.c); 32.768 kHz crystal, antenna on LNAP,
 * its audio straight to the speaker amplifier (radio.c: the amplifier's broadcast user).
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "rda5807.h"
#include "radio.h"
#include "fm_broadcast.h"

// Registers
#define REG_CTRL    0x02
#define REG_CHANNEL 0x03
#define REG_VOLUME  0x05
#define REG_STATUS  0x0A
#define REG_RSSI    0x0B
#define REG_RDS_A   0x0C // blocks A - D: 0x0C - 0x0F

// REG_CTRL
#define CTRL_DHIZ       0x8000 // audio outputs driven
#define CTRL_DMUTE      0x4000 // not muted
#define CTRL_SEEKUP     0x0200
#define CTRL_SEEK       0x0100
#define CTRL_RDS_EN     0x0008
#define CTRL_NEW_METHOD 0x0004
#define CTRL_SOFT_RESET 0x0002
#define CTRL_ENABLE     0x0001
#define CTRL_ON         (CTRL_DHIZ | CTRL_RDS_EN | CTRL_NEW_METHOD | CTRL_ENABLE) // 32.768 kHz clock: CLK_MODE 000

// REG_CHANNEL: channel in bits 15 - 6, TUNE; band 87 - 108 MHz (00), 100 kHz spacing (00)
#define CHANNEL_SHIFT 6
#define CHANNEL_TUNE  0x0010
// REG_VOLUME: LNAP input (the antenna is there), volume 15 (the volume knob does the rest)
#define VOLUME_VALUE 0x888F
// REG_STATUS
#define STATUS_RDSR 0x8000 // an RDS group is ready
#define STATUS_STC  0x4000 // seek / tune complete
#define STATUS_ST   0x0400 // stereo
#define STATUS_CHAN 0x03FF
// REG_RSSI
#define RSSI_SHIFT   9
#define RSSI_FM_TRUE 0x0100
#define RSSI_BLERB   0x0003 // block B errors (0: none)

#define CRYSTAL_START_MS 500 // datasheet: 0.5 s after enabling, before the volume and the tune

#define RDS_GROUP_PS        0 // programme service name
#define RDS_GROUP_TEXT      2 // radio text
#define RDS_VERSION_B       0x0800
#define RDS_CARRIAGE_RETURN 0x0D // end of the radio text
#define RDS_SAME_GROUP_MS   60   // the same blocks again sooner: the group read twice (a group every 88 ms)

static bool is_on = false;
static bool seeking = false;
static bool muted = false;
static uint16_t frequency = 999;

// RDS name: 4 segments of 2 characters, sent in turn 0, 1, 2, 3; stations scroll text through it. A turn is taken
// when its 4 segments came in order, each the same as in the turn before (block D, the characters, has no error flag
// on the RDA5807M); then the whole name is shown, else the last one stays. A name changing within a turn, a wrong
// character or a lost group spoils that turn only: no name mixing two (counted per segment, they were mixed). The radio
// text shown, and the last character received at each place.
#define RDS_NAME_SEGMENTS 4
#define RDS_NAME_ALL      ((1U << RDS_NAME_SEGMENTS) - 1)

static char rds_name[FM_BROADCAST_RDS_NAME + 1];
static char rds_name_heard[FM_BROADCAST_RDS_NAME]; // each segment as last received
static uint8_t rds_name_got;                       // bit n: segment n came in this turn
static bool rds_name_same;                         // this turn's segments so far as in the turn before
static int rds_name_last_segment = RDS_NAME_SEGMENTS - 1;
static char rds_text[FM_BROADCAST_RDS_TEXT + 1];
static char rds_text_candidate[FM_BROADCAST_RDS_TEXT];
static int rds_text_ab = -1;
static uint16_t rds_last_blocks[4];
static TickType_t rds_last_tick;

/* The chip */

static bool chip_write(uint8_t reg, uint16_t value)
{
    return rda5807_write(reg, value);
}

static bool chip_read(uint8_t reg, uint16_t *value)
{
    *value = 0;
    return rda5807_read(reg, value);
}

static void chip_wait_crystal(void)
{
    vTaskDelay(pdMS_TO_TICKS(CRYSTAL_START_MS)); // the caller is the UI task
}

static void audio_amplifier(bool on)
{
    radio_broadcast_amp(on);
}

/* RDS */

static void rds_clear(void)
{
    memset(rds_name, 0, sizeof(rds_name));
    memset(rds_name_heard, 0, sizeof(rds_name_heard));
    rds_name_got = 0;
    rds_name_same = false;
    rds_name_last_segment = RDS_NAME_SEGMENTS - 1;
    memset(rds_text, 0, sizeof(rds_text));
    memset(rds_text_candidate, 0, sizeof(rds_text_candidate));
    rds_text_ab = -1;
}

static uint8_t rds_char(uint8_t c)
{
    return ((c < 0x20) || (c > 0x7E)) ? ' ' : c; // RDS characters outside ASCII: not drawn
}

// A name segment; true when a new name is shown
static bool rds_name_segment(int segment, uint8_t c1, uint8_t c2)
{
    char *heard = &rds_name_heard[segment * 2];
    char c[2] = {(char)rds_char(c1), (char)rds_char(c2)};
    bool turn_done;

    if (segment <= rds_name_last_segment) { // a new turn
        rds_name_got = 0;
        rds_name_same = true;
    }
    if (segment != (rds_name_last_segment + 1) % RDS_NAME_SEGMENTS) {
        rds_name_same = false; // a segment lost
    }
    rds_name_last_segment = segment;
    if (memcmp(heard, c, 2) != 0) {
        rds_name_same = false;
        memcpy(heard, c, 2);
    }
    rds_name_got |= (uint8_t)(1U << segment);

    turn_done = (segment == (RDS_NAME_SEGMENTS - 1));
    if (!turn_done || !rds_name_same || (rds_name_got != RDS_NAME_ALL) ||
        (memcmp(rds_name, rds_name_heard, FM_BROADCAST_RDS_NAME) == 0)) {
        return false;
    }
    memcpy(rds_name, rds_name_heard, FM_BROADCAST_RDS_NAME);
    return true;
}

// Puts a received radio text character at its place: an empty place shows it at once, a shown one changes when the new
// character comes twice; true when the text shown changed
static bool rds_put(char *shown, char *candidate, int at, uint8_t c)
{
    if (c == RDS_CARRIAGE_RETURN) {
        bool changed = (shown[at] != 0);

        shown[at] = 0;
        return changed;
    }
    c = rds_char(c);
    if ((shown[at] == 0) || ((candidate[at] == c) && (shown[at] != c))) {
        bool changed = (shown[at] != c);

        shown[at] = (char)c;
        candidate[at] = (char)c;
        return changed;
    }
    candidate[at] = (char)c;
    return false;
}

bool fm_broadcast_poll_rds(void)
{
    uint16_t status = 0;
    uint16_t errors = 0;
    uint16_t block[4];
    bool changed = false;

    if (!is_on || seeking || !chip_read(REG_STATUS, &status) || ((status & STATUS_RDSR) == 0)) {
        return false;
    }
    chip_read(REG_RSSI, &errors);
    for (int i = 0; i < 4; i++) {
        chip_read(REG_RDS_A + i, &block[i]);
    }
    if (errors & RSSI_BLERB) {
        return false; // the group type is not certain
    }

    TickType_t now = xTaskGetTickCount();

    if ((memcmp(block, rds_last_blocks, sizeof(block)) == 0) && ((now - rds_last_tick) < pdMS_TO_TICKS(RDS_SAME_GROUP_MS))) {
        return false; // the group just read: not a second reception
    }
    memcpy(rds_last_blocks, block, sizeof(block));
    rds_last_tick = now;

    uint8_t group = block[1] >> 12;

    if (group == RDS_GROUP_PS) {
        changed |= rds_name_segment(block[1] & 0x03, block[3] >> 8, block[3] & 0xFF); // 2 characters in block D
    } else if (group == RDS_GROUP_TEXT) {
        int ab = (block[1] >> 4) & 1;
        int segment = block[1] & 0x0F;

        if (ab != rds_text_ab) { // a new text
            changed = (rds_text[0] != 0);
            memset(rds_text, 0, sizeof(rds_text));
            memset(rds_text_candidate, 0, sizeof(rds_text_candidate));
            rds_text_ab = ab;
        }
        if (block[1] & RDS_VERSION_B) { // 2 characters in block D
            changed |= rds_put(rds_text, rds_text_candidate, segment * 2, block[3] >> 8);
            changed |= rds_put(rds_text, rds_text_candidate, (segment * 2) + 1, block[3] & 0xFF);
        } else { // 4 in blocks C and D
            changed |= rds_put(rds_text, rds_text_candidate, segment * 4, block[2] >> 8);
            changed |= rds_put(rds_text, rds_text_candidate, (segment * 4) + 1, block[2] & 0xFF);
            changed |= rds_put(rds_text, rds_text_candidate, (segment * 4) + 2, block[3] >> 8);
            changed |= rds_put(rds_text, rds_text_candidate, (segment * 4) + 3, block[3] & 0xFF);
        }
    }
    return changed;
}

const char *fm_broadcast_rds_name(void)
{
    return rds_name;
}

const char *fm_broadcast_rds_text(void)
{
    return rds_text;
}

/* Receiver */

static uint16_t ctrl(void)
{
    return CTRL_ON | (muted ? 0 : CTRL_DMUTE);
}

void fm_broadcast_mute(bool mute)
{
    if (is_on && (mute != muted)) {
        muted = mute;
        chip_write(REG_CTRL, ctrl() | (seeking ? CTRL_SEEK : 0));
    }
}

bool fm_broadcast_start(uint16_t start_frequency)
{
    if (!chip_write(REG_CTRL, CTRL_SOFT_RESET)) {
        return false;
    }
    muted = false;
    chip_write(REG_CTRL, ctrl());
    chip_wait_crystal();
    chip_write(REG_VOLUME, VOLUME_VALUE);
    is_on = true;
    seeking = false;
    fm_broadcast_tune(start_frequency);
    audio_amplifier(true);
    return true;
}

void fm_broadcast_stop(void)
{
    if (is_on) {
        audio_amplifier(false);
        chip_write(REG_CTRL, 0x0000); // powered down
        is_on = false;
        seeking = false;
    }
}

bool fm_broadcast_is_on(void)
{
    return is_on;
}

void fm_broadcast_tune(uint16_t to)
{
    if (to < FM_BROADCAST_FREQ_MIN) {
        to = FM_BROADCAST_FREQ_MAX;
    } else if (to > FM_BROADCAST_FREQ_MAX) {
        to = FM_BROADCAST_FREQ_MIN;
    }
    if (seeking) {
        chip_write(REG_CTRL, ctrl()); // stops the seek
        seeking = false;
    }
    frequency = to;
    rds_clear();
    chip_write(REG_CHANNEL, ((to - FM_BROADCAST_FREQ_MIN) << CHANNEL_SHIFT) | CHANNEL_TUNE);
}

void fm_broadcast_seek(bool up)
{
    chip_write(REG_CTRL, ctrl() | CTRL_SEEK | (up ? CTRL_SEEKUP : 0)); // the chip wraps at the band edges
    seeking = true;
    rds_clear();
}

void fm_broadcast_get_status(fm_broadcast_status_t *status)
{
    uint16_t chip_status = 0;
    uint16_t rssi = 0;
    bool read = chip_read(REG_STATUS, &chip_status);

    chip_read(REG_RSSI, &rssi);

    if (seeking) {
        if (!read) {
            seeking = false; // no chip: the seek never ends otherwise
        } else {
            frequency = FM_BROADCAST_FREQ_MIN + (chip_status & STATUS_CHAN); // follows the seek, kept at its end
            if (chip_status & STATUS_STC) {
                chip_write(REG_CTRL, ctrl());
                seeking = false;
            }
        }
    }

    status->frequency = frequency;
    status->rssi = rssi >> RSSI_SHIFT;
    status->station = ((rssi & RSSI_FM_TRUE) != 0);
    status->stereo = ((chip_status & STATUS_ST) != 0);
    status->seeking = seeking;
}
